/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic Phase 3b: ingress (RX) datapath bring-up for the Marvell AGNIC
 * (CN9130 EP). Builds ONE ingress TC + ONE RX data queue backed by a buffer
 * pool, runs the firmware config sequence (INIT -> INGRESS_TC_ADD ->
 * INGRESS_DATA_Q_ADD -> INIT_DONE), creates a FreeBSD ether if_t, and -- once
 * the interface is ifconfig'd up -- ENABLEs the port and services RX frames
 * poll-first (1 Hz callout) with an MSI-X doorbell fast-path as a bonus.
 *
 * Clean-room reimplementation for OPNsense/FreeBSD. Struct layouts, byte
 * offsets, opcodes and the bring-up ORDER are interface FACTS transcribed from
 * the GPL-2.0 Marvell reference (giu_nic_hw.h / giu_nic.c); no GPL .c logic is
 * copied.
 *
 * DATAPATH ABI (see if_agnic.h + agnic_giu.h for the numbers):
 *   - RX data ring: DEVICE is producer (writes agnic_rx_desc + bumps the RX
 *     producer index word on BAR0), HOST is consumer.
 *   - bpool ring:   HOST is producer (posts agnic_bpool_desc empty buffers +
 *     bumps the bpool producer index word), DEVICE is consumer.
 *   All four index words live in the BAR0 ring_indices_arr at
 *   giu_off + dev_use_size + slot*4, continuing after the 4 mgmt slots -- the
 *   exact mechanism P3a proved for the cmd/notif rings. Bumping an index word
 *   in BAR0 IS the doorbell; the device polls it. MSI-X is only a hint.
 *
 * Everything is BOUNDED. Teardown DISABLEs the device (CC_PF_DISABLE,
 * fire-and-forget) FIRST, then quiesces the callout/taskqueue, then frees the
 * rings + mbufs + ifnet. It NEVER triggers an FLR.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/endian.h>
#include <sys/sockio.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/mbuf.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/atomic.h>

#include <sys/socket.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include "if_agnic.h"

/* RX data buffers are single-cluster: full frame + pport prefix (1584) <= MCLBYTES. */
#define	AGNIC_RX_CLSIZE		MCLBYTES		/* 2048 */

/*
 * GIU frame size advertised to PF_INIT (mtu_override/mru_override). The GIU
 * frame carries the 66-byte pport prefix ([tag][hdr]) ahead of the ethernet
 * frame, so it must be 1518 + PPORT_PREFIX = 1584 -- otherwise a full 1500-MTU
 * frame + prefix overflows the GIU limit and is clipped/wedged (only small
 * frames pass). Still well under the RX cluster (2048) and NPU max_buf (9304).
 */
#define	AGNIC_RX_FRAME_SIZE	(ETHERMTU + ETHER_HDR_LEN + ETHER_CRC_LEN + PPORT_PREFIX)

/*
 * CC_PF_ENABLE retry budget. A custom NPU data-plane app (dp_fwd) may still be in
 * its guest/pp2/bpool setup (not yet servicing the mgmt channel) when the first
 * ENABLE arrives, so it times out. Retry for a while to let it come ready; if it
 * still fails, bring up mvmgmt0 anyway so the NPU stays reachable for debug/revert.
 */
#define	AGNIC_ENABLE_RETRIES	20

static void	agnic_rx_poll(void *xsc);
static void	agnic_rx_task(void *ctx, int pending);
static void	agnic_rx_service(struct agnic_softc *sc);
static void	agnic_bp_refill_locked(struct agnic_softc *sc, uint32_t n);
static void	agnic_txrx_free_rings(struct agnic_softc *sc);

/* ------------------------------------------------------------------------- */
/* Ring + buffer-pool allocation.                                            */
/* ------------------------------------------------------------------------- */

/*
 * Create the bpool's per-buffer busdma tag (36-bit ceiling, single segment,
 * cluster-sized) and the parallel agnic_rxbuf[count] tracking array with one
 * preallocated map per slot. rxb slots start empty (m == NULL).
 */
static int
agnic_bp_alloc_bufmgmt(struct agnic_softc *sc)
{
	struct agnic_data_ring *bp = &sc->bp_ring;
	device_t dev = sc->dev;
	int error, i;

	error = bus_dma_tag_create(
	    sc->parent_dmat,		/* parent (36-bit ceiling) */
	    1, 0,			/* alignment, boundary */
	    AGNIC_DMA_LOWADDR,		/* lowaddr = 36-bit */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    AGNIC_RX_CLSIZE,		/* maxsize */
	    1,				/* nsegments (single cluster) */
	    AGNIC_RX_CLSIZE,		/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &bp->buf_tag);
	if (error != 0) {
		device_printf(dev, "P3b: bpool buf DMA tag failed: %d\n", error);
		bp->buf_tag = NULL;
		return (error);
	}

	bp->rxb = malloc((size_t)bp->count * sizeof(struct agnic_rxbuf),
	    M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < (int)bp->count; i++) {
		error = bus_dmamap_create(bp->buf_tag, 0, &bp->rxb[i].map);
		if (error != 0) {
			device_printf(dev,
			    "P3b: bpool dmamap_create[%d] failed: %d\n", i,
			    error);
			return (error);
		}
	}
	return (0);
}

/*
 * Fill the bpool ring with empty clusters (count-1 of them, leaving one slot
 * free so the producer sits one lag behind the consumer). Each descriptor's
 * buff_cookie is its ring slot index; the device echoes it back in
 * rx_desc.cookie so we can locate the mbuf. Sets bp->prod_shadow = count-1.
 */
static int
agnic_bp_fill(struct agnic_softc *sc)
{
	struct agnic_data_ring *bp = &sc->bp_ring;
	struct agnic_bpool_desc *ring = (struct agnic_bpool_desc *)bp->mem.vaddr;
	device_t dev = sc->dev;
	uint32_t i;

	for (i = 0; i < bp->count - 1; i++) {
		struct agnic_rxbuf *rb = &bp->rxb[i];
		bus_dma_segment_t seg;
		struct mbuf *m;
		int nsegs, error;

		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL) {
			device_printf(dev,
			    "P3b: m_getcl failed filling bpool at %u\n", i);
			return (ENOBUFS);
		}
		m->m_len = m->m_pkthdr.len = AGNIC_RX_CLSIZE;

		error = bus_dmamap_load_mbuf_sg(bp->buf_tag, rb->map, m, &seg,
		    &nsegs, BUS_DMA_NOWAIT);
		if (error != 0 || nsegs != 1) {
			device_printf(dev,
			    "P3b: bpool load_mbuf_sg[%u] failed: %d (nsegs %d)\n",
			    i, error, nsegs);
			m_freem(m);
			return (error != 0 ? error : EFBIG);
		}
		bus_dmamap_sync(bp->buf_tag, rb->map, BUS_DMASYNC_PREREAD);

		rb->m = m;
		rb->paddr = seg.ds_addr;
		ring[i].buff_addr_phys = (uint64_t)seg.ds_addr + sc->host_headroom;
		ring[i].buff_cookie = i;
	}

	bp->prod_shadow = bp->count - 1;
	bp->cons_shadow = 0;
	bus_dmamap_sync(bp->mem.tag, bp->mem.map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	return (0);
}

