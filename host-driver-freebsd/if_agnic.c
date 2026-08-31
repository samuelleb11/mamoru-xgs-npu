/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic - native FreeBSD driver for the Marvell AGNIC (Armada GIU-NIC)
 * PCIe-endpoint NIC, as exposed by the Marvell CN9130 "NPU" on Sophos XGS
 * appliances (PCI 11ab:7080 PF / 7081 VF).
 *
 * Clean-room reimplementation for OPNsense/FreeBSD. ABI constants are interface
 * FACTS transcribed from the GPL-2.0 Marvell reference; no GPL .c logic is copied.
 *
 * PHASE 1: newbus resource acquisition, no NPU handshake yet.
 *   - map BAR0 (1M) / BAR2 (16M) / BAR4 (16M) as SYS_RES_MEMORY
 *   - create a 36-bit busdma parent tag (EP iATU = 16 x 4GB windows -> 64GB)
 *   - allocate MSI-X vectors (table lives in BAR0 @0x1000 per the firmware ABI)
 *   - NO FLR, NO reset path: detach only releases resources, leaving the NPU
 *     firmware running (an FLR would nuke it).
 *
 * PHASE 2a: locate + validate the NPU barmap descriptor in BAR2 (cookie/version)
 *   and dump the facility map.
 *
 * PHASE 2b: DEFERRED CTRL-facility handshake (config_intrhook, so a not-ready
 *   NPU can never hang boot). In the deferred hook, all busy-waits from the GPL
 *   reference become BOUNDED polls that fail cleanly on timeout:
 *     1. wait barmap cookie (bounded), resolve facility windows from the barmap
 *     2. validate ctrl_map cookie 0xAFACAFAC (bounded)
 *     3. wait CTRL_FCLT_TRGT_INIT (bounded; this is a HARD spin in the GPL src)
 *     4. arm t2h MSI-X doorbell interrupt handlers (stub counters)
 *     5. publish CTRL_FCLT_HOST_INIT (read-modify-write OR into handshake)
 *     6. start a 1 Hz CTRL_FCLT_HOST_ALIVE heartbeat callout
 *     7. wait GIU config_mem DEV_READY (bounded) and print the firmware MAC
 *   detach tears down callout + interrupts + intrhook, then releases P1
 *   resources -- still never an FLR.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/callout.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include "if_agnic.h"

#define	AGNIC_VENDOR_MARVELL	0x11ab
#define	AGNIC_DEV_PF		0x7080
#define	AGNIC_DEV_VF		0x7081

/* AGNIC_DMA_LOWADDR (36-bit ceiling) is defined in if_agnic.h (shared w/ P3a). */

/* P2b bounded-poll budgets (milliseconds). */
#define	AGNIC_CTRL_COOKIE_TIMEOUT_MS	2000	/* ctrl_map cookie appears  */
#define	AGNIC_BARMAP_COOKIE_TIMEOUT_MS	2000	/* barmap cookie (re-check) */
#define	AGNIC_TRGT_INIT_TIMEOUT_MS	2000	/* CTRL_FCLT_TRGT_INIT      */
#define	AGNIC_DEV_READY_TIMEOUT_MS	10000	/* GIU DEV_READY (~10s)     */

static const struct agnic_pci_id {
	uint16_t	vendor;
	uint16_t	device;
	const char     *desc;
} agnic_ids[] = {
	{ AGNIC_VENDOR_MARVELL, AGNIC_DEV_PF, "Marvell AGNIC GIU-NIC (PF)" },
	{ 0, 0, NULL }
};

/* BAR0, BAR2, BAR4 (BAR0/BAR2 are 64-bit; use the low register's rid). */
static const int  agnic_bar_rid[AGNIC_NBARS]  = { PCIR_BAR(0), PCIR_BAR(2), PCIR_BAR(4) };
static const char *agnic_bar_name[AGNIC_NBARS] = { "BAR0", "BAR2", "BAR4" };

