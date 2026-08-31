/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic Phase 3a: management CMD/NOTIF ring bring-up and a single safe
 * command round-trip (CC_PF_MGMT_ECHO) against the Marvell AGNIC (CN9130 EP).
 *
 * Clean-room reimplementation for OPNsense/FreeBSD. Struct layouts, byte
 * offsets, opcodes and the handshake ORDER are interface FACTS transcribed from
 * the GPL-2.0 Marvell reference (giu_nic_hw.h / giu_nic_mgmt.c / giu_nic.c); no
 * GPL .c logic is copied.
 *
 * KEY ABI FACT (verified against the GPL source, agnic_init_q_indices):
 *   The mgmt cmd/notif producer & consumer index words live ON BAR0, in the
 *   ring_indices_arr placed immediately after config_mem at BAR0 offset
 *   config_mem.dev_use_size. ring_indices_arr_phys == dev_use_size (a BAR-base-
 *   relative OFFSET, NOT a host DRAM address -- the GPL comment says so
 *   explicitly). So q_prod_offs/q_cons_offs published into config_mem are BAR0
 *   byte offsets, and the host reads/writes those index words via BAR0 MMIO.
 *   Only q_addr (the 64-byte descriptor ring body) is a host-DRAM 36-bit bus
 *   address that the device DMAs.
 *
 * There is NO BAR MMIO doorbell required for the mgmt path: the device polls the
 * producer index word. We still ALSO ring the h2t BAR4 doorbell (index 0 = RPC
 * mgmt channel) after publishing the producer index, per the bring-up plan --
 * it is real, FLR-safe hardware and harmless if the device is already polling.
 *
 * Everything here is BOUNDED. On any timeout we log and return an error, leaving
 * P2b (cookie/HOST_INIT/heartbeat/DEV_READY) intact and NEVER triggering an FLR.
 * Ring DMA allocated here is freed only in agnic_mgmt_teardown (detach): once the
 * queues are published we must not free them out from under a polling device.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/callout.h>
#include <sys/proc.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/atomic.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include "if_agnic.h"

/* P3a bounded-poll budgets (milliseconds). */
#define	AGNIC_DEV_MGMT_READY_TIMEOUT_MS	2000	/* DEV_MGMT_READY after publish */
#define	AGNIC_H2T_DBELL_TIMEOUT_MS	2000	/* TRGT_H2T_DBELL + entry filled */
#define	AGNIC_MGMT_CMD_TIMEOUT_MS	5000	/* per-command response wait     */

/* ------------------------------------------------------------------------- */
/* Coherent DMA helpers.                                                     */
/* ------------------------------------------------------------------------- */

static void
agnic_dma_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *paddr = arg;

	if (error == 0 && nseg == 1)
		*paddr = segs[0].ds_addr;
	else
		*paddr = 0;
}

/*
 * Allocate one coherent, single-segment, 36-bit-safe DMA buffer (a descriptor
 * ring). Returns 0 on success with m->{tag,map,vaddr,paddr,size} populated.
 * Non-static: shared with agnic_txrx.c for the P3b datapath ring bodies.
 */
int
agnic_dma_alloc(struct agnic_softc *sc, struct agnic_dma_mem *m,
    bus_size_t size, const char *name)
{
	device_t dev = sc->dev;
	int error;

	bzero(m, sizeof(*m));
	m->size = size;

	error = bus_dma_tag_create(
	    sc->parent_dmat,		/* parent (inherits 36-bit ceiling) */
	    PAGE_SIZE, 0,		/* alignment, boundary */
	    AGNIC_DMA_LOWADDR,		/* lowaddr = 36-bit */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    size,			/* maxsize */
	    1,				/* nsegments (single contiguous) */
	    size,			/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &m->tag);
	if (error != 0) {
		device_printf(dev, "P3a: %s DMA tag failed: %d\n", name, error);
		m->tag = NULL;
		return (error);
	}

	error = bus_dmamem_alloc(m->tag, &m->vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT | BUS_DMA_ZERO, &m->map);
	if (error != 0) {
		device_printf(dev, "P3a: %s dmamem_alloc failed: %d\n",
		    name, error);
		bus_dma_tag_destroy(m->tag);
		m->tag = NULL;
		return (error);
	}

	error = bus_dmamap_load(m->tag, m->map, m->vaddr, size,
	    agnic_dma_cb, &m->paddr, BUS_DMA_NOWAIT);
	if (error != 0 || m->paddr == 0) {
		device_printf(dev, "P3a: %s dmamap_load failed: %d\n",
		    name, error);
		bus_dmamem_free(m->tag, m->vaddr, m->map);
		bus_dma_tag_destroy(m->tag);
		m->tag = NULL;
		return (error != 0 ? error : ENOMEM);
	}

	if ((uint64_t)m->paddr > AGNIC_DMA_LOWADDR) {
		device_printf(dev,
		    "P3a: %s paddr 0x%jx exceeds 36-bit ceiling\n",
		    name, (uintmax_t)m->paddr);
		bus_dmamap_unload(m->tag, m->map);
		bus_dmamem_free(m->tag, m->vaddr, m->map);
		bus_dma_tag_destroy(m->tag);
		m->tag = NULL;
		return (ERANGE);
	}

	return (0);
}