/*
 * Allocate both datapath ring bodies (coherent host DRAM), the bpool buffer
 * management, and assign + zero the four BAR0 index words. On error frees
 * whatever was allocated. mgmt already validated dev_use_size + slots fit.
 */
static int
agnic_txrx_alloc(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	uint32_t idx_base;
	bus_size_t bytes;
	int error;

	/* RX data ring body: count * 32B. */
	sc->rx_ring.count = AGNIC_RX_RING_LEN;
	sc->rx_ring.prod_shadow = 0;
	sc->rx_ring.cons_shadow = 0;
	bytes = (bus_size_t)AGNIC_RX_RING_LEN * AGNIC_RXD_SIZE;
	error = agnic_dma_alloc(sc, &sc->rx_ring.mem, bytes, "rx-ring");
	if (error != 0)
		return (error);

	/* bpool ring body: count * 16B. */
	sc->bp_ring.count = AGNIC_BP_RING_LEN;
	sc->bp_ring.prod_shadow = 0;
	sc->bp_ring.cons_shadow = 0;
	sc->bp_ring.buf_size = AGNIC_RX_CLSIZE;
	bytes = (bus_size_t)AGNIC_BP_RING_LEN * AGNIC_BPD_SIZE;
	error = agnic_dma_alloc(sc, &sc->bp_ring.mem, bytes, "bp-ring");
	if (error != 0)
		goto fail;

	/* bpool buffer tag + rxb[] + per-slot maps. */
	error = agnic_bp_alloc_bufmgmt(sc);
	if (error != 0)
		goto fail;

	/*
	 * Egress (TX) ring body: count * 32B. Registered so CC_PF_ENABLE succeeds;
	 * the TX datapath (filling descriptors) lands in P4. No buffer pool: TX
	 * buffers come from transmit mbufs, not a prealloc'd bpool.
	 */
	sc->tx_ring.count = AGNIC_TX_RING_LEN;
	sc->tx_ring.prod_shadow = 0;
	sc->tx_ring.cons_shadow = 0;
	bytes = (bus_size_t)AGNIC_TX_RING_LEN * AGNIC_TXD_SIZE;
	error = agnic_dma_alloc(sc, &sc->tx_ring.mem, bytes, "tx-ring");
	if (error != 0)
		goto fail;

	/*
	 * TX copy buffers: one AGNIC_RX_CLSIZE (2KB) coherent buffer per tx-ring
	 * slot. agnic_giu_tx m_copydata()s the frame into buffer[prod] and points
	 * the descriptor's buffer_addr at it, so the device DMAs from stable
	 * coherent memory (no per-mbuf bus_dmamap juggling on the hot path). Max
	 * frame 1518 + 66B pport prefix = 1584 < 2048.
	 */
	bytes = (bus_size_t)AGNIC_TX_RING_LEN * AGNIC_RX_CLSIZE;
	error = agnic_dma_alloc(sc, &sc->tx_bufs, bytes, "tx-bufs");
	if (error != 0)
		goto fail;
	mtx_init(&sc->tx_mtx, "agnic_tx", NULL, MTX_DEF);
	sc->tx_inited = 1;
	sc->tx_dbg = 0;			/* quiet by default; sysctl tx_dbg to arm  */
	sc->rx_dbg = 0;			/* quiet by default; sysctl rx_dbg to arm  */
	sc->tx_hdr_mode = 3;		/* SFOS-exact 0xC0+i metadata (default)   */

	/*
	 * Index words: BAR0 MMIO offset = giu_off + dev_use_size + slot*4 (giu_off
	 * is 0). Published offsets (in the q_add command) are config_mem-relative
	 * = dev_use_size + slot*4. Slots 4..9 continue after the mgmt slots 0..3.
	 */
	idx_base = sc->giu_off + sc->dev_use_size;

	sc->rx_ring.prod_bar_off = idx_base + AGNIC_DATA_SLOT_RX_PROD * 4;
	sc->rx_ring.cons_bar_off = idx_base + AGNIC_DATA_SLOT_RX_CONS * 4;
	sc->bp_ring.prod_bar_off = idx_base + AGNIC_DATA_SLOT_BP_PROD * 4;
	sc->bp_ring.cons_bar_off = idx_base + AGNIC_DATA_SLOT_BP_CONS * 4;
	sc->tx_ring.prod_bar_off = idx_base + AGNIC_DATA_SLOT_TX_PROD * 4;
	sc->tx_ring.cons_bar_off = idx_base + AGNIC_DATA_SLOT_TX_CONS * 4;

	sc->rx_ring.pub_prod_off = sc->dev_use_size + AGNIC_DATA_SLOT_RX_PROD * 4;
	sc->rx_ring.pub_cons_off = sc->dev_use_size + AGNIC_DATA_SLOT_RX_CONS * 4;
	sc->bp_ring.pub_prod_off = sc->dev_use_size + AGNIC_DATA_SLOT_BP_PROD * 4;
	sc->bp_ring.pub_cons_off = sc->dev_use_size + AGNIC_DATA_SLOT_BP_CONS * 4;
	sc->tx_ring.pub_prod_off = sc->dev_use_size + AGNIC_DATA_SLOT_TX_PROD * 4;
	sc->tx_ring.pub_cons_off = sc->dev_use_size + AGNIC_DATA_SLOT_TX_CONS * 4;

	/* Zero all six index words before advertising the queues. */
	AGNIC_WR4(sc, AGNIC_BAR0, sc->rx_ring.prod_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->rx_ring.cons_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->bp_ring.prod_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->bp_ring.cons_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->tx_ring.prod_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->tx_ring.cons_bar_off, 0);
	bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
	    BUS_SPACE_BARRIER_WRITE);

	/* Pre-fill the bpool with empty clusters. */
	error = agnic_bp_fill(sc);
	if (error != 0)
		goto fail;

	device_printf(dev,
	    "P3b: rx ring dma 0x%jx (%u desc), bpool dma 0x%jx (%u buf), "
	    "tx ring dma 0x%jx (%u desc), idx@BAR0+0x%x slots 4..9\n",
	    (uintmax_t)sc->rx_ring.mem.paddr, sc->rx_ring.count,
	    (uintmax_t)sc->bp_ring.mem.paddr, sc->bp_ring.count,
	    (uintmax_t)sc->tx_ring.mem.paddr, sc->tx_ring.count, idx_base);
	return (0);

fail:
	agnic_txrx_free_rings(sc);
	return (error);
}

