/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * mamoru-agnic: clean-room Linux driver for the Marvell AGNIC (Armada GIU-NIC)
 * on the Sophos XGS 116 CN9130 NPU (PCIe PF 11ab:7080). Private state + helpers.
 *
 * Transcribed (interface facts only) from the proven BSD-2 FreeBSD if_agnic
 * driver; datapath logic is rewritten for Linux (skb/NAPI/dma_alloc_coherent).
 */
#ifndef _AGNIC_H_
#define _AGNIC_H_

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include "agnic_abi.h"

/* One coherent, 36-bit-safe DMA region (descriptor ring body / buffer block). */
struct agnic_dma_mem {
	void		*va;
	dma_addr_t	da;
	size_t		size;
};

/* A GIU management ring (cmd = host->dev, notif = dev->host). The descriptor body
 * is host DRAM the device DMAs; the producer/consumer index words live on BAR0. */
struct agnic_mgmt_ring {
	struct agnic_dma_mem mem;
	u32		count;		/* # descriptors (256)           */
	u32		prod_shadow;	/* local producer index          */
	u32		cons_shadow;	/* local consumer index          */
	u32		prod_bar_off;	/* BAR0 byte off of prod idx word*/
	u32		cons_bar_off;	/* BAR0 byte off of cons idx word*/
	u32		pub_prod_off;	/* value published in q_prod_offs*/
	u32		pub_cons_off;	/* value published in q_cons_offs*/
};

/* A GIU datapath ring (rx: device-producer completions; bpool: host-producer free
 * buffers; tx: host-producer). Like the mgmt rings, the prod/cons INDEX words live
 * on BAR0 at config_mem.dev_use_size; only the descriptor body is host DRAM. */
struct agnic_data_ring {
	struct agnic_dma_mem mem;	/* coherent descriptor body          */
	u32		count;		/* # descriptors (power of 2)        */
	u32		desc_size;	/* 32 (rx/tx) or 16 (bpool)          */
	u32		prod_shadow;	/* local producer index              */
	u32		cons_shadow;	/* local consumer index              */
	u32		prod_bar_off;	/* absolute BAR0 byte off of prod idx*/
	u32		cons_bar_off;	/* absolute BAR0 byte off of cons idx*/
	u32		pub_prod_off;	/* config_mem-relative prod off      */
	u32		pub_cons_off;	/* config_mem-relative cons off      */
};

/* Per-slot RX buffer tracking, indexed by bpool cookie (= slot index). */
struct agnic_rxbuf {
	struct sk_buff	*skb;
	dma_addr_t	dma;
};

/* One t2h MSI-X doorbell vector. The device fires the GIU RX doorbell on RX so a
 * real interrupt drains the ring immediately; the 10ms poll is the fallback. */
struct agnic_dbell_vec {
	struct agnic	*ag;
	int		idx;		/* global msix_id; RX = AGNIC_RX_DBELL_ID */
	int		irq;		/* Linux IRQ number for the vector        */
	u64		count;		/* interrupts serviced                    */
};

#define AGNIC_DRV_NAME "mamoru-agnic"

/* PCI BARs this device uses (BAR0 1MB, BAR2 16MB, BAR4 16MB). */
#define AGNIC_BAR_GIU	0	/* GIU config_mem + NW_AGENT windows      */
#define AGNIC_BAR_CTRL	2	/* barmap descriptor + facility tail      */
#define AGNIC_BAR_DBELL	4	/* host->target doorbell writes           */
#define AGNIC_BAR_MASK	(BIT(0) | BIT(2) | BIT(4))

/* Per-device driver state. */
struct agnic {
	struct pci_dev		*pdev;
	struct device		*dev;

	/* Mapped BARs, indexed by PCI BAR number (only 0/2/4 populated). */
	void __iomem		*bar[6];
	resource_size_t		bar_len[6];

	/* Resolved facility windows: which BAR + absolute byte offset in it. */
	int			fac_bar[AGNIC_FAC_COUNT];
	u32			fac_off[AGNIC_FAC_COUNT];

	u32			barmap_version;
	bool			dev_ready;
	u8			mac[ETH_ALEN];

	/* P3: GIU management command channel (agnic_mgmt.c). */
	struct agnic_mgmt_ring	cmd_ring, notif_ring;
	u32			dev_use_size;	/* BAR0 offset of the index array */
	u16			cmd_idx_gen;	/* next cmd_idx cookie (1..1023)  */
	struct mutex		cmd_lock;	/* serialize command senders      */
	spinlock_t		drain_lock;	/* wait-state + ring shadows ONLY —
						 * never hold this across MMIO    */
	struct delayed_work	drain_work;	/* continuous notif-ring drainer  */
	bool			mgmt_inited;
	bool			mgmt_ready;
	bool			mgmt_dead;	/* EP stopped answering (all-ones) */
	u32			max_buf_size;
	/* single armed command waiter (matched by the drainer). */
	u16			wait_idx;
	u8			wait_status;
	u8			*wait_buf;
	size_t			wait_len;
	struct completion	wait_cmpl;
	/* h2t mgmt doorbell (BAR4 kick), latched from the CTRL facility. */
	bool			h2t_valid;
	u32			h2t_bar4_off;
	u32			h2t_data;