void
agnic_dma_free(struct agnic_dma_mem *m)
{

	if (m->tag == NULL)
		return;
	if (m->paddr != 0) {
		bus_dmamap_unload(m->tag, m->map);
		m->paddr = 0;
	}
	if (m->vaddr != NULL) {
		bus_dmamem_free(m->tag, m->vaddr, m->map);
		m->vaddr = NULL;
	}
	bus_dma_tag_destroy(m->tag);
	m->tag = NULL;
}

/* ------------------------------------------------------------------------- */
/* Ring publish + handshake.                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Write one agnic_q_hw_info block (24B) into config_mem at BAR0 offset q_off:
 *   q_addr  u64 @0x00 = ring DMA bus addr
 *   q_prod  u32 @0x08 = BAR0 offset of producer index word
 *   q_cons  u32 @0x0C = BAR0 offset of consumer index word
 *   len     u32 @0x10 = descriptor count
 * Split the 64-bit addr into lo/hi 32-bit MMIO stores (LE ABI, amd64 LE).
 */
static void
agnic_mgmt_publish_q(struct agnic_softc *sc, uint32_t q_off,
    const struct agnic_mgmt_ring *r)
{
	uint64_t addr = (uint64_t)r->mem.paddr;

	AGNIC_WR4(sc, AGNIC_BAR0, q_off + AGNIC_QHW_ADDR_OFF,
	    (uint32_t)(addr & 0xFFFFFFFFU));
	AGNIC_WR4(sc, AGNIC_BAR0, q_off + AGNIC_QHW_ADDR_OFF + 4,
	    (uint32_t)(addr >> 32));
	AGNIC_WR4(sc, AGNIC_BAR0, q_off + AGNIC_QHW_PROD_OFF, r->pub_prod_off);
	AGNIC_WR4(sc, AGNIC_BAR0, q_off + AGNIC_QHW_CONS_OFF, r->pub_cons_off);
	AGNIC_WR4(sc, AGNIC_BAR0, q_off + AGNIC_QHW_LEN_OFF, r->count);
}

/*
 * Latch the h2t mgmt doorbell (ctrl_map.h2t_dbell_msg[0] == RPC mgmt channel):
 * wait CTRL_FCLT_TRGT_H2T_DBELL, then read the 16-byte entry, retrying while its
 * address field is still zero (target has not populated that vec_id). Bounded.
 */
static void
agnic_mgmt_latch_dbell(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	uint32_t base, alo, ahi, data, cnt;
	uint64_t addr;
	int step = (hz >= 100) ? (hz / 100) : 1;
	int budget = (int)(((int64_t)AGNIC_H2T_DBELL_TIMEOUT_MS * hz) / 1000);
	int spent = 0;

	sc->h2t_dbell_valid = 0;

	if (agnic_poll(sc, sc->ctrl_bar,
	    sc->ctrl_off + AGNIC_CTRL_HANDSHAKE_OFF,
	    CTRL_FCLT_TRGT_H2T_DBELL, CTRL_FCLT_TRGT_H2T_DBELL,
	    AGNIC_H2T_DBELL_TIMEOUT_MS, "CTRL_FCLT_TRGT_H2T_DBELL") != 0)
		return;

	cnt = AGNIC_RD4(sc, sc->ctrl_bar,
	    sc->ctrl_off + AGNIC_CTRL_H2T_DBELL_CNT_OFF);
	if (cnt <= AGNIC_H2T_DBELL_MGMT) {
		device_printf(dev,
		    "P3a: h2t dbell count %u too small for mgmt idx %d\n",
		    cnt, AGNIC_H2T_DBELL_MGMT);
		return;
	}

	base = sc->ctrl_off + AGNIC_CTRL_H2T_DBELL_MSG(AGNIC_H2T_DBELL_MGMT);
	for (;;) {
		alo = AGNIC_RD4(sc, sc->ctrl_bar, base + AGNIC_DBELL_MSG_ADDR_OFF);
		ahi = AGNIC_RD4(sc, sc->ctrl_bar, base + AGNIC_DBELL_MSG_ADDR_OFF + 4);
		addr = ((uint64_t)ahi << 32) | alo;
		if (addr != 0)
			break;
		if (spent >= budget) {
			device_printf(dev,
			    "P3a: h2t mgmt dbell entry addr stayed 0 (timeout)\n");
			return;
		}
		pause("agnicdb", step);
		spent += step;
	}
	data = AGNIC_RD4(sc, sc->ctrl_bar, base + AGNIC_DBELL_MSG_DATA_OFF);

	/* address is a BAR4-relative target-register offset (low 24 bits). */
	sc->h2t_mgmt_bar4_off = (uint32_t)(addr & 0xFFFFFFU);
	sc->h2t_mgmt_data = data;
	sc->h2t_dbell_valid = 1;
	device_printf(dev,
	    "P3a: h2t mgmt doorbell latched: BAR4+0x%06x data 0x%08x\n",
	    sc->h2t_mgmt_bar4_off, sc->h2t_mgmt_data);
}