static void	agnic_config_hook(void *arg);
static void	agnic_heartbeat(void *arg);
static void	agnic_dbell_intr(void *arg);
static void	agnic_p2b_teardown(struct agnic_softc *sc);

/* ------------------------------------------------------------------------- */
/* Phase 1 resource teardown (unchanged from P1).                            */
/* ------------------------------------------------------------------------- */

static void
agnic_free_resources(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	int i;

	if (sc->msix_count > 0) {
		pci_release_msi(dev);
		sc->msix_count = 0;
	}
	for (i = 0; i < AGNIC_NBARS; i++) {
		if (sc->bar[i] != NULL) {
			bus_release_resource(dev, SYS_RES_MEMORY,
			    sc->bar_rid[i], sc->bar[i]);
			sc->bar[i] = NULL;
		}
	}
	if (sc->parent_dmat != NULL) {
		bus_dma_tag_destroy(sc->parent_dmat);
		sc->parent_dmat = NULL;
	}
}

/* ------------------------------------------------------------------------- */
/* Phase 2b helpers.                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Bounded poll of a 32-bit MMIO word until (reg & mask) == want, or the
 * timeout elapses. Sleeps ~10 ms between reads (safe: the deferred config
 * hook runs in a sleepable context). Returns 0 on match, ETIMEDOUT on timeout.
 * Non-static: shared with agnic_mgmt.c (prototype in if_agnic.h).
 */
int
agnic_poll(struct agnic_softc *sc, int bar, bus_size_t off,
    uint32_t mask, uint32_t want, int timeout_ms, const char *what)
{
	int step = (hz >= 100) ? (hz / 100) : 1;	/* ~10 ms in ticks */
	int budget = (int)(((int64_t)timeout_ms * hz) / 1000);
	int spent = 0;
	uint32_t r;

	for (;;) {
		r = AGNIC_RD4(sc, bar, off);
		if ((r & mask) == want)
			return (0);
		if (spent >= budget) {
			device_printf(sc->dev,
			    "P2b: timeout (%d ms) waiting for %s; reg=0x%08x\n",
			    timeout_ms, what, r);
			return (ETIMEDOUT);
		}
		pause("agnicp", step);
		spent += step;
	}
}

/*
 * Read the validated barmap and resolve each facility's window to an offset
 * within OUR BAR mapping. Pure math over MMIO reads -- no side effects on the
 * device. facility_map[] is ordered GIU,CTRL,MGMT,NWA,RPC, so match by .type.
 */
static void
agnic_resolve_facilities(struct agnic_softc *sc, uint32_t barmap_off,
    uint32_t tail)
{
	int i;

	for (i = 0; i < AGNIC_FAC_COUNT; i++) {
		sc->fac_bar[i] = -1;
		sc->fac_off[i] = 0;
	}

	for (i = 0; i < AGNIC_FAC_COUNT; i++) {
		uint32_t e = barmap_off + AGNIC_BARMAP_FACMAP_OFF +
		    i * AGNIC_FACMAP_ENTRY_SIZE;
		uint32_t bar  = AGNIC_RD4(sc, AGNIC_BAR2, e + 0);
		uint32_t type = AGNIC_RD4(sc, AGNIC_BAR2, e + 4);
		uint32_t off  = AGNIC_RD4(sc, AGNIC_BAR2, e + 8);
		int busbar;
		uint32_t base;

		if (type >= (uint32_t)AGNIC_FAC_COUNT)
			continue;
		if (bar == AGNIC_SHM_BAR0) {
			busbar = AGNIC_BAR0;
			base = 0;		/* BAR0 facility base = phys 0 */
		} else if (bar == AGNIC_SHM_BAR2) {
			busbar = AGNIC_BAR2;
			base = tail;		/* BAR2 tail facility base     */
		} else {
			continue;
		}
		sc->fac_bar[type] = busbar;
		sc->fac_off[type] = base + off;
	}

	sc->ctrl_bar = sc->fac_bar[AGNIC_FAC_CONTROL];
	sc->ctrl_off = sc->fac_off[AGNIC_FAC_CONTROL];
	sc->giu_bar  = sc->fac_bar[AGNIC_FAC_GIU];
	sc->giu_off  = sc->fac_off[AGNIC_FAC_GIU];
}

