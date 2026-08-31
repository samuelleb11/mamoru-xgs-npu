// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * mamoru-agnic P3b/P4: the GIU datapath — LIF configuration + descriptor rings.
 *
 * This file currently implements P3b: allocate the RX / bpool / TX descriptor
 * rings + BM buffer pool, run the firmware LIF-config sequence over the proven
 * mgmt command path (CC_PF_INIT -> INGRESS_TC_ADD -> INGRESS_DATA_Q_ADD ->
 * EGRESS_TC_ADD -> EGRESS_DATA_Q_ADD -> INIT_DONE), publish the bpool producer,
 * then CC_PF_ENABLE. The egress TC + data-q are registered even though the TX
 * datapath itself lands in P4, because the NPU refuses CC_PF_ENABLE for an
 * RX-only config. RX/TX reap + per-port netdevs (P4/P5) build on this.
 *
 * Clean-room Linux port of the proven BSD-2 FreeBSD agnic_txrx.c: the descriptor
 * layouts, the CC_PF param serialization, the BAR0 ring-index mechanism and the
 * bring-up order are interface FACTS; logic is rewritten for Linux (skb /
 * dma_alloc_coherent / dma_map_single). No NPU reset/FLR anywhere.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/unaligned.h>
#include "agnic.h"

/* Poll interval for the reliable (non-MSI-X) RX reaper. */
#define AGNIC_RX_POLL_MS	10

/* ---- CC_PF_* param serializers (pure LE byte packing into a 48-byte buffer) -- */

static void pf_init_params(u8 *b, u32 num_egress_tc, u32 num_ingress_tc,
			   u16 mtu, u16 mru, u8 egress_sched)
{
	put_unaligned_le32(num_egress_tc, &b[0]);
	put_unaligned_le32(num_ingress_tc, &b[4]);
	put_unaligned_le16(mtu, &b[8]);
	put_unaligned_le16(mru, &b[10]);
	b[12] = egress_sched;
}

static void ingress_tc_add_params(u8 *b, u32 tc, u32 num_queues, u32 pkt_offset, u8 hash_type)
{
	put_unaligned_le32(tc, &b[0]);
	put_unaligned_le32(num_queues, &b[4]);
	put_unaligned_le32(pkt_offset, &b[8]);
	b[12] = hash_type;
}

/* Ingress data-q ABI order: q_phys, q_prod, q_cons, bpool_phys, bpool_prod,
 * bpool_cons, q_len, msix, tc, q_buf_size (the bpool block sits between the
 * q-offsets and q_len — do NOT reorder). */
static void ingress_data_q_add_params(u8 *b, u64 q_phys, u32 q_prod, u32 q_cons,
				      u64 bpool_phys, u32 bp_prod, u32 bp_cons,
				      u32 q_len, u32 msix_id, u32 tc, u32 q_buf_size)
{
	put_unaligned_le64(q_phys, &b[0]);
	put_unaligned_le32(q_prod, &b[8]);
	put_unaligned_le32(q_cons, &b[12]);
	put_unaligned_le64(bpool_phys, &b[16]);
	put_unaligned_le32(bp_prod, &b[24]);
	put_unaligned_le32(bp_cons, &b[28]);
	put_unaligned_le32(q_len, &b[32]);
	put_unaligned_le32(msix_id, &b[36]);
	put_unaligned_le32(tc, &b[40]);
	put_unaligned_le32(q_buf_size, &b[44]);
}

static void egress_tc_add_params(u8 *b, u32 tc, u32 num_queues, u32 num_queues_per_dma)
{
	put_unaligned_le32(tc, &b[0]);
	put_unaligned_le32(num_queues, &b[4]);
	put_unaligned_le32(num_queues_per_dma, &b[8]);
}

/* Egress data-q ABI order: q_phys, q_prod, q_cons, q_len, q_wrr_weight, tc, msix
 * (NO bpool block; q_len precedes wrr/tc/msix — DIFFERS from ingress). */
static void egress_data_q_add_params(u8 *b, u64 q_phys, u32 q_prod, u32 q_cons,
				     u32 q_len, u32 wrr, u32 tc, u32 msix_id)
{
	put_unaligned_le64(q_phys, &b[0]);
	put_unaligned_le32(q_prod, &b[8]);
	put_unaligned_le32(q_cons, &b[12]);
	put_unaligned_le32(q_len, &b[16]);
	put_unaligned_le32(wrr, &b[20]);
	put_unaligned_le32(tc, &b[24]);
	put_unaligned_le32(msix_id, &b[28]);
}