/* Next cmd_idx: rolls 1..1023, never 0 (ILLEGAL) or 0xFFFF (NOTIFICATION). */
static uint16_t
agnic_next_cmd_idx(struct agnic_softc *sc)
{
	uint16_t v = sc->cmd_idx_gen + 1;

	if (v >= AGNIC_CMD_COOKIE_COUNT)
		v = 1;
	sc->cmd_idx_gen = v;
	return (v);
}

/* ------------------------------------------------------------------------- */
/* One command round-trip: send + reap (single-descriptor path only).        */
/* ------------------------------------------------------------------------- */

/*
 * Fill one cmd descriptor at the cmd-ring producer slot, publish the producer
 * index (BAR0 MMIO), and ring the h2t BAR4 doorbell. Returns the allocated
 * cmd_idx via *out_idx. mgmt_lock must be held.
 *
 * flags selects AGNIC_DESC_FLAGS_SINGLE_RESP (want a response) or
 * AGNIC_DESC_FLAGS_SINGLE_NORESP (fire-and-forget). params/plen serialize up to
 * AGNIC_MGMT_DESC_DATA_LEN bytes into data[]; the whole descriptor is zeroed
 * first so any unused param tail is deterministic 0.
 */
static void
agnic_mgmt_post(struct agnic_softc *sc, uint16_t idx, uint8_t cmd_code,
    uint8_t flags, const void *params, size_t plen)
{
	struct agnic_mgmt_ring *r = &sc->cmd_ring;
	struct agnic_cmd_desc *d;

	d = (struct agnic_cmd_desc *)((uint8_t *)r->mem.vaddr +
	    (size_t)r->prod_shadow * AGNIC_MGMT_DESC_SIZE);
	bzero(d, sizeof(*d));
	d->cmd_idx = idx;
	d->app_code = AGNIC_AC_PF_MANAGER;
	d->cmd_code = cmd_code;
	d->client_id = 0;
	d->client_type = AGNIC_CDT_PF;
	d->flags = flags;
	if (params != NULL && plen > 0) {
		if (plen > (size_t)AGNIC_MGMT_DESC_DATA_LEN)
			plen = (size_t)AGNIC_MGMT_DESC_DATA_LEN;
		memcpy(d->data, params, plen);
	}
	/* Any remaining param tail stays zero from the bzero above. */

	/* Ensure the descriptor is visible in host DRAM before the doorbell. */
	bus_dmamap_sync(r->mem.tag, r->mem.map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	atomic_thread_fence_rel();

	/* Advance + publish the producer index (BAR0 index word). */
	r->prod_shadow = AGNIC_RING_INC(r->prod_shadow, r->count);
	AGNIC_WR4(sc, AGNIC_BAR0, r->prod_bar_off, r->prod_shadow);
	bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
	    BUS_SPACE_BARRIER_WRITE);

	/*
	 * Ring the h2t mgmt doorbell (first real BAR4 write). The mgmt path does
	 * not strictly require this -- the device polls the producer index -- but
	 * it is FLR-safe and covers a device that wants the MSI-X kick.
	 */
	if (sc->h2t_dbell_valid) {
		AGNIC_WR4(sc, AGNIC_BAR4, sc->h2t_mgmt_bar4_off,
		    sc->h2t_mgmt_data);
		bus_barrier(sc->bar[AGNIC_BAR4], 0, sc->bar_size[AGNIC_BAR4],
		    BUS_SPACE_BARRIER_WRITE);
	}
}

/*
 * Continuous notif-ring drainer (GPL agnic_mgmt_notif_handle model). Runs from a
 * poll callout with mgmt_poll_mtx held; consumes EVERY pending notif descriptor
 * -- dispatching a command response to the single armed waiter, silently
 * consuming async keep-alives (no ack needed) -- and advances the consumer index
 * so the device keeps posting. Without this the consumer stalls between commands
 * and the device stops responding (this is why CC_PF_ENABLE hung).
 */