/* Free rings, bpool buffers, maps, tag. Idempotent; safe on partial alloc. */
static void
agnic_txrx_free_rings(struct agnic_softc *sc)
{
	struct agnic_data_ring *bp = &sc->bp_ring;
	uint32_t i;

	if (bp->rxb != NULL) {
		for (i = 0; i < bp->count; i++) {
			struct agnic_rxbuf *rb = &bp->rxb[i];

			if (rb->m != NULL) {
				bus_dmamap_sync(bp->buf_tag, rb->map,
				    BUS_DMASYNC_POSTREAD);
				bus_dmamap_unload(bp->buf_tag, rb->map);
				m_freem(rb->m);
				rb->m = NULL;
			}
			if (rb->map != NULL) {
				bus_dmamap_destroy(bp->buf_tag, rb->map);
				rb->map = NULL;
			}
		}
		free(bp->rxb, M_DEVBUF);
		bp->rxb = NULL;
	}
	if (bp->buf_tag != NULL) {
		bus_dma_tag_destroy(bp->buf_tag);
		bp->buf_tag = NULL;
	}
	if (sc->tx_inited != 0) {
		/*
		 * Clear the guard BEFORE destroying so any (impossible-by-now, since
		 * agnic_detach ether_ifdetach'd the pports first and epoch-drained
		 * their in-flight transmits) late agnic_giu_tx caller bails at the
		 * tx_inited check instead of locking a destroyed mutex.
		 */
		sc->tx_inited = 0;
		atomic_thread_fence_rel();
		mtx_destroy(&sc->tx_mtx);
	}
	agnic_dma_free(&sc->tx_bufs);
	agnic_dma_free(&sc->tx_ring.mem);
	agnic_dma_free(&sc->bp_ring.mem);
	agnic_dma_free(&sc->rx_ring.mem);
}

/* ------------------------------------------------------------------------- */
/* Firmware config sequence (INIT / TC_ADD / DATA_Q_ADD / INIT_DONE).        */
/* ------------------------------------------------------------------------- */

static int
agnic_txrx_config_queues(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	uint8_t params[AGNIC_MGMT_PARAMS_LEN];
	uint8_t resp[AGNIC_MGMT_DESC_DATA_LEN];
	struct agnic_ingress_q_cfg qc;
	uint8_t status;
	uint16_t frame = AGNIC_RX_FRAME_SIZE;
	int error;

	/* --- CC_PF_INIT --- */
	bzero(params, sizeof(params));
	agnic_pf_init_params(params, AGNIC_EGRESS_TCS, AGNIC_INGRESS_TCS,
	    frame, frame, AGNIC_ES_STRICT_SCHED);
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_INIT, params, sizeof(params),
	    resp, sizeof(resp), &status);
	if (error != 0 || status != AGNIC_NOTIF_STATUS_OK) {
		device_printf(dev,
		    "P3b: CC_PF_INIT failed (err %d status 0x%02x)\n", error,
		    status);
		return (error != 0 ? error : EIO);
	}
	device_printf(dev, "P3b: CC_PF_INIT OK (ingress_tc %d egress_tc %d "
	    "frame %u)\n", AGNIC_INGRESS_TCS, AGNIC_EGRESS_TCS, frame);

	/* --- CC_PF_INGRESS_TC_ADD (tc 0) --- */
	bzero(params, sizeof(params));
	agnic_ingress_tc_add_params(params, 0, AGNIC_QS_PER_TC, 0,
	    AGNIC_ING_HASH_TYPE_NONE);
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_INGRESS_TC_ADD, params,
	    sizeof(params), resp, sizeof(resp), &status);
	if (error != 0 || status != AGNIC_NOTIF_STATUS_OK) {
		device_printf(dev,
		    "P3b: CC_PF_INGRESS_TC_ADD failed (err %d status 0x%02x)\n",
		    error, status);
		return (error != 0 ? error : EIO);
	}
	device_printf(dev, "P3b: CC_PF_INGRESS_TC_ADD OK (tc 0, %d queue)\n",
	    AGNIC_QS_PER_TC);

	/* --- CC_PF_INGRESS_DATA_Q_ADD (tc 0, queue 0) --- */
	bzero(&qc, sizeof(qc));
	qc.q_phys = (uint64_t)sc->rx_ring.mem.paddr;
	qc.q_prod_offs = sc->rx_ring.pub_prod_off;
	qc.q_cons_offs = sc->rx_ring.pub_cons_off;
	qc.bpool_phys = (uint64_t)sc->bp_ring.mem.paddr;
	qc.bpool_prod_offs = sc->bp_ring.pub_prod_off;
	qc.bpool_cons_offs = sc->bp_ring.pub_cons_off;
	qc.q_len = sc->rx_ring.count;
	/*
	 * Register the RX queue's t2h doorbell so the NPU raises an MSI-X on RX
	 * (interrupt-driven delivery, not the 1 Hz poll). SFOS does the same:
	 * giu_nic.c mv_get_msi_id(v_idx) -> pf_ingress_data_q_add.msix_id. In our
	 * doorbell numbering (MGMT dummy id0, GIU id1..4) the RX vector is
	 * AGNIC_RX_DBELL_ID (1) -- exactly the id agnic_dbell_intr already kicks the
	 * RX taskqueue for. The 1 Hz poll callout stays as a safety-net fallback.
	 */
	qc.msix_id = AGNIC_RX_DBELL_ID;
	qc.tc = 0;
	qc.q_buf_size = sc->bp_ring.buf_size;

	bzero(params, sizeof(params));
	agnic_ingress_data_q_add_params(params, &qc);
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_INGRESS_DATA_Q_ADD, params,
	    sizeof(params), resp, sizeof(resp), &status);
	if (error != 0 || status != AGNIC_NOTIF_STATUS_OK) {
		device_printf(dev,
		    "P3b: CC_PF_INGRESS_DATA_Q_ADD failed (err %d status 0x%02x)\n",
		    error, status);
		return (error != 0 ? error : EIO);
	}
	/* q_add_resp: status@0, q_inf@1 (u64), bpool_q_inf@9 (u64). */
	{
		uint64_t q_inf = le64dec(&resp[1]);
		uint64_t bp_inf = le64dec(&resp[9]);

		device_printf(dev,
		    "P3b: CC_PF_INGRESS_DATA_Q_ADD OK (q_inf %ju bpool_q_inf %ju)\n",
		    (uintmax_t)q_inf, (uintmax_t)bp_inf);
		if ((q_inf & 0xFF) != AGNIC_Q_INF_STATUS_OK ||
		    (bp_inf & 0xFF) != AGNIC_Q_INF_STATUS_OK)
			device_printf(dev,
			    "P3b: WARNING q/bpool inf reports non-OK status\n");
	}

	/*
	 * --- CC_PF_EGRESS_TC_ADD (tc 0) ---
	 * The NPU will not CC_PF_ENABLE without a registered egress queue, even
	 * for an RX-only driver. egress_dma_engines is 1, so num_queues ==
	 * num_queues_per_dma == AGNIC_QS_PER_TC (1).
	 */
	bzero(params, sizeof(params));
	agnic_egress_tc_add_params(params, 0, AGNIC_QS_PER_TC, AGNIC_QS_PER_TC);
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_EGRESS_TC_ADD, params,
	    sizeof(params), resp, sizeof(resp), &status);
	if (error != 0 || status != AGNIC_NOTIF_STATUS_OK) {
		device_printf(dev,
		    "P3b: CC_PF_EGRESS_TC_ADD failed (err %d status 0x%02x)\n",
		    error, status);
		return (error != 0 ? error : EIO);
	}
	device_printf(dev, "P3b: CC_PF_EGRESS_TC_ADD OK (tc 0, %d queue)\n",
	    AGNIC_QS_PER_TC);

	/* --- CC_PF_EGRESS_DATA_Q_ADD (tc 0, queue 0) --- */
	{
		struct agnic_egress_q_cfg eq;

		bzero(&eq, sizeof(eq));
		eq.q_phys = (uint64_t)sc->tx_ring.mem.paddr;
		eq.q_prod_offs = sc->tx_ring.pub_prod_off;
		eq.q_cons_offs = sc->tx_ring.pub_cons_off;
		eq.q_len = sc->tx_ring.count;
		eq.q_wrr_weight = 0;		/* strict priority */
		eq.tc = 0;
		eq.msix_id = 0;			/* 0 = none (TX completion via poll in P4) */

		bzero(params, sizeof(params));
		agnic_egress_data_q_add_params(params, &eq);
		status = 0xFF;
		error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_EGRESS_DATA_Q_ADD, params,
		    sizeof(params), resp, sizeof(resp), &status);
		if (error != 0 || status != AGNIC_NOTIF_STATUS_OK) {
			device_printf(dev,
			    "P3b: CC_PF_EGRESS_DATA_Q_ADD failed (err %d "
			    "status 0x%02x)\n", error, status);
			return (error != 0 ? error : EIO);
		}
		/* q_add_resp: status@0, q_inf@1 (u64). */
		device_printf(dev,
		    "P3b: CC_PF_EGRESS_DATA_Q_ADD OK (q_inf %ju)\n",
		    (uintmax_t)le64dec(&resp[1]));
	}

	/* --- CC_PF_INIT_DONE (no params) --- */
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_INIT_DONE, NULL, 0, resp,
	    sizeof(resp), &status);
	if (error != 0 || status != AGNIC_NOTIF_STATUS_OK) {
		device_printf(dev,
		    "P3b: CC_PF_INIT_DONE failed (err %d status 0x%02x)\n",
		    error, status);
		return (error != 0 ? error : EIO);
	}
	device_printf(dev, "P3b: CC_PF_INIT_DONE OK\n");

	/*
	 * Publish the bpool producer index so the device sees count-1 free
	 * buffers. It will not consume them until CC_PF_ENABLE
	 * (agnic_datapath_start).
	 */
	atomic_thread_fence_rel();
	AGNIC_WR4(sc, AGNIC_BAR0, sc->bp_ring.prod_bar_off,
	    sc->bp_ring.prod_shadow);
	bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
	    BUS_SPACE_BARRIER_WRITE);
	return (0);
}