/* ---- ring allocation + BM pool ---------------------------------------------- */

/* Compute a ring's BAR0 index-word offsets. prod/cons slots index the ring-index
 * array the device placed on BAR0 at giu_off + dev_use_size. The *_bar_off are
 * absolute BAR0 MMIO offsets (host readl/writel); the pub_*_off are config_mem-
 * relative and are what we advertise to the device in the DATA_Q_ADD params. */
static void ring_set_idx(struct agnic *ag, struct agnic_data_ring *r,
			 u32 giu_off, u32 prod_slot, u32 cons_slot)
{
	u32 idx_base = giu_off + ag->dev_use_size;

	r->prod_bar_off = idx_base + prod_slot * sizeof(u32);
	r->cons_bar_off = idx_base + cons_slot * sizeof(u32);
	r->pub_prod_off = ag->dev_use_size + prod_slot * sizeof(u32);
	r->pub_cons_off = ag->dev_use_size + cons_slot * sizeof(u32);
	r->prod_shadow = 0;
	r->cons_shadow = 0;
}

static int agnic_ring_alloc(struct agnic *ag, struct agnic_data_ring *r, u32 count, u32 desc_size)
{
	r->count = count;
	r->desc_size = desc_size;
	return agnic_dma_alloc(ag, &r->mem, (size_t)count * desc_size);
}

static void agnic_bp_unfill(struct agnic *ag)
{
	u32 i;

	if (!ag->rxb)
		return;
	for (i = 0; i < ag->bp_ring.count; i++) {
		if (ag->rxb[i].skb) {
			dma_unmap_single(ag->dev, ag->rxb[i].dma, AGNIC_RX_CLSIZE, DMA_FROM_DEVICE);
			dev_kfree_skb(ag->rxb[i].skb);
			ag->rxb[i].skb = NULL;
		}
	}
}

static void agnic_txrx_free_rings(struct agnic *ag)
{
	agnic_bp_unfill(ag);
	kfree(ag->rxb);
	ag->rxb = NULL;
	agnic_dma_free(ag, &ag->tx_bufs);
	agnic_dma_free(ag, &ag->tx_ring.mem);
	agnic_dma_free(ag, &ag->bp_ring.mem);
	agnic_dma_free(ag, &ag->rx_ring.mem);
}

/* Post count-1 free RX buffers into the bpool ring (leave one slot free so the
 * producer sits one behind the consumer). Sets prod_shadow; the producer index
 * is NOT published to the device here — that happens after INIT_DONE. */
static int agnic_bp_fill(struct agnic *ag)
{
	struct agnic_bpool_desc *bd = ag->bp_ring.mem.va;
	u32 i, n = ag->bp_ring.count - 1;

	for (i = 0; i < n; i++) {
		struct sk_buff *skb = alloc_skb(AGNIC_RX_CLSIZE, GFP_KERNEL);
		dma_addr_t dma;

		if (!skb)
			return -ENOMEM;
		dma = dma_map_single(ag->dev, skb->data, AGNIC_RX_CLSIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(ag->dev, dma)) {
			dev_kfree_skb(skb);
			return -ENOMEM;
		}
		ag->rxb[i].skb = skb;
		ag->rxb[i].dma = dma;
		bd[i].buff_addr_phys = cpu_to_le64(dma + ag->host_headroom);
		bd[i].buff_cookie = cpu_to_le64(i);
	}
	ag->bp_ring.prod_shadow = n;
	ag->bp_ring.cons_shadow = 0;
	return 0;
}