/*
 * OR a bit into the shared 32-bit handshake word (read-modify-write). The GPL
 * source only ever OR-sets its own bits so target-set bits are preserved; a
 * write barrier ensures the poke lands before we proceed / re-arm.
 */
static void
agnic_handshake_set(struct agnic_softc *sc, uint32_t bit)
{
	uint32_t h;

	h = AGNIC_RD4(sc, sc->ctrl_bar, sc->ctrl_off + AGNIC_CTRL_HANDSHAKE_OFF);
	AGNIC_WR4(sc, sc->ctrl_bar, sc->ctrl_off + AGNIC_CTRL_HANDSHAKE_OFF,
	    h | bit);
	bus_barrier(sc->bar[sc->ctrl_bar], 0, sc->bar_size[sc->ctrl_bar],
	    BUS_SPACE_BARRIER_WRITE);
}

/*
 * t2h doorbell handler. Always counts. For the GIU RX vector (index
 * AGNIC_RX_DBELL_ID) it also kicks the P3b RX taskqueue so a real interrupt
 * drains the ring immediately; the 1 Hz poll callout is the fallback for when
 * the (unverified) RX MSI-X id never fires. All other vectors stay pure
 * counters. Runs in an ithread, so taking rx_mtx inside the kick is legal.
 */
static void
agnic_dbell_intr(void *arg)
{
	struct agnic_dbell_vec *v = arg;

	v->count++;
	if (v->idx == AGNIC_RX_DBELL_ID && v->sc != NULL)
		agnic_txrx_dbell_kick(v->sc);
}

/*
 * Arm the target->host MSI-X doorbell handlers. The GPL facilities[] walk
 * order is MGMT_NETDEV(1 dummy) then GIU(4), giving msix_id 0..4; msix_id ==
 * MSI-X vector index == IRQ rid - 1. We keep the MGMT dummy at vector 0 so the
 * four real GIU data-queue vectors stay non-zero (1..4) as the target expects.
 */
static void
agnic_setup_dbells(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	int k, nvec;

	nvec = AGNIC_N_T2H_DBELLS;
	if (nvec > sc->msix_count)
		nvec = sc->msix_count;

	sc->dbell_nvec = 0;
	for (k = 0; k < nvec; k++) {
		struct agnic_dbell_vec *v = &sc->dbell[k];
		int rid = k + 1;		/* MSI-X vector k -> IRQ rid k+1 */

		v->count = 0;
		v->idx = k;
		v->sc = sc;
		v->rid = rid;
		v->res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
		    RF_ACTIVE);
		if (v->res == NULL) {
			device_printf(dev,
			    "P2b: IRQ alloc failed for t2h dbell %d\n", k);
			break;
		}
		if (bus_setup_intr(dev, v->res, INTR_TYPE_NET | INTR_MPSAFE,
		    NULL, agnic_dbell_intr, v, &v->tag) != 0) {
			device_printf(dev,
			    "P2b: bus_setup_intr failed for t2h dbell %d\n", k);
			bus_release_resource(dev, SYS_RES_IRQ, v->rid, v->res);
			v->res = NULL;
			break;
		}
		sc->dbell_nvec = k + 1;
	}
	device_printf(dev,
	    "P2b: %d t2h MSI-X doorbell handler(s) armed "
	    "(id0=MGMT dummy, id1-4=GIU)\n", sc->dbell_nvec);
}

/*
 * Read the firmware MAC directly from GIU config_mem (valid once DEV_READY) and
 * latch it into the softc for P3b's ether_ifattach.
 */
