// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * mamoru-agnic P5: mvmgmt0 — the PCIe management netdev to the CN9130 NPU.
 *
 * This is the NPU's `mv_pcinet_trgt` channel: a shared-memory config descriptor in
 * the MGMT_NETDEV facility window (BAR2) points at two host-DRAM SPSC rings (rx:
 * NPU->host, tx: host->NPU). No doorbell — both sides poll. A link handshake
 * (link_status/link_change) advances NETIF_OPEN -> HOST_UP -> ESTABLISHED. Once up,
 * the host reaches the NPU over this netdev (its fixed link-local) to run dp_launch.sh
 * / start dp_fwd — the data-plane forwarder that then stamps the AGNIC CTRL cookie
 * and drives GIU to DEV_READY.
 *
 * Clean-room Linux port of the proven BSD-2 FreeBSD agnic_pcinet.c: the config-
 * descriptor layout, the SPSC ring ABI (pc_q / pc_ent) and the handshake are
 * interface FACTS; the datapath is rewritten for Linux (skb / dma_alloc_coherent /
 * delayed_work). No NPU reset/FLR anywhere.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include "agnic.h"

/* Config descriptor (struct pci_net_shared_cfg), MMIO in the MGMT_NETDEV window. */
#define PC_CFG_RX_Q_PHYS	0x00	/* u64: host rx control-struct dma addr */
#define PC_CFG_TX_Q_PHYS	0x08	/* u64: host tx control-struct dma addr */
#define PC_CFG_LINK_STATUS	0x10	/* u32: enum link_status               */
#define PC_CFG_LINK_CHANGE	0x14	/* u32: change flag                    */
#define PC_CFG_STATUS		0x18	/* u32: ready pattern                  */
#define PC_CFG_REMOTE_MAC	0x20	/* cookie[2] + mac[6] (NPU)            */

#define PC_PATTERN_READY	0xAA55AA55U
#define PC_PATTERN_ACK		0xBB66BB66U

/* enum link_status. */
#define PC_LINK_IS_DOWN		0x00
#define PC_NETIF_OPEN		0x80
#define PC_NETIF_STOP		0x81
#define PC_LINK_HOST_UP		0x82
#define PC_LINK_ESTABLISHED	0x83

/* Ring control struct in host DRAM (the NPU reads it). Field offsets are ABI. */
struct pc_q {
	__le64	queue;		/* @0x00 host vaddr of entry array (info)   */
	__le64	dev_queue;	/* @0x08 NPU-side vaddr (NPU fills)         */
	__le32	q_size;		/* @0x10                                    */
	__le32	q_last;		/* @0x14 q_size - 1                         */
	__le32	push_idx;	/* @0x18 producer index                     */
	__le32	pop_idx;	/* @0x1c consumer index                     */
	__le32	sanity_val;	/* @0x20                                    */
	__le32	_pad;		/* @0x24                                    */
	__le64	q_phys_addr;	/* @0x28 dma addr of the entry array        */
} __packed;

/* One ring slot in host DRAM. */
struct pc_ent {
	__le32	status;		/* @0x00 HOST_OWN etc.                      */
	__le32	size;		/* @0x04 frame length                       */
	__le64	pbuf_phys;	/* @0x08 data buffer dma addr               */
	__le64	pbuf_virt;	/* @0x10 host use                           */
	__le64	skb;		/* @0x18 host use (unused)                  */
} __packed;

#define PC_ENTRY_STATUS_HOST_OWN	0x80000000U
#define PC_SANITY			0x01234567U

#define PC_Q_SIZE	128
#define PC_BUF_SIZE	2048

#define PC_READY_MS	4000	/* wait for the ready pattern */
#define PC_LINK_MS	200	/* handshake poll             */
#define PC_RX_MS	5	/* rx drain poll              */

/* One coherent DMA region. */
struct pc_dma {
	void		*va;
	dma_addr_t	da;
	size_t		len;
};

struct agnic_pcinet {
	struct agnic	*ag;
	struct net_device *ndev;
	int		cfg_bar;
	u32		cfg_off;

	struct delayed_work link_work;	/* handshake state machine */
	struct delayed_work rx_work;	/* rx drain poll           */
	bool		running;
	bool		link_up;

	/* rx: NPU->host. tx: host->NPU. */
	struct pc_dma	rxq_ctrl, rxq_ents, rxq_bufs;
	struct pc_dma	txq_ctrl, txq_ents, txq_bufs;

	u64		rx_frames, tx_frames, tx_drops;
};