static void
agnic_mgmt_drain(struct agnic_softc *sc)
{
	struct agnic_mgmt_ring *r = &sc->notif_ring;
	uint32_t prod, scanned = 0;

	prod = AGNIC_RD4(sc, AGNIC_BAR0, r->prod_bar_off);
	while (r->cons_shadow != prod && scanned < r->count) {
		struct agnic_cmd_desc *d;
		uint16_t rcmd;

		scanned++;
		bus_dmamap_sync(r->mem.tag, r->mem.map, BUS_DMASYNC_POSTREAD);
		d = (struct agnic_cmd_desc *)((uint8_t *)r->mem.vaddr +
		    (size_t)r->cons_shadow * AGNIC_MGMT_DESC_SIZE);
		rcmd = d->cmd_idx;

		if (rcmd == AGNIC_CMD_ID_NOTIFICATION) {
			/* Async notification: no ack needed. */
			if (d->cmd_code == AGNIC_NC_PF_LINK_CHANGE) {
				uint32_t ls = le32dec(&d->data[0]);

				sc->link_up = (ls != 0);
				sc->link_changes++;
				device_printf(sc->dev,
				    "[Phase 3b] NC_PF_LINK_CHANGE: link %s "
				    "(status 0x%x)\n",
				    sc->link_up ? "UP" : "DOWN", ls);
			} else {
				/* keep-alive (or unknown): just reset watchdog. */
				sc->mgmt_keepalive++;
			}
		} else if (sc->mgmt_wait_idx != 0 && rcmd == sc->mgmt_wait_idx &&
		    !sc->mgmt_wait_done) {
			sc->mgmt_wait_status = d->data[0];
			if (sc->mgmt_wait_buf != NULL && sc->mgmt_wait_len > 0) {
				size_t n = sc->mgmt_wait_len;

				if (n > (size_t)AGNIC_MGMT_DESC_DATA_LEN)
					n = (size_t)AGNIC_MGMT_DESC_DATA_LEN;
				memcpy(sc->mgmt_wait_buf, d->data, n);
			}
			sc->mgmt_wait_done = 1;
			wakeup(&sc->mgmt_wait_done);
		}
		/* else: stale/unmatched response -- consume silently. */

		r->cons_shadow = AGNIC_RING_INC(r->cons_shadow, r->count);
		AGNIC_WR4(sc, AGNIC_BAR0, r->cons_bar_off, r->cons_shadow);
	}
	if (scanned != 0)
		bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
		    BUS_SPACE_BARRIER_WRITE);
}

/* Poll callout: drain then reschedule (runs with mgmt_poll_mtx held). */
static void
agnic_mgmt_poll_cb(void *arg)
{
	struct agnic_softc *sc = arg;

	agnic_mgmt_drain(sc);
	callout_reset(&sc->mgmt_poll, 1, agnic_mgmt_poll_cb, sc);
}

/*
 * (legacy, now unused) Poll the notif ring for the response whose cmd_idx matches want_idx. Copies up
 * to AGNIC_MGMT_DESC_DATA_LEN response bytes into resp[] and returns the
 * response status byte via *out_status. Async notifications (cmd_idx==0xFFFF)
 * are logged and skipped. Bounded; returns 0 on match, ETIMEDOUT otherwise.
 * mgmt_lock must be held (it is an sx: this loop sleeps via pause()).
 */
static int
agnic_mgmt_reap(struct agnic_softc *sc, uint16_t want_idx,
    uint8_t *resp, size_t resp_len, uint8_t *out_status)
{
	struct agnic_mgmt_ring *r = &sc->notif_ring;
	device_t dev = sc->dev;
	int step = (hz >= 100) ? (hz / 100) : 1;
	int budget = (int)(((int64_t)AGNIC_MGMT_CMD_TIMEOUT_MS * hz) / 1000);
	int spent = 0;
	uint32_t prod, scanned;

	for (;;) {
		/* Device (producer) wrote its index word on BAR0; read it. */
		prod = AGNIC_RD4(sc, AGNIC_BAR0, r->prod_bar_off);

		/*
		 * Sanity bound (P3a TODO): a garbage producer index must never
		 * spin us forever. Consume at most r->count entries per pass --
		 * that is the whole ring -- then bail if prod still disagrees.
		 */
		scanned = 0;
		while (r->cons_shadow != prod && scanned < r->count) {
			struct agnic_cmd_desc *d;
			uint16_t rcmd;

			scanned++;

			bus_dmamap_sync(r->mem.tag, r->mem.map,
			    BUS_DMASYNC_POSTREAD);
			d = (struct agnic_cmd_desc *)((uint8_t *)r->mem.vaddr +
			    (size_t)r->cons_shadow * AGNIC_MGMT_DESC_SIZE);
			rcmd = d->cmd_idx;

			if (rcmd == AGNIC_CMD_ID_NOTIFICATION) {
				device_printf(dev,
				    "P3a: async notif (code 0x%02x) skipped\n",
				    d->cmd_code);
			} else if (rcmd == want_idx) {
				if (out_status != NULL)
					*out_status = d->data[0];
				if (resp != NULL && resp_len > 0) {
					size_t n = resp_len;

					if (n > (size_t)AGNIC_MGMT_DESC_DATA_LEN)
						n = (size_t)AGNIC_MGMT_DESC_DATA_LEN;
					memcpy(resp, d->data, n);
				}
				/* Consume this slot, then return success. */
				r->cons_shadow = AGNIC_RING_INC(r->cons_shadow,
				    r->count);
				AGNIC_WR4(sc, AGNIC_BAR0, r->cons_bar_off,
				    r->cons_shadow);
				bus_barrier(sc->bar[AGNIC_BAR0], 0,
				    sc->bar_size[AGNIC_BAR0],
				    BUS_SPACE_BARRIER_WRITE);
				return (0);
			} else {
				device_printf(dev,
				    "P3a: unexpected notif cmd_idx 0x%04x "
				    "(want 0x%04x)\n", rcmd, want_idx);
			}

			/* Consume and continue scanning. */
			r->cons_shadow = AGNIC_RING_INC(r->cons_shadow, r->count);
			AGNIC_WR4(sc, AGNIC_BAR0, r->cons_bar_off,
			    r->cons_shadow);
			bus_barrier(sc->bar[AGNIC_BAR0], 0,
			    sc->bar_size[AGNIC_BAR0], BUS_SPACE_BARRIER_WRITE);
		}

		/*
		 * If we walked the entire ring without reaching the reported
		 * producer, the producer index word is bogus (device wedge /
		 * torn MMIO). Do not loop again on the same poisoned value.
		 */
		if (scanned >= r->count && r->cons_shadow != prod) {
			device_printf(dev,
			    "P3a: notif producer 0x%x unreachable after scanning "
			    "%u descriptors (cons 0x%x); aborting reap\n",
			    prod, scanned, r->cons_shadow);
			return (EIO);
		}

		if (spent >= budget) {
			device_printf(dev,
			    "P3a: timeout (%d ms) waiting for response to "
			    "cmd_idx 0x%04x\n", AGNIC_MGMT_CMD_TIMEOUT_MS,
			    want_idx);
			return (ETIMEDOUT);
		}
		pause("agnicrp", step);
		spent += step;
	}
}