static void
agnic_print_mac(struct agnic_softc *sc)
{
	int i;

	for (i = 0; i < 6; i++)
		sc->mac[i] = AGNIC_RD1(sc, sc->giu_bar,
		    sc->giu_off + AGNIC_GIU_MAC_OFF + i);

	device_printf(sc->dev,
	    "P2b: GIU DEV_READY; firmware MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
	    sc->mac[0], sc->mac[1], sc->mac[2], sc->mac[3], sc->mac[4],
	    sc->mac[5]);
}

/* 1 Hz heartbeat callout (runs with hb_mtx held via callout_init_mtx). */
static void
agnic_heartbeat(void *arg)
{
	struct agnic_softc *sc = arg;

	agnic_handshake_set(sc, CTRL_FCLT_HOST_ALIVE);
	callout_reset(&sc->heartbeat, hz, agnic_heartbeat, sc);
}

/*
 * Deferred handshake, run once after interrupts are enabled. Every wait here
 * is bounded; on any timeout we log and bail cleanly, leaving P1 resources up
 * and never touching an FLR. If the NPU is not ready at all, the very first
 * bounded poll fails and boot proceeds.
 */
static void
agnic_config_hook(void *arg)
{
	struct agnic_softc *sc = arg;
	device_t dev = sc->dev;
	uint32_t barmap_off = AGNIC_BARMAP_OFF(sc->bar_size[AGNIC_BAR2]);
	uint32_t tail = AGNIC_BAR2_TAIL_OFF(sc->bar_size[AGNIC_BAR2]);

	/*
	 * Re-entry guard: this is a one-shot hook. If it is ever invoked again
	 * (e.g. run_interrupt_driven_config_hooks re-walking the list before the
	 * establish call returns), disestablish and return rather than re-running
	 * the handshake / re-allocating the MSI-X IRQs (which would fail "busy").
	 */
	if (sc->config_hook_done) {
		if (sc->config_hook_established) {
			config_intrhook_disestablish(&sc->config_hook);
			sc->config_hook_established = 0;
		}
		return;
	}

	/* 1. Barmap cookie ready (re-checked bounded; P2a may have raced). */
	if (agnic_poll(sc, AGNIC_BAR2, barmap_off + AGNIC_BARMAP_COOKIE_OFF,
	    0xFFFFFFFFU, AGNIC_BARMAP_COOKIE, AGNIC_BARMAP_COOKIE_TIMEOUT_MS,
	    "barmap cookie") != 0)
		goto done;

	/* 2. Resolve facility windows from the validated barmap. */
	agnic_resolve_facilities(sc, barmap_off, tail);
	if (sc->ctrl_bar < 0 || sc->giu_bar < 0) {
		device_printf(dev,
		    "P2b: CTRL(bar %d) or GIU(bar %d) facility missing; abort\n",
		    sc->ctrl_bar, sc->giu_bar);
		goto done;
	}
	device_printf(dev,
	    "P2b: CTRL @BAR%d+0x%x, GIU @BAR%d+0x%x\n",
	    sc->ctrl_bar, sc->ctrl_off, sc->giu_bar, sc->giu_off);

	/*
	 * 2b. Bring up the mvmgmt0 management link NOW, decoupled from the GIU
	 *     DEV_READY gate (step 8). MGMT_NETDEV is a self-contained facility
	 *     (its own BAR window + a poll-based 0xAA55AA55/0xBB66BB66 mailbox,
	 *     NO GIU queues/doorbells), published by the NPU kernel modules alone
	 *     with no userspace NMP. agnic_pcinet_bringup() self-guards (returns
	 *     early if the facility is absent or the ready pattern is unseen), so
	 *     the host<->NPU control link comes up even when the front-panel GIU
	 *     data plane -- which DOES need DEV_READY + the userspace NMP master --
	 *     is not ready. Idempotent: the later agnic_datapath_start() path sees
	 *     sc->pcinet != NULL and returns early. Teardown stays in agnic_detach.
	 */
	(void)agnic_pcinet_bringup(sc);

	/* 3. Validate ctrl_map cookie. */
	if (agnic_poll(sc, sc->ctrl_bar, sc->ctrl_off + AGNIC_CTRL_COOKIE_OFF,
	    0xFFFFFFFFU, AGNIC_FACILITY_COOKIE, AGNIC_CTRL_COOKIE_TIMEOUT_MS,
	    "ctrl_map cookie") != 0)
		goto done;
	device_printf(dev, "P2b: ctrl_map cookie 0x%08x OK\n",
	    AGNIC_FACILITY_COOKIE);

	/* 4. Wait for target init (a HARD busy-spin in the GPL src; bounded). */
	if (agnic_poll(sc, sc->ctrl_bar,
	    sc->ctrl_off + AGNIC_CTRL_HANDSHAKE_OFF,
	    CTRL_FCLT_TRGT_INIT, CTRL_FCLT_TRGT_INIT,
	    AGNIC_TRGT_INIT_TIMEOUT_MS, "CTRL_FCLT_TRGT_INIT") != 0)
		goto done;
	device_printf(dev, "P2b: target reports TRGT_INIT\n");

	/*
	 * 5. t2h + h2t doorbell init. Set up host-side MSI-X doorbell handlers
	 *    now; the h2t bookkeeping (h2t_dbell_msg[] addresses) is filled
	 *    asynchronously by the target and picked up by the P3 worker, so
	 *    there is nothing to write into ctrl_map here.
	 */
	agnic_setup_dbells(sc);

	/* 6. Publish HOST_INIT (AFTER dbell init, matching the GPL ordering). */
	agnic_handshake_set(sc, CTRL_FCLT_HOST_INIT);
	device_printf(dev, "P2b: HOST_INIT published\n");

	/* 7. Start the 1 Hz HOST_ALIVE heartbeat. */
	mtx_lock(&sc->hb_mtx);
	sc->heartbeat_running = 1;
	callout_reset(&sc->heartbeat, hz, agnic_heartbeat, sc);
	mtx_unlock(&sc->hb_mtx);
	device_printf(dev, "P2b: HOST_ALIVE heartbeat started (1 Hz)\n");

	/* 8. Wait GIU DEV_READY, then read + print the firmware MAC. */
	if (agnic_poll(sc, sc->giu_bar, sc->giu_off + AGNIC_GIU_STATUS_OFF,
	    AGNIC_CFG_STATUS_DEV_READY, AGNIC_CFG_STATUS_DEV_READY,
	    AGNIC_DEV_READY_TIMEOUT_MS, "GIU DEV_READY") != 0)
		goto done;
	agnic_print_mac(sc);

	device_printf(dev, "[Phase 2b] handshake complete; starting P3a mgmt rings\n");

	/*
	 * 9. Phase 3a: bring up the mgmt CMD/NOTIF rings and round-trip one safe
	 *    command. Bounded; on any failure it logs and returns non-zero,
	 *    leaving P2b (heartbeat/DEV_READY) up. Ring DMA it allocated is freed
	 *    in detach via agnic_mgmt_teardown -- never here, since a published
	 *    queue may still be polled by the device. NEVER an FLR.
	 *
	 * 10. Phase 3b: on mgmt success, bring up ONE ingress TC + RX data queue
	 *    with a buffer pool, run INIT/TC_ADD/DATA_Q_ADD/INIT_DONE, and create
	 *    the ether if_t. ENABLE + RX servicing start when the interface is
	 *    ifconfig'd up. Also bounded and FLR-free; freed at detach.
	 */
	if (agnic_mgmt_bringup(sc) == 0 && agnic_txrx_bringup(sc) == 0) {
		/*
		 * 11. Auto-start the datapath NOW (CC_PF_ENABLE + NW_AGENT front-
		 *    panel ports + mvmgmt0 + pport demux), rather than waiting for
		 *    `ifconfig <trunk> up`. The GIU trunk has no OS-visible ifnet;
		 *    the front-panel port ifnets (port1..port9) must exist before
		 *    OPNsense's boot-time interface assignment runs, or it assigns
		 *    the (now-hidden) trunk as LAN and can never obtain DHCP. This
		 *    runs inside the config_intrhook, so the boot waits for it and
		 *    port1..port9 are present by the time rc/interface-assign runs.
		 */
		agnic_datapath_start(sc);
	}

done:
	/*
	 * Disestablish ourselves. Set done BEFORE the disestablish so a
	 * concurrent detach observes it and does not double-free the hook.
	 */
	sc->config_hook_done = 1;
	if (sc->config_hook_established) {
		config_intrhook_disestablish(&sc->config_hook);
		sc->config_hook_established = 0;
	}
}