/*
 * Expose datapath counters + TX-format experimentation knobs under
 * dev.agnic.<unit>.  Counters are read-only; tx_dbg/rx_dbg re-arm the frame
 * hex-dumps; tx_hdr_mode/tx_hdr_magic/tx_pkt_offset let us discover the pport
 * TX format the NPU accepts without a rebuild per attempt.
 */
static void
agnic_add_sysctls(struct agnic_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->dev);
	struct sysctl_oid_list *ch = SYSCTL_CHILDREN(tree);

	SYSCTL_ADD_U64(ctx, ch, OID_AUTO, "tx_packets", CTLFLAG_RD,
	    &sc->tx_packets, 0, "frames enqueued on the trunk TX ring");
	SYSCTL_ADD_U64(ctx, ch, OID_AUTO, "tx_dropped", CTLFLAG_RD,
	    &sc->tx_dropped, 0, "TX frames dropped (ring full / oversized / down)");
	SYSCTL_ADD_U64(ctx, ch, OID_AUTO, "rx_frames", CTLFLAG_RD,
	    &sc->rx_frames, 0, "frames delivered from the RX ring");
	SYSCTL_ADD_U64(ctx, ch, OID_AUTO, "rx_dbell_count", CTLFLAG_RD,
	    &sc->rx_dbell_count, 0, "RX MSI-X doorbells serviced (inline)");
	SYSCTL_ADD_U64(ctx, ch, OID_AUTO, "rx_dropped", CTLFLAG_RD,
	    &sc->rx_dropped, 0, "RX completions dropped (torn/oob/oversized)");
	SYSCTL_ADD_INT(ctx, ch, OID_AUTO, "tx_dbg", CTLFLAG_RW,
	    &sc->tx_dbg, 0, "remaining egress frames to hex-dump to dmesg");
	SYSCTL_ADD_INT(ctx, ch, OID_AUTO, "rx_dbg", CTLFLAG_RW,
	    &sc->rx_dbg, 0, "remaining large (>140B) ingress frames to hex-dump");
	SYSCTL_ADD_INT(ctx, ch, OID_AUTO, "tx_hdr_mode", CTLFLAG_RW,
	    &sc->tx_hdr_mode, 0, "pport TX 64B header: 0=zeros 1=copy-RX 2=magic");
	SYSCTL_ADD_U64(ctx, ch, OID_AUTO, "tx_hdr_magic", CTLFLAG_RW,
	    &sc->tx_hdr_magic, 0, "8-byte magic (LE) for tx_hdr_mode=2");
	SYSCTL_ADD_INT(ctx, ch, OID_AUTO, "tx_pkt_offset", CTLFLAG_RW,
	    &sc->tx_pkt_offset, 0, "TX descriptor pkt_offset (0 default)");
	SYSCTL_ADD_INT(ctx, ch, OID_AUTO, "rx_last_hdr_valid", CTLFLAG_RD,
	    &sc->rx_last_hdr_valid, 0, "a real RX pport header has been captured");
}

/* ------------------------------------------------------------------------- */
/* ifnet creation + P3b bring-up entry point.                                */
/* ------------------------------------------------------------------------- */