static inline u32 pc_rd(struct agnic_pcinet *p, u32 off)
{
	return agnic_rd(p->ag, p->cfg_bar, p->cfg_off + off);
}
static inline void pc_wr(struct agnic_pcinet *p, u32 off, u32 v)
{
	agnic_wr(p->ag, p->cfg_bar, p->cfg_off + off, v);
}
static inline void pc_wr64(struct agnic_pcinet *p, u32 off, u64 v)
{
	pc_wr(p, off, lower_32_bits(v));
	pc_wr(p, off + 4, upper_32_bits(v));
}

static inline s32 pc_q_inc(s32 idx, s32 last)
{
	return (idx == last) ? 0 : (idx + 1);
}
static inline bool pc_q_full(struct pc_q *q)
{
	s32 push = le32_to_cpu(q->push_idx), pop = le32_to_cpu(q->pop_idx);
	s32 last = le32_to_cpu(q->q_last);

	return ((push + 1) == pop) || (push == last && pop == 0);
}

static void pc_dma_free(struct agnic *ag, struct pc_dma *m)
{
	if (m->va)
		dma_free_coherent(ag->dev, m->len, m->va, m->da);
	m->va = NULL;
}

static int pc_dma_alloc(struct agnic *ag, struct pc_dma *m, size_t len)
{
	m->va = dma_alloc_coherent(ag->dev, len, &m->da, GFP_KERNEL);
	if (!m->va)
		return -ENOMEM;
	m->len = len;
	return 0;
}

/* Allocate one ring: control struct + entry array + buffer block, wire them up. */
static int pc_alloc_ring(struct agnic *ag, struct pc_dma *ctrl,
			 struct pc_dma *ents, struct pc_dma *bufs)
{
	struct pc_q *q;
	struct pc_ent *e;
	int i, ret;

	ret = pc_dma_alloc(ag, ctrl, sizeof(struct pc_q));
	if (ret)
		return ret;
	ret = pc_dma_alloc(ag, ents, (size_t)PC_Q_SIZE * sizeof(struct pc_ent));
	if (ret)
		return ret;
	ret = pc_dma_alloc(ag, bufs, (size_t)PC_Q_SIZE * PC_BUF_SIZE);
	if (ret)
		return ret;

	q = ctrl->va;
	memset(q, 0, sizeof(*q));
	q->queue = cpu_to_le64((u64)(uintptr_t)ents->va);
	q->q_size = cpu_to_le32(PC_Q_SIZE);
	q->q_last = cpu_to_le32(PC_Q_SIZE - 1);
	q->sanity_val = cpu_to_le32(PC_SANITY);
	q->q_phys_addr = cpu_to_le64(ents->da);

	e = ents->va;
	for (i = 0; i < PC_Q_SIZE; i++) {
		memset(&e[i], 0, sizeof(e[i]));
		e[i].pbuf_phys = cpu_to_le64(bufs->da + (u64)i * PC_BUF_SIZE);
		e[i].pbuf_virt = cpu_to_le64((u64)(uintptr_t)((u8 *)bufs->va + (size_t)i * PC_BUF_SIZE));
	}
	return 0;
}

static void pc_free_rings(struct agnic_pcinet *p)
{
	struct agnic *ag = p->ag;

	pc_dma_free(ag, &p->txq_bufs); pc_dma_free(ag, &p->txq_ents); pc_dma_free(ag, &p->txq_ctrl);
	pc_dma_free(ag, &p->rxq_bufs); pc_dma_free(ag, &p->rxq_ents); pc_dma_free(ag, &p->rxq_ctrl);
}

/* RX drain: pop NPU-pushed frames into skbs and hand them to the stack. */
static void pc_rx_work(struct work_struct *w)
{
	struct agnic_pcinet *p = container_of(to_delayed_work(w), struct agnic_pcinet, rx_work);
	struct pc_q *rq = p->rxq_ctrl.va;
	struct pc_ent *ents = p->rxq_ents.va;
	s32 last = le32_to_cpu(rq->q_last);
	s32 prod, pop;
	int guard = 0;

	if (!p->running)
		return;

	dma_rmb();
	prod = le32_to_cpu(rq->push_idx);
	pop = le32_to_cpu(rq->pop_idx);
	if (prod < 0 || prod >= PC_Q_SIZE)	/* garbage from the NPU */
		prod = pop;

	while (pop != prod && guard++ < PC_Q_SIZE) {
		struct pc_ent *e = &ents[pop];
		u32 len = le32_to_cpu(e->size);

		if (len >= ETH_HLEN && len <= PC_BUF_SIZE) {
			struct sk_buff *skb = netdev_alloc_skb(p->ndev, len + NET_IP_ALIGN);

			if (skb) {
				skb_reserve(skb, NET_IP_ALIGN);
				skb_put_data(skb, (u8 *)p->rxq_bufs.va + (size_t)pop * PC_BUF_SIZE, len);
				skb->protocol = eth_type_trans(skb, p->ndev);
				p->ndev->stats.rx_packets++;
				p->ndev->stats.rx_bytes += len;
				p->rx_frames++;
				netif_rx(skb);
			} else {
				p->ndev->stats.rx_dropped++;
			}
		}
		e->status = 0;
		e->size = 0;
		pop = pc_q_inc(pop, last);
		rq->pop_idx = cpu_to_le32(pop);
	}
	dma_wmb();

	if (p->running)
		schedule_delayed_work(&p->rx_work, msecs_to_jiffies(PC_RX_MS));
}