/* Tear down every P2b object; safe to call whether or not the hook ran. */
static void
agnic_p2b_teardown(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	int k;

	if (!sc->p2b_inited)
		return;

	/* If the deferred hook never ran, cancel it. */
	if (sc->config_hook_established && !sc->config_hook_done) {
		config_intrhook_disestablish(&sc->config_hook);
		sc->config_hook_established = 0;
	}

	/* Stop the heartbeat (drains a possibly in-flight callback). */
	callout_drain(&sc->heartbeat);

	/* Tear down doorbell interrupt handlers + IRQ resources. */
	for (k = 0; k < AGNIC_N_T2H_DBELLS; k++) {
		struct agnic_dbell_vec *v = &sc->dbell[k];

		if (v->tag != NULL) {
			bus_teardown_intr(dev, v->res, v->tag);
			v->tag = NULL;
		}
		if (v->res != NULL) {
			bus_release_resource(dev, SYS_RES_IRQ, v->rid, v->res);
			v->res = NULL;
		}
	}
	sc->dbell_nvec = 0;

	mtx_destroy(&sc->hb_mtx);
	sc->p2b_inited = 0;
}

/* ------------------------------------------------------------------------- */
/* newbus methods.                                                           */
/* ------------------------------------------------------------------------- */

static int
agnic_probe(device_t dev)
{
	uint16_t vendor = pci_get_vendor(dev);
	uint16_t device = pci_get_device(dev);
	const struct agnic_pci_id *id;

	for (id = agnic_ids; id->desc != NULL; id++) {
		if (vendor == id->vendor && device == id->device) {
			device_set_desc(dev, id->desc);
			return (BUS_PROBE_DEFAULT);
		}
	}
	return (ENXIO);
}