int
agnic_txrx_bringup(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	uint32_t need;
	int error;

	if (!sc->mgmt_ready) {
		device_printf(dev, "P3b: mgmt not ready; skip datapath\n");
		return (ENXIO);
	}
	if (sc->giu_bar != AGNIC_BAR0) {
		device_printf(dev, "P3b: GIU not on BAR0; abort\n");
		return (EINVAL);
	}
	/* mgmt owns index slots 0..3; we need 4..7 to also fit config_mem. */
	need = sc->dev_use_size +
	    (AGNIC_MGMT_IDX_SLOTS + AGNIC_DATA_IDX_SLOTS) * 4;
	if (sc->dev_use_size == 0 || need > AGNIC_CONFIG_BAR_SIZE) {
		device_printf(dev,
		    "P3b: index slots do not fit (need 0x%x <= 0x%x); abort\n",
		    need, AGNIC_CONFIG_BAR_SIZE);
		return (ERANGE);
	}

	sc->host_headroom = AGNIC_RX_HEADROOM;
	sc->rx_frames = 0;
	sc->rx_dropped = 0;
	sc->rx_first_done = 0;

	/* Construct the RX servicing objects before marking txrx live. */
	mtx_init(&sc->rx_mtx, "agnic_rx", NULL, MTX_DEF);
	/*
	 * mtx-based callout: the poll callback runs with rx_mtx held and only
	 * kicks the RX task + reschedules itself. Binding it to rx_mtx makes
	 * callout_drain race-free against a self-reschedule (the callback sees
	 * if_running==0 under the lock and stops), so no stray callback can ever
	 * fire after the rings are freed.
	 */
	callout_init_mtx(&sc->rx_poll, &sc->rx_mtx, 0);

	sc->rx_tq = taskqueue_create("agnic_rx", M_NOWAIT,
	    taskqueue_thread_enqueue, &sc->rx_tq);
	if (sc->rx_tq == NULL) {
		device_printf(dev, "P3b: taskqueue_create failed\n");
		mtx_destroy(&sc->rx_mtx);
		return (ENOMEM);
	}
	TASK_INIT(&sc->rx_task, 0, agnic_rx_task, sc);
	taskqueue_start_threads(&sc->rx_tq, 1, PI_NET, "%s rxtq",
	    device_get_nameunit(dev));

	sc->txrx_inited = 1;	/* rx_mtx now valid: the dbell kick may run */

	/* Allocate rings + bpool + index words. */
	error = agnic_txrx_alloc(sc);
	if (error != 0)
		goto fail;

	/* Run the firmware ingress config sequence. */
	error = agnic_txrx_config_queues(sc);
	if (error != 0)
		goto fail_rings;

	/*
	 * The GIU trunk is an INTERNAL driver construct -- it carries every
	 * front-panel port's traffic tagged, so it is not a usable ethernet
	 * interface for the stack. We deliberately do NOT create an OS-visible
	 * ifnet for it: the only interfaces the OS sees are the demuxed
	 * front-panel ports (port1..port9, agnic_pport.c). Exposing the trunk
	 * would let OPNsense's boot-time interface assignment pick it as LAN
	 * (it is the only interface present before the datapath auto-starts),
	 * which can never DHCP. sc->ifp stays NULL; the datapath is driven
	 * directly off the rings and auto-started by agnic_datapath_start().
	 */
	sc->ifp = NULL;
	agnic_add_sysctls(sc);

	device_printf(dev,
	    "[Phase 3b] GIU trunk datapath ready (internal, MAC "
	    "%02x:%02x:%02x:%02x:%02x:%02x); auto-starting -- OS sees only "
	    "port1..port9\n", sc->mac[0], sc->mac[1], sc->mac[2], sc->mac[3],
	    sc->mac[4], sc->mac[5]);
	return (0);

fail_rings:
	agnic_txrx_free_rings(sc);
fail:
	/* Tear down the RX servicing objects; interrupts not yet relevant. */
	sc->txrx_inited = 0;
	if (sc->rx_tq != NULL) {
		taskqueue_free(sc->rx_tq);
		sc->rx_tq = NULL;
	}
	callout_drain(&sc->rx_poll);
	mtx_destroy(&sc->rx_mtx);
	return (error);
}

void
agnic_txrx_teardown(struct agnic_softc *sc)
{
	device_t dev = sc->dev;

	if (!sc->txrx_inited)
		return;

	/*
	 * Callers guarantee the MSI-X doorbells are already torn down (detach
	 * runs agnic_p2b_teardown first), so no new RX kick can arrive here.
	 */
	if (sc->ifp != NULL) {
		/* Bring the port down (DISABLE) + quiesce, then detach. */
		agnic_stop(sc);
		ether_ifdetach(sc->ifp);
		if_free(sc->ifp);
		sc->ifp = NULL;
	} else {
		/* Never attached an ifnet: still DISABLE + quiesce. */
		agnic_stop(sc);
	}

	callout_drain(&sc->rx_poll);
	if (sc->rx_tq != NULL) {
		taskqueue_drain(sc->rx_tq, &sc->rx_task);
		taskqueue_free(sc->rx_tq);
		sc->rx_tq = NULL;
	}

	agnic_txrx_free_rings(sc);

	mtx_destroy(&sc->rx_mtx);
	sc->txrx_inited = 0;
	device_printf(dev, "P3b: datapath torn down\n");
}

/*
 * GIU RX doorbell ISR path (runs in the MSI-X ithread). Deliver RX INLINE here
 * -- like Linux NAPI running in softirq right after the IRQ -- instead of
 * bouncing to a separate taskqueue thread, which added a second scheduler
 * wakeup and ~tens-of-ms latency spikes. agnic_rx_service takes rx_mtx itself,
 * so this must NOT hold it; concurrent runs (this ithread + the 1 Hz poll's
 * taskqueue) serialize on rx_mtx and split the ring safely. The poll callout +
 * taskqueue remain as the fallback if a doorbell is ever missed.
 */
void
agnic_txrx_dbell_kick(struct agnic_softc *sc)
{

	if (!sc->txrx_inited)
		return;
	sc->rx_dbell_count++;
	if (sc->if_running)
		agnic_rx_service(sc);
}

/* ------------------------------------------------------------------------- */
/* ifnet init / stop / ioctl / transmit.                                     */
/* ------------------------------------------------------------------------- */

/*
 * Auto-start the GIU trunk datapath: CC_PF_ENABLE, start RX servicing, put the
 * trunk in promiscuous mode, then bring up the NW_AGENT front-panel ports, the
 * mvmgmt0 NPU link, and the pport demux. Called once from the config hook after
 * the rings are up -- NOT from an if_init, because the trunk has no OS-visible
 * ifnet; the front-panel port ifnets must exist before OPNsense assigns
 * interfaces at boot. Sleepable (issues mgmt commands). Idempotent.
 */