static int agnic_txrx_alloc(struct agnic *ag, u32 giu_off)
{
	int ret;

	ret = agnic_ring_alloc(ag, &ag->rx_ring, AGNIC_RX_RING_LEN, AGNIC_RXD_SIZE);
	if (ret)
		return ret;
	ret = agnic_ring_alloc(ag, &ag->bp_ring, AGNIC_BP_RING_LEN, AGNIC_BPD_SIZE);
	if (ret)
		goto err;
	ret = agnic_ring_alloc(ag, &ag->tx_ring, AGNIC_TX_RING_LEN, AGNIC_TXD_SIZE);
	if (ret)
		goto err;
	ret = agnic_dma_alloc(ag, &ag->tx_bufs, (size_t)AGNIC_TX_RING_LEN * AGNIC_RX_CLSIZE);
	if (ret)
		goto err;

	ag->rxb = kcalloc(AGNIC_BP_RING_LEN, sizeof(*ag->rxb), GFP_KERNEL);
	if (!ag->rxb) {
		ret = -ENOMEM;
		goto err;
	}

	/* Slot assignment: host WRITES rx-cons/bp-prod/tx-prod; device WRITES the rest. */
	ring_set_idx(ag, &ag->rx_ring, giu_off, AGNIC_DATA_SLOT_RX_PROD, AGNIC_DATA_SLOT_RX_CONS);
	ring_set_idx(ag, &ag->bp_ring, giu_off, AGNIC_DATA_SLOT_BP_PROD, AGNIC_DATA_SLOT_BP_CONS);
	ring_set_idx(ag, &ag->tx_ring, giu_off, AGNIC_DATA_SLOT_TX_PROD, AGNIC_DATA_SLOT_TX_CONS);

	/* Zero all six datapath index words before advertising any queue. */
	agnic_wr(ag, AGNIC_BAR_GIU, ag->rx_ring.prod_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->rx_ring.cons_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->bp_ring.prod_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->bp_ring.cons_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->tx_ring.prod_bar_off, 0);
	agnic_wr(ag, AGNIC_BAR_GIU, ag->tx_ring.cons_bar_off, 0);
	wmb();

	ret = agnic_bp_fill(ag);
	if (ret)
		goto err;
	return 0;
err:
	agnic_txrx_free_rings(ag);
	return ret;
}

/* ---- the CC_PF_* LIF-config sequence ---------------------------------------- */

/* Send one CC_PF_* command. params==NULL => no-param command (plen 0); otherwise
 * always send the full 48-byte param area (tail is deterministic 0). */
static int pf_cmd(struct agnic *ag, u8 code, const u8 *params, const char *name)
{
	u8 resp[AGNIC_MGMT_DESC_DATA_LEN];
	u8 status = 0xff;
	int ret;

	ret = agnic_mgmt_cmd(ag, code, params, params ? AGNIC_MGMT_PARAMS_LEN : 0,
			     resp, sizeof(resp), &status);
	if (ret || status != AGNIC_NOTIF_STATUS_OK) {
		dev_err(ag->dev, "P3b: %s failed (err %d status 0x%02x)\n", name, ret, status);
		return ret ? ret : -EIO;
	}
	dev_info(ag->dev, "P3b: %s OK\n", name);
	return 0;
}

