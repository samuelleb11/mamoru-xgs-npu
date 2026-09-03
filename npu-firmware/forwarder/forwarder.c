/*******************************************************************************
 * Copyright (C) Marvell International Ltd. and its affiliates
 *
 * This software file (the "File") is owned and distributed by Marvell
 * International Ltd. and/or its affiliates ("Marvell") under the following
 * alternative licensing terms.  Once you have made an election to distribute the
 * File under one of the following license alternatives, please (i) delete this
 * introductory statement regarding license alternatives, (ii) delete the three
 * license alternatives that you have not elected to use and (iii) preserve the
 * Marvell copyright notice above.
 *
 ********************************************************************************
 * Marvell Commercial License Option
 *
 * If you received this File from Marvell and you have entered into a commercial
 * license agreement (a "Commercial License") with Marvell, the File is licensed
 * to you under the terms of the applicable Commercial License.
 *
 ********************************************************************************
 * Marvell GPL License Option
 *
 * If you received this File from Marvell, you may opt to use, redistribute and/or
 * modify this File in accordance with the terms and conditions of the General
 * Public License Version 2, June 1991 (the "GPL License"), a copy of which is
 * available along with the File in the license.txt file or by writing to the Free
 * Software Foundation, Inc., or on the worldwide web at http://www.gnu.org/licenses/gpl.txt.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE ARE EXPRESSLY
 * DISCLAIMED.  The GPL License provides additional details about this warranty
 * disclaimer.
 *
 ********************************************************************************
 * Marvell GNU General Public License FreeRTOS Exception
 *
 * If you received this File from Marvell, you may opt to use, redistribute and/or
 * modify this File in accordance with the terms and conditions of the Lesser
 * General Public License Version 2.1 plus the following FreeRTOS exception.
 * An independent module is a module which is not derived from or based on
 * FreeRTOS.
 * Clause 1:
 * Linking FreeRTOS statically or dynamically with other modules is making a
 * combined work based on FreeRTOS. Thus, the terms and conditions of the GNU
 * General Public License cover the whole combination.
 * As a special exception, the copyright holder of FreeRTOS gives you permission
 * to link FreeRTOS with independent modules that communicate with FreeRTOS solely
 * through the FreeRTOS API interface, regardless of the license terms of these
 * independent modules, and to copy and distribute the resulting combined work
 * under terms of your choice, provided that:
 * 1. Every copy of the combined work is accompanied by a written statement that
 * details to the recipient the version of FreeRTOS used and an offer by yourself
 * to provide the FreeRTOS source code (including any modifications you may have
 * made) should the recipient request it.
 * 2. The combined work is not itself an RTOS, scheduler, kernel or related
 * product.
 * 3. The independent modules add significant and primary functionality to
 * FreeRTOS and do not merely extend the existing functionality already present in
 * FreeRTOS.
 * Clause 2:
 * FreeRTOS may not be used for any competitive or comparative purpose, including
 * the publication of any form of run time or compile time metric, without the
 * express permission of Real Time Engineers Ltd. (this is the norm within the
 * industry and is intended to ensure information accuracy).
 *
 ********************************************************************************
 * Marvell BSD License Option
 *
 * If you received this File from Marvell, you may opt to use, redistribute and/or
 * modify this File under the following licensing terms.
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *	* Redistributions of source code must retain the above copyright notice,
 *	  this list of conditions and the following disclaimer.
 *
 *	* Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 *
 *	* Neither the name of Marvell nor the names of its contributors may be
 *	  used to endorse or promote products derived from this software without
 *	  specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>	/* D84 #24-f: mmap the host-visible BAR0 capability window */
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>

#include "mv_std.h"
/* D84 switch-register handler. Included as a SINGLE TRANSLATION UNIT because the MUSDK
 * app makefile builds exactly one .c for pkt_echo, and adding an object would mean
 * patching the vendor autotools build -- far more fragile than this. dp_swop.c is still
 * compiled and unit-tested STANDALONE on the host (src/dp_swop_test.c, 40 checks incl.
 * mutation controls), so this is a link-time convenience, not a testing shortcut. */
#include "dp_swop.c"
#include "lib/lib_misc.h"
#include "lib/file_utils.h"
#include "env/mv_sys_dma.h"

#include "mvapp.h"
#include "lib/mv_pme.h"
#include "mv_pp2.h"
#include "mv_pp2_hif.h"
#include "mv_pp2_bpool.h"
#include "mv_pp2_ppio.h"

#include "pp2_utils.h"

#include "nmp_guest_utils.h"
#include "mv_giu_gpio.h"
#include "giu_utils.h"
#include "mng/mv_nmp.h"
#include "mng/mv_nmp_guest.h"

/* dp_app: pport<->DSA framing (forked giu_pkt_echo -> our giu<->pp2<->switch forwarder). */
#include "tag_dsa.h"

/* dp_app forwarding instrumentation: where do frames stop? Printed ~1 Hz from main_loop_cb. */
static volatile unsigned long dp_giu_rx, dp_pp2_tx;	/* host->front: giu recv, pp2 sent */
static volatile unsigned long dp_pp2_rx, dp_giu_tx;	/* front->host: pp2 recv, giu sent */
/* dp_app drop counters (Phase A robustness): frames we deliberately drop instead of forwarding
 * corrupt/undeliverable. A drop is always better than emitting an un-translated frame or wedging. */
static volatile unsigned long dp_t2h_drop;	/* front->host: t2h transform failed (bad/short DSA frame) */
/* Counted apart from dp_t2h_drop on purpose: a short/malformed frame is line noise, but an
 * UNKNOWN SWITCH SOURCE PORT means the switch handed us an ingress we do not model, which is a
 * topology or tag-decode fault. It cannot happen in normal operation (see tag_dsa.h), so any
 * non-zero value here is worth investigating rather than averaging into a generic drop count. */
static volatile unsigned long dp_t2h_badport;	/* front->host: DSA src port not in portmap (-4) */
static volatile unsigned long dp_h2t_drop;	/* host->front: h2t transform failed (bad pport tag) */
static volatile unsigned long dp_egr_full_drop;	/* host->front: pp2 TX ring stayed full after retries */


#define PKT_ECHO_APP_MAX_NUM_CORES		3

#define PKT_ECHO_APP_TX_RETRY_MAX		3
#define PKT_ECHO_APP_DEF_Q_SIZE			1024
#define PKT_ECHO_APP_HIF_Q_SIZE			(8 * PKT_ECHO_APP_DEF_Q_SIZE)
#define PKT_ECHO_APP_RX_Q_SIZE			(2 * PKT_ECHO_APP_DEF_Q_SIZE)
#define PKT_ECHO_APP_TX_Q_SIZE			(2 * PKT_ECHO_APP_DEF_Q_SIZE)

#define PKT_ECHO_APP_GIU_BP_SIZE		4096

#define PKT_ECHO_APP_NET_INTERFACE		"eth0"

#define PKT_ECHO_APP_MAX_BURST_SIZE		((PKT_ECHO_APP_RX_Q_SIZE) >> 2)
/* as GIU is the bottleneck, set the burst size to GIU_Q_SIZE / 4 */
#define PKT_ECHO_APP_DFLT_BURST_SIZE		(PKT_ECHO_APP_MAX_BURST_SIZE >> 1)

#define PKT_ECHO_APP_DMA_MEM_SIZE		(80 * 1024 * 1024)
#define PKT_ECHO_APP_STATS_DFLT_THRESH		1000
#define PKT_ECHO_APP_CTRL_TRD_THRESH		200

#define PKT_ECHO_APP_FIRST_INQ			0
#define PKT_ECHO_APP_MAX_NUM_TCS_PER_PORT	1
#define PKT_ECHO_APP_MAX_NUM_QS_PER_CORE	PKT_ECHO_APP_MAX_NUM_TCS_PER_PORT

#define PKT_ECHO_APP_PKT_ECHO_SUPPORT
#define PKT_ECHO_APP_PREFETCH_SHIFT		7

#define PKT_ECHO_APP_BPOOLS_INF		{ {384, 4096, 0, NULL}, {2048, 4096, 0, NULL} }
#define PKT_ECHO_APP_BPOOLS_JUMBO_INF	{ {2048, 4096}, {10240, 512} }

#define QUEUE_OCCUPANCY(prod, cons, q_size)	\
	(((prod) - (cons) + (q_size)) & ((q_size) - 1))

#define QUEUE_SPACE(prod, cons, q_size)	\
	((q_size) - QUEUE_OCCUPANCY((prod), (cons), (q_size)) - 1)

/* NMP Guest ID */
#define PKT_ECHO_APP_NMP_GUEST_ID	2
/* NMP Guest Timeout (ms)*/
#define PKT_ECHO_APP_NMP_GUEST_TIMEOUT	10000

#define CHECK_CYCLES
#ifdef CHECK_CYCLES
#define START_COUNT_CYCLES(_ev_cnt)		pme_ev_cnt_start(_ev_cnt)
#define STOP_COUNT_CYCLES(_ev_cnt, _num)	pme_ev_cnt_stop(_ev_cnt, _num)
#else
#define START_COUNT_CYCLES(_ev_cnt)
#define STOP_COUNT_CYCLES(_ev_cnt, _num)
#endif /* CHECK_CYCLES */


/* When pkt-echo is running as part of the management application
 * the egress/ingress loop code is running as part of the management scheduling.
 * Therefore, we would like to limit it to a single pass/loop over the
 * ingress/egress (and not infinite loop at it should be in standalone app)
 */

struct glob_arg {
	struct glb_common_args		 cmn_args; /* Keep first */

	u16				 rxq_size;
	int				 loopback;
	int				 maintain_stats;
	int				 pkt_rate_stats;
	pthread_mutex_t			 trd_lock;

	struct giu_bpools_desc		 giu_bpools_desc;
	struct giu_port_desc		 giu_port_desc;
	struct nmp			*nmp;
	struct nmp_guest		*nmp_guest;
	struct nmp_guest_info		guest_info;
	char				*prb_str;
};

struct local_arg {
	struct local_common_args	 cmn_args; /* Keep first */

	struct lcl_giu_port_desc	 giu_ports_desc[MVAPPS_GIU_MAX_NUM_PORTS];
};

static struct glob_arg garg = {};

/* set by main_loop_cb once the forwarder owns RX/TX; the init-time MNG pump reads it to stop
 * scheduling RX/TX after the handshake (see mng_pump_thread). */
static volatile int dp_main_loop_running = 0;

#ifdef CHECK_CYCLES
static int pme_ev_cnt_pp2_rx = -1, pme_ev_cnt_pp2_tx = -1, pme_ev_cnt_pp2_txd = -1;
static int pme_ev_cnt_giu_rx = -1, pme_ev_cnt_giu_tx = -1, pme_ev_cnt_giu_txd = -1;
static int pme_ev_cnt_gie_ingr = -1, pme_ev_cnt_gie_egr = -1;
#endif /* CHECK_CYCLES */

#define PKT_ECHO_APP_INC_RX_COUNT(core, port, cnt)		(rx_buf_cnt[core][port] += cnt)
#define PKT_ECHO_APP_INC_TX_COUNT(core, port, cnt)		(tx_buf_cnt[core][port] += cnt)
#define PKT_ECHO_APP_INC_TX_RETRY_COUNT(core, port, cnt)	(tx_buf_retry[core][port] += cnt)
#define PKT_ECHO_APP_INC_TX_DROP_COUNT(core, port, cnt)	(tx_buf_drop[core][port] += cnt)
#define PKT_ECHO_APP_INC_FREE_COUNT(core, port, cnt)		(free_buf_cnt[core][port] += cnt)
#define PKT_ECHO_APP_SET_MAX_RESENT(core, port, cnt)		\
	{ if (cnt > tx_max_resend[core][port]) tx_max_resend[core][port] = cnt; }