void
agnic_datapath_start(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	uint8_t resp[AGNIC_MGMT_DESC_DATA_LEN];
	uint8_t status;
	int error;

	if (sc->if_running)
		return;

	/*
	 * CC_PF_ENABLE (no params). Do NOT hold rx_mtx: this reap sleeps.
	 * Retry: a custom NPU app may not be servicing the mgmt channel yet.
	 */
	{
		int attempt, enable_ok = 0;

		for (attempt = 0; attempt < AGNIC_ENABLE_RETRIES; attempt++) {
			status = 0xFF;
			error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_ENABLE, NULL, 0,
			    resp, sizeof(resp), &status);
			if (error == 0 && status == AGNIC_NOTIF_STATUS_OK) {
				enable_ok = 1;
				break;
			}
			if (attempt == 0)
				device_printf(dev, "P3b: CC_PF_ENABLE not ready "
				    "(err %d status 0x%02x); retrying up to %d\n",
				    error, status, AGNIC_ENABLE_RETRIES);
			pause("agenbl", hz / 2);
		}
		if (!enable_ok) {
			device_printf(dev, "P3b: CC_PF_ENABLE still failing after "
			    "%d tries; interface stays down, bringing up mvmgmt0 "
			    "anyway so the NPU is reachable\n", AGNIC_ENABLE_RETRIES);
			if (sc->pcinet == NULL)
				(void)agnic_pcinet_bringup(sc);
			return;
		}
		device_printf(dev, "P3b: CC_PF_ENABLE OK (attempt %d)\n",
		    attempt + 1);
	}

	/*
	 * Diagnostic: ask the NPU for the current link state (best effort; the
	 * authoritative signal is the async NC_PF_LINK_CHANGE the drainer logs).
	 * This tells us whether the NPU sees a live physical link on the PF.
	 */
	{
		uint8_t lr[AGNIC_MGMT_DESC_DATA_LEN];
		uint8_t lst = 0xFF;

		if (agnic_mgmt_cmd(sc, AGNIC_CC_PF_LINK_STATUS, NULL, 0, lr,
		    sizeof(lr), &lst) == 0 && lst == AGNIC_NOTIF_STATUS_OK)
			device_printf(dev,
			    "[Phase 3b] CC_PF_LINK_STATUS: link_status=0x%x (%s)\n",
			    le32dec(&lr[1]), le32dec(&lr[1]) ? "UP" : "DOWN");
		else
			device_printf(dev,
			    "[Phase 3b] CC_PF_LINK_STATUS query unsupported/failed "
			    "(err/st 0x%02x)\n", lst);
	}

	/*
	 * Start RX servicing under rx_mtx (the callout's lock): the 1 Hz poll
	 * callout is the reliable path; the RX doorbell task is a fast-path bonus.
	 */
	mtx_lock(&sc->rx_mtx);
	sc->if_running = 1;
	callout_reset(&sc->rx_poll, hz, agnic_rx_poll, sc);
	mtx_unlock(&sc->rx_mtx);

	device_printf(dev,
	    "[Phase 3b] port ENABLEd; RX poll @1Hz + MSI-X dbell id %d\n",
	    AGNIC_RX_DBELL_ID);

	/*
	 * P4b: put the trunk PF in promiscuous mode. Front-panel frames are
	 * forwarded to the host tagged (their apparent dst MAC is 0x81pp...), so
	 * the trunk must accept ALL frames, not just those matching its own MAC.
	 * Without this the NPU host-facing filter drops every tagged frame and
	 * Ipkts stays 0.
	 */
	{
		uint8_t p[AGNIC_MGMT_PARAMS_LEN];
		uint8_t rr[AGNIC_MGMT_DESC_DATA_LEN];
		uint8_t st = 0xFF;

		bzero(p, sizeof(p));
		p[0] = 1;			/* AGNIC_PROMISC_ENABLE */
		if (agnic_mgmt_cmd(sc, AGNIC_CC_PF_PROMISC, p, sizeof(p),
		    rr, sizeof(rr), &st) == 0 && st == AGNIC_NOTIF_STATUS_OK)
			device_printf(dev, "[Phase 4b] trunk promiscuous ON\n");
		else
			device_printf(dev,
			    "[Phase 4b] promisc enable failed (st 0x%02x)\n", st);
	}

	/*
	 * P4a: now that the GIU trunk is ENABLEd/linked, bring up the NW_AGENT
	 * mailbox and admin-up the physical front-panel ports. The NPU only
	 * publishes the nwa mailbox after the trunk is live (SFOS shows it ~5 s
	 * after "AGNIC Link is Up"), so this must run here, not in the config
	 * hook. Non-fatal; runs once.
	 */
	if (!sc->nwa_ready)
		(void)agnic_nwa_bringup(sc);

	/*
	 * P5: bring up the mvmgmt0 management link to the NPU. This is the
	 * channel over which we SSH the NPU the `host_breakout_complete`
	 * handshake that makes it start forwarding front-panel ports. Non-fatal.
	 */
	if (sc->pcinet == NULL)
		(void)agnic_pcinet_bringup(sc);

	/*
	 * P4b: bring up the per-front-panel-port demux (port1..port9). Once the
	 * NPU forwards front-panel traffic up the trunk (tagged), this splits it
	 * into individually-addressable ifnets. Non-fatal.
	 */
	if (sc->pport == NULL)
		(void)agnic_pport_bringup(sc);
}

/* Stop RX + DISABLE the port (fire-and-forget). Safe to call repeatedly. */
void
agnic_stop(struct agnic_softc *sc)
{
	int was_running;

	/*
	 * rx_mtx / rx_poll / rx_tq only exist once txrx_inited is set (agnic_txrx.c
	 * sets it after mtx_init). Detach may call agnic_stop on the deferred-
	 * handshake path where the datapath was never brought up -- nothing to stop.
	 */
	if (sc->txrx_inited == 0)
		return;

	mtx_lock(&sc->rx_mtx);
	was_running = sc->if_running;
	sc->if_running = 0;
	if (sc->ifp != NULL)
		if_setdrvflagbits(sc->ifp, 0,
		    IFF_DRV_RUNNING | IFF_DRV_OACTIVE);
	mtx_unlock(&sc->rx_mtx);

	/* Stop the poll callout + drain any in-flight RX task. */
	callout_drain(&sc->rx_poll);
	if (sc->rx_tq != NULL)
		taskqueue_drain(sc->rx_tq, &sc->rx_task);

	/*
	 * DISABLE stops the device DMAing into our buffers / index words. Fire-
	 * and-forget (the device may set resp_msg_len=0 for DISABLE). Only poke
	 * if we were actually running so a spurious down does not disturb a
	 * never-enabled device.
	 */
	if (was_running)
		agnic_mgmt_cmd_noresp(sc, AGNIC_CC_PF_DISABLE, NULL, 0);
}

/*
 * Transmit one mbuf on the GIU trunk egress ring.
 *
 * SPSC egress ring: host is producer (tx_ring.prod_shadow / prod_bar_off word),
 * device is consumer (cons_bar_off word). Full when (prod+1)==cons. We copy the
 * whole frame into the coherent tx_bufs slot for this producer index (stable DMA
 * source, no per-mbuf map churn), fill a single-segment descriptor pointing at
 * it, publish the new producer index to the BAR, and let the polling data-plane
 * pick it up. Mirrors giu_nic.c start_xmit (flags|=SG_SINGLE_ENTRY, byte_cnt,
 * pkt_offset=0, buffer_addr, then writel(producer_p)); no csum-parsing flags, so
 * the frame is transmitted verbatim (the host stack already checksummed it).
 *
 * Consumes m in all paths. Returns 0 on enqueue, ENOBUFS when the ring is full
 * or the frame is oversized.
 */