/* ------------------------------------------------------------------------- */
/* P3a bring-up entry point.                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Called from the deferred config hook AFTER GIU DEV_READY. Allocates the two
 * mgmt rings, publishes them, completes the HOST_MGMT_READY/DEV_MGMT_READY
 * handshake, latches the h2t mgmt doorbell, and round-trips ONE echo command.
 * Returns 0 on full success; on any bounded timeout logs + returns an error,
 * leaving already-allocated rings in place (freed at detach) and P2b intact.
 */
int
agnic_mgmt_bringup(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	bus_size_t ring_bytes;
	uint32_t idx_base, arr_bytes;
	uint8_t status, resp[AGNIC_MGMT_DESC_DATA_LEN];
	int error;

	if (!sc->mgmt_inited) {
		sx_init(&sc->mgmt_lock, "agnic_mgmt");
		sc->mgmt_inited = 1;
	}

	if (sc->giu_bar != AGNIC_BAR0) {
		device_printf(dev,
		    "P3a: GIU config_mem not on BAR0 (bar %d); abort\n",
		    sc->giu_bar);
		return (EINVAL);
	}

	/* Read where the device placed the ring_indices_arr on BAR0. */
	sc->dev_use_size = AGNIC_RD4(sc, AGNIC_BAR0,
	    sc->giu_off + AGNIC_GIU_DEV_USE_SIZE_OFF);
	arr_bytes = AGNIC_MGMT_IDX_SLOTS * (uint32_t)sizeof(uint32_t);
	if (sc->dev_use_size == 0 ||
	    sc->dev_use_size + arr_bytes > AGNIC_CONFIG_BAR_SIZE) {
		device_printf(dev,
		    "P3a: bad dev_use_size 0x%x (need +%u <= 0x%x); abort\n",
		    sc->dev_use_size, arr_bytes, AGNIC_CONFIG_BAR_SIZE);
		return (ERANGE);
	}

	/* --- Allocate the two coherent descriptor rings. --- */
	ring_bytes = (bus_size_t)AGNIC_CMD_Q_LEN * AGNIC_MGMT_DESC_SIZE;
	error = agnic_dma_alloc(sc, &sc->cmd_ring.mem, ring_bytes, "cmd-ring");
	if (error != 0)
		return (error);
	ring_bytes = (bus_size_t)AGNIC_NOTIF_Q_LEN * AGNIC_MGMT_DESC_SIZE;
	error = agnic_dma_alloc(sc, &sc->notif_ring.mem, ring_bytes,
	    "notif-ring");
	if (error != 0) {
		agnic_dma_free(&sc->cmd_ring.mem);
		return (error);
	}

	/* --- Derive index-word offsets (BAR0) and published offsets. --- */
	sc->cmd_ring.count = AGNIC_CMD_Q_LEN;
	sc->cmd_ring.prod_shadow = 0;
	sc->cmd_ring.cons_shadow = 0;
	sc->notif_ring.count = AGNIC_NOTIF_Q_LEN;
	sc->notif_ring.prod_shadow = 0;
	sc->notif_ring.cons_shadow = 0;

	/* BAR0 MMIO offset of the config_mem base + index array. */
	idx_base = sc->giu_off + sc->dev_use_size;

	sc->cmd_ring.prod_bar_off =
	    idx_base + AGNIC_MGMT_SLOT_CMD_PROD * sizeof(uint32_t);
	sc->cmd_ring.cons_bar_off =
	    idx_base + AGNIC_MGMT_SLOT_CMD_CONS * sizeof(uint32_t);
	sc->notif_ring.prod_bar_off =
	    idx_base + AGNIC_MGMT_SLOT_NOTIF_PROD * sizeof(uint32_t);
	sc->notif_ring.cons_bar_off =
	    idx_base + AGNIC_MGMT_SLOT_NOTIF_CONS * sizeof(uint32_t);

	/*
	 * Published q_prod_offs/q_cons_offs are relative to the config_mem base
	 * (== ring_indices_arr_phys + slot*4 == dev_use_size + slot*4). With GIU
	 * at BAR0:0 (giu_off == 0) these equal the MMIO offsets above.
	 */
	sc->cmd_ring.pub_prod_off =
	    sc->dev_use_size + AGNIC_MGMT_SLOT_CMD_PROD * sizeof(uint32_t);
	sc->cmd_ring.pub_cons_off =
	    sc->dev_use_size + AGNIC_MGMT_SLOT_CMD_CONS * sizeof(uint32_t);
	sc->notif_ring.pub_prod_off =
	    sc->dev_use_size + AGNIC_MGMT_SLOT_NOTIF_PROD * sizeof(uint32_t);
	sc->notif_ring.pub_cons_off =
	    sc->dev_use_size + AGNIC_MGMT_SLOT_NOTIF_CONS * sizeof(uint32_t);

	/* Zero our four index words on BAR0 before advertising the queues. */
	AGNIC_WR4(sc, AGNIC_BAR0, sc->cmd_ring.prod_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->cmd_ring.cons_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->notif_ring.prod_bar_off, 0);
	AGNIC_WR4(sc, AGNIC_BAR0, sc->notif_ring.cons_bar_off, 0);

	/* --- Publish both q_hw_info blocks into config_mem. --- */
	agnic_mgmt_publish_q(sc, sc->giu_off + AGNIC_GIU_CMD_Q_OFF,
	    &sc->cmd_ring);
	agnic_mgmt_publish_q(sc, sc->giu_off + AGNIC_GIU_NOTIF_Q_OFF,
	    &sc->notif_ring);

	device_printf(dev,
	    "P3a: mgmt rings published (cmd dma 0x%jx, notif dma 0x%jx, "
	    "idx@BAR0+0x%x len %d)\n",
	    (uintmax_t)sc->cmd_ring.mem.paddr,
	    (uintmax_t)sc->notif_ring.mem.paddr,
	    idx_base, AGNIC_CMD_Q_LEN);

	/* Ensure the config_mem writes land before we flip HOST_MGMT_READY. */
	bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
	    BUS_SPACE_BARRIER_WRITE);

	/* --- Set HOST_MGMT_READY (RMW, preserve DEV_READY). --- */
	{
		uint32_t st = AGNIC_RD4(sc, AGNIC_BAR0,
		    sc->giu_off + AGNIC_GIU_STATUS_OFF);

		AGNIC_WR4(sc, AGNIC_BAR0, sc->giu_off + AGNIC_GIU_STATUS_OFF,
		    st | AGNIC_CFG_STATUS_HOST_MGMT_READY);
		bus_barrier(sc->bar[AGNIC_BAR0], 0, sc->bar_size[AGNIC_BAR0],
		    BUS_SPACE_BARRIER_WRITE);
	}
	device_printf(dev, "P3a: HOST_MGMT_READY set; waiting DEV_MGMT_READY\n");

	/* --- Wait DEV_MGMT_READY (bounded). --- */
	if (agnic_poll(sc, AGNIC_BAR0, sc->giu_off + AGNIC_GIU_STATUS_OFF,
	    AGNIC_CFG_STATUS_DEV_MGMT_READY, AGNIC_CFG_STATUS_DEV_MGMT_READY,
	    AGNIC_DEV_MGMT_READY_TIMEOUT_MS, "GIU DEV_MGMT_READY") != 0)
		return (ETIMEDOUT);
	device_printf(dev, "P3a: DEV_MGMT_READY; mgmt rings live\n");

	/* --- Start the continuous notif drainer BEFORE any command. --- */
	if (!sc->mgmt_poll_inited) {
		mtx_init(&sc->mgmt_poll_mtx, "agnic_mpoll", NULL, MTX_DEF);
		callout_init_mtx(&sc->mgmt_poll, &sc->mgmt_poll_mtx, 0);
		sc->mgmt_poll_inited = 1;
	}
	mtx_lock(&sc->mgmt_poll_mtx);
	callout_reset(&sc->mgmt_poll, 1, agnic_mgmt_poll_cb, sc);
	mtx_unlock(&sc->mgmt_poll_mtx);

	/* --- Latch the h2t mgmt doorbell for the BAR4 kick. --- */
	agnic_mgmt_latch_dbell(sc);

	/* --- Round-trip ONE safe command: CC_PF_MGMT_ECHO (via drainer). --- */
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_PF_MGMT_ECHO, NULL, 0,
	    resp, sizeof(resp), &status);

	if (error != 0) {
		device_printf(dev, "P3a: ECHO round-trip FAILED: %d\n", error);
		return (error);
	}
	if (status != AGNIC_NOTIF_STATUS_OK) {
		device_printf(dev,
		    "P3a: ECHO returned non-OK status 0x%02x\n", status);
		return (EIO);
	}

	sc->mgmt_ready = 1;
	device_printf(dev,
	    "[Phase 3a] mgmt ECHO round-trip OK (status OK); "
	    "mgmt command path is live\n");

	/*
	 * --- P3b step 1: CC_GET_CAPABILITIES (read-only) --- resp layout:
	 *   data[0] status, data[1..4] flags, data[5..8] max_buf_size,
	 *   data[9] egress_num_dma_engines (pack(1): unaligned; decode via
	 *   le32dec on the byte pointer). Non-fatal: default max_buf_size on
	 *   any failure so the datapath can still size its clusters.
	 */
	status = 0xFF;
	error = agnic_mgmt_cmd(sc, AGNIC_CC_GET_CAPABILITIES, NULL, 0,
	    resp, sizeof(resp), &status);

	if (error == 0 && status == AGNIC_NOTIF_STATUS_OK) {
		uint32_t caps_flags = le32dec(&resp[1]);
		uint32_t max_buf = le32dec(&resp[5]);
		uint8_t ndma = resp[9];

		sc->max_buf_size = max_buf;
		device_printf(dev,
		    "[Phase 3b] capabilities: flags 0x%08x%s max_buf_size %u "
		    "egress_dma_engines %u\n", caps_flags,
		    (caps_flags & AGNIC_CAPABILITIES_SG) ? " [SG]" : "",
		    max_buf, ndma);
	} else {
		sc->max_buf_size = MCLBYTES;
		device_printf(dev,
		    "[Phase 3b] CC_GET_CAPABILITIES failed (err %d status 0x%02x); "
		    "defaulting max_buf_size to %u\n", error, status,
		    sc->max_buf_size);
	}

	return (0);
}