#define PKT_ECHO_APP_SET_MAX_BURST(core, port, burst)	\
	{ if (burst > tx_max_burst[core][port]) tx_max_burst[core][port] = burst; }

u32 rx_buf_cnt[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];
u32 free_buf_cnt[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];
u32 tx_buf_cnt[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];
u32 tx_buf_drop[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];
u32 tx_buf_retry[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];
u32 tx_max_resend[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];
u32 tx_max_burst[MVAPPS_MAX_NUM_CORES][MVAPPS_PP2_MAX_NUM_PORTS];

/* globals for ingress/egress packet rate statistics */
struct timeval		 ctrl_trd_last_time;
u64			 lst_rx_cnt[MVAPPS_PP2_MAX_NUM_PORTS];
u64			 lst_tx_cnt[MVAPPS_PP2_MAX_NUM_PORTS];


static int dump_perf(struct glob_arg *garg)
{
	struct timeval	 curr_time;
	u64		 tmp_time_inter;
	u32		 tmp_rx_cnt[MVAPPS_PP2_MAX_NUM_PORTS];
	u32		 tmp_tx_cnt[MVAPPS_PP2_MAX_NUM_PORTS];
	u32		 drop_cnt[MVAPPS_PP2_MAX_NUM_PORTS];
	int		 i, j;

	gettimeofday(&curr_time, NULL);
	tmp_time_inter = (curr_time.tv_sec - ctrl_trd_last_time.tv_sec) * 1000;
	tmp_time_inter += (curr_time.tv_usec - ctrl_trd_last_time.tv_usec) / 1000;

	for (j = 0; j < 2; j++) {
		drop_cnt[j] = 0;
		for (i = 0; i < garg->cmn_args.cpus; i++)
			drop_cnt[j] += tx_buf_drop[i][j];
	}
	for (j = 0; j < 2; j++) {
		tmp_rx_cnt[j] = tmp_tx_cnt[j] = 0;
		for (i = 0; i < garg->cmn_args.cpus; i++) {
			tmp_rx_cnt[j] += rx_buf_cnt[i][j];
			tmp_tx_cnt[j] += tx_buf_cnt[i][j];
		}
	}
	printf("Perf:");
	for (j = 0; j < 2; j++) {
		printf("%s: %dKpps (Rx: %dKpps)\t",
			j ? " egress" : " ingress",
			(int)((tmp_tx_cnt[j] - lst_tx_cnt[j]) / tmp_time_inter),
			(int)((tmp_rx_cnt[j] - lst_rx_cnt[j]) / tmp_time_inter));
		lst_rx_cnt[j] = tmp_rx_cnt[j];
		lst_tx_cnt[j] = tmp_tx_cnt[j];
		if (drop_cnt[j])
			printf(", drop: %u", drop_cnt[j]);
	}
	printf("\n");
	gettimeofday(&ctrl_trd_last_time, NULL);

	return 0;
}

static int maintain_stats(void *arg)
{
	struct glob_arg *garg = (struct glob_arg *)arg;
	struct timeval	 curr_time;
	u64		 tmp_time_inter;

	if (!garg) {
		pr_err("no obj!\n");
		return -EINVAL;
	}

	gettimeofday(&curr_time, NULL);
	tmp_time_inter = (curr_time.tv_sec - ctrl_trd_last_time.tv_sec) * 1000;
	tmp_time_inter += (curr_time.tv_usec - ctrl_trd_last_time.tv_usec) / 1000;
	if (tmp_time_inter >= garg->cmn_args.ctrl_thresh)
		return dump_perf(garg);

	return 0;
}

static int perf_cmd_cb(void *arg, int argc, char *argv[])
{
	struct glob_arg *garg = (struct glob_arg *)arg;

	if (!garg) {
		pr_err("no garg obj passed!\n");
		return -EINVAL;
	}
	if (argc != 1) {
		pr_err("Invalid number of arguments for perf cmd!\n");
		return -EINVAL;
	}

	return dump_perf(arg);
}

#ifdef CHECK_CYCLES
static int pme_cmd_cb(void *arg, int argc, char *argv[])
{
	struct glob_arg *garg = (struct glob_arg *)arg;

	if (!garg) {
		pr_err("no garg obj passed!\n");
		return -EINVAL;
	}
	if (argc != 1) {
		pr_err("Invalid number of arguments for PME cmd!\n");
		return -EINVAL;
	}

	printf("Ingress:\n");
	pme_ev_cnt_dump(pme_ev_cnt_pp2_rx, 1);
	pme_ev_cnt_dump(pme_ev_cnt_giu_tx, 1);
	pme_ev_cnt_dump(pme_ev_cnt_pp2_txd, 1);
	pme_ev_cnt_dump(pme_ev_cnt_gie_ingr, 1);
	printf("Egress:\n");
	pme_ev_cnt_dump(pme_ev_cnt_gie_egr, 1);
	pme_ev_cnt_dump(pme_ev_cnt_giu_rx, 1);
	pme_ev_cnt_dump(pme_ev_cnt_pp2_tx, 1);
	pme_ev_cnt_dump(pme_ev_cnt_giu_txd, 1);
	return 0;
}
#endif /* CHECK_CYCLES */

static inline u16 giu_free_buffers(struct lcl_giu_port_desc	*rx_port,
				   u16				start_idx,
				   u16				num,
				   u8				tc)
{
	u16			i, free_cnt = 0, idx = start_idx;
	struct giu_bpool	*bpool;
	struct giu_buff_inf	*binf;
	struct giu_tx_shadow_q	*shadow_q;

	shadow_q = &rx_port->shadow_qs[tc];

	for (i = 0; i < num; i++) {
		bpool = shadow_q->ents[idx].bpool;
		binf = (struct giu_buff_inf *)&shadow_q->ents[idx].buff_inf;
		if (unlikely(!binf->cookie || !binf->addr || !bpool)) {
			pr_warn("Shadow memory @%d: cookie(%lx), pa(%lx), pool(%lx)!\n",
				i, (u64)binf->cookie, (u64)binf->addr, (u64)bpool);
		} else {
			giu_bpool_put_buff(bpool, binf);
			free_cnt++;
		}
		/* Advance idx on EVERY entry, including the corrupt-skip path: a bare `continue`
		 * here left idx stuck, so the returned read_ind desynced from write_ind and the
		 * shadow buffer accounting drifted (leak / double-free) after one bad entry. */
		if (++idx == rx_port->shadow_q_size)
			idx = 0;
	}

	PKT_ECHO_APP_INC_FREE_COUNT(rx_port->lcl_id, rx_port->id, free_cnt);
	return idx;
}

/* During ingress flow, the PP2 (Rx side) saves the buffers in its shadow Q.
 * These buffers are taken from PP2 during receive and used by GIU during transmit.
 * In addition, the shadow Q also contains the PP2 BPool.
 *
 * In this function the transmitted buffers are being placed back to the PP2 BPool.
 */
static inline u16 pp2_free_multi_buffers(struct lcl_port_desc		*rx_port,
					 struct lcl_giu_port_desc	*tx_port,
					 struct pp2_hif			*hif,
					 u16				start_idx,
					 u16				num,
					 u8				tc)
{
	u16			idx = start_idx;
	u16			cont_in_shadow, req_num;
	struct tx_shadow_q	*shadow_q;

	shadow_q = &rx_port->shadow_qs[tc];

	cont_in_shadow = rx_port->shadow_q_size - start_idx;

	if (num <= cont_in_shadow) {
		req_num = num;
		pp2_bpool_put_buffs(hif, (struct buff_release_entry *)&shadow_q->ents[idx], &req_num);
		idx = idx + num;
		if (idx == rx_port->shadow_q_size)
			idx = 0;
	} else {
		req_num = cont_in_shadow;
		pp2_bpool_put_buffs(hif, (struct buff_release_entry *)&shadow_q->ents[idx], &req_num);

		req_num = num - cont_in_shadow;
		pp2_bpool_put_buffs(hif, (struct buff_release_entry *)&shadow_q->ents[0], &req_num);
		idx = num - cont_in_shadow;
	}

	PKT_ECHO_APP_INC_FREE_COUNT(rx_port->lcl_id, rx_port->id, num);

	return idx;
}

/* This function is called by during Ingress flow.
 * In this flow the PP2 is the Rx side (as it reads the packets from the network)
 * and the GIU is the Tx side (Transmits the packet to the host over PCI).
 * The PP2 is allocating the buffers (which GIU uses in its packets) so the
 * buffers should be put back to the PP2 BPool.
 * Therefore, the flow is:
 *	- Read how many packets the GIU has transmitted (since last time it
 *	  was sampled).
 *	- Use the shadow Q (of Rx side) to get the (PP2) BPool and buffers.
 *	- Put back the buffers to the PP2 BPool.
 *
 * Note: some of the functionality described above is done in the inner functions
 *	 (called by this function).
 */
static inline void pp2_free_sent_buffers(struct lcl_port_desc		*rx_port,
					 struct lcl_giu_port_desc	*tx_port,
					 struct pp2_hif			*hif,
					 u8				tc,
					 u8				qid)
{
	u16 tx_num;

	START_COUNT_CYCLES(pme_ev_cnt_pp2_txd);
	/* Read from giu how many packets were already transmitted
	 * so their buffers can be put back to the pp2 bpool
	 */
	giu_gpio_get_num_outq_done(tx_port->gpio, tc, qid, &tx_num);

	/* No buffer to release */
	if (tx_num == 0) {
		STOP_COUNT_CYCLES(pme_ev_cnt_pp2_txd, 0);
		return;
	}

	/* Return back the buffers to pp2 */
	rx_port->shadow_qs[tc].read_ind =
		pp2_free_multi_buffers(rx_port, tx_port, hif, rx_port->shadow_qs[tc].read_ind, tx_num, tc);

	STOP_COUNT_CYCLES(pme_ev_cnt_pp2_txd, tx_num);
}

/* This function is called by during Egress flow.
 * In this flow the GIU is the Rx side (as it reads the host packets over PCI)
 * and the PP2 is the Tx side (Transmits the packet to the network).
 * The GIU is allocating the buffers (which PP2 uses in its packets) so the
 * buffers should be put back to the GIU BPool.
 * Therefore, the flow is:
 *	- Read how many packets the PP2 has transmitted (since last time it
 *	  was sampled).
 *	- Use the shadow Q (of Rx side) to get the (GIU) BPool and buffers
 *	- Put back the buffers to the GIU BPool.
 *
 * Note: some of the functionality described above is done in the inner functions
 *	 (called by this function).
 */
static inline void giu_free_sent_buffers(struct lcl_giu_port_desc	*rx_port,
					 struct lcl_port_desc		*tx_port,
					 struct pp2_hif			*hif,
					 u8				tc)
{
	u16 tx_num;

	START_COUNT_CYCLES(pme_ev_cnt_giu_txd);
	/* Read from pp2 how many packets were already transmitted
	 * so their buffers can be put back to the giu bpool
	 */
	pp2_ppio_get_num_outq_done(tx_port->ppio, hif, tc, &tx_num);

	/* No buffer to release */
	if (tx_num == 0) {
		STOP_COUNT_CYCLES(pme_ev_cnt_giu_txd, 0);
		return;
	}

	/* Return back the buffers to giu */
	rx_port->shadow_qs[tc].read_ind =
		giu_free_buffers(rx_port, rx_port->shadow_qs[tc].read_ind, tx_num, tc);

	STOP_COUNT_CYCLES(pme_ev_cnt_giu_txd, tx_num);
}