int
agnic_giu_tx(struct agnic_softc *sc, struct mbuf *m)
{
	struct agnic_data_ring *tx = &sc->tx_ring;
	struct agnic_tx_desc *ring;
	struct agnic_tx_desc *d;
	uint32_t prod, next, cons;
	int len, po;
	uint8_t *dst;

	len = m->m_pkthdr.len;
	if (len <= 0 || len > AGNIC_RX_CLSIZE) {
		sc->tx_dropped++;
		m_freem(m);
		return (ENOBUFS);
	}

	/*
	 * Guard BEFORE mtx_lock: during detach, agnic_txrx_free_rings destroys
	 * tx_mtx and frees tx_bufs/tx_ring while (briefly) the pport ifnets may
	 * still be attached. Locking a destroyed mutex would panic, so bail if the
	 * TX resources are gone. (Detach also reorders to detach the pports first;
	 * this is defense-in-depth.)
	 */
	if (sc->tx_inited == 0) {
		sc->tx_dropped++;
		m_freem(m);
		return (ENXIO);
	}

	mtx_lock(&sc->tx_mtx);
	if (!sc->if_running) {
		mtx_unlock(&sc->tx_mtx);
		sc->tx_dropped++;
		m_freem(m);
		return (ENOBUFS);
	}

	prod = tx->prod_shadow;
	next = AGNIC_RING_INC(prod, tx->count);
	cons = AGNIC_RD4(sc, AGNIC_BAR0, tx->cons_bar_off);
	if (next == cons) {			/* ring full: device hasn't drained */
		mtx_unlock(&sc->tx_mtx);
		sc->tx_dropped++;
		m_freem(m);
		return (ENOBUFS);
	}

	/*
	 * Copy the frame into this slot's coherent TX buffer at the (tunable)
	 * pkt_offset; the device reads byte_cnt bytes from buffer_addr + pkt_offset.
	 * Default pkt_offset 0 = frame at the buffer start.
	 */
	po = sc->tx_pkt_offset;
	if (po < 0 || (size_t)po + len > AGNIC_RX_CLSIZE)
		po = 0;
	dst = (uint8_t *)sc->tx_bufs.vaddr + (size_t)prod * AGNIC_RX_CLSIZE + po;
	m_copydata(m, 0, len, dst);

	ring = (struct agnic_tx_desc *)tx->mem.vaddr;
	d = &ring[prod];
	bzero(d, sizeof(*d));
	/*
	 * SG_SINGLE_ENTRY marks a one-segment packet. The two csum-DISABLE bits are
	 * mandatory here: the driver advertises no TX offload and the host stack has
	 * already checksummed the frame, so with the bits CLEAR the device would
	 * (per the GPL contract, giu_nic.c:441-443) regenerate IPv4/L4 checksums and
	 * rewrite frame bytes -- corrupting egress and stalling the GIU. Setting
	 * them tells the device to transmit verbatim.
	 */
	d->flags = AGNIC_TXD_FLAGS_SG_SINGLE_ENTRY |
	    AGNIC_TXD_FLAGS_GEN_L4_CSUM_NOT | AGNIC_TXD_FLAGS_GEN_IPV4_CSUM_DIS;
	d->byte_cnt = (uint16_t)len;
	d->pkt_offset = (uint8_t)po;
	d->buffer_addr = (uint64_t)sc->tx_bufs.paddr +
	    (uint64_t)prod * AGNIC_RX_CLSIZE;
	d->cookie = prod;

	/* One-shot diagnostic: dump the first few TX descriptors + frame heads so a
	 * cold-boot test shows exactly what egresses (tag/header/eth) and whether
	 * the device is draining the ring (cons advancing). */
	if (sc->tx_dbg > 0) {
		int cap = (len < 80) ? len : 80;

		sc->tx_dbg--;
		device_printf(sc->dev,
		    "[TXdbg] prod=%u cons=%u len=%d po=%d hdrmode=%d flags=0x%08x "
		    "buf=0x%jx frame[%d]: %*D\n", prod, cons, len, po,
		    sc->tx_hdr_mode, d->flags, (uintmax_t)d->buffer_addr,
		    cap, cap, dst, " ");
	}

	/* Publish the descriptor + buffer before advancing the producer index. */
	bus_dmamap_sync(sc->tx_bufs.tag, sc->tx_bufs.map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(tx->mem.tag, tx->mem.map, BUS_DMASYNC_PREWRITE);
	atomic_thread_fence_rel();

	tx->prod_shadow = next;
	AGNIC_WR4(sc, AGNIC_BAR0, tx->prod_bar_off, next);
	bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
	    BUS_SPACE_BARRIER_WRITE);

	sc->tx_packets++;
	mtx_unlock(&sc->tx_mtx);

	m_freem(m);
	return (0);
}

/* ------------------------------------------------------------------------- */
/* RX servicing (poll callout + MSI-X task funnel here).                     */
/* ------------------------------------------------------------------------- */

/*
 * 1 Hz poll callout. Runs with rx_mtx held (mtx-based callout). It does NOT
 * touch the ring directly -- it enqueues the RX task (which locks rx_mtx and
 * delivers frames outside the lock) and reschedules itself. Checking if_running
 * under the lock before rescheduling is what makes callout_drain race-free.
 */
static void
agnic_rx_poll(void *xsc)
{
	struct agnic_softc *sc = xsc;

	mtx_assert(&sc->rx_mtx, MA_OWNED);
	if (!sc->if_running)
		return;
	if (sc->rx_tq != NULL)
		taskqueue_enqueue(sc->rx_tq, &sc->rx_task);
	callout_reset(&sc->rx_poll, hz, agnic_rx_poll, sc);
}

static void
agnic_rx_task(void *ctx, int pending)
{

	(void)pending;
	agnic_rx_service((struct agnic_softc *)ctx);
}

/*
 * Refill up to n bpool slots with fresh clusters at the advancing producer,
 * then publish the new bpool producer index. Called with rx_mtx held. Under the
 * single-queue lockstep the freed slots line up with prod_shadow; stop early if
 * a slot is unexpectedly occupied or an allocation fails (device just runs with
 * fewer buffers until the next pass).
 */
static void
agnic_bp_refill_locked(struct agnic_softc *sc, uint32_t n)
{
	struct agnic_data_ring *bp = &sc->bp_ring;
	struct agnic_bpool_desc *ring = (struct agnic_bpool_desc *)bp->mem.vaddr;
	uint32_t posted = 0;
	uint32_t i;

	for (i = 0; i < n; i++) {
		uint32_t pos = bp->prod_shadow;
		struct agnic_rxbuf *rb = &bp->rxb[pos];
		bus_dma_segment_t seg;
		struct mbuf *m;
		int nsegs, error;

		if (rb->m != NULL)		/* ring full: nothing to reclaim */
			break;
		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL)
			break;
		m->m_len = m->m_pkthdr.len = AGNIC_RX_CLSIZE;
		error = bus_dmamap_load_mbuf_sg(bp->buf_tag, rb->map, m, &seg,
		    &nsegs, BUS_DMA_NOWAIT);
		if (error != 0 || nsegs != 1) {
			m_freem(m);
			break;
		}
		bus_dmamap_sync(bp->buf_tag, rb->map, BUS_DMASYNC_PREREAD);
		rb->m = m;
		rb->paddr = seg.ds_addr;
		ring[pos].buff_addr_phys = (uint64_t)seg.ds_addr +
		    sc->host_headroom;
		ring[pos].buff_cookie = pos;
		bp->prod_shadow = AGNIC_RING_INC(bp->prod_shadow, bp->count);
		posted++;
	}

	if (posted > 0) {
		bus_dmamap_sync(bp->mem.tag, bp->mem.map, BUS_DMASYNC_PREWRITE);
		atomic_thread_fence_rel();
		AGNIC_WR4(sc, AGNIC_BAR0, bp->prod_bar_off, bp->prod_shadow);
		bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
		    BUS_SPACE_BARRIER_WRITE);
	}
}