static int agnic_txrx_config_queues(struct agnic *ag)
{
	u8 p[AGNIC_MGMT_PARAMS_LEN];
	int ret;

	/* (1) CC_PF_INIT — 1 egress TC, 1 ingress TC, frame size incl. 66B prefix. */
	memset(p, 0, sizeof(p));
	pf_init_params(p, AGNIC_EGRESS_TCS, AGNIC_INGRESS_TCS,
		       AGNIC_RX_FRAME_SIZE, AGNIC_RX_FRAME_SIZE, AGNIC_ES_STRICT_SCHED);
	ret = pf_cmd(ag, AGNIC_CC_PF_INIT, p, "CC_PF_INIT");
	if (ret)
		return ret;

	/* (2) CC_PF_INGRESS_TC_ADD — tc 0, 1 queue, pkt_offset 0, no hash. */
	memset(p, 0, sizeof(p));
	ingress_tc_add_params(p, 0, AGNIC_QS_PER_TC, AGNIC_INGRESS_PKT_OFFSET,
			      AGNIC_ING_HASH_TYPE_NONE);
	ret = pf_cmd(ag, AGNIC_CC_PF_INGRESS_TC_ADD, p, "INGRESS_TC_ADD");
	if (ret)
		return ret;

	/* (3) CC_PF_INGRESS_DATA_Q_ADD — RX ring + bpool. msix_id = AGNIC_RX_DBELL_ID:
	 * agnic_msix_setup() has already allocated the PCI MSI-X vectors + armed the
	 * doorbell ISRs, so the device fires a real interrupt on RX (the ISR kicks the
	 * reaper). If MSI-X could not be set up, ag->msix_nvec==0 and we pass 0 (no RX
	 * irq) so the device does not DMA an interrupt to a masked/zero vector (which
	 * would fault) — the 10ms poll then drives RX. */
	memset(p, 0, sizeof(p));
	ingress_data_q_add_params(p,
		(u64)ag->rx_ring.mem.da, ag->rx_ring.pub_prod_off, ag->rx_ring.pub_cons_off,
		(u64)ag->bp_ring.mem.da, ag->bp_ring.pub_prod_off, ag->bp_ring.pub_cons_off,
		ag->rx_ring.count,
		ag->msix_nvec > AGNIC_RX_DBELL_ID ? AGNIC_RX_DBELL_ID : 0 /* msix_id */,
		0, AGNIC_RX_CLSIZE);
	ret = pf_cmd(ag, AGNIC_CC_PF_INGRESS_DATA_Q_ADD, p, "INGRESS_DATA_Q_ADD");
	if (ret)
		return ret;

	/* (4) CC_PF_EGRESS_TC_ADD — tc 0, 1 queue, 1 queue/dma (1 dma engine). */
	memset(p, 0, sizeof(p));
	egress_tc_add_params(p, 0, AGNIC_QS_PER_TC, AGNIC_QS_PER_TC);
	ret = pf_cmd(ag, AGNIC_CC_PF_EGRESS_TC_ADD, p, "EGRESS_TC_ADD");
	if (ret)
		return ret;

	/* (5) CC_PF_EGRESS_DATA_Q_ADD — TX ring (required even for RX-only). */
	memset(p, 0, sizeof(p));
	egress_data_q_add_params(p,
		(u64)ag->tx_ring.mem.da, ag->tx_ring.pub_prod_off, ag->tx_ring.pub_cons_off,
		ag->tx_ring.count, 0, 0, 0);
	ret = pf_cmd(ag, AGNIC_CC_PF_EGRESS_DATA_Q_ADD, p, "EGRESS_DATA_Q_ADD");
	if (ret)
		return ret;

	/* (6) CC_PF_INIT_DONE — no params. */
	ret = pf_cmd(ag, AGNIC_CC_PF_INIT_DONE, NULL, "INIT_DONE");
	if (ret)
		return ret;

	/* Publish the bpool producer: the device now sees the free buffers (it does
	 * not consume them until CC_PF_ENABLE). RX/TX prod/cons words stay 0. */
	dma_wmb();
	agnic_wr(ag, AGNIC_BAR_GIU, ag->bp_ring.prod_bar_off, ag->bp_ring.prod_shadow);
	wmb();
	dev_info(ag->dev, "P3b: bpool published (%u free buffers)\n", ag->bp_ring.prod_shadow);
	return 0;
}

/* ---- RX reap + bpool refill + TX enqueue (P4) ------------------------------- */

/* Top the bpool up toward FULL every call (self-healing): re-post a fresh buffer
 * at each reclaimed slot from prod_shadow up to one behind the device consumer
 * (BP_CONS), stopping at the first still-device-owned slot (rb->skb != NULL) or an
 * allocation failure. Driving toward full — rather than only replacing the n
 * consumed this tick — means a transient GFP_ATOMIC/DMA failure that under-posts is
 * simply made up on a later tick, instead of permanently shrinking the pool and
 * latching RX off (a device that runs out of buffers posts no completions, so an
 * n-gated refill would never run again). Called with rx_lock held. */
static void agnic_bp_refill(struct agnic *ag)
{
	struct agnic_bpool_desc *bd = ag->bp_ring.mem.va;
	u32 count = ag->bp_ring.count;
	u32 cons = agnic_rd(ag, AGNIC_BAR_GIU, ag->bp_ring.cons_bar_off) & (count - 1);
	u32 posted = 0;

	while (AGNIC_RING_INC(ag->bp_ring.prod_shadow, count) != cons) {
		u32 pos = ag->bp_ring.prod_shadow;
		struct agnic_rxbuf *rb = &ag->rxb[pos];
		struct sk_buff *skb;
		dma_addr_t dma;

		if (rb->skb)		/* slot still holds a device-owned buffer */
			break;
		skb = alloc_skb(AGNIC_RX_CLSIZE, GFP_ATOMIC);
		if (!skb)
			break;
		dma = dma_map_single(ag->dev, skb->data, AGNIC_RX_CLSIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(ag->dev, dma)) {
			dev_kfree_skb_any(skb);
			break;
		}
		rb->skb = skb;
		rb->dma = dma;
		bd[pos].buff_addr_phys = cpu_to_le64(dma + ag->host_headroom);
		bd[pos].buff_cookie = cpu_to_le64(pos);
		ag->bp_ring.prod_shadow = AGNIC_RING_INC(pos, count);
		posted++;
	}
	if (posted) {
		dma_wmb();
		agnic_wr(ag, AGNIC_BAR_GIU, ag->bp_ring.prod_bar_off, ag->bp_ring.prod_shadow);
	}
}