/* ------------------------------------------------------------------------- */
/* P3b public command API + parameter serializers.                           */
/* ------------------------------------------------------------------------- */

/*
 * Generic single-descriptor command round-trip. Acquires mgmt_lock, sends the
 * command (with optional serialized params), reaps the matching response, and
 * returns its status byte via *out_status. Safe from any sleepable context
 * (if_init / config hook); NEVER call under a spin mutex -- the reap sleeps.
 */
int
agnic_mgmt_cmd(struct agnic_softc *sc, uint8_t cmd_code, const void *params,
    size_t plen, uint8_t *resp, size_t resp_len, uint8_t *out_status)
{
	uint16_t idx;
	int timo, deadline, done;

	if (!sc->mgmt_inited || !sc->mgmt_poll_inited) {
		if (out_status != NULL)
			*out_status = 0xFF;
		return (ENXIO);
	}

	sx_xlock(&sc->mgmt_lock);		/* serialize senders */
	mtx_lock(&sc->mgmt_poll_mtx);		/* sync with the drainer */

	idx = agnic_next_cmd_idx(sc);
	sc->mgmt_wait_idx = idx;
	sc->mgmt_wait_done = 0;
	sc->mgmt_wait_status = 0xFF;
	sc->mgmt_wait_buf = resp;
	sc->mgmt_wait_len = resp_len;

	/*
	 * Arm the waiter BEFORE publishing the producer index. The drainer runs
	 * under mgmt_poll_mtx (which we hold until msleep drops it), so it can
	 * never observe the response before the waiter is armed.
	 */
	agnic_mgmt_post(sc, idx, cmd_code, AGNIC_DESC_FLAGS_SINGLE_RESP,
	    params, plen);

	timo = (int)(((int64_t)AGNIC_MGMT_CMD_TIMEOUT_MS * hz) / 1000);
	if (timo < 1)
		timo = 1;
	deadline = ticks + timo;
	while (!sc->mgmt_wait_done) {
		int remain = deadline - ticks;

		if (remain <= 0)
			break;
		(void)msleep(&sc->mgmt_wait_done, &sc->mgmt_poll_mtx, PZERO,
		    "agnicmc", remain);
	}

	done = sc->mgmt_wait_done;
	if (out_status != NULL)
		*out_status = done ? sc->mgmt_wait_status : 0xFF;
	sc->mgmt_wait_idx = 0;
	sc->mgmt_wait_buf = NULL;
	mtx_unlock(&sc->mgmt_poll_mtx);
	sx_xunlock(&sc->mgmt_lock);

	if (!done) {
		device_printf(sc->dev,
		    "P3b: timeout waiting for response to cmd_idx 0x%04x "
		    "(code 0x%02x)\n", idx, cmd_code);
		return (ETIMEDOUT);
	}
	return (0);
}