/* In Ingress flow, the PP2 is the Rx side (as it reads the packets from the network)
 * and the GIU is the Tx side (Transmits the packet to the host over PCI).
 *
 * After the PP2 receives the packets (and allocates the buffers), the descriptors are
 * changed to fit GIU format and then the GIU can transmit them to the host.
 *
 * At the end of the flow the transmitted buffers (may also be from previous egress)
 * are placed back in the PP2 BPool.
 *
 * Therefore, the flow is:
 *	- PP2 receives packets from network.
 *	- The descriptors are manipulated to fit GIU format.
 *	- GIU transmits the packets (and saves the buffers in its shadow).
 *	- The transmitted buffers are placed back to the PP2 BPool.
 *
 * The PP2 (Rx side) shadow Q is used to save the BPool and buffers.
 * After they are transmitted, they can be released.
 */
static inline int loop_sw_ingress(struct local_arg	*larg,
				  u8			 rx_ppio_id,
				  u8			 tx_ppio_id,
				  u8			 tc,
				  u8			 qid,
				  u16			 num)
{
	struct tx_shadow_q	 *shadow_q;
	int			 shadow_q_size;
	struct pp2_ppio_desc	 pp2_descs[PKT_ECHO_APP_MAX_BURST_SIZE]; /* TODO - remove from stack to malloc */
	struct pp2_lcl_common_args	*pp2_args = (struct pp2_lcl_common_args *) larg->cmn_args.plat;
	struct lcl_port_desc		*pp2_port_desc = &(pp2_args->lcl_ports_desc[rx_ppio_id]);
	struct lcl_giu_port_desc	*giu_port_desc = &(larg->giu_ports_desc[tx_ppio_id]);
	struct giu_gpio_desc		*giu_descs;
	u16			 i, tx_num, free_count;
	u16			 desc_idx = 0, cnt = 0, out_i = 0;	/* out_i: forwarded-frame count after drops */
	u16			 pkt_offset = MVAPPS_PP2_PKT_EFEC_OFFS(pp2_port_desc->pkt_offset[tc]);
	enum pp2_inq_vlan_tag	 vlan_tag;
	enum pp2_inq_l2_cast_type l2_cast;
	enum pp2_inq_l3_cast_type l3_cast;
	enum pp2_inq_l3_type     l3_type;
	enum pp2_inq_l4_type     l4_type;
	enum pp2_inq_desc_status desc_status;
	enum giu_outq_ip_status ip_status = GIU_OUTQ_IPV4_CHECKSUM_UNKNOWN;
	enum giu_outq_l4_status l4_status = GIU_OUTQ_L4_CHECKSUM_UNKNOWN;
	u8                       l3_offset, l4_offset;

	/* Note: PP2 descriptors and GIU descriptors has similar
	 *	 structure so it's ok to use the same descriptors
	 *	 for both interfaces.
	 */
	giu_descs = (struct giu_gpio_desc *)pp2_descs;

	/* Use PP2 Shadow Q to save allocated buffers */
	shadow_q = &pp2_port_desc->shadow_qs[tc];
	shadow_q_size = pp2_port_desc->shadow_q_size;

	/* pr_info("ingress: tid %d check on tc %d, qid %d\n", larg->cmn_args.id, tc, qid); */
	/* pthread_mutex_lock(&larg->garg->trd_lock); */

	/* Make sure that we are not trying to send more packets than the space
	 * we have in the tx-shadow queue.
	 * In this case, simply clip the number of sent packets to the space
	 * available in the tx-shadow queue.
	 */
	free_count = QUEUE_SPACE(shadow_q->write_ind, shadow_q->read_ind, shadow_q_size);
	if (num > free_count)
		num = free_count;

	if (num) {
		START_COUNT_CYCLES(pme_ev_cnt_pp2_rx);
		pp2_ppio_recv(pp2_port_desc->ppio, tc, qid, pp2_descs, &num);
		dp_pp2_rx += num;
		STOP_COUNT_CYCLES(pme_ev_cnt_pp2_rx, num);
	}

	/* pthread_mutex_unlock(&larg->garg->trd_lock); */
	/* if (num) pr_info("ingress: got %d pkts on tc %d, qid %d\n", num, tc, qid); */

	PKT_ECHO_APP_INC_RX_COUNT(larg->cmn_args.id, 0, num);

	for (i = 0; i < num; i++) {
		char *buff    = (void *)(app_get_high_addr() | pp2_ppio_inq_desc_get_cookie(&pp2_descs[i]));
		dma_addr_t pa = pp2_ppio_inq_desc_get_phys_addr(&pp2_descs[i]);
		u16 len       = pp2_ppio_inq_desc_get_pkt_len(&pp2_descs[i]);

		/* Get pp2 bpool (as the received buffers should be returned to it) */
		void *bpool = pp2_ppio_inq_desc_get_bpool(&pp2_descs[i], pp2_port_desc->ppio);

#if 0
		/* This code displays the packet' buffer */
		char *tmp_buff = (char *)((uintptr_t)(buff));

		tmp_buff += pkt_offset;
		printf("In packet: (@%p,0x%x)\n", buff, pa); mem_disp(tmp_buff, len);
#endif
		/* Get L2 error state */
		desc_status = pp2_ppio_inq_desc_get_l2_pkt_error(&pp2_descs[i]);

		ip_status = GIU_OUTQ_IP_OK;

		/* If no L2 error ,check L3 error state */
		if (!desc_status)
			desc_status = pp2_ppio_inq_desc_get_l3_pkt_error(&pp2_descs[i]);
		if (desc_status)
			ip_status = GIU_OUTQ_IPV4_CHECKSUM_ERR;

		l4_status = GIU_OUTQ_IP_OK;
		/* If no L3 error ,check L4 error state */
		if (!desc_status)
			desc_status = pp2_ppio_inq_desc_get_l4_pkt_error(&pp2_descs[i]);
		if (desc_status)
			l4_status = GIU_OUTQ_L4_CHECKSUM_ERR;

		pp2_ppio_inq_desc_get_vlan_tag(&pp2_descs[i], &vlan_tag);
		pp2_ppio_inq_desc_get_l2_cast_info(&pp2_descs[i], &l2_cast);
		pp2_ppio_inq_desc_get_l3_cast_info(&pp2_descs[i], &l3_cast);

		pp2_ppio_inq_desc_get_l3_info(&pp2_descs[i], &l3_type, &l3_offset);
		pp2_ppio_inq_desc_get_l4_info(&pp2_descs[i], &l4_type, &l4_offset);

		/* dp_app front->host framing: strip DSA + prepend 66B pport prefix.
		 * Frame sits at pa+pkt_offset; the pkt_offset headroom absorbs the prepend. */
		uint8_t *fv = mv_sys_dma_mem_phys2virt(pa + pkt_offset);
		uint8_t *fo = NULL; uint16_t folen = 0;
		/* DSA-format debug: dump the raw [dst6][src6][DSA4][etype] of the first frames
		 * so we can verify the 88E6193X TO_CPU tag layout and mirror it for FROM_CPU. */
		static int dp_dsa_dbg;
		if (fv && dp_dsa_dbg < 16) {
			dp_dsa_dbg++;
			const uint8_t *dsa = fv + ETH_MAC_LEN;
			pr_info("dp_dsa_rx: hdroom=%u dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x DSA=%02x %02x %02x %02x b16=%02x%02x cmd=%u srcport=%u idx=%d len=%u\n",
				pkt_offset, fv[0],fv[1],fv[2],fv[3],fv[4],fv[5], fv[6],fv[7],fv[8],fv[9],fv[10],fv[11],
				fv[12],fv[13],fv[14],fv[15], fv[16],fv[17],
				dsa_cmd(dsa), dsa_src_port(dsa), dp_dsa_to_index(dsa_src_port(dsa)), len);
		}
		/* Phase A: a frame we cannot translate MUST be DROPPED, never forwarded raw — a raw
		 * DSA-tagged frame would land on the wrong host ifnet (host demuxes on frame byte0)
		 * or drive the parser off the end of a runt. Return the buffer to the pp2 pool. */
		int t2h_rc = fv ? dp_t2h_transform(fv, len, pkt_offset, NULL, &fo, &folen) : -1;
		if (t2h_rc < 0) {
			struct pp2_buff_inf bi = { .addr = pa, .cookie = (uintptr_t)buff };
			pp2_bpool_put_buff(pp2_args->hif, (struct pp2_bpool *)bpool, &bi);
			if (t2h_rc == -4)
				dp_t2h_badport++;	/* unknown DSA src port -- see tag_dsa.h */
			else
				dp_t2h_drop++;
			continue;
		}
		dma_addr_t giu_pa  = (pa + pkt_offset) + (dma_addr_t)(fo - fv);
		u16        giu_len = folen;

		/* Reset the descriptor (compacted at out_i so dropped frames leave no gap). */
		giu_gpio_outq_desc_reset(&giu_descs[out_i]);

		/* Update relevant fields in the descriptor (offload/proto-info deferred to M4). */
		giu_gpio_outq_desc_set_phys_addr(&giu_descs[out_i], giu_pa);
		giu_gpio_outq_desc_set_pkt_offset(&giu_descs[out_i], 0);
		giu_gpio_outq_desc_set_pkt_len(&giu_descs[out_i], giu_len);
		(void)ip_status; (void)l4_status; (void)l2_cast; (void)vlan_tag; (void)l3_cast;
		(void)l3_type; (void)l3_offset; (void)l4_type; (void)l4_offset;

		shadow_q->ents[shadow_q->write_ind].buff_ptr.cookie = (uintptr_t)buff;
		shadow_q->ents[shadow_q->write_ind].buff_ptr.addr = pa;
		shadow_q->ents[shadow_q->write_ind].bpool = bpool;
		pr_debug("buff_ptr.cookie(0x%lx)\n", (u64)shadow_q->ents[shadow_q->write_ind].buff_ptr.cookie);
		shadow_q->write_ind++;
		if (shadow_q->write_ind == shadow_q_size)
			shadow_q->write_ind = 0;
		out_i++;
	}
	num = out_i;	/* forward only the frames we successfully translated */
	PKT_ECHO_APP_SET_MAX_BURST(larg->cmn_args.id, 0, num);

	/* Work in retry mode: Try to send the packets till all packets were transmitted */
	do {
		tx_num = num;
		if (num) {
			START_COUNT_CYCLES(pme_ev_cnt_giu_tx);
			giu_gpio_send(giu_port_desc->gpio, tc, qid, &giu_descs[desc_idx], &tx_num);
			dp_giu_tx += tx_num;
			STOP_COUNT_CYCLES(pme_ev_cnt_giu_tx, tx_num);
			if (num > tx_num) {
				if (!cnt)
					PKT_ECHO_APP_INC_TX_RETRY_COUNT(larg->cmn_args.id, 0, num - tx_num);
				cnt++;
				/* When working in single CPU mode and with a single thread, live lock might
				 * occur in a case there are no free buffers and there are still packets to sent.
				 * This might happen because the pkt-echo tries to send them endlessly and
				 * the buffer free code cannot be called.
				 * Therefore we use a transmit limit and, in case it was reached, we free the buffers
				 * which were not sent.
				 */
				if ((larg->cmn_args.garg->cmn_args.cpus == 1) && (cnt >= PKT_ECHO_APP_TX_RETRY_MAX)) {
					u16 not_sent = num - tx_num;
					/* Free un-sent buffers */
					shadow_q->write_ind = (shadow_q->write_ind < not_sent) ?
							(shadow_q_size - not_sent + shadow_q->write_ind) :
							shadow_q->write_ind - not_sent;
					pp2_free_multi_buffers(pp2_port_desc,
							       giu_port_desc,
							       pp2_args->hif,
							       shadow_q->write_ind,
							       not_sent,
							       tc);
					/* Update statistics */
					PKT_ECHO_APP_INC_TX_COUNT(larg->cmn_args.id, 0, tx_num);
					PKT_ECHO_APP_INC_TX_DROP_COUNT(larg->cmn_args.id, 0, not_sent);
					break;
				}
			}
			desc_idx += tx_num;
			num -= tx_num;
			PKT_ECHO_APP_INC_TX_COUNT(larg->cmn_args.id, 0, tx_num);
		}

		pp2_free_sent_buffers(pp2_port_desc, giu_port_desc, pp2_args->hif, tc, qid);
	} while (num);
	PKT_ECHO_APP_SET_MAX_RESENT(larg->cmn_args.id, 0, cnt);

	return 0;
}