/* Drain the RX ring: read the device producer, turn each completion into an skb
 * (collected on a local list), publish RX_CONS, refill the bpool, then hand the
 * frames to the pport demux OUTSIDE the lock. */
/* Reap the RX ring once. Returns true if it hit the per-pass cap (the ring may
 * still hold more completions) so the caller can re-poll immediately. */
static bool agnic_rx_reap(struct agnic *ag)
{
	struct agnic_rx_desc *ring = ag->rx_ring.mem.va;
	struct sk_buff_head list;
	struct sk_buff *skb;
	u32 prod, guard = 0, n = 0;
	bool more;

	__skb_queue_head_init(&list);
	spin_lock_bh(&ag->rx_lock);
	if (!ag->if_running) {
		spin_unlock_bh(&ag->rx_lock);
		return false;
	}
	prod = agnic_rd(ag, AGNIC_BAR_GIU, ag->rx_ring.prod_bar_off);
	dma_rmb();

	while (ag->rx_ring.cons_shadow != prod && guard < ag->rx_ring.count) {
		struct agnic_rx_desc *d = &ring[ag->rx_ring.cons_shadow];
		u64 cookie = le64_to_cpu(d->cookie);
		u16 len = le16_to_cpu(d->byte_cnt);
		u32 trim = ag->host_headroom + d->pkt_offset;
		struct agnic_rxbuf *rb;

		guard++;
		n++;	/* each completion consumed exactly one bpool buffer */
		ag->rx_ring.cons_shadow = AGNIC_RING_INC(ag->rx_ring.cons_shadow, ag->rx_ring.count);

		if (cookie >= ag->bp_ring.count) {	/* poisoned / out of range */
			ag->rx_dropped++;
			continue;
		}
		rb = &ag->rxb[cookie];
		skb = rb->skb;
		if (!skb || len == 0 || len > AGNIC_RX_CLSIZE || trim + len > AGNIC_RX_CLSIZE) {
			ag->rx_dropped++;
			if (skb) {
				dma_unmap_single(ag->dev, rb->dma, AGNIC_RX_CLSIZE, DMA_FROM_DEVICE);
				dev_kfree_skb_any(skb);
				rb->skb = NULL;
			}
			continue;
		}
		dma_unmap_single(ag->dev, rb->dma, AGNIC_RX_CLSIZE, DMA_FROM_DEVICE);
		rb->skb = NULL;

		skb_put(skb, trim + len);	/* expose headroom+offset+frame */
		if (trim)
			skb_pull(skb, trim);	/* advance to the frame (tag byte0) */
		__skb_queue_tail(&list, skb);
		ag->rx_frames++;
	}

	if (n) {
		dma_wmb();
		agnic_wr(ag, AGNIC_BAR_GIU, ag->rx_ring.cons_bar_off, ag->rx_ring.cons_shadow);
	}
	/* Self-healing bpool top-up EVERY tick (even n==0): recovers pool depth after a
	 * transient alloc/DMA failure that would otherwise permanently drain it. */
	agnic_bp_refill(ag);
	more = (guard >= ag->rx_ring.count);	/* hit the cap -> ring may hold more */
	spin_unlock_bh(&ag->rx_lock);

	/* Deliver via the per-port demux outside the lock. */
	while ((skb = __skb_dequeue(&list)))
		agnic_pport_rx(ag, skb);

	return more;
}

static void agnic_rx_work_fn(struct work_struct *w)
{
	struct agnic *ag = container_of(to_delayed_work(w), struct agnic, rx_work);
	bool more = agnic_rx_reap(ag);

	/* Adaptive re-arm: when a pass drained a full ring there is likely more waiting,
	 * so re-poll immediately (poll-until-empty) rather than capping RX at
	 * ring_depth / AGNIC_RX_POLL_MS (~25k pps). Fall back to the timer once drained. */
	if (ag->if_running)
		queue_delayed_work(ag->rx_wq, &ag->rx_work, more ? 0 : msecs_to_jiffies(AGNIC_RX_POLL_MS));
}

/* ---- MSI-X t2h doorbells (event-driven RX; the poll stays as the fallback) --- */