/* Handshake state machine (host side). */
static void pc_link_work(struct work_struct *w)
{
	struct agnic_pcinet *p = container_of(to_delayed_work(w), struct agnic_pcinet, link_work);
	u32 change, status;

	if (!p->running)
		return;

	change = pc_rd(p, PC_CFG_LINK_CHANGE) & 0xff;
	status = pc_rd(p, PC_CFG_LINK_STATUS);
	if (change) {
		switch (status) {
		case PC_NETIF_OPEN:
			pc_wr(p, PC_CFG_LINK_STATUS, PC_LINK_HOST_UP);
			break;
		case PC_LINK_ESTABLISHED:
			pc_wr(p, PC_CFG_LINK_CHANGE, 0);
			if (!p->link_up) {
				p->link_up = true;
				netif_carrier_on(p->ndev);
				dev_info(p->ag->dev, "P5: mvmgmt0 link ESTABLISHED\n");
				schedule_delayed_work(&p->rx_work, msecs_to_jiffies(PC_RX_MS));
			}
			break;
		case PC_NETIF_STOP:
		case PC_LINK_IS_DOWN:
			pc_wr(p, PC_CFG_LINK_STATUS, PC_LINK_IS_DOWN);
			pc_wr(p, PC_CFG_LINK_CHANGE, 0);
			break;
		default:
			break;	/* HOST_UP: waiting for the NPU */
		}
	}
	schedule_delayed_work(&p->link_work, msecs_to_jiffies(PC_LINK_MS));
}

static int pc_ndo_open(struct net_device *ndev)
{
	struct agnic_pcinet *p = netdev_priv(ndev);

	if (p->running)
		return 0;
	p->running = true;
	netif_carrier_off(ndev);
	/* Announce NETIF_OPEN and start the handshake poller. */
	pc_wr(p, PC_CFG_LINK_STATUS, PC_NETIF_OPEN);
	pc_wr(p, PC_CFG_LINK_CHANGE, 1);
	schedule_delayed_work(&p->link_work, msecs_to_jiffies(PC_LINK_MS));
	netif_start_queue(ndev);
	return 0;
}

static int pc_ndo_stop(struct net_device *ndev)
{
	struct agnic_pcinet *p = netdev_priv(ndev);

	netif_stop_queue(ndev);
	p->running = false;
	p->link_up = false;
	netif_carrier_off(ndev);
	pc_wr(p, PC_CFG_LINK_STATUS, PC_NETIF_STOP);
	pc_wr(p, PC_CFG_LINK_CHANGE, 1);
	cancel_delayed_work_sync(&p->link_work);
	cancel_delayed_work_sync(&p->rx_work);
	return 0;
}

/* TX: copy the frame into the tx ring, set HOST_OWN, advance producer. */
static netdev_tx_t pc_ndo_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct agnic_pcinet *p = netdev_priv(ndev);
	struct pc_q *tq = p->txq_ctrl.va;
	struct pc_ent *ents = p->txq_ents.va;
	s32 idx, last = le32_to_cpu(tq->q_last);
	struct pc_ent *e;
	u32 len, pad;

	if (!p->running || !p->link_up)
		goto drop;
	if (pc_q_full(tq)) {
		p->tx_drops++;
		ndev->stats.tx_dropped++;
		goto drop;
	}
	idx = le32_to_cpu(tq->push_idx);
	if (idx < 0 || idx >= PC_Q_SIZE)
		goto drop;

	len = min_t(u32, skb->len, PC_BUF_SIZE);
	e = &ents[idx];
	skb_copy_bits(skb, 0, (u8 *)p->txq_bufs.va + (size_t)idx * PC_BUF_SIZE, len);
	pad = max_t(u32, len, ETH_ZLEN);	/* min ethernet frame */
	if (pad > len)
		memset((u8 *)p->txq_bufs.va + (size_t)idx * PC_BUF_SIZE + len, 0, pad - len);
	e->size = cpu_to_le32(pad);
	dma_wmb();
	e->status = cpu_to_le32(PC_ENTRY_STATUS_HOST_OWN);
	tq->push_idx = cpu_to_le32(pc_q_inc(idx, last));
	dma_wmb();

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += len;
	p->tx_frames++;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops pc_netdev_ops = {
	.ndo_open       = pc_ndo_open,
	.ndo_stop       = pc_ndo_stop,
	.ndo_start_xmit = pc_ndo_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr   = eth_validate_addr,
};