/*
 * Fire-and-forget command (NO_RESP). Used by CC_PF_DISABLE at teardown: we
 * publish the command and ring the doorbell but do not wait for a response
 * (the device may already be tearing the queues down). Still FLR-free.
 */
void
agnic_mgmt_cmd_noresp(struct agnic_softc *sc, uint8_t cmd_code,
    const void *params, size_t plen)
{
	uint16_t idx;

	if (!sc->mgmt_inited || !sc->mgmt_poll_inited)
		return;

	sx_xlock(&sc->mgmt_lock);
	mtx_lock(&sc->mgmt_poll_mtx);
	idx = agnic_next_cmd_idx(sc);
	agnic_mgmt_post(sc, idx, cmd_code, AGNIC_DESC_FLAGS_SINGLE_NORESP,
	    params, plen);
	mtx_unlock(&sc->mgmt_poll_mtx);
	sx_xunlock(&sc->mgmt_lock);
}

/* CC_PF_INIT (0x1) params: 13 meaningful bytes, 48 on the wire. */
void
agnic_pf_init_params(uint8_t *buf, uint32_t num_egress_tc,
    uint32_t num_ingress_tc, uint16_t mtu_override, uint16_t mru_override,
    uint8_t egress_sched)
{

	le32enc(&buf[0], num_egress_tc);
	le32enc(&buf[4], num_ingress_tc);
	le16enc(&buf[8], mtu_override);
	le16enc(&buf[10], mru_override);
	buf[12] = egress_sched;
}