static int
agnic_attach(device_t dev)
{
	struct agnic_softc *sc = device_get_softc(dev);
	int i, error, nvec, want;

	sc->dev = dev;
	pci_enable_busmaster(dev);

	/* Map BAR0/2/4. */
	for (i = 0; i < AGNIC_NBARS; i++) {
		sc->bar_rid[i] = agnic_bar_rid[i];
		sc->bar[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
		    &sc->bar_rid[i], RF_ACTIVE);
		if (sc->bar[i] == NULL) {
			device_printf(dev, "could not map %s (rid 0x%02x)\n",
			    agnic_bar_name[i], agnic_bar_rid[i]);
			error = ENXIO;
			goto fail;
		}
		sc->bar_bt[i]   = rman_get_bustag(sc->bar[i]);
		sc->bar_bh[i]   = rman_get_bushandle(sc->bar[i]);
		sc->bar_size[i] = rman_get_size(sc->bar[i]);
		device_printf(dev, "%s: pa 0x%jx  size %ju KiB  (rid 0x%02x)\n",
		    agnic_bar_name[i],
		    (uintmax_t)rman_get_start(sc->bar[i]),
		    (uintmax_t)(sc->bar_size[i] / 1024),
		    agnic_bar_rid[i]);
	}

	/* 36-bit busdma parent tag; children (rings/buffers) inherit the ceiling. */
	error = bus_dma_tag_create(
	    bus_get_dma_tag(dev),	/* parent */
	    1, 0,			/* alignment, boundary */
	    AGNIC_DMA_LOWADDR,		/* lowaddr = 36-bit */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    BUS_SPACE_MAXSIZE,		/* maxsize */
	    BUS_SPACE_UNRESTRICTED,	/* nsegments */
	    BUS_SPACE_MAXSIZE,		/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->parent_dmat);
	if (error != 0) {
		device_printf(dev, "36-bit parent DMA tag failed: %d\n", error);
		goto fail;
	}
	device_printf(dev, "parent DMA tag OK (lowaddr 0x%jx, 36-bit)\n",
	    (uintmax_t)AGNIC_DMA_LOWADDR);

	/*
	 * MSI-X. The table lives inside BAR0 at 0x1000 (firmware ABI); FreeBSD
	 * maps it from the BAR we already hold. Non-fatal so we can see exactly
	 * what the hardware/kernel do together.
	 */
	nvec = pci_msix_count(dev);
	device_printf(dev, "MSI-X: %d vector(s) advertised\n", nvec);
	if (nvec > 0) {
		want = nvec;	/* P2b uses GIU(4)+MGMT(1)=5 doorbell vectors */
		error = pci_alloc_msix(dev, &want);
		if (error != 0) {
			device_printf(dev,
			    "pci_alloc_msix failed: %d (continuing)\n", error);
		} else {
			sc->msix_count = want;
			device_printf(dev, "MSI-X: allocated %d vector(s)\n", want);
		}
	}

	/* --- Phase 2a: locate the NPU's published barmap descriptor in BAR2 --- */
	{
		uint32_t off = AGNIC_BARMAP_OFF(sc->bar_size[AGNIC_BAR2]);
		uint32_t ver = AGNIC_RD4(sc, AGNIC_BAR2, off + AGNIC_BARMAP_VERSION_OFF);
		uint32_t ck  = AGNIC_RD4(sc, AGNIC_BAR2, off + AGNIC_BARMAP_COOKIE_OFF);

		device_printf(dev,
		    "barmap @BAR2+0x%x: version 0x%08x cookie 0x%08x %s\n",
		    off, ver, ck,
		    ck == AGNIC_BARMAP_COOKIE ?
		      (ver == AGNIC_BARMAP_VERSION ? "[COOKIE+VER OK]" :
		       "[COOKIE OK, VERSION MISMATCH]") :
		      "[cookie mismatch: NPU not ready / bad offset]");
		if (ck == AGNIC_BARMAP_COOKIE) {
			for (i = 0; i < AGNIC_FAC_COUNT; i++) {
				uint32_t b = off + AGNIC_BARMAP_FACMAP_OFF +
				    i * AGNIC_FACMAP_ENTRY_SIZE;
				device_printf(dev,
				    "  facility[%d]: bar%u type%u offset 0x%x size 0x%x\n",
				    i,
				    AGNIC_RD4(sc, AGNIC_BAR2, b + 0),
				    AGNIC_RD4(sc, AGNIC_BAR2, b + 4),
				    AGNIC_RD4(sc, AGNIC_BAR2, b + 8),
				    AGNIC_RD4(sc, AGNIC_BAR2, b + 12));
			}
		}
	}

	/*
	 * --- Phase 2b: DEFER the CTRL-facility handshake ---
	 * Construct the heartbeat mutex/callout up front so teardown is always
	 * well-defined, then hand the handshake to a config_intrhook that runs
	 * after interrupts are live. This keeps every NPU busy-wait off the boot
	 * path: a wedged/absent NPU can never hang attach.
	 */
	mtx_init(&sc->hb_mtx, "agnic_hb", NULL, MTX_DEF);
	callout_init_mtx(&sc->heartbeat, &sc->hb_mtx, 0);
	sc->p2b_inited = 1;

	/*
	 * The NW_AGENT mailbox is single-outstanding (SFOS nwa_mailbox.cmd_mtx).
	 * agnic_nwa_cmd sleeps (agnic_poll/pause), so a sleepable sx serializes
	 * concurrent callers (discovery bring-up vs. per-port ifnet ioctls).
	 */
	sx_init(&sc->nwa_sx, "agnic_nwa");
	sc->nwa_sx_inited = 1;

	sc->config_hook.ich_func = agnic_config_hook;
	sc->config_hook.ich_arg = sc;
	/*
	 * Set established BEFORE the establish call: post-boot (cold==0) FreeBSD
	 * may run the hook INLINE from within config_intrhook_establish(), before
	 * this function returns. If the flag were still 0 during that inline run,
	 * config_hook's done: path would skip config_intrhook_disestablish(), the
	 * hook would stay on the list, and run_interrupt_driven_config_hooks()
	 * would re-invoke it forever (observed: 10s DEV_READY-timeout loop, leaked
	 * MSI-X IRQs "resource busy", wedged kldload). Setting it first makes the
	 * self-disestablish fire on the very first (possibly inline) run.
	 */
	sc->config_hook_established = 1;
	if (config_intrhook_establish(&sc->config_hook) != 0) {
		device_printf(dev,
		    "config_intrhook_establish failed; P2b handshake skipped\n");
		sc->config_hook_established = 0;
	}

	device_printf(dev, "[Phase 1+2a] resources acquired; barmap probed; "
	    "P2b handshake deferred; no FLR\n");
	return (0);

fail:
	agnic_free_resources(sc);
	pci_disable_busmaster(dev);
	return (error);
}