/* In Egress flow, the GIU is the Rx side (as it reads the packets from host over PCI)
 * and the PP2 is the Tx side (Transmits the packet to the network).
 *
 * After the GIU receives the packets (and allocates the buffers), the descriptors are
 * changed to fit PP2 format and then the PP2 can transmit them out.
 *
 * At the end of the flow the transmitted buffers (may also be from previous egress)
 * are placed back in the PP2 BPool.
 *
 * Therefore, the flow is:
 *	- GIU receives packets from host.
 *	- The descriptors are manipulated to fit PP2 format.
 *	- PP2 transmits the packets (and saves the buffers in its shadow).
 *	- The transmitted buffers are placed back to the GIU BPool.
 *
 * The GIU (Rx side) shadow Q is used to save the BPool and buffers.
 * After they are transmitted, they can be released.
 */
static inline int loop_sw_egress(struct local_arg	*larg,
				 u8			 rx_ppio_id,
				 u8			 tx_ppio_id,
				 u8			 tc,
				 u8			 qid,
				 u16			 num)
{
	struct giu_tx_shadow_q	 *shadow_q;
	int			 shadow_q_size;
	struct pp2_ppio_desc	 pp2_descs[PKT_ECHO_APP_MAX_BURST_SIZE]; /* TODO - remove from stack to malloc */
	struct pp2_lcl_common_args	*pp2_args = (struct pp2_lcl_common_args *) larg->cmn_args.plat;
	struct lcl_port_desc		*pp2_port_desc = &(pp2_args->lcl_ports_desc[tx_ppio_id]);
	struct lcl_giu_port_desc	*giu_port_desc = &(larg->giu_ports_desc[rx_ppio_id]);
	struct giu_gpio_desc	*giu_descs;
	u16			 i, tx_num;
	u16			 desc_idx = 0, cnt = 0, out_i = 0;	/* out_i: sendable-frame count after drops */
	/* L3/L4 info parameters */
	u32			 l3_type, l4_type;
	u8			 l3_offset, l4_offset;
	int			 gen_l3_chk, gen_l4_chk;

	/* Note: PP2 descriptors and GIU descriptors has similar
	 *	 structure so it's ok to use the same descriptors
	 *	 for both interfaces.
	 */
	giu_descs = (struct giu_gpio_desc *)pp2_descs;

	/* Use GIU Shadow Q to save allocated buffers */
	shadow_q = &giu_port_desc->shadow_qs[tc];
	shadow_q_size = giu_port_desc->shadow_q_size;

	/* pr_info("egress: tid %d check on tc %d, qid %d\n", larg->cmn_args.id, tc, qid); */
	/* pthread_mutex_lock(&larg->garg->trd_lock); */
	START_COUNT_CYCLES(pme_ev_cnt_giu_rx);
	giu_gpio_recv((struct giu_gpio *)giu_port_desc->gpio, tc, qid, giu_descs, &num);
	dp_giu_rx += num;
	STOP_COUNT_CYCLES(pme_ev_cnt_giu_rx, num);

	/* pthread_mutex_unlock(&larg->garg->trd_lock); */
	/* if (num) pr_info("egress: got %d pkts on tc %d, qid %d\n", num, tc, qid); */

	PKT_ECHO_APP_INC_RX_COUNT(larg->cmn_args.id, 1, num);

	for (i = 0; i < num; i++) {
		char *buff    = (void *)(app_get_high_addr() | giu_gpio_inq_desc_get_cookie(&giu_descs[i]));
		dma_addr_t pa = giu_gpio_inq_desc_get_phys_addr(&giu_descs[i]);
		u16 len       = giu_gpio_inq_desc_get_pkt_len(&giu_descs[i]);

		/* Get giu bpool (as the received buffers should be returned to it) */
		void *bpool = giu_gpio_inq_desc_get_bpool(&giu_descs[i], giu_port_desc->gpio);

		/* Read L3 info */
		giu_gpio_inq_desc_get_l3_info(&giu_descs[i], &l3_type, &l3_offset, &gen_l3_chk);

		/* Read L4 info */
		giu_gpio_inq_desc_get_l4_info(&giu_descs[i], &l4_type, &l4_offset, &gen_l4_chk);

		/* dp_app host->front framing: strip 66B pport prefix + insert FROM_CPU DSA.
		 * Keep `pa` (buffer base) for the shadow/bpool reclaim; only the TX desc moves. */
		uint8_t *fv = mv_sys_dma_mem_phys2virt(pa);
		uint8_t *fo = NULL; uint16_t folen = 0;
		/* Phase A: a frame we cannot translate MUST be DROPPED, never sent raw — a raw
		 * pport-prefixed frame would leak our internal 66B header onto the switch wire.
		 * Return the buffer to the GIU pool. */
		if (!fv || dp_h2t_transform(fv, len, &fo, &folen) != 0) {
			struct giu_buff_inf bi = { .addr = pa, .cookie = (uintptr_t)buff };
			giu_bpool_put_buff((struct giu_bpool *)bpool, &bi);
			dp_h2t_drop++;
			continue;
		}
		dma_addr_t tx_pa  = pa + (dma_addr_t)(fo - fv);
		u16        tx_len = folen;

		/* Reset the descriptor (compacted at out_i so dropped frames leave no gap). */
		pp2_ppio_outq_desc_reset(&pp2_descs[out_i]);

		/* Update relevant fields in the descriptor (offload/proto-info deferred to M4). */
		pp2_ppio_outq_desc_set_phys_addr(&pp2_descs[out_i], tx_pa);
		pp2_ppio_outq_desc_set_pkt_offset(&pp2_descs[out_i], 0);
		pp2_ppio_outq_desc_set_pkt_len(&pp2_descs[out_i], tx_len);
		(void)l3_type; (void)l4_type; (void)l3_offset;
		(void)l4_offset; (void)gen_l3_chk; (void)gen_l4_chk;

		shadow_q->ents[shadow_q->write_ind].buff_inf.cookie = (uintptr_t)buff;
		shadow_q->ents[shadow_q->write_ind].buff_inf.addr = pa;
		shadow_q->ents[shadow_q->write_ind].bpool = bpool;
		pr_debug("buff_ptr.cookie(0x%lx)\n", (u64)shadow_q->ents[shadow_q->write_ind].buff_inf.cookie);

		shadow_q->write_ind++;
		if (shadow_q->write_ind == shadow_q_size)
			shadow_q->write_ind = 0;
		out_i++;
	}
	num = out_i;	/* send only the frames we successfully translated */
	PKT_ECHO_APP_SET_MAX_BURST(larg->cmn_args.id, 1, num);

	/* Work in retry mode: Try to send the packets till all packets were transmitted */
	do {
		tx_num = num;
		if (num) {
			START_COUNT_CYCLES(pme_ev_cnt_pp2_tx);
			pp2_ppio_send(pp2_port_desc->ppio, pp2_args->hif,
				      tc, &pp2_descs[desc_idx], &tx_num);
			dp_pp2_tx += tx_num;
			STOP_COUNT_CYCLES(pme_ev_cnt_pp2_tx, tx_num);
			if (num > tx_num) {
				if (!cnt)
					PKT_ECHO_APP_INC_TX_RETRY_COUNT(larg->cmn_args.id, 1, num - tx_num);
				cnt++;
				/* Phase A (P0-1): bound the egress retry, mirroring the ingress path.
				 * Without this, a pp2 TX ring that stays full — a congested or link-DOWN
				 * front port, or 802.3x pause — makes pp2_ppio_send return 0 forever, num
				 * never decreases, and this loop (hence the whole single-core forwarder)
				 * wedges until a cold reboot. A link flap is enough to trigger it. Instead:
				 * after TX_RETRY_MAX tries, drop the un-sent frames back to the GIU pool. */
				if ((larg->cmn_args.garg->cmn_args.cpus == 1) && (cnt >= PKT_ECHO_APP_TX_RETRY_MAX)) {
					u16 not_sent = num - tx_num;
					/* rewind the GIU shadow write_ind to the first un-sent entry, free those
					 * buffers back to the GIU bpool (they were never transmitted). */
					shadow_q->write_ind = (shadow_q->write_ind < not_sent) ?
							(shadow_q_size - not_sent + shadow_q->write_ind) :
							shadow_q->write_ind - not_sent;
					giu_free_buffers(giu_port_desc, shadow_q->write_ind, not_sent, tc);
					PKT_ECHO_APP_INC_TX_COUNT(larg->cmn_args.id, 1, tx_num);
					PKT_ECHO_APP_INC_TX_DROP_COUNT(larg->cmn_args.id, 1, not_sent);
					dp_egr_full_drop += not_sent;
					break;
				}
			}
			desc_idx += tx_num;
			num -= tx_num;
			PKT_ECHO_APP_INC_TX_COUNT(larg->cmn_args.id, 1, tx_num);
		}
		giu_free_sent_buffers(giu_port_desc, pp2_port_desc, pp2_args->hif, tc);
	} while (num);
	PKT_ECHO_APP_SET_MAX_RESENT(larg->cmn_args.id, 1, cnt);

	return 0;
}