	/* P3: async front-panel bring-up retrier. Built-in, the driver probes at
	 * device_initcall (seconds into boot) and can beat the NPU's ~60s boot, so the
	 * DEV_MGMT_READY handshake cannot be a one-shot wait — this worker keeps polling
	 * and brings up the datapath whenever dp_fwd answers, in any host/NPU boot order. */
	struct workqueue_struct	*bringup_wq;
	struct delayed_work	bringup_work;
	bool			bringup_active;
	u32			bringup_tries;

	/* P5: mvmgmt0 management netdev to the NPU (agnic_pcinet.c). */
	struct agnic_pcinet	*pcinet;

	/* P3b/P4: GIU datapath rings + BM pool (agnic_txrx.c). */
	struct agnic_data_ring	rx_ring, bp_ring, tx_ring;
	struct agnic_dma_mem	tx_bufs;	/* count * AGNIC_RX_CLSIZE TX copy slots */
	struct agnic_rxbuf	*rxb;		/* AGNIC_BP_RING_LEN entries            */
	u32			host_headroom;
	bool			datapath_inited;
	bool			if_running;

	/* P4/P5: poll-driven RX reaper, single TX ring, per-port netdevs. */
	struct workqueue_struct	*rx_wq;		/* dedicated RX wq (no system_wq jitter)*/
	struct delayed_work	rx_work;	/* reliable RX poll (like pcinet)      */
	spinlock_t		rx_lock;	/* serialize the RX reaper            */
	spinlock_t		tx_lock;	/* serialize the single trunk TX ring */
	u8			rx_last_hdr[AGNIC_PPORT_HDR_LEN];	/* snapshot for TX replay */
	bool			rx_last_hdr_valid;
	struct net_device	*ports[AGNIC_PPORT_COUNT];
	u64			rx_frames, rx_dropped, tx_frames, tx_drops;

	/* MSI-X t2h doorbells (event-driven RX; poll fallback stays armed). */
	int			msix_nvec;
	struct agnic_dbell_vec	dbell[AGNIC_N_T2H_DBELLS];
};

/* Per-port netdev private (netdev_priv of each port%d). */
struct agnic_port {
	struct agnic	*ag;
	int		index;		/* 0 .. AGNIC_PPORT_COUNT-1 */
};

/* Coherent 36-bit-safe DMA alloc/free (shared by P3 rings). */
static inline int agnic_dma_alloc(struct agnic *ag, struct agnic_dma_mem *m, size_t size)
{
	m->va = dma_alloc_coherent(ag->dev, size, &m->da, GFP_KERNEL);
	if (!m->va)
		return -ENOMEM;
	m->size = size;
	return 0;
}

static inline void agnic_dma_free(struct agnic *ag, struct agnic_dma_mem *m)
{
	if (m->va)
		dma_free_coherent(ag->dev, m->size, m->va, m->da);
	m->va = NULL;
}

/* 32-bit LE MMIO into a facility's BAR. `bar` is the PCI BAR number. */
static inline u32 agnic_rd(struct agnic *ag, int bar, u32 off)
{
	return ioread32(ag->bar[bar] + off);
}

static inline void agnic_wr(struct agnic *ag, int bar, u32 off, u32 v)
{
	iowrite32(v, ag->bar[bar] + off);
}

/* Read a device 6-byte MAC laid out as 6 consecutive bytes at (bar, off). */
static inline void agnic_rd_mac(struct agnic *ag, int bar, u32 off, u8 mac[ETH_ALEN])
{
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		mac[i] = ioread8(ag->bar[bar] + off + i);
}

const char *agnic_facility_name(enum agnic_facility f);

/* P3: GIU management channel (agnic_mgmt.c). Split so the persistent half (publish
 * rings + HOST_MGMT_READY) runs once at probe, and the handshake half is retriable:
 * agnic_mgmt_finish() returns -EAGAIN until the NPU answers DEV_MGMT_READY. */
int agnic_mgmt_publish(struct agnic *ag);
int agnic_mgmt_finish(struct agnic *ag);
void agnic_mgmt_teardown(struct agnic *ag);
int agnic_mgmt_cmd(struct agnic *ag, u8 cmd_code, const void *params, size_t plen,
		   u8 *resp, size_t resp_len, u8 *out_status);

/* P5: mvmgmt0 management netdev (agnic_pcinet.c). */
int agnic_pcinet_bringup(struct agnic *ag);
void agnic_pcinet_teardown(struct agnic *ag);

/* P3b/P4: GIU datapath (agnic_txrx.c). */
int agnic_txrx_bringup(struct agnic *ag);	/* alloc rings + CC_PF_* config (port DOWN) */
void agnic_datapath_start(struct agnic *ag);	/* CC_PF_ENABLE + promisc + start RX + ports */
void agnic_txrx_teardown(struct agnic *ag);
netdev_tx_t agnic_giu_tx(struct agnic *ag, struct sk_buff *skb);	/* single trunk TX ring */

/* P4/P5: per-port netdevs + pport demux (agnic_pport.c). */
int agnic_pport_bringup(struct agnic *ag);	/* create port1..port9 (born UP)        */
void agnic_pport_teardown(struct agnic *ag);
void agnic_pport_rx(struct agnic *ag, struct sk_buff *skb);	/* demux one RX frame  */

#endif /* _AGNIC_H_ */