static int
agnic_detach(device_t dev)
{
	struct agnic_softc *sc = device_get_softc(dev);

	/*
	 * Order matters. agnic_p2b_teardown() bus_teardown_intr's the MSI-X
	 * doorbells FIRST -- once it returns no RX kick can ever fire again.
	 *
	 * Then agnic_stop() quiesces RX (if_running=0, drains the callout + RX
	 * task) so agnic_rx_service can no longer call agnic_pport_rx. Only THEN is
	 * it safe to agnic_pport_teardown() -- which ether_ifdetach's port1..port9,
	 * draining any in-flight agnic_pport_transmit and preventing new ones. With
	 * no pport ifnet left to reach agnic_giu_tx, agnic_txrx_teardown() can free
	 * tx_bufs/tx_ring and destroy tx_mtx without a lock-of-destroyed-mutex or
	 * use-after-free race (found by the P4c TX review). agnic_stop is idempotent
	 * (txrx_teardown calls it again). mgmt rings stay live until last so the
	 * CC_PF_DISABLE inside agnic_stop/txrx_teardown reaches the device. No FLR.
	 */
	agnic_p2b_teardown(sc);		/* remove MSI-X dbell ISRs: no more kicks */
	agnic_pcinet_teardown(sc);	/* P5: stop mvmgmt0 + free its rings */
	agnic_stop(sc);			/* quiesce RX: no more agnic_pport_rx callers */
	agnic_pport_teardown(sc);	/* P4b: detach port1..9: no more TX callers */
	agnic_txrx_teardown(sc);	/* DISABLE + ifdetach trunk + free TX/RX rings */
	agnic_mgmt_teardown(sc);	/* free P3a mgmt rings + lock */
	if (sc->nwa_sx_inited) {	/* no pport ifnets left to issue nwa cmds */
		sc->nwa_sx_inited = 0;
		sx_destroy(&sc->nwa_sx);
	}
	agnic_free_resources(sc);
	pci_disable_busmaster(dev);
	device_printf(dev, "detach: resources released (no FLR; NPU left running)\n");
	return (0);
}

static device_method_t agnic_methods[] = {
	DEVMETHOD(device_probe,		agnic_probe),
	DEVMETHOD(device_attach,	agnic_attach),
	DEVMETHOD(device_detach,	agnic_detach),
	DEVMETHOD_END
};

static driver_t agnic_driver = {
	"agnic",
	agnic_methods,
	sizeof(struct agnic_softc)
};

DRIVER_MODULE(if_agnic, pci, agnic_driver, NULL, NULL);
MODULE_PNP_INFO("U16:vendor;U16:device;D:#", pci, if_agnic,
    agnic_ids, nitems(agnic_ids) - 1);
MODULE_DEPEND(if_agnic, pci, 1, 1, 1);
MODULE_VERSION(if_agnic, 1);