/*
 * Drain the RX ring: read the device-updated producer index, convert each
 * completion into an mbuf (collected into a local list), publish the new RX
 * consumer index, refill the bpool, then hand the mbufs to the stack OUTSIDE
 * rx_mtx. Bounded to at most count descriptors per pass. Always runs in the RX
 * task (enqueued by both the poll callout and the MSI-X doorbell); the single
 * task thread + rx_mtx keep it serialized.
 */
static void
agnic_rx_service(struct agnic_softc *sc)
{
	struct agnic_data_ring *rx = &sc->rx_ring;
	struct agnic_data_ring *bp = &sc->bp_ring;
	struct agnic_rx_desc *ring;
	struct mbuf *mh = NULL, *mt = NULL;
	uint32_t prod, guard = 0, n = 0;
	uint8_t first_hdr[80];
	uint16_t first_len = 0;
	int have_first = 0, first_cap = 0;
	uint8_t big_hdr[80];
	uint16_t big_len = 0;
	int have_big = 0, big_cap = 0;

	mtx_lock(&sc->rx_mtx);
	if (!sc->if_running) {
		mtx_unlock(&sc->rx_mtx);
		return;
	}
	ring = (struct agnic_rx_desc *)rx->mem.vaddr;

	prod = AGNIC_RD4(sc, AGNIC_BAR0, rx->prod_bar_off);
	atomic_thread_fence_acq();
	bus_dmamap_sync(rx->mem.tag, rx->mem.map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	while (rx->cons_shadow != prod && guard < rx->count) {
		struct agnic_rx_desc *d = &ring[rx->cons_shadow];
		uint64_t cookie = d->cookie;
		uint16_t len = d->byte_cnt;
		struct agnic_rxbuf *rb;
		struct mbuf *m;

		guard++;

		/* Every completion consumed exactly one bpool buffer. */
		n++;
		rx->cons_shadow = AGNIC_RING_INC(rx->cons_shadow, rx->count);

		/* Defensive: skip DMA-reordered / poisoned / out-of-range descs. */
		if (cookie == AGNIC_COOKIE_DEVICE_WATERMARK ||
		    cookie == AGNIC_COOKIE_DRIVER_WATERMARK ||
		    cookie >= bp->count) {
			sc->rx_dropped++;
			continue;
		}
		rb = &bp->rxb[(uint32_t)cookie];
		m = rb->m;
		if (m == NULL || len == 0 || len > bp->buf_size) {
			sc->rx_dropped++;
			if (m != NULL) {
				bus_dmamap_sync(bp->buf_tag, rb->map,
				    BUS_DMASYNC_POSTREAD);
				bus_dmamap_unload(bp->buf_tag, rb->map);
				m_freem(m);
				rb->m = NULL;
			}
			continue;
		}

		bus_dmamap_sync(bp->buf_tag, rb->map, BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(bp->buf_tag, rb->map);
		rb->m = NULL;

		/* Trim to the real frame start + length. */
		if (sc->host_headroom + d->pkt_offset > 0)
			m_adj(m, sc->host_headroom + d->pkt_offset);
		m->m_len = m->m_pkthdr.len = len;
		m->m_pkthdr.rcvif = NULL;	/* pport demux sets the real port */
		m->m_nextpkt = NULL;

		if (!sc->rx_first_done && !have_first) {
			first_cap = (len < (int)sizeof(first_hdr)) ?
			    (int)len : (int)sizeof(first_hdr);
			memcpy(first_hdr, mtod(m, uint8_t *), first_cap);
			first_len = len;
			have_first = 1;
		}

		/* Capture the first larger-than-ARP frame this pass (>140B incl the
		 * 66B pport prefix ~= an IP frame like a DHCP OFFER / ICMP reply) so
		 * we can confirm large-frame RX independent of the small broadcasts. */
		if (sc->rx_dbg > 0 && !have_big && len > 140) {
			big_cap = (len < (int)sizeof(big_hdr)) ?
			    (int)len : (int)sizeof(big_hdr);
			memcpy(big_hdr, mtod(m, uint8_t *), big_cap);
			big_len = len;
			have_big = 1;
		}

		if (mh == NULL)
			mh = mt = m;
		else {
			mt->m_nextpkt = m;
			mt = m;
		}
		sc->rx_frames++;
	}

	if (n > 0) {
		/* Publish the RX consumer index; then hand freed slots back. */
		atomic_thread_fence_rel();
		AGNIC_WR4(sc, AGNIC_BAR0, rx->cons_bar_off, rx->cons_shadow);
		bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
		    BUS_SPACE_BARRIER_WRITE);
		agnic_bp_refill_locked(sc, n);
	}

	{
		uint64_t total = sc->rx_frames;

		mtx_unlock(&sc->rx_mtx);

		/* Deliver to the stack outside the lock. Every frame is routed to
		 * its front-panel port ifnet (strip the tag+header) by the pport
		 * demux. The trunk has no OS-visible ifnet, so any frame that
		 * arrives before the demux is up (a tiny window at bring-up) is
		 * just dropped -- it would be a raw tagged frame the stack can't
		 * use anyway. */
		while (mh != NULL) {
			struct mbuf *nx = mh->m_nextpkt;

			mh->m_nextpkt = NULL;
			if (sc->pport != NULL)
				agnic_pport_rx(sc, mh);
			else
				m_freem(mh);
			mh = nx;
		}

		if (have_first) {
			device_printf(sc->dev,
			    "[Phase 4b] first RX frame (%uB) raw: %*D\n",
			    first_len, first_cap, first_hdr, " ");
			sc->rx_first_done = 1;
		}
		if (have_big && sc->rx_dbg > 0) {
			sc->rx_dbg--;
			device_printf(sc->dev,
			    "[RXdbg] big frame (%uB) raw: %*D\n",
			    big_len, big_cap, big_hdr, " ");
		}
		if (n > 0 && total <= 8)
			device_printf(sc->dev,
			    "[Phase 3b] RX frames delivered: %ju (+%u this pass, "
			    "%ju dropped)\n", (uintmax_t)total, n,
			    (uintmax_t)sc->rx_dropped);
	}
}
