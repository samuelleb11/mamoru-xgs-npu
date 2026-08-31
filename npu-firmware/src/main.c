// SPDX-License-Identifier: MIT
/*
 * dp_app — custom NPU data-plane for the Sophos XGS116 (CN9130).
 * Replaces Sophos musdk_nmp_standalone + dpdk_target_sample_app + NetAgent with ONE
 * self-built binary that owns the GIU LIF + CC_PF_* host handshake and (from M1b) the
 * giu<->pp2 forwarder. See ARCHITECTURE.md.
 *
 * M1a (this file, first cut): init + host handshake + NMP scheduling ONLY.
 *   mv_sys_dma_mem_init -> nmp_read_cfg_file -> nmp_init (blocks until host sets
 *   HOST_MGMT_READY, pf.c:438) -> ctrl thread loops NMP_SCHED_MNG (processes CC_PF_INIT/
 *   TC_ADD/DATA_Q_ADD/INIT_DONE/ENABLE), dp0 thread loops NMP_SCHED_RX/TX (drives the GIE).
 *   No forwarder yet -> the GIU comes up and the host sees the ports, but no traffic moves.
 *   This isolates "does OUR-built MUSDK nmp handshake with our FreeBSD if_agnic?".
 * M1b will add giu_setup(guest+gpio) + pp2_setup + the forwarder onto this skeleton.
 *
 * Thread model (ARCHITECTURE §A.2): ctrl/init on core 0 (non-isolated), dp0 on core 1
 * (isolcpus 1-3). NW_AGENT + switch threads arrive at M3 and must be spawned BEFORE the
 * nmp_init blocking spin (they run during the host-wait); until then nmp_init blocks main.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "mv_std.h"
#include "env/mv_sys_dma.h"
#include "mng/mv_nmp.h"

#include "dp_config.h"
#include "portmap.h"

#define DP_DMA_MEM_SIZE		(48 * 1024 * 1024)	/* CMA arena for our app (room for M1b pools) */

struct dp_ctx {
	struct nmp	*nmp;
	char		 nmp_cfg[256];
	int		 affinity_base;		/* -a: base core, or -1 */
	volatile int	 running;
};

static struct dp_ctx dp = { .affinity_base = -1, .running = 1 };

static void on_sig(int s) { (void)s; dp.running = 0; }

/* Pin the calling thread to a single core (best-effort; warns, never fails hard). */
static void pin_core(int core, const char *who)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(core, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
		pr_warn("dp_app: could not pin %s to core %d: %s\n", who, core, strerror(errno));
	else
		pr_info("dp_app: %s pinned to core %d\n", who, core);
}

/* ctrl thread: process the host management channel (CC_PF_* + keep-alives) forever. */
static void *ctrl_thread(void *arg)
{
	(void)arg;
	pin_core(DP_CTRL_CORE, "ctrl");
	pr_info("dp_app: ctrl thread up (NMP_SCHED_MNG)\n");
	while (dp.running)
		nmp_schedule(dp.nmp, NMP_SCHED_MNG, NULL);
	return NULL;
}

/* dp0 thread: drive the GIE DMA engines (host<->local). Forwarder loop is added in M1b. */
static void *dp0_thread(void *arg)
{
	int core = (dp.affinity_base >= 0) ? DP_DP0_CORE : DP_DP0_CORE;
	(void)arg;
	pin_core(core, "dp0");
	pr_info("dp_app: dp0 thread up (NMP_SCHED_RX/TX; no forwarder yet [M1a])\n");
	while (dp.running) {
		nmp_schedule(dp.nmp, NMP_SCHED_RX, NULL);	/* GIU_ENG_OUT: NPU->host */
		nmp_schedule(dp.nmp, NMP_SCHED_TX, NULL);	/* GIU_ENG_IN:  host->NPU */
		/* TODO M1b: forwarder_poll(): giu_gpio_recv->pp2_ppio_send, pp2_ppio_recv->giu_gpio_send */
	}
	return NULL;
}

static void online_cpu_check(void)
{
	FILE *f = fopen("/sys/devices/system/cpu/online", "r");
	char buf[64] = {0};
	if (f) { if (fgets(buf, sizeof buf, f)) buf[strcspn(buf, "\n")] = 0; fclose(f); }
	pr_info("dp_app: cpu online = '%s' (need core %d for dp0)\n", buf, DP_DP0_CORE);
}

static int parse_args(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-f") && i + 1 < argc) {
			strncpy(dp.nmp_cfg, argv[++i], sizeof(dp.nmp_cfg) - 1);
		} else if (!strcmp(argv[i], "-a") && i + 1 < argc) {
			dp.affinity_base = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			++i;	/* accepted for launcher compatibility; ignored (we own our threads) */
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("dp_app -f <nmp-config> [-a <core>] [-c <n>]\n");
			exit(0);
		} else {
			pr_err("dp_app: unknown arg '%s'\n", argv[i]);
			return -EINVAL;
		}
	}
	if (!dp.nmp_cfg[0]) { pr_err("dp_app: -f <nmp-config> required\n"); return -EINVAL; }
	return 0;
}

int main(int argc, char **argv)
{
	struct nmp_params nmp_params;
	pthread_t ctrl_tid, dp0_tid;
	int err;

	setbuf(stdout, NULL);
	pr_info("dp_app: starting (M1a: handshake+schedule, no forwarder)\n");

	err = parse_args(argc, argv);
	if (err)
		return -err;

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	online_cpu_check();

	err = mv_sys_dma_mem_init(DP_DMA_MEM_SIZE);
	if (err) { pr_err("dp_app: mv_sys_dma_mem_init(%d) failed: %d\n", DP_DMA_MEM_SIZE, err); return 1; }
	pr_info("dp_app: CMA arena %d MiB ready\n", DP_DMA_MEM_SIZE >> 20);

	memset(&nmp_params, 0, sizeof(nmp_params));
	err = nmp_read_cfg_file(dp.nmp_cfg, &nmp_params);
	if (err) { pr_err("dp_app: nmp_read_cfg_file(%s) failed: %d\n", dp.nmp_cfg, err); return 1; }
	pr_info("dp_app: nmp cfg '%s' parsed\n", dp.nmp_cfg);

	/* Blocks at pf.c:438 until the host asserts HOST_MGMT_READY (host must be present). */
	pr_info("dp_app: nmp_init() — waiting for host (if_agnic) HOST_MGMT_READY ...\n");
	err = nmp_init(&nmp_params, &dp.nmp);
	if (err) { pr_err("dp_app: nmp_init failed: %d\n", err); return 1; }
	pr_info("dp_app: nmp_init OK — host handshake reached; starting schedulers\n");

	if (pthread_create(&ctrl_tid, NULL, ctrl_thread, NULL) != 0) { pr_err("ctrl thread create failed\n"); return 1; }
	if (pthread_create(&dp0_tid, NULL, dp0_thread, NULL) != 0)  { pr_err("dp0 thread create failed\n");  return 1; }

	pthread_join(dp0_tid, NULL);
	pthread_join(ctrl_tid, NULL);

	pr_info("dp_app: shutting down\n");
	mv_sys_dma_mem_destroy();
	return 0;
}