static int main_loop_cb(void *arg, int *running)
{
	struct local_arg	*larg = (struct local_arg *)arg;
	int			 err = 0;
	u16			 num, tmp_num;
	u8			 tc = 0, qid = 0;
	u8			 pp2_port_id = 0, giu_port_id = 0;

	if (!larg) {
		pr_err("no obj!\n");
		return -EINVAL;
	}

	num = larg->cmn_args.burst;

	/* dp_app FIX: the forwarder now owns RX/TX — tell the init-time MNG pump to stop driving
	 * the GIE (it keeps pumping NMP_SCHED_MNG for keep-alives). Avoids two threads on RX/TX. */
	dp_main_loop_running = 1;

	{
		unsigned long dp_dbg_iter = 0;
	while (*running) {
		/* dp_app: periodic forwarding counters, emitted ONLY on the ~periodic tick (P1-6).
		 * The old print-on-change clause fired on essentially every burst under load, and
		 * with stdout unbuffered (setbuf NULL) each print is a write() syscall on the data-
		 * plane core -> throughput collapse + serial-console flood. Counters live in memory;
		 * this just samples them. giu_rx=host->NPU on GIU, pp2_tx=->eth0, pp2_rx=front->NPU,
		 * giu_tx=->host, drop=frames dropped (transform-fail / TX-full backpressure). */
		if (((++dp_dbg_iter) & 0x3FFFFF) == 0) {
			pr_info("dp_dbg: host->front[giu_rx=%lu pp2_tx=%lu drop=%lu/%lu]  front->host[pp2_rx=%lu giu_tx=%lu drop=%lu badport=%lu]\n",
				dp_giu_rx, dp_pp2_tx, dp_h2t_drop, dp_egr_full_drop, dp_pp2_rx, dp_giu_tx,
				dp_t2h_drop, dp_t2h_badport);
		}
		/* TODO: Find next TC to consume */

		if (larg->cmn_args.garg->cmn_args.cpus == 1) {
			/* when using single CPU, run all routines on it */
			/* PP2 is Rx port and GIU is Tx port */
			err  = loop_sw_ingress(larg, pp2_port_id, giu_port_id, tc, qid, num);
			/* Schedule GIE execution */
			START_COUNT_CYCLES(pme_ev_cnt_gie_egr);
			tmp_num = nmp_schedule(garg.nmp, NMP_SCHED_TX, NULL);
			STOP_COUNT_CYCLES(pme_ev_cnt_gie_egr, tmp_num);
			START_COUNT_CYCLES(pme_ev_cnt_gie_ingr);
			tmp_num = nmp_schedule(garg.nmp, NMP_SCHED_RX, NULL);
			STOP_COUNT_CYCLES(pme_ev_cnt_gie_ingr, tmp_num);
			/* GIU is Rx port and PP2 is Tx port */
			err |= loop_sw_egress(larg, giu_port_id, pp2_port_id, tc, qid, num);
		} else if (larg->cmn_args.id == 0) {
			/* In this case, we either use 2 or 3 CPUs and it is thread0,
			 * run GIE on thread0 and the other routines on the other CPUs.
			 */
			/* Schedule GIE execution */
			START_COUNT_CYCLES(pme_ev_cnt_gie_egr);
			tmp_num = nmp_schedule(garg.nmp, NMP_SCHED_TX, NULL);
			STOP_COUNT_CYCLES(pme_ev_cnt_gie_egr, tmp_num);
			START_COUNT_CYCLES(pme_ev_cnt_gie_ingr);
			tmp_num = nmp_schedule(garg.nmp, NMP_SCHED_RX, NULL);
			STOP_COUNT_CYCLES(pme_ev_cnt_gie_ingr, tmp_num);
		} else {
			/* In this case, we either use 2 or 3 CPUs (but not thread0) so we handle
			 * the second/third thread.
			 */
			if ((larg->cmn_args.garg->cmn_args.cpus == 2) ||
			    (larg->cmn_args.id == 1))
				/* PP2 is Rx port and GIU is Tx port */
				err  = loop_sw_ingress(larg, pp2_port_id, giu_port_id, tc, qid, num);
			if ((larg->cmn_args.garg->cmn_args.cpus == 2) ||
			    (larg->cmn_args.id == 2))
				/* GIU is Rx port and PP2 is Tx port */
				err |= loop_sw_egress(larg, giu_port_id, pp2_port_id, tc, qid, num);
		}
		if (err != 0)
			return err;
	}
	}	/* dp_app dbg scope */

	return 0;
}

static int ctrl_cb(void *arg)
{
	struct glob_arg *garg = (struct glob_arg *)arg;

	if (!garg) {
		pr_err("no obj!\n");
		return -EINVAL;
	}

	/* MASTER management channel is pumped by the dedicated mng_pump_thread (started right
	 * after nmp_init) — NOT here: ctrl_cb only runs after the full (slow) pp2/local init, far
	 * too late for the host's post-HOST_MGMT_READY P3a/P3b handshake. See init_all_modules.
	 *
	 * D84: the GUEST channel is no longer pumped here either, for the SAME reason, and this
	 * is the fix for a measured bench failure rather than a tidy-up. nmp_guest_schedule() is
	 * the only thing that delivers a CUSTOM message to guest_ev_cb — the A0.1 echo and every
	 * swop register op — and until 2026-09-03 this call was its ONLY caller. ctrl_cb is an
	 * mvapp callback: init_app_params() memsets mvapp_params and never sets a ctrl_cb
	 * threshold, and use_cli is 0 on a production boot, so how often it runs is mvapp's
	 * business and not something this app states or controls.
	 *
	 * The consequence is not a slow custom channel, it is a DEAD one that reports healthy.
	 * The PF channel has its own always-running pump, so CC_PF_* and the 1 Hz keep-alive
	 * keep succeeding and the host's npu_mgmt keeps reading `ok` — while every CUSTOM send
	 * times out at 5 s. Measured on the appliance 2026-09-03: PF handshake clean through
	 * boot, then the first custom send (the A0.1 echo) and four swop reads all -110, with
	 * npu_mgmt still `ok` at the time. Two channels, one health word, and the word belongs
	 * to the half that was working.
	 *
	 * Pumping it from mng_pump_thread instead keeps ONE thread driving the guest, so this
	 * is a move rather than an addition — calling it from both would race nmp_guest state.
	 */

	if (!garg->cmn_args.cli && garg->pkt_rate_stats)
		maintain_stats(garg);

	return 0;
}

static int guest_ev_cb(void *arg, enum nmp_guest_lf_type client, u8 id, u8 code, u16 indx, void *msg, u16 len)
{
	int ret = 0;
	u32 swop_magic = 0;

	/* Read the swop magic BEFORE anything can mutate the buffer. The #ifdef DEBUG block
	 * below overwrites the first four bytes -- i.e. exactly the magic -- so classifying
	 * after it would make every swop request fall through to the echo in a DEBUG build,
	 * silently, and bring-up is precisely when someone turns DEBUG on (mamoru-d7, F7).
	 * memcpy rather than a cast: msg is a void* with no alignment guarantee, and the
	 * guarded-copy pattern is the one used inside dp_swop_service. */
	if (len >= sizeof(struct dp_swop_req))
		memcpy(&swop_magic, msg, sizeof(swop_magic));

	pr_debug("guest_ev_cb was called with: client %d, id %d, code %d, indx %d len %d msg 0x%x\n",
		 client, id, code, indx, len, *(u32 *)msg);
#ifdef DEBUG
	*(u32 *)msg = 0xCDCDCDCD;
#endif /* DEBUG */

	pr_debug("guest_ev_cb Sent Notification msg 0x%x\n", *(u32 *)msg);

	if (client == NMP_GUEST_LF_T_CUSTOM) {
		/* D84: a custom message carrying the SWOP magic is a switch-register
		 * operation; anything else keeps the historical verbatim echo.
		 *
		 * The echo is NOT legacy cruft to be tidied away -- the host driver's A0.1
		 * probe proves the custom channel is live by sending a position-dependent
		 * pattern and requiring it back byte-for-byte. Dispatching on the magic
		 * keeps that probe meaningful while adding the register path beside it, so
		 * "the channel works" and "the handler works" stay separately observable.
		 * If they shared one path, a broken handler and a dead channel would look
		 * the same from the host.
		 *
		 * dp_swop_init() is idempotent and called lazily here rather than at
		 * startup ON PURPOSE: the management pump's timing is delicate (see
		 * mng_pump_thread below -- starving it fails the host's P3a ECHO and tears
		 * the link down), and a /dev/mem open + mmap during init is exactly the
		 * kind of work that has broken it before. Lazily, the cost lands on the
		 * first switch request, where a stall is diagnosable rather than fatal. */
		if (swop_magic == DP_SWOP_MAGIC) {
			u8 rbuf[sizeof(struct dp_swop_resp)];
			int n;

			(void)dp_swop_init();	/* status surfaces as E_NOMAP in the reply */
			n = dp_swop_service(msg, len, rbuf, sizeof(rbuf));
			if (n > 0)
				return nmp_guest_send_msg(garg.nmp_guest, code, indx,
							  rbuf, (u16)n);
			/* Could not even form a reply: fall through to the echo rather
			 * than leaving the host waiting on a response that never comes. */
		}
		ret = nmp_guest_send_msg(garg.nmp_guest, code, indx, msg, len);
	}

	return ret;
}

/* dp_app FIX: dedicated MASTER management pump. mvapp only calls ctrl_cb AFTER the full
 * (slow) pp2/local init, but the host, the instant it sets HOST_MGMT_READY, drives the P3a
 * ECHO + P3b CC_PF_* handshake and then 1 Hz NC_PF_KEEP_ALIVE — all serviced by NMP_SCHED_MNG.
 * Without pumping MNG during dp_fwd's init the host times out ("ECHO round-trip FAILED") and
 * tears the link down / dp_fwd aborts. So we start this thread right after nmp_init (mirrors
 * dp_app/main.c). Pinned to an IDLE ISOLATED core: core 0 is shared with the main thread,
 * which busy-waits in nmp_guest_init/init_local_modules right after nmp_init and STARVES the
 * pump there (symptom: "MNG pump thread up" printed only after "Local initializations", too
 * late for the host's P3a ECHO -> ECHO FAILED -> handshake never completes -> dp_fwd aborts).
 * isolcpus 2/3 are unused by the single-core forwarder (runs on core 1). */
#define DP_MNG_CORE 2
static volatile int mng_pump_run = 1;
/* Set once the guest is initialised AND its event handler is registered, so the pump never
 * touches garg.nmp_guest before init_all_modules has finished building it. The pump thread
 * starts right after nmp_init, which is well before guest init — the ordering that makes
 * this flag load-bearing rather than defensive. */