/* CC_PF_INGRESS_TC_ADD (0x5) params: 13 meaningful bytes. */
void
agnic_ingress_tc_add_params(uint8_t *buf, uint32_t tc, uint32_t num_queues,
    uint32_t pkt_offset, uint8_t hash_type)
{

	le32enc(&buf[0], tc);
	le32enc(&buf[4], num_queues);
	le32enc(&buf[8], pkt_offset);
	buf[12] = hash_type;
}

/*
 * CC_PF_INGRESS_DATA_Q_ADD (0x6) params: exactly 48 bytes. Field ORDER is
 * fixed by the ABI -- the bpool block sits BETWEEN the data-q offsets and
 * q_len/msix_id/tc/q_buf_size. Do not reorder.
 */
void
agnic_ingress_data_q_add_params(uint8_t *buf,
    const struct agnic_ingress_q_cfg *c)
{

	le64enc(&buf[0], c->q_phys);
	le32enc(&buf[8], c->q_prod_offs);
	le32enc(&buf[12], c->q_cons_offs);
	le64enc(&buf[16], c->bpool_phys);
	le32enc(&buf[24], c->bpool_prod_offs);
	le32enc(&buf[28], c->bpool_cons_offs);
	le32enc(&buf[32], c->q_len);
	le32enc(&buf[36], c->msix_id);
	le32enc(&buf[40], c->tc);
	le32enc(&buf[44], c->q_buf_size);
}

/* CC_PF_EGRESS_TC_ADD (0x3) params: 12 meaningful bytes. */
void
agnic_egress_tc_add_params(uint8_t *buf, uint32_t tc, uint32_t num_queues,
    uint32_t num_queues_per_dma)
{

	le32enc(&buf[0], tc);
	le32enc(&buf[4], num_queues);
	le32enc(&buf[8], num_queues_per_dma);
}

/*
 * CC_PF_EGRESS_DATA_Q_ADD (0x4) params: 32 meaningful bytes. Field ORDER is
 * fixed by the ABI (pf_egress_q_add): q_phys, q_prod, q_cons, q_len,
 * q_wrr_weight, tc, msix_id -- q_len sits BEFORE wrr/tc/msix, unlike the
 * ingress q_add. Do not reorder.
 */
void
agnic_egress_data_q_add_params(uint8_t *buf,
    const struct agnic_egress_q_cfg *c)
{

	le64enc(&buf[0], c->q_phys);
	le32enc(&buf[8], c->q_prod_offs);
	le32enc(&buf[12], c->q_cons_offs);
	le32enc(&buf[16], c->q_len);
	le32enc(&buf[20], c->q_wrr_weight);
	le32enc(&buf[24], c->tc);
	le32enc(&buf[28], c->msix_id);
}

/* Free the mgmt rings + lock. Idempotent; safe whether or not bring-up ran. */
void
agnic_mgmt_teardown(struct agnic_softc *sc)
{

	/* Stop the notif drainer BEFORE freeing the rings it reads. */
	if (sc->mgmt_poll_inited) {
		callout_drain(&sc->mgmt_poll);
		mtx_destroy(&sc->mgmt_poll_mtx);
		sc->mgmt_poll_inited = 0;
	}

	agnic_dma_free(&sc->notif_ring.mem);
	agnic_dma_free(&sc->cmd_ring.mem);
	sc->mgmt_ready = 0;

	if (sc->mgmt_inited) {
		sx_destroy(&sc->mgmt_lock);
		sc->mgmt_inited = 0;
	}
}