static irqreturn_t agnic_dbell_isr(int irq, void *arg)
{
	struct agnic_dbell_vec *v = arg;

	v->count++;
	/* GIU RX doorbell: bump the reaper ahead of the 10ms fallback timer so a real
	 * interrupt drains the ring immediately (mod_ forces it even if already queued).
	 * Other vectors are pure counters. */
	if (v->idx == AGNIC_RX_DBELL_ID && READ_ONCE(v->ag->if_running))
		mod_delayed_work(v->ag->rx_wq, &v->ag->rx_work, 0);
	return IRQ_HANDLED;
}

/* Allocate the standard PCI MSI-X vectors and arm a handler per t2h doorbell. The
 * facility walk is MGMT_NETDEV(1) then GIU(4), so global vector 0 is the MGMT dummy
 * and 1..4 are the GIU data queues; the RX queue advertises msix_id
 * AGNIC_RX_DBELL_ID(1). Non-fatal: on any failure RX stays purely poll-driven (the
 * reliable path), so the datapath still works. */
static void agnic_msix_setup(struct agnic *ag)
{
	int nvec, k;

	ag->msix_nvec = 0;
	nvec = pci_alloc_irq_vectors(ag->pdev, AGNIC_N_T2H_DBELLS, AGNIC_N_T2H_DBELLS,
				     PCI_IRQ_MSIX);
	if (nvec < 0) {
		dev_info(ag->dev, "P4: MSI-X unavailable (%d); RX stays poll-driven\n", nvec);
		return;
	}
	for (k = 0; k < nvec; k++) {
		struct agnic_dbell_vec *v = &ag->dbell[k];
		int ret;

		v->ag = ag;
		v->idx = k;
		v->count = 0;
		v->irq = pci_irq_vector(ag->pdev, k);
		ret = request_irq(v->irq, agnic_dbell_isr, 0, "agnic-dbell", v);
		if (ret) {
			dev_warn(ag->dev, "P4: request_irq for t2h dbell %d failed (%d)\n", k, ret);
			break;
		}
		ag->msix_nvec = k + 1;
	}
	if (ag->msix_nvec == 0) {
		pci_free_irq_vectors(ag->pdev);
		dev_info(ag->dev, "P4: no t2h doorbell IRQ armed; RX stays poll-driven\n");
		return;
	}
	dev_info(ag->dev, "P4: %d MSI-X t2h doorbell(s) armed (RX kick on vec %d); 10ms poll is the fallback\n",
		 ag->msix_nvec, AGNIC_RX_DBELL_ID);
}

static void agnic_msix_teardown(struct agnic *ag)
{
	int k;

	if (!ag->msix_nvec)
		return;
	for (k = 0; k < ag->msix_nvec; k++)
		free_irq(ag->dbell[k].irq, &ag->dbell[k]);
	pci_free_irq_vectors(ag->pdev);
	ag->msix_nvec = 0;
}

/* Enqueue one frame (already carrying the 66-byte pport prefix) on the single
 * trunk TX ring: copy into the coherent per-slot buffer, fill the descriptor,
 * advance TX_PROD (the doorbell). Host is the sole producer (tx_lock). */