static volatile int dp_guest_ready;
static void *mng_pump_thread(void *arg)
{
	struct glob_arg *g = (struct glob_arg *)arg;
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(DP_MNG_CORE, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
		pr_warn("dp_app: MNG pump could not pin to core %d\n", DP_MNG_CORE);
	pr_info("dp_app: MNG pump thread up (NMP_SCHED_MNG + init-time RX/TX)\n");
	while (mng_pump_run) {
		nmp_schedule(g->nmp, NMP_SCHED_MNG, NULL);
		/* Until the forwarder main_loop takes over RX/TX, ALSO drive the GIE here. The host's
		 * mgmt cmd/notif rings live in HOST memory and are moved across PCIe by the GIE
		 * (NMP_SCHED_RX/TX) — so the P3a ECHO / P3b CC_PF handshake needs RX/TX pumped DURING
		 * init, not just MNG (dp_app does this in its dp0 thread). main_loop_cb sets
		 * dp_main_loop_running when it owns RX/TX, so we stop then (no double-scheduling). */
		if (!dp_main_loop_running) {
			nmp_schedule(g->nmp, NMP_SCHED_RX, NULL);
			nmp_schedule(g->nmp, NMP_SCHED_TX, NULL);
		}
		/* D84: drive the GUEST channel from the same dedicated thread that drives the
		 * master one. This is the custom path — the A0.1 echo and every swop register
		 * op — and it used to be pumped only from ctrl_cb, where it starved. See the
		 * note in ctrl_cb for the bench evidence. */
		if (dp_guest_ready)
			nmp_guest_schedule(g->nmp_guest);
	}
	return NULL;
}

/* MUSDK names these in src/mng/pci_ep_def.h, which is internal and not on the app include
 * path. Mirrored here as literals rather than reaching into vendor internals; if a future
 * MUSDK renames them, dp_cap_find fails loudly and publishes nothing, which is the strict
 * direction — a carrier that wrongly says "not capable" costs telemetry an operator can
 * re-arm, while one that wrongly says "capable" costs a wedge that needs mains.
 *
 * THE CONSTANT IS NOT THE SYSFS NAME, and mirroring it was not enough. MUSDK passes "pci_ep"
 * as the sys_iomem *devname* and the UIO layer publishes it as "uio_<devname>_<index>", so the
 * name in /sys/class/uio/uioN/name is "uio_pci_ep_0" — measured on the appliance 2026-09-03,
 * where the first publishing build logged "no uio 'pci_ep' region 'bar0'" and published nothing.
 * The map name IS literally "bar0", so only the device name needed the convention applied.
 *
 * Both spellings are accepted: the bare constant in case a future MUSDK registers it directly,
 * and the "uio_pci_ep" prefix that this platform actually produces. The discriminator that keeps
 * this strict is not the device name anyway — it is the REQUIRED map named exactly "bar0",
 * without which dp_cap_find refuses. */
#define PCI_EP_UIO_MEM_NAME_STR	"pci_ep"
#define PCI_EP_UIO_MEM_NAME_PFX	"uio_pci_ep"
#define PCI_EP_UIO_BAR0_NAME_STR	"bar0"

/* ---- D84 / #24-f: the swop capability carrier ---------------------------------------------
 *
 * Publishes DP_SWOP_CAP_MAGIC + DP_SWOP_CAP_VERSION into the NW_AGENT window of the host-visible
 * BAR0, so the host driver can decide whether this firmware speaks swop WITHOUT sending it a
 * message to find out. See the contract note in dp_swop.h for why the vendor's barmap version
 * could not carry this and why no probe may.
 *
 * PUBLISHED ON THE REGISTRATION SUCCESS PATH, NOT AT INIT. The predicate the host's gate needs is
 * not "was this image built with swop source" — it is "is a handler registered right now". A word
 * written at startup, or baked into .rodata, publishes even when the handler was compiled out,
 * failed to register, or registered and was torn down. Writing it where registration succeeds is
 * the only placement whose subject is the thing the gate cares about.
 *
 * The window is reached through the UIO device MUSDK maps as "pci_ep" region "bar0", discovered
 * from sysfs because the uio index and map index are assigned at bind time. Mapping it here
 * independently of MUSDK's own mapping keeps this out of nmp internals, which are not app-visible.
 */
static int dp_cap_fd = -1;
static void *dp_cap_base;
static size_t dp_cap_len;

static int dp_cap_read_sysfs(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(out, 1, cap - 1, f);
	fclose(f);
	out[n] = '\0';
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = '\0';
	return 0;
}

/* Locate uioN named "pci_ep" and the index of its map named "bar0". */
static int dp_cap_find(int *uio_no, int *map_no, unsigned long *map_len)
{
	char path[256], val[64];
	int u, m;

	for (u = 0; u < 32; u++) {
		snprintf(path, sizeof(path), "/sys/class/uio/uio%d/name", u);
		if (dp_cap_read_sysfs(path, val, sizeof(val)) != 0)
			continue;
		if (strcmp(val, PCI_EP_UIO_MEM_NAME_STR) != 0 &&
		    strncmp(val, PCI_EP_UIO_MEM_NAME_PFX,
			    sizeof(PCI_EP_UIO_MEM_NAME_PFX) - 1) != 0)
			continue;
		for (m = 0; m < 8; m++) {
			snprintf(path, sizeof(path),
				 "/sys/class/uio/uio%d/maps/map%d/name", u, m);
			if (dp_cap_read_sysfs(path, val, sizeof(val)) != 0)
				continue;
			if (strcmp(val, PCI_EP_UIO_BAR0_NAME_STR) != 0)
				continue;
			snprintf(path, sizeof(path),
				 "/sys/class/uio/uio%d/maps/map%d/size", u, m);
			if (dp_cap_read_sysfs(path, val, sizeof(val)) != 0)
				return -1;
			*map_len = strtoul(val, NULL, 0);
			*uio_no = u;
			*map_no = m;
			return 0;
		}
	}
	return -1;
}

static void dp_cap_retract(void)
{
	volatile uint32_t *slot;

	if (!dp_cap_base)
		return;
	slot = (volatile uint32_t *)((char *)dp_cap_base +
				     DP_SWOP_CAP_WINDOW_OFF + DP_SWOP_CAP_SLOT_OFF);
	/* Magic first: a reader must never see a live magic beside a cleared version. */
	slot[0] = 0;
	__sync_synchronize();
	slot[1] = 0;
	__sync_synchronize();
}

static int dp_cap_publish(void)
{
	unsigned long len = 0;
	volatile uint32_t *slot;
	char dev[64];
	int uio_no, map_no;

	if (dp_cap_find(&uio_no, &map_no, &len) != 0) {
		pr_warn("dp_app: swop capability NOT published — no uio '%s'/'%s*' with region '%s'\n",
			PCI_EP_UIO_MEM_NAME_STR, PCI_EP_UIO_MEM_NAME_PFX,
			PCI_EP_UIO_BAR0_NAME_STR);
		return -1;
	}
	/* Refuse rather than write past the end of a window smaller than the contract assumes. */
	if (len < DP_SWOP_CAP_WINDOW_OFF + DP_SWOP_CAP_SLOT_OFF + 8) {
		pr_warn("dp_app: swop capability NOT published — bar0 map is 0x%lx, too small for the slot\n",
			len);
		return -1;
	}
	snprintf(dev, sizeof(dev), "/dev/uio%d", uio_no);
	dp_cap_fd = open(dev, O_RDWR | O_SYNC);
	if (dp_cap_fd < 0) {
		pr_warn("dp_app: swop capability NOT published — open %s: %s\n", dev, strerror(errno));
		return -1;
	}
	/* UIO maps region N at file offset N * pagesize. */
	dp_cap_base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
			   dp_cap_fd, (off_t)map_no * (off_t)getpagesize());
	if (dp_cap_base == MAP_FAILED) {
		pr_warn("dp_app: swop capability NOT published — mmap %s: %s\n", dev, strerror(errno));
		dp_cap_base = NULL;
		close(dp_cap_fd);
		dp_cap_fd = -1;
		return -1;
	}
	dp_cap_len = len;

	slot = (volatile uint32_t *)((char *)dp_cap_base +
				     DP_SWOP_CAP_WINDOW_OFF + DP_SWOP_CAP_SLOT_OFF);
	/* Version first, magic second — the magic is the commit point, so a host that sees it is
	 * guaranteed the version beside it is already the one that belongs to it. */
	slot[1] = DP_SWOP_CAP_VERSION;
	__sync_synchronize();
	slot[0] = DP_SWOP_CAP_MAGIC;
	__sync_synchronize();
	pr_info("dp_app: swop capability published at bar0+0x%x (magic 0x%08x version %u) via %s map%d\n",
		DP_SWOP_CAP_WINDOW_OFF + DP_SWOP_CAP_SLOT_OFF,
		DP_SWOP_CAP_MAGIC, DP_SWOP_CAP_VERSION, dev, map_no);
	return 0;
}

static int init_all_modules(void)
{
	struct pp2_init_params	 pp2_params;
	struct pp2_glb_common_args *pp2_args = (struct pp2_glb_common_args *)garg.cmn_args.plat;
	int			err;
	char			file[PP2_MAX_BUF_STR_LEN];
	int			num_rss_tables = 0;
	struct			nmp_params nmp_params;
	struct			nmp_guest_params nmp_guest_params;

	pr_info("Global initializations ...\n");

	err = mv_sys_dma_mem_init(PKT_ECHO_APP_DMA_MEM_SIZE);
	if (err)
		return err;

	/* NMP initializations */
	memset(&nmp_params, 0, sizeof(nmp_params));
	err = app_read_nmp_cfg_file(garg.cmn_args.nmp_cfg_location, &nmp_params);
	if (err) {
		pr_err("NMP preinit failed with error %d\n", err);
		return -EIO;
	}
	err = nmp_init(&nmp_params, &(garg.nmp));
	if (err)
		return err;

	/* dp_app FIX: complete the master handshake HERE, in the main thread, BEFORE any guest
	 * init. The host, right after HOST_MGMT_READY (== nmp_init returning), drives P3a ECHO +
	 * P3b CC_PF_* — moved across PCIe by the GIE (RX/TX) and processed by MNG. dp_app succeeds
	 * because it has NO guest init competing; doing nmp_guest_init/init_local_modules
	 * concurrently starves/blocks this and the ECHO times out ("ECHO round-trip FAILED"). So
	 * pump MNG+RX+TX synchronously for a bounded window until the handshake settles. */
	{
		time_t t0 = time(NULL);

		pr_info("dp_app: pumping master handshake (MNG+RX/TX) before guest init ...\n");
		while ((time(NULL) - t0) < 6) {
			nmp_schedule(garg.nmp, NMP_SCHED_MNG, NULL);
			nmp_schedule(garg.nmp, NMP_SCHED_RX, NULL);
			nmp_schedule(garg.nmp, NMP_SCHED_TX, NULL);
		}
		pr_info("dp_app: master handshake pump window done; starting guest init\n");
	}

	/* dp_app FIX: keep pumping MNG (+RX/TX until main_loop owns it) for ongoing keep-alives
	 * during the remaining (slow) guest/pp2 init. */
	{
		pthread_t mng_tid;

		if (pthread_create(&mng_tid, NULL, mng_pump_thread, &garg) == 0)
			pthread_detach(mng_tid);
		else
			pr_err("dp_app: failed to start MNG pump thread\n");
	}

	/* NMP Guest initializations */
	memset(&nmp_guest_params, 0, sizeof(nmp_guest_params));
	nmp_guest_params.id = garg.cmn_args.guest_id;
	nmp_guest_params.timeout = PKT_ECHO_APP_NMP_GUEST_TIMEOUT;
	nmp_guest_params.nmp =  (void *)garg.nmp;
	err = nmp_guest_init(&nmp_guest_params, &garg.nmp_guest);
	if (err)
		return err;

	/* PP2 initializations */
	memset(&pp2_params, 0, sizeof(pp2_params));

	/* Check how many RSS tables are in use by kernel. This parameter is needed for configuring RSS */
	/* Relevant only if cpus is bigger than 1 */
	if (garg.cmn_args.cpus > 1) {
		num_rss_tables = app_rss_num_tbl_get(pp2_args->ports_desc[0].name, file);
		if (num_rss_tables < 0)
			return -EFAULT;
	}

	pp2_params.rss_tbl_reserved_map = (1 << num_rss_tables) - 1;
	pp2_params.res_maps_auto_detect_map = PP2_RSRVD_MAP_HIF_AUTO | PP2_RSRVD_MAP_BM_POOL_AUTO;

	pp2_params.skip_hw_init = 1;

	err = pp2_init(&pp2_params);
	if (err)
		return err;

	/* Must be after pp2_init */
	app_used_hifmap_init(pp2_params.hif_reserved_map);
	app_used_bm_pool_map_init(pp2_params.bm_pool_reserved_map);

	nmp_guest_get_probe_str(garg.nmp_guest, &garg.prb_str);

	/* THE RETURN VALUE WAS BEING DISCARDED. nmp_guest_register_event_handler returns int and
	 * refuses a second registration, so a failure here means no custom message is ever
	 * delivered — the A0.1 echo and every swop op — while every other part of bring-up looks
	 * healthy. That is the same silent shape as the starved pump above, and it was one line
	 * away from it. Checked now, and nothing downstream is armed unless it succeeded. */
	err = nmp_guest_register_event_handler(garg.nmp_guest,
					       NMP_GUEST_LF_T_NICPF,
					       0,
					       (NMP_GUEST_EV_NICPF_MTU | NMP_GUEST_EV_NICPF_MAC_ADDR),
					       &garg,
					       guest_ev_cb);
	if (err) {
		pr_err("dp_app: guest event handler registration FAILED (%d) — no custom message can be delivered\n",
		       err);
		dp_cap_retract();
		return err;
	}

	/* Only now may the pump touch the guest: it is initialised and its handler is armed.
	 * Written last on purpose — the pump thread has been running since just after nmp_init. */
	dp_guest_ready = 1;

	/* D84 #24-f: publish the capability HERE, on the success path of the registration that
	 * installs the handler — see dp_swop.h. Publishing earlier would advertise a handler that
	 * may not exist; publishing on failure would advertise one that certainly does not. A
	 * publish failure is logged and left unpublished: the host then reads no capability and
	 * refuses telemetry, which is the safe direction. */
	(void)dp_cap_publish();

	return 0;
}