/* Bring up mvmgmt0: wait for the NPU ready pattern, publish rings, register netdev. */
int agnic_pcinet_bringup(struct agnic *ag)
{
	struct net_device *ndev;
	struct agnic_pcinet *p;
	int ret, ms;
	u32 st;
	static const u8 mac[ETH_ALEN] = { 0x00, 0x00, 0x12, 0x13, 0x14, 0x15 };

	if (ag->fac_bar[AGNIC_FAC_MGMT_NETDEV] < 0) {
		dev_warn(ag->dev, "P5: MGMT_NETDEV facility absent; no mvmgmt0\n");
		return -ENXIO;
	}

	ndev = alloc_etherdev(sizeof(struct agnic_pcinet));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, ag->dev);
	p = netdev_priv(ndev);
	p->ag = ag;
	p->ndev = ndev;
	p->cfg_bar = ag->fac_bar[AGNIC_FAC_MGMT_NETDEV];
	p->cfg_off = ag->fac_off[AGNIC_FAC_MGMT_NETDEV];
	INIT_DELAYED_WORK(&p->link_work, pc_link_work);
	INIT_DELAYED_WORK(&p->rx_work, pc_rx_work);

	dev_info(ag->dev, "P5: pcinet MGMT_NETDEV @BAR%d+0x%x\n", p->cfg_bar, p->cfg_off);

	/* Wait (briefly) for the NPU's first-boot ready pattern, then ack it. It is ONE-SHOT
	 * per NPU boot: the first host attach acks + zeros CFG_STATUS. On a later re-attach the
	 * pattern is gone (status 0), so we do NOT abort — re-publishing the ring addresses below
	 * re-establishes the channel against the same live NPU pcinet. */
	for (ms = 0; ms < PC_READY_MS; ms += 100) {
		st = pc_rd(p, PC_CFG_STATUS);
		if (st == PC_PATTERN_READY)
			break;
		msleep(100);
	}
	st = pc_rd(p, PC_CFG_STATUS);
	if (st == PC_PATTERN_READY) {
		dev_info(ag->dev, "P5: pcinet ready pattern seen; acking\n");
		pc_wr(p, PC_CFG_STATUS, PC_PATTERN_ACK);
	} else {
		dev_info(ag->dev, "P5: no fresh ready pattern (status 0x%08x) — re-attach, proceeding\n", st);
	}

	ret = pc_alloc_ring(ag, &p->rxq_ctrl, &p->rxq_ents, &p->rxq_bufs);
	if (ret)
		goto err_free_rings;
	ret = pc_alloc_ring(ag, &p->txq_ctrl, &p->txq_ents, &p->txq_bufs);
	if (ret)
		goto err_free_rings;

	/* Publish the control-struct DMA addresses + clear status. */
	dma_wmb();
	pc_wr64(p, PC_CFG_RX_Q_PHYS, p->rxq_ctrl.da);
	pc_wr64(p, PC_CFG_TX_Q_PHYS, p->txq_ctrl.da);
	pc_wr(p, PC_CFG_STATUS, 0);

	eth_hw_addr_set(ndev, mac);
	ndev->netdev_ops = &pc_netdev_ops;
	strscpy(ndev->name, "mvmgmt%d", IFNAMSIZ);

	ret = register_netdev(ndev);
	if (ret) {
		dev_err(ag->dev, "P5: register_netdev failed: %d\n", ret);
		goto err_free_rings;
	}
	ag->pcinet = p;
	dev_info(ag->dev, "P5: %s created (MAC %pM). `ip link set %s up`, then reach the NPU over it.\n",
		 ndev->name, mac, ndev->name);
	return 0;

err_free_rings:
	pc_free_rings(p);
	free_netdev(ndev);
	return ret;
}

void agnic_pcinet_teardown(struct agnic *ag)
{
	struct agnic_pcinet *p = ag->pcinet;

	if (!p)
		return;
	p->running = false;
	if (p->ndev->reg_state == NETREG_REGISTERED)
		unregister_netdev(p->ndev);
	cancel_delayed_work_sync(&p->link_work);
	cancel_delayed_work_sync(&p->rx_work);
	/* Tell the NPU we're going away (best effort). */
	pc_wr(p, PC_CFG_LINK_STATUS, PC_NETIF_STOP);
	pc_wr(p, PC_CFG_LINK_CHANGE, 1);
	pc_free_rings(p);
	free_netdev(p->ndev);
	ag->pcinet = NULL;
	dev_info(ag->dev, "P5: mvmgmt0 torn down\n");
}