netdev_tx_t agnic_giu_tx(struct agnic *ag, struct sk_buff *skb)
{
	struct agnic_tx_desc *ring = ag->tx_ring.mem.va;
	u32 len = skb->len, prod, next, cons;
	unsigned long flags;
	struct agnic_tx_desc *d;

	if (len == 0 || len > AGNIC_RX_CLSIZE)
		goto drop;

	spin_lock_irqsave(&ag->tx_lock, flags);
	if (!ag->if_running) {
		spin_unlock_irqrestore(&ag->tx_lock, flags);
		goto drop;
	}
	prod = ag->tx_ring.prod_shadow;
	next = AGNIC_RING_INC(prod, ag->tx_ring.count);
	cons = agnic_rd(ag, AGNIC_BAR_GIU, ag->tx_ring.cons_bar_off);
	if (next == cons) {		/* ring full — drop (no queue stop: shared trunk) */
		ag->tx_drops++;
		spin_unlock_irqrestore(&ag->tx_lock, flags);
		goto drop;
	}
	skb_copy_bits(skb, 0, (u8 *)ag->tx_bufs.va + (size_t)prod * AGNIC_RX_CLSIZE, len);

	d = &ring[prod];
	memset(d, 0, sizeof(*d));
	d->flags = cpu_to_le32(AGNIC_TXD_FLAGS_SG_SINGLE_ENTRY |
			       AGNIC_TXD_FLAGS_GEN_L4_CSUM_NOT |
			       AGNIC_TXD_FLAGS_GEN_IPV4_CSUM_DIS);
	d->byte_cnt = cpu_to_le16(len);
	d->pkt_offset = 0;
	d->buffer_addr = cpu_to_le64((u64)ag->tx_bufs.da + (u64)prod * AGNIC_RX_CLSIZE);
	d->cookie = cpu_to_le64(prod);
	dma_wmb();
	ag->tx_ring.prod_shadow = next;
	agnic_wr(ag, AGNIC_BAR_GIU, ag->tx_ring.prod_bar_off, next);
	ag->tx_frames++;
	spin_unlock_irqrestore(&ag->tx_lock, flags);

	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;
drop:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

/* ---- entry points ----------------------------------------------------------- */

int agnic_txrx_bringup(struct agnic *ag)
{
	u32 giu_off, need;
	int ret;

	if (!ag->mgmt_ready) {
		dev_warn(ag->dev, "P3b: mgmt channel not ready; skipping datapath\n");
		return -ENXIO;
	}
	if (ag->fac_bar[AGNIC_FAC_GIU] != AGNIC_BAR_GIU) {
		dev_err(ag->dev, "P3b: GIU facility not on BAR0; abort\n");
		return -EINVAL;
	}
	giu_off = ag->fac_off[AGNIC_FAC_GIU];

	/* mgmt owns index slots 0..3; the datapath needs 4..9 to also fit config_mem. */
	need = ag->dev_use_size + (AGNIC_MGMT_IDX_SLOTS + AGNIC_DATA_IDX_SLOTS) * sizeof(u32);
	if (!ag->dev_use_size || need > AGNIC_CONFIG_BAR_SIZE) {
		dev_err(ag->dev, "P3b: index slots do not fit (need 0x%x <= 0x%x); abort\n",
			need, AGNIC_CONFIG_BAR_SIZE);
		return -ERANGE;
	}

	ag->host_headroom = 0;
	spin_lock_init(&ag->rx_lock);
	spin_lock_init(&ag->tx_lock);
	INIT_DELAYED_WORK(&ag->rx_work, agnic_rx_work_fn);

	/* Dedicated RX workqueue: WQ_MEM_RECLAIM (a rescuer so RX refill makes progress
	 * under memory pressure) + WQ_HIGHPRI (low RX latency), single in-flight worker.
	 * Isolates the adaptive poll (which re-queues at delay 0 under load) from
	 * system_wq, where it could starve other work / suffer scheduling jitter. */
	ag->rx_wq = alloc_workqueue("agnic-rx", WQ_MEM_RECLAIM | WQ_HIGHPRI, 1);
	if (!ag->rx_wq)
		return -ENOMEM;

	ret = agnic_txrx_alloc(ag, giu_off);
	if (ret) {
		dev_err(ag->dev, "P3b: datapath ring alloc failed (%d)\n", ret);
		goto err_wq;
	}

	/* Arm the t2h MSI-X doorbells BEFORE config_queues advertises the RX queue's
	 * msix_id — the device must find real (allocated) vectors, not a masked table. */
	agnic_msix_setup(ag);

	/* The device does not touch the datapath rings until CC_PF_ENABLE, so it is
	 * safe to free them on a config failure here (unlike the mgmt rings). */
	ret = agnic_txrx_config_queues(ag);
	if (ret) {
		agnic_msix_teardown(ag);
		agnic_txrx_free_rings(ag);
		goto err_wq;
	}

	ag->datapath_inited = true;
	dev_info(ag->dev, "P3b: GIU datapath configured (rings up; port DOWN pending ENABLE)\n");
	return 0;

err_wq:
	destroy_workqueue(ag->rx_wq);
	ag->rx_wq = NULL;
	return ret;
}

void agnic_datapath_start(struct agnic *ag)
{
	u8 resp[AGNIC_MGMT_DESC_DATA_LEN];
	u8 p[AGNIC_MGMT_PARAMS_LEN];
	u8 status;
	int attempt, ret, enable_ok = 0;

	if (!ag->datapath_inited || ag->if_running)
		return;

	/* CC_PF_ENABLE, bounded retry — a custom NPU dp app may still be initializing. */
	for (attempt = 0; attempt < AGNIC_ENABLE_RETRIES; attempt++) {
		status = 0xff;
		ret = agnic_mgmt_cmd(ag, AGNIC_CC_PF_ENABLE, NULL, 0, resp, sizeof(resp), &status);
		if (!ret && status == AGNIC_NOTIF_STATUS_OK) {
			enable_ok = 1;
			break;
		}
		if (attempt == 0)
			dev_info(ag->dev, "P3b: CC_PF_ENABLE not ready (err %d status 0x%02x); retrying up to %d\n",
				 ret, status, AGNIC_ENABLE_RETRIES);
		msleep(500);
	}
	if (!enable_ok) {
		dev_warn(ag->dev, "P3b: CC_PF_ENABLE still failing after %d tries; datapath stays down (no FLR)\n",
			 AGNIC_ENABLE_RETRIES);
		return;
	}
	dev_info(ag->dev, "P3b: CC_PF_ENABLE OK (attempt %d) — GIU LIF is live\n", attempt + 1);

	/* CC_PF_PROMISC(1): front-panel frames carry dst-MAC 0x81pp.. so without
	 * promiscuous mode the trunk drops every tagged frame and RX stays 0. */
	memset(p, 0, sizeof(p));
	p[0] = 1;
	status = 0xff;
	ret = agnic_mgmt_cmd(ag, AGNIC_CC_PF_PROMISC, p, AGNIC_MGMT_PARAMS_LEN, resp, sizeof(resp), &status);
	if (ret || status != AGNIC_NOTIF_STATUS_OK)
		dev_warn(ag->dev, "P4: CC_PF_PROMISC failed (err %d status 0x%02x); tagged RX may be dropped\n",
			 ret, status);
	else
		dev_info(ag->dev, "P4: trunk set promiscuous\n");

	/* Create the front-panel netdevs BEFORE starting the reaper, so the reaper
	 * never delivers into a not-yet-created port (and the pport bring-up error
	 * path can't free a netdev the reaper is mid-delivery into). */
	ret = agnic_pport_bringup(ag);
	if (ret) {
		dev_warn(ag->dev, "P4: front-panel netdev creation failed (%d); RX poller not started\n", ret);
		return;
	}
	/* Only NOW mark the datapath live — after the port netdevs exist. This gates
	 * BOTH reaper starters (the MSI-X doorbell ISR and the poll below) so neither
	 * can deliver into a not-yet-created (or, on the pport error path above, freed)
	 * port. Any RX arriving in the enable->ports window just fills the ring and is
	 * drained on the first reap. */
	ag->if_running = true;
	queue_delayed_work(ag->rx_wq, &ag->rx_work, msecs_to_jiffies(AGNIC_RX_POLL_MS));
}

void agnic_txrx_teardown(struct agnic *ag)
{
	u8 resp[AGNIC_MGMT_DESC_DATA_LEN];
	u8 status = 0xff;

	if (!ag->datapath_inited)
		return;

	/* Reverse of bring-up (never an FLR). STOP THE REAPER FIRST: it delivers RX
	 * frames INTO the port netdevs outside any lock, so the netdevs must not be
	 * freed until the reaper is quiesced. if_running=false makes agnic_giu_tx drop
	 * and short-circuits the reaper; cancel_delayed_work_sync waits out any in-
	 * flight reap; only THEN unregister/free the netdevs, DISABLE the LIF, and free
	 * the device-owned rings. DISABLE is gated on datapath_inited (not if_running):
	 * the device may have applied CC_PF_ENABLE even if we never saw the ACK. The
	 * mgmt drainer must still be alive — agnic_remove runs this BEFORE agnic_mgmt_teardown. */
	/* Free the t2h doorbell IRQs FIRST — no ISR can kick the reaper after this. */
	agnic_msix_teardown(ag);
	ag->if_running = false;
	cancel_delayed_work_sync(&ag->rx_work);
	if (ag->rx_wq) {
		destroy_workqueue(ag->rx_wq);	/* flushes any last reap; if_running=false stops re-queue */
		ag->rx_wq = NULL;
	}
	agnic_pport_teardown(ag);

	(void)agnic_mgmt_cmd(ag, AGNIC_CC_PF_DISABLE, NULL, 0, resp, sizeof(resp), &status);
	/* CC_PF_DISABLE drops the LIF link (NC_PF_LINK_CHANGE 0) so the device stops
	 * delivering RX, but a still-forwarding dp_fwd may have frames in-flight. Let
	 * that tail drain before we dma_unmap/free the bpool buffers — otherwise the
	 * device DMAs into freed pages (the host IOMMU blocks it harmlessly, but it
	 * storms the fault log on an rmmod-under-traffic). */
	msleep(100);

	agnic_txrx_free_rings(ag);
	ag->datapath_inited = false;
	dev_info(ag->dev, "P3b: datapath torn down\n");
}