static int init_local_modules(struct glob_arg *garg)
{
	int				err;
	struct bpool_inf		std_infs[] = PKT_ECHO_APP_BPOOLS_INF;
	struct bpool_inf		jumbo_infs[] = PKT_ECHO_APP_BPOOLS_JUMBO_INF;
	struct bpool_inf		*infs;
	struct pp2_glb_common_args	*pp2_args = (struct pp2_glb_common_args *) garg->cmn_args.plat;

	pr_info("Local initializations ...\n");

	/**************************/
	/* PP2 Port Init	  */
	/**************************/
	err = app_hif_init(&pp2_args->hif, PKT_ECHO_APP_HIF_Q_SIZE, NULL);
	if (err)
		return err;

	if (garg->cmn_args.mtu > DEFAULT_MTU) {
		infs = jumbo_infs;
		pp2_args->num_pools = ARRAY_SIZE(jumbo_infs);
	} else {
		infs = std_infs;
		pp2_args->num_pools = ARRAY_SIZE(std_infs);
	}

	nmp_guest_get_relations_info(garg->nmp_guest, &garg->guest_info);

	err = app_guest_utils_build_all_giu_bpools(garg->prb_str,
			&garg->guest_info,
			&garg->giu_bpools_desc);
	if (err)
		return err;

	err = app_nmp_guest_giu_port_init(garg->prb_str,
			&garg->guest_info,
			&garg->giu_port_desc);
	if (err)
		return err;

	err = app_guest_utils_allocate_all_giu_bpools(&garg->giu_bpools_desc,
			PKT_ECHO_APP_GIU_BP_SIZE);
	if (err)
		return err;

	err = app_guest_utils_build_all_pp2_bpools(garg->prb_str,
			&garg->guest_info,
			&pp2_args->pools_desc,
			pp2_args,
			infs);
	if (err)
		return err;

	err = app_nmp_guest_pp2_port_init(garg->prb_str, &garg->guest_info, &pp2_args->ports_desc[0]);
	if (err)
		return err;

	/*
	 * dp_app: app_nmp_guest_pp2_port_init() only PROBES the pp2 port; unlike the giu
	 * side (app_nmp_guest_giu_port_init -> giu_gpio_enable) it never enables it. In the
	 * stock guest deployment the master nmp process enables pp2; we are master+guest in
	 * one process, so enable eth0 here or it stays down and nothing forwards.
	 */
	if (pp2_args->ports_desc[0].ppio) {
		err = pp2_ppio_enable(pp2_args->ports_desc[0].ppio);
		if (err)
			pr_err("dp_app: pp2_ppio_enable(eth0) failed: %d\n", err);
		else
			pr_info("dp_app: pp2 eth0 ppio ENABLED\n");
	}

	garg->cmn_args.num_ports = garg->guest_info.ports_info.num_ports;

	return 0;
}

static int register_cli_cmds(struct glob_arg *garg)
{
	struct cli_cmd_params	 cmd_params;

	app_register_cli_common_cmds(&garg->cmn_args);

	memset(&cmd_params, 0, sizeof(cmd_params));
	cmd_params.name		= "prefetch";
	cmd_params.desc		= "Prefetch ahead shift (number of buffers)";
	cmd_params.format	= "<shift>";
	cmd_params.cmd_arg	= garg;
	cmd_params.do_cmd_cb	= (int (*)(void *, int, char *[]))apps_prefetch_cmd_cb;
	mvapp_register_cli_cmd(&cmd_params);

	/* statistics command */
	memset(&cmd_params, 0, sizeof(cmd_params));
	cmd_params.name		= "perf";
	cmd_params.desc		= "Dump performance statistics";
	cmd_params.format	= NULL;
	cmd_params.cmd_arg	= garg;
	cmd_params.do_cmd_cb	= (int (*)(void *, int, char *[]))perf_cmd_cb;
	mvapp_register_cli_cmd(&cmd_params);

#ifdef CHECK_CYCLES
	memset(&cmd_params, 0, sizeof(cmd_params));
	cmd_params.name		= "pme";
	cmd_params.desc		= "Performance Montitor Emulator";
	cmd_params.format	= NULL;
	cmd_params.cmd_arg	= garg;
	cmd_params.do_cmd_cb	= (int (*)(void *, int, char *[]))pme_cmd_cb;
	mvapp_register_cli_cmd(&cmd_params);
#endif /* CHECK_CYCLES */

	return 0;
}

static int unregister_cli_cmds(struct glob_arg *garg)
{
	/* TODO: unregister cli cmds */
	return 0;
}

static int init_global(void *arg)
{
	struct glob_arg *garg = (struct glob_arg *)arg;
	int		 err;

	if (!garg) {
		pr_err("no obj!\n");
		return -EINVAL;
	}

	if (pthread_mutex_init(&garg->trd_lock, NULL) != 0) {
		pr_err("init lock failed!\n");
		return -EIO;
	}

	err = init_all_modules();
	if (err)
		return err;

	err = init_local_modules(garg);
	if (err)
		return err;

	if (garg->cmn_args.cli) {
		err = register_cli_cmds(garg);
		if (err)
			return err;
	}

	gettimeofday(&garg->cmn_args.ctrl_trd_last_time, NULL);

#ifdef CHECK_CYCLES
	pme_ev_cnt_pp2_rx = pme_ev_cnt_create("PP2-PPIO Recv", 1000000, 0);
	if (pme_ev_cnt_pp2_rx < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_pp2_rx;
	}
	pme_ev_cnt_pp2_tx = pme_ev_cnt_create("PP2-PPIO Send", 1000000, 0);
	if (pme_ev_cnt_pp2_tx < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_pp2_tx;
	}
	pme_ev_cnt_pp2_txd = pme_ev_cnt_create("PP2-PPIO Done", 1000000, 0);
	if (pme_ev_cnt_pp2_txd < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_pp2_txd;
	}
	pme_ev_cnt_giu_rx = pme_ev_cnt_create("GIU-GPIO Recv", 1000000, 0);
	if (pme_ev_cnt_giu_rx < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_giu_rx;
	}
	pme_ev_cnt_giu_tx = pme_ev_cnt_create("GIU-GPIO Send", 1000000, 0);
	if (pme_ev_cnt_giu_tx < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_giu_tx;
	}
	pme_ev_cnt_giu_txd = pme_ev_cnt_create("GIU-GPIO Done", 1000000, 0);
	if (pme_ev_cnt_giu_txd < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_giu_tx;
	}
	pme_ev_cnt_gie_ingr = pme_ev_cnt_create("GIE Ingress", 1000000, 0);
	if (pme_ev_cnt_gie_ingr < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_gie_ingr;
	}
	pme_ev_cnt_gie_egr = pme_ev_cnt_create("GIE Egress", 1000000, 0);
	if (pme_ev_cnt_gie_egr < 0) {
		pr_err("PME failed!\n");
		return pme_ev_cnt_gie_egr;
	}
#endif /* CHECK_CYCLES */

	return 0;
}

static void deinit_global(void *arg)
{
	apps_pp2_deinit_global(arg);
}

static int init_local(void *arg, int id, void **_larg)
{
	struct glob_arg		*garg = (struct glob_arg *)arg;
	struct local_arg	*larg;
	struct pp2_glb_common_args *glb_pp2_args = (struct pp2_glb_common_args *) garg->cmn_args.plat;
	struct pp2_lcl_common_args *lcl_pp2_args;
	int			giu_port_id = 0;
	int			giu_id = 0; /* TODO: this should not be hard-coded */
	int			i, err;

	if (!garg) {
		pr_err("no obj!\n");
		return -EINVAL;
	}

	larg = (struct local_arg *)malloc(sizeof(struct local_arg));
	if (!larg) {
		pr_err("No mem for local arg obj!\n");
		return -ENOMEM;
	}
	memset(larg, 0, sizeof(struct local_arg));

	larg->cmn_args.id		= id;
	larg->cmn_args.num_ports	= garg->cmn_args.num_ports;
	larg->cmn_args.burst		= garg->cmn_args.burst;
	larg->cmn_args.busy_wait	= garg->cmn_args.busy_wait;
	larg->cmn_args.echo		= garg->cmn_args.echo;
	larg->cmn_args.prefetch_shift	= garg->cmn_args.prefetch_shift;

	larg->cmn_args.plat = (struct pp2_lcl_common_args *)malloc(sizeof(struct pp2_lcl_common_args));
	if (!larg) {
		pr_err("No mem for local plat arg obj!\n");
		free(larg);
		return -ENOMEM;
	}
	lcl_pp2_args = (struct pp2_lcl_common_args *) larg->cmn_args.plat;
	lcl_pp2_args->lcl_ports_desc = (struct lcl_port_desc *)
					   malloc(larg->cmn_args.num_ports * sizeof(struct lcl_port_desc));
	if (!lcl_pp2_args->lcl_ports_desc) {
		pr_err("no mem for local-port-desc obj!\n");
		free(larg->cmn_args.plat);
		free(larg);
		return -ENOMEM;
	}
	memset(lcl_pp2_args->lcl_ports_desc, 0, larg->cmn_args.num_ports * sizeof(struct lcl_port_desc));

	pthread_mutex_lock(&garg->trd_lock);
	err = app_hif_init(&lcl_pp2_args->hif, PKT_ECHO_APP_HIF_Q_SIZE, NULL);
	pthread_mutex_unlock(&garg->trd_lock);
	if (err)
		return err;

	for (i = 0; i < larg->cmn_args.num_ports; i++)
		app_port_local_init(i, larg->cmn_args.id, &lcl_pp2_args->lcl_ports_desc[i],
				    &glb_pp2_args->ports_desc[i]);

	lcl_pp2_args->pools_desc	= glb_pp2_args->pools_desc;
	lcl_pp2_args->multi_buffer_release = glb_pp2_args->multi_buffer_release;

	larg->cmn_args.garg = garg;
	larg->cmn_args.qs_map = garg->cmn_args.qs_map << (garg->cmn_args.qs_map_shift * id);

	/* Update garg refs to local */
	garg->cmn_args.largs[id] = larg;
	for (i = 0; i < larg->cmn_args.num_ports; i++)
		glb_pp2_args->ports_desc[i].lcl_ports_desc[id] = &lcl_pp2_args->lcl_ports_desc[i];

	pr_debug("thread %d (cpu %d) mapped to Qs %llx\n",
		 larg->cmn_args.id, sched_getcpu(), (unsigned long long)larg->cmn_args.qs_map);

	/* TODO: create and use GIU global port descriptor (similar to PP2 port local init) */
	app_giu_port_local_init(giu_port_id,
		larg->cmn_args.id,
		giu_id,
		&larg->giu_ports_desc[giu_id],
		garg->giu_port_desc.gpio);

	*_larg = larg;
	return 0;
}

static void deinit_local(void *arg)
{
	apps_pp2_deinit_local(arg);
}

static void usage(char *progname)
{
	printf("\n"
	       "MUSDK packet-echo application.\n"
	       "\n"
	       "Usage: %s OPTIONS\n"
	       "  E.g. %s -i eth1 -c 1\n"
	       "\n"
	       "Mandatory OPTIONS:\n"
	       "\t-g, --guestid <id>      Guest ID for retrieving parameters from cfg file.\n"
	       "\n"
	       "Optional OPTIONS:\n"
	       "\t-b <size>                Burst size, num_pkts handled in a batch.(default is %d)\n"
	       "\t-i <interface name>      Network Interface to use.(default is %s)\n"
	       "\t--mtu <mtu>              Set MTU (default is %d)\n"
	       "\t-c, --cores <number>     Number of CPUs to use\n"
	       "\t-a, --affinity <number>  Use setaffinity (default is no affinity)\n"
	       "\t-s                       Maintain statistics\n"
	       "\t- f, --file              Location and name of the nmp-config file to load\n"
	       "\t-w <cycles>              Cycles to busy_wait between recv&send, simulating app behavior (default=0)\n"
	       "\t--rxq <size>             Size of rx_queue (default is %d)\n"
	       "\t--pkt-offset <size>      Packet offset in buffer, must be multiple of 32-byte (default is %d)\n"
	       "\t--no-echo                Don't perform 'pkt_echo', N/A w/o define PKT_ECHO_APP_PKT_ECHO_SUPPORT\n"
	       "\t--cli                    Use CLI\n"
	       "\t--no-stat                Disable the packet's runtime statistics display\n"
	       "\t?, -h, --help            Display help and exit.\n\n"
	       "\n", MVAPPS_NO_PATH(progname), MVAPPS_NO_PATH(progname),
	       PKT_ECHO_APP_MAX_BURST_SIZE, PKT_ECHO_APP_NET_INTERFACE,
	       DEFAULT_MTU, PKT_ECHO_APP_RX_Q_SIZE, MVAPPS_PP2_PKT_DEF_OFFS);
}

static int parse_args(struct glob_arg *garg, int argc, char *argv[])
{
	int	i = 1;
	struct pp2_glb_common_args *pp2_args = (struct pp2_glb_common_args *) garg->cmn_args.plat;

	garg->cmn_args.cli = 0;
	garg->cmn_args.cpus = 1;
	garg->cmn_args.affinity = MVAPPS_INVALID_AFFINITY;
	garg->cmn_args.burst = PKT_ECHO_APP_DFLT_BURST_SIZE;
	garg->cmn_args.mtu = DEFAULT_MTU;
	garg->cmn_args.busy_wait	= 0;
	garg->rxq_size = PKT_ECHO_APP_RX_Q_SIZE;
	garg->cmn_args.echo = 1;
	garg->cmn_args.qs_map = 0;
	garg->cmn_args.qs_map_shift = 0;
	garg->cmn_args.pkt_offset = 0;
	garg->cmn_args.prefetch_shift = PKT_ECHO_APP_PREFETCH_SHIFT;
	garg->cmn_args.ctrl_thresh = PKT_ECHO_APP_STATS_DFLT_THRESH;
	garg->maintain_stats = 0;
	garg->pkt_rate_stats = 1;
	garg->cmn_args.guest_id = PKT_ECHO_APP_NMP_GUEST_ID;

	pp2_args->multi_buffer_release = 1;

	/* TODO: init hardcoded ports?!?!?! */
	garg->cmn_args.num_ports = 1;
	snprintf(pp2_args->ports_desc[0].name,
		sizeof(pp2_args->ports_desc[0].name),
		"%s", PKT_ECHO_APP_NET_INTERFACE);

	while (i < argc) {
		if ((strcmp(argv[i], "?") == 0) ||
		    (strcmp(argv[i], "-h") == 0) ||
		    (strcmp(argv[i], "--help") == 0)) {
			usage(argv[0]);
			exit(0);
		} else if (strcmp(argv[i], "-g") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			garg->cmn_args.guest_id = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-f") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}

			strcpy(garg->cmn_args.nmp_cfg_location, argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-i") == 0) {
			char *token;

			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid interface arguments format!\n");
				return -EINVAL;
			}

			/* count the number of tokens separated by ',' */
			for (token = strtok(argv[i + 1], ","), garg->cmn_args.num_ports = 0;
			     token;
			     token = strtok(NULL, ","), garg->cmn_args.num_ports++)
				snprintf(pp2_args->ports_desc[garg->cmn_args.num_ports].name,
					 sizeof(pp2_args->ports_desc[garg->cmn_args.num_ports].name),
					 "%s", token);

			if (garg->cmn_args.num_ports == 0) {
				pr_err("Invalid interface arguments format!\n");
				return -EINVAL;
			} else if (garg->cmn_args.num_ports > MVAPPS_PP2_MAX_NUM_PORTS) {
				pr_err("too many ports specified (%d vs %d)\n",
				       garg->cmn_args.num_ports, MVAPPS_PP2_MAX_NUM_PORTS);
				return -EINVAL;
			}
			i += 2;
		} else if (strcmp(argv[i], "-b") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			garg->cmn_args.burst = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "--mtu") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			garg->cmn_args.mtu = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-c") == 0) {
			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			garg->cmn_args.cpus = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-a") == 0) {
			garg->cmn_args.affinity = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-s") == 0) {
			garg->maintain_stats = 1;
			i += 1;
		} else if (strcmp(argv[i], "-w") == 0) {
			garg->cmn_args.busy_wait = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "--rxq") == 0) {
			garg->rxq_size = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "--pkt-offset") == 0) {
			garg->cmn_args.pkt_offset = atoi(argv[i + 1]);
			i += 2;
		} else if (strcmp(argv[i], "-m") == 0) {
			int rv;

			if (argc < (i + 2)) {
				pr_err("Invalid number of arguments!\n");
				return -EINVAL;
			}
			if (argv[i + 1][0] == '-') {
				pr_err("Invalid arguments format!\n");
				return -EINVAL;
			}
			rv = sscanf(argv[i + 1], "%x:%x", (unsigned int *)&garg->cmn_args.qs_map,
				    &garg->cmn_args.qs_map_shift);
			if (rv != 2) {
				pr_err("Failed to parse -m parameter\n");
				return -EINVAL;
			}
			i += 2;
		} else if (strcmp(argv[i], "--no-echo") == 0) {
			garg->cmn_args.echo = 0;
			i += 1;
		} else if (strcmp(argv[i], "--cli") == 0) {
			garg->cmn_args.cli = 1;
			i += 1;
		} else if (strcmp(argv[i], "--no-stat") == 0) {
			garg->pkt_rate_stats = 0;
			i += 1;
		} else {
			pr_err("argument (%s) not supported!\n", argv[i]);
			return -EINVAL;
		}
	}

	/* Now, check validity of all inputs */
	if (!garg->cmn_args.num_ports ||
	    !pp2_args->ports_desc[0].name) {
		pr_err("No port defined!\n");
		return -EINVAL;
	}
	if (garg->cmn_args.burst > PKT_ECHO_APP_MAX_BURST_SIZE) {
		pr_err("illegal burst size requested (%d vs %d)!\n",
		       garg->cmn_args.burst, PKT_ECHO_APP_MAX_BURST_SIZE);
		return -EINVAL;
	}
	if (garg->cmn_args.cpus > PKT_ECHO_APP_MAX_NUM_CORES) {
		pr_err("illegal num cores requested (%d vs %d)!\n",
		       garg->cmn_args.cpus, PKT_ECHO_APP_MAX_NUM_CORES);
		return -EINVAL;
	}
	if ((garg->cmn_args.affinity != -1) &&
	    ((garg->cmn_args.cpus + garg->cmn_args.affinity) > MVAPPS_MAX_NUM_CORES)) {
		pr_err("illegal num cores or affinity requested (%d,%d vs %d)!\n",
		       garg->cmn_args.cpus, garg->cmn_args.affinity, MVAPPS_MAX_NUM_CORES);
		return -EINVAL;
	}

	if (garg->cmn_args.qs_map &&
	    (mvapp_pp2_max_num_qs_per_tc == 1) &&
	    (PKT_ECHO_APP_MAX_NUM_TCS_PER_PORT == 1)) {
		pr_warn("no point in queues-mapping; ignoring.\n");
		garg->cmn_args.qs_map = 1;
		garg->cmn_args.qs_map_shift = 1;
	} else if (!garg->cmn_args.qs_map) {
		garg->cmn_args.qs_map = 1;
		garg->cmn_args.qs_map_shift = PKT_ECHO_APP_MAX_NUM_TCS_PER_PORT;
	}

	if ((garg->cmn_args.cpus != 1) &&
	    (garg->cmn_args.qs_map & (garg->cmn_args.qs_map << garg->cmn_args.qs_map_shift))) {
		pr_err("Invalid queues-mapping (ovelapping CPUs)!\n");
		return -EINVAL;
	}

	return 0;
}

static void init_app_params(struct mvapp_params *mvapp_params, u64 cores_mask)
{
	memset(mvapp_params, 0, sizeof(struct mvapp_params));
	mvapp_params->use_cli		= garg.cmn_args.cli;
	mvapp_params->num_cores		= garg.cmn_args.cpus;
	mvapp_params->cores_mask	= cores_mask;
	mvapp_params->global_arg	= (void *)&garg;
	mvapp_params->init_global_cb	= init_global;
	mvapp_params->deinit_global_cb	= deinit_global;
	mvapp_params->init_local_cb	= init_local;
	mvapp_params->deinit_local_cb	= deinit_local;
	mvapp_params->main_loop_cb	= main_loop_cb;
	mvapp_params->ctrl_cb		= ctrl_cb;
	mvapp_params->ctrl_cb_threshold	= PKT_ECHO_APP_CTRL_TRD_THRESH;
}

static void reset_statistics(void)
{
	int i;

	for (i = 0; i < MVAPPS_MAX_NUM_CORES; i++) {
		int j;

		for (j = 0; j < garg.cmn_args.num_ports; j++) {
			rx_buf_cnt[i][j] = 0;
			tx_buf_cnt[i][j] = 0;
			free_buf_cnt[i][j] = 0;
			tx_buf_retry[i][j] = 0;
			tx_buf_drop[i][j] = 0;
			tx_max_burst[i][j] = 0;
			tx_max_resend[i][j] = 0;
		}
	}
}


int main(int argc, char *argv[])
{
	struct mvapp_params		 mvapp_params;
	struct pp2_glb_common_args	*pp2_args;
	u64				 cores_mask;
	int				 err;

	setbuf(stdout, NULL);
	app_set_max_num_qs_per_tc();

	pr_info("GIU pkt-echo is started\n");
	pr_debug("pr_debug is enabled\n");

	garg.cmn_args.plat = (struct pp2_glb_common_args *)malloc(sizeof(struct pp2_glb_common_args));
	if (!garg.cmn_args.plat) {
		pr_err("No mem for global plat arg obj!\n");
		return -ENOMEM;
	}
	pp2_args = (struct pp2_glb_common_args *) garg.cmn_args.plat;

	err = parse_args(&garg, argc, argv);
	if (err)
		return err;

	pp2_args->pp2_num_inst = pp2_get_num_inst();

	cores_mask = apps_cores_mask_create(garg.cmn_args.cpus, garg.cmn_args.affinity);

	init_app_params(&mvapp_params, cores_mask);

	reset_statistics();

	return mvapp_go(&mvapp_params);
}

