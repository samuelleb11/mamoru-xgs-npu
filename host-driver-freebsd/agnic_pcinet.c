/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic Phase 5: mvmgmt0 -- the PCIe management netdev / IPv6 link to the
 * CN9130 NPU (SFOS's mv_pcinet_drv, "PCI-NET NIC driver"). This is the channel
 * SFOS uses to SSH into the NPU and hand it the `/tmp/host_breakout_complete`
 * handshake that makes the NPU start forwarding front-panel ports to the host
 * trunk. Once mvmgmt0 is up, the host reaches the NPU at its fixed link-local
 * fe80::<npu-eui64>%mvmgmt0.
 *
 * Clean-room reimplementation for OPNsense/FreeBSD. The config-descriptor
 * layout, the shared-memory SPSC ring ABI (pci_net_q / pci_net_q_entry) and the
 * link handshake are interface FACTS transcribed from the GPL-2.0 pcinet.c /
 * pcinet_host.c / pcinet.h; no GPL .c logic is copied.
 *
 * MODEL: a shared-memory config descriptor in the MGMT_NETDEV facility window
 * (MMIO on BAR2) points at two host-DRAM SPSC rings (rx: NPU->host, tx:
 * host->NPU). Each ring is a control struct (indices + phys of the entry array)
 * + an entry array + a block of per-entry data buffers, all coherent host DRAM
 * the NPU reaches over PCIe. No doorbell (MGMT_NETDEV has 0 dbells): both sides
 * poll. A link handshake (link_status/link_change in the config) advances
 * NETIF_OPEN -> HOST_UP -> (NPU) ESTABLISHED -> carrier up.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/endian.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/atomic.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>

#include "if_agnic.h"

/* Config descriptor (struct pci_net_shared_cfg) -- MMIO in the MGMT_NETDEV
 * facility window. Byte offsets fixed by the ABI. */
#define	PC_CFG_RX_Q_PHYS	0x00	/* u64: host rx control-struct phys */
#define	PC_CFG_TX_Q_PHYS	0x08	/* u64: host tx control-struct phys */
#define	PC_CFG_LINK_STATUS	0x10	/* u32: enum link_status           */
#define	PC_CFG_LINK_CHANGE	0x14	/* u8 (accessed as u32): change flag*/
#define	PC_CFG_STATUS		0x18	/* u32: ready pattern              */
#define	PC_CFG_REMOTE_MAC	0x20	/* u64: cookie[2] + mac[6] (NPU)   */

#define	PC_PATTERN_READY	0xAA55AA55U	/* NPU published the mailbox */
#define	PC_PATTERN_ACK		0xBB66BB66U	/* host acks                 */

/* enum link_status. */
#define	PC_LINK_IS_DOWN		0x00
#define	PC_NETIF_OPEN		0x80
#define	PC_NETIF_STOP		0x81
#define	PC_LINK_HOST_UP		0x82
#define	PC_LINK_ESTABLISHED	0x83

/* struct pci_net_q -- ring control struct in host DRAM (NPU reads it). */
struct pc_q {
	uint64_t	queue;		/* @0x00 host vaddr of entry array   */
	uint64_t	dev_queue;	/* @0x08 NPU-side vaddr (NPU fills)  */
	int32_t		q_size;		/* @0x10                             */
	int32_t		q_last;		/* @0x14 q_size - 1                  */
	volatile int32_t push_idx;	/* @0x18 producer index             */
	volatile int32_t pop_idx;	/* @0x1c consumer index             */
	uint32_t	sanity_val;	/* @0x20 PCINET_HOST_DEV_COMM_SANITY */
	uint32_t	_pad;		/* @0x24                             */
	uint64_t	q_phys_addr;	/* @0x28 phys of the entry array     */
};

/* struct pci_net_q_entry -- one ring slot in host DRAM. */
struct pc_ent {
	volatile uint32_t status;	/* @0x00 HOST_OWN etc.               */
	volatile uint32_t size;		/* @0x04 frame length                */
	uint64_t	pbuf_phys;	/* @0x08 data buffer phys            */
	uint64_t	pbuf_virt;	/* @0x10 host use                    */
	uint64_t	skb;		/* @0x18 host use (unused)           */
};

#define	PC_ENTRY_STATUS_HOST_OWN	0x80000000U
#define	PC_SANITY			0x01234567U

/* Ring depth + per-buffer size. 128 slots is plenty for a mgmt link. */
#define	PC_Q_SIZE		128
#define	PC_BUF_SIZE		2048

/* Poll/handshake cadences (ms). */
#define	PC_READY_MS		4000	/* wait for the ready pattern */
#define	PC_LINK_MS		200	/* handshake poll             */
#define	PC_RX_MS		5	/* rx drain poll              */

static MALLOC_DEFINE(M_AGNIC_PC, "agnic_pcinet", "agnic mvmgmt0");

struct agnic_pcinet {
	struct agnic_softc     *sc;
	if_t			ifp;
	int			cfg_bar;	/* MGMT_NETDEV facility BAR    */
	uint32_t		cfg_off;	/* facility window base        */

	struct mtx		mtx;
	struct callout		link_co;	/* handshake state machine     */
	struct callout		rx_co;		/* rx poll -> kicks rx_task     */
	struct taskqueue       *rx_tq;		/* delivers RX off the callout  */
	struct task		rx_task;
	int			inited;
	int			running;
	int			link_up;

	/* rx: NPU->host (NPU pushes, host pops). tx: host->NPU. */
	struct agnic_dma_mem	rxq_ctrl, rxq_ents, rxq_bufs;
	struct agnic_dma_mem	txq_ctrl, txq_ents, txq_bufs;

	uint8_t			mac[6];
	uint64_t		rx_frames, tx_frames, tx_drops;
};

/* Config-window MMIO helpers (BAR2 facility window). */
static __inline uint32_t
pc_cfg_rd(struct agnic_pcinet *p, uint32_t off)
{
	return (AGNIC_RD4(p->sc, p->cfg_bar, p->cfg_off + off));
}
static __inline void
pc_cfg_wr(struct agnic_pcinet *p, uint32_t off, uint32_t v)
{
	AGNIC_WR4(p->sc, p->cfg_bar, p->cfg_off + off, v);
}
static __inline void
pc_cfg_wr64(struct agnic_pcinet *p, uint32_t off, uint64_t v)
{
	AGNIC_WR4(p->sc, p->cfg_bar, p->cfg_off + off, (uint32_t)v);
	AGNIC_WR4(p->sc, p->cfg_bar, p->cfg_off + off + 4,
	    (uint32_t)(v >> 32));
}

/* SPSC ring index helpers (operate on the coherent control struct). */
static __inline int
pc_q_empty(struct pc_q *q)
{
	return (q->push_idx == q->pop_idx);
}
static __inline int
pc_q_full(struct pc_q *q)
{
	return (((q->push_idx + 1) == q->pop_idx) ||
	    ((q->push_idx == q->q_last) && (q->pop_idx == 0)));
}
static __inline int32_t
pc_q_inc(struct pc_q *q, int32_t idx)
{
	return (idx == q->q_last) ? 0 : (idx + 1);
}

static void	agnic_pcinet_init(void *xp);
static int	agnic_pcinet_transmit(if_t ifp, struct mbuf *m);
static void	agnic_pcinet_qflush(if_t ifp);
static int	agnic_pcinet_ioctl(if_t ifp, u_long cmd, caddr_t data);
static void	agnic_pcinet_link_cb(void *xp);
static void	agnic_pcinet_rx_cb(void *xp);
static void	agnic_pcinet_rx_task(void *ctx, int pending);

/* Allocate one ring: control struct + entry array + buffer block, wire them. */
static int
pc_alloc_ring(struct agnic_softc *sc, struct agnic_dma_mem *ctrl,
    struct agnic_dma_mem *ents, struct agnic_dma_mem *bufs, const char *name)
{
	struct pc_q *q;
	struct pc_ent *e;
	char nm[24];
	int i, error;

	snprintf(nm, sizeof(nm), "%s-ctrl", name);
	error = agnic_dma_alloc(sc, ctrl, sizeof(struct pc_q), nm);
	if (error != 0)
		return (error);
	snprintf(nm, sizeof(nm), "%s-ents", name);
	error = agnic_dma_alloc(sc, ents,
	    (bus_size_t)PC_Q_SIZE * sizeof(struct pc_ent), nm);
	if (error != 0)
		return (error);
	snprintf(nm, sizeof(nm), "%s-bufs", name);
	error = agnic_dma_alloc(sc, bufs,
	    (bus_size_t)PC_Q_SIZE * PC_BUF_SIZE, nm);
	if (error != 0)
		return (error);

	q = (struct pc_q *)ctrl->vaddr;
	bzero(q, sizeof(*q));
	q->queue = (uint64_t)(uintptr_t)ents->vaddr;
	q->q_size = PC_Q_SIZE;
	q->q_last = PC_Q_SIZE - 1;
	q->push_idx = 0;
	q->pop_idx = 0;
	q->sanity_val = PC_SANITY;
	q->q_phys_addr = (uint64_t)ents->paddr;

	e = (struct pc_ent *)ents->vaddr;
	for (i = 0; i < PC_Q_SIZE; i++) {
		bzero(&e[i], sizeof(e[i]));
		e[i].pbuf_phys = (uint64_t)bufs->paddr + (uint64_t)i * PC_BUF_SIZE;
		e[i].pbuf_virt = (uint64_t)(uintptr_t)((uint8_t *)bufs->vaddr +
		    (size_t)i * PC_BUF_SIZE);
	}
	return (0);
}

int
agnic_pcinet_bringup(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	struct agnic_pcinet *p;
	if_t ifp;
	int error, ms;
	uint32_t st;

	if (sc->pcinet != NULL)
		return (0);			/* already up */

	if (sc->fac_bar[AGNIC_FAC_MGMT_NETDEV] < 0) {
		device_printf(dev, "P5: MGMT_NETDEV facility absent; no mvmgmt0\n");
		return (ENXIO);
	}

	p = malloc(sizeof(*p), M_AGNIC_PC, M_WAITOK | M_ZERO);
	p->sc = sc;
	p->cfg_bar = sc->fac_bar[AGNIC_FAC_MGMT_NETDEV];
	p->cfg_off = sc->fac_off[AGNIC_FAC_MGMT_NETDEV];
	device_printf(dev, "P5: pcinet MGMT_NETDEV @BAR%d+0x%x\n", p->cfg_bar,
	    p->cfg_off);

	/* Wait for the NPU to publish the ready pattern. */
	for (ms = 0; ms < PC_READY_MS; ms += 100) {
		st = pc_cfg_rd(p, PC_CFG_STATUS);
		if (st == PC_PATTERN_READY)
			break;
		pause("pcrdy", hz / 10);
	}
	st = pc_cfg_rd(p, PC_CFG_STATUS);
	if (st != PC_PATTERN_READY) {
		device_printf(dev, "P5: no pcinet ready pattern (status 0x%08x "
		    "cfg[0..3]=%08x %08x %08x %08x); abort\n", st,
		    pc_cfg_rd(p, 0), pc_cfg_rd(p, 4), pc_cfg_rd(p, 8),
		    pc_cfg_rd(p, 0xc));
		free(p, M_AGNIC_PC);
		return (ENXIO);
	}
	device_printf(dev, "P5: pcinet ready pattern seen; acking\n");
	pc_cfg_wr(p, PC_CFG_STATUS, PC_PATTERN_ACK);

	/* Allocate the two rings. */
	error = pc_alloc_ring(sc, &p->rxq_ctrl, &p->rxq_ents, &p->rxq_bufs,
	    "pcrx");
	if (error != 0)
		goto fail;
	error = pc_alloc_ring(sc, &p->txq_ctrl, &p->txq_ents, &p->txq_bufs,
	    "pctx");
	if (error != 0)
		goto fail;

	/* Publish the control-struct physical addresses + clear status. */
	atomic_thread_fence_rel();
	pc_cfg_wr64(p, PC_CFG_RX_Q_PHYS, (uint64_t)p->rxq_ctrl.paddr);
	pc_cfg_wr64(p, PC_CFG_TX_Q_PHYS, (uint64_t)p->txq_ctrl.paddr);
	pc_cfg_wr(p, PC_CFG_STATUS, 0);

	/* mvmgmt0 MAC: match SFOS's fixed mgmt MAC. */
	p->mac[0] = 0x00; p->mac[1] = 0x00; p->mac[2] = 0x12;
	p->mac[3] = 0x13; p->mac[4] = 0x14; p->mac[5] = 0x15;

	mtx_init(&p->mtx, "agnic_pc", NULL, MTX_DEF);
	callout_init(&p->link_co, 1);		/* MPSAFE; we lock manually */
	callout_init(&p->rx_co, 1);
	TASK_INIT(&p->rx_task, 0, agnic_pcinet_rx_task, p);
	p->rx_tq = taskqueue_create("agnic_mvm", M_NOWAIT,
	    taskqueue_thread_enqueue, &p->rx_tq);
	if (p->rx_tq == NULL) {
		device_printf(dev, "P5: rx taskqueue create failed\n");
		callout_drain(&p->link_co);
		callout_drain(&p->rx_co);
		mtx_destroy(&p->mtx);
		error = ENOMEM;
		goto fail;
	}
	taskqueue_start_threads(&p->rx_tq, 1, PI_NET, "%s mvmtq",
	    device_get_nameunit(dev));
	p->inited = 1;

	ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "P5: if_alloc failed\n");
		error = ENOSPC;
		goto fail;
	}
	p->ifp = ifp;
	sc->pcinet = p;
	if_setsoftc(ifp, p);
	if_initname(ifp, "mvmgmt", device_get_unit(dev));
	if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_setmtu(ifp, ETHERMTU);
	if_setinitfn(ifp, agnic_pcinet_init);
	if_setioctlfn(ifp, agnic_pcinet_ioctl);
	if_settransmitfn(ifp, agnic_pcinet_transmit);
	if_setqflushfn(ifp, agnic_pcinet_qflush);
	if_setcapabilities(ifp, 0);
	if_setcapenable(ifp, 0);
	ether_ifattach(ifp, p->mac);

	device_printf(dev,
	    "[Phase 5] mvmgmt0 created (MAC %6D); ifconfig up, then SSH the NPU "
	    "at fe80::<npu-eui64>%%mvmgmt0\n", p->mac, ":");
	return (0);

fail:
	if (p->inited) {
		callout_drain(&p->link_co);
		callout_drain(&p->rx_co);
		if (p->rx_tq != NULL) {
			taskqueue_drain(p->rx_tq, &p->rx_task);
			taskqueue_free(p->rx_tq);
		}
		mtx_destroy(&p->mtx);
	}
	agnic_dma_free(&p->txq_bufs); agnic_dma_free(&p->txq_ents);
	agnic_dma_free(&p->txq_ctrl);
	agnic_dma_free(&p->rxq_bufs); agnic_dma_free(&p->rxq_ents);
	agnic_dma_free(&p->rxq_ctrl);
	if (sc->pcinet == p)
		sc->pcinet = NULL;
	free(p, M_AGNIC_PC);
	return (error);
}

/* if_init: kick the handshake (host -> NETIF_OPEN) + start the pollers. */
static void
agnic_pcinet_init(void *xp)
{
	struct agnic_pcinet *p = xp;

	mtx_lock(&p->mtx);
	if (!p->running) {
		p->running = 1;
		if_setdrvflagbits(p->ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);
		/* announce NETIF_OPEN to the NPU. */
		pc_cfg_wr(p, PC_CFG_LINK_STATUS, PC_NETIF_OPEN);
		pc_cfg_wr(p, PC_CFG_LINK_CHANGE, 1);
		callout_reset(&p->link_co, hz * PC_LINK_MS / 1000,
		    agnic_pcinet_link_cb, p);
	}
	mtx_unlock(&p->mtx);
}

/* Handshake state machine (host side). Runs under mtx from the callout. */
static void
agnic_pcinet_link_cb(void *xp)
{
	struct agnic_pcinet *p = xp;
	uint32_t change, status;

	mtx_lock(&p->mtx);
	if (!p->running) {
		mtx_unlock(&p->mtx);
		return;
	}
	change = pc_cfg_rd(p, PC_CFG_LINK_CHANGE) & 0xFF;
	status = pc_cfg_rd(p, PC_CFG_LINK_STATUS);
	if (change != 0) {
		switch (status) {
		case PC_NETIF_OPEN:
			/* rings already allocated at bringup; advance. */
			pc_cfg_wr(p, PC_CFG_LINK_STATUS, PC_LINK_HOST_UP);
			break;
		case PC_LINK_ESTABLISHED:
			pc_cfg_wr(p, PC_CFG_LINK_CHANGE, 0);
			if (!p->link_up) {
				p->link_up = 1;
				if_link_state_change(p->ifp, LINK_STATE_UP);
				device_printf(p->sc->dev,
				    "[Phase 5] mvmgmt0 link ESTABLISHED\n");
				callout_reset(&p->rx_co, hz * PC_RX_MS / 1000,
				    agnic_pcinet_rx_cb, p);
			}
			break;
		case PC_NETIF_STOP:
		case PC_LINK_IS_DOWN:
			pc_cfg_wr(p, PC_CFG_LINK_STATUS, PC_LINK_IS_DOWN);
			pc_cfg_wr(p, PC_CFG_LINK_CHANGE, 0);
			break;
		default:
			break;		/* HOST_UP: waiting for the NPU */
		}
	}
	callout_reset(&p->link_co, hz * PC_LINK_MS / 1000,
	    agnic_pcinet_link_cb, p);
	mtx_unlock(&p->mtx);
}

/* RX poll callout: kick the delivery task + reschedule. Cheap; no delivery. */
static void
agnic_pcinet_rx_cb(void *xp)
{
	struct agnic_pcinet *p = xp;

	mtx_lock(&p->mtx);
	if (!p->running) {
		mtx_unlock(&p->mtx);
		return;
	}
	if (p->rx_tq != NULL)
		taskqueue_enqueue(p->rx_tq, &p->rx_task);
	callout_reset(&p->rx_co, hz * PC_RX_MS / 1000, agnic_pcinet_rx_cb, p);
	mtx_unlock(&p->mtx);
}

/*
 * RX delivery task (dedicated thread, proper stack): drain the ring under the
 * lock into a local list, then deliver to the stack outside the lock. Buffer
 * addresses are index-computed from our own block (never the NPU-writable
 * pbuf_virt), and the producer index is snapshotted + range-checked.
 */
static void
agnic_pcinet_rx_task(void *ctx, int pending)
{
	struct agnic_pcinet *p = ctx;
	struct pc_q *rq = (struct pc_q *)p->rxq_ctrl.vaddr;
	struct pc_ent *ents = (struct pc_ent *)p->rxq_ents.vaddr;
	struct mbuf *mh = NULL, *mt = NULL;
	if_t ifp = p->ifp;
	int32_t prod;
	int guard = 0;

	(void)pending;
	mtx_lock(&p->mtx);
	if (!p->running) {
		mtx_unlock(&p->mtx);
		return;
	}
	atomic_thread_fence_acq();
	prod = rq->push_idx;
	if (prod < 0 || prod >= PC_Q_SIZE)		/* garbage from NPU */
		prod = rq->pop_idx;
	while (rq->pop_idx != prod && guard++ < PC_Q_SIZE) {
		int32_t idx = rq->pop_idx;
		struct pc_ent *e = &ents[idx];
		uint32_t len = e->size;
		struct mbuf *m;

		if (len >= ETHER_HDR_LEN && len <= PC_BUF_SIZE) {
			m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
			if (m != NULL) {
				memcpy(mtod(m, void *),
				    (uint8_t *)p->rxq_bufs.vaddr +
				    (size_t)idx * PC_BUF_SIZE, len);
				m->m_len = m->m_pkthdr.len = len;
				m->m_pkthdr.rcvif = ifp;
				m->m_nextpkt = NULL;
				if (mh == NULL)
					mh = mt = m;
				else { mt->m_nextpkt = m; mt = m; }
				p->rx_frames++;
			}
		}
		e->status = 0;
		e->size = 0;
		rq->pop_idx = pc_q_inc(rq, idx);
	}
	atomic_thread_fence_rel();
	mtx_unlock(&p->mtx);

	while (mh != NULL) {
		struct mbuf *nx = mh->m_nextpkt;

		mh->m_nextpkt = NULL;
		if_input(ifp, mh);
		mh = nx;
	}
}

/* TX: copy the frame into the tx ring, set HOST_OWN, advance producer. */
static int
agnic_pcinet_transmit(if_t ifp, struct mbuf *m)
{
	struct agnic_pcinet *p = if_getsoftc(ifp);
	struct pc_q *tq = (struct pc_q *)p->txq_ctrl.vaddr;
	struct pc_ent *ents = (struct pc_ent *)p->txq_ents.vaddr;
	struct pc_ent *e;
	int len, pad;

	mtx_lock(&p->mtx);
	if (!p->running || !p->link_up) {
		mtx_unlock(&p->mtx);
		m_freem(m);
		return (ENETDOWN);
	}
	if (pc_q_full(tq)) {
		p->tx_drops++;
		mtx_unlock(&p->mtx);
		m_freem(m);
		return (ENOBUFS);
	}
    {
	int32_t idx = tq->push_idx;
	uint8_t *buf;

	if (idx < 0 || idx >= PC_Q_SIZE) {
		mtx_unlock(&p->mtx);
		m_freem(m);
		return (EIO);
	}
	len = m->m_pkthdr.len;
	if (len > PC_BUF_SIZE)
		len = PC_BUF_SIZE;
	e = &ents[idx];
	buf = (uint8_t *)p->txq_bufs.vaddr + (size_t)idx * PC_BUF_SIZE;
	m_copydata(m, 0, len, buf);
	pad = (len < 64) ? 64 : len;		/* min ethernet frame */
	if (pad > len)
		bzero(buf + len, pad - len);
	e->size = pad;
	atomic_thread_fence_rel();
	e->status = PC_ENTRY_STATUS_HOST_OWN;
	tq->push_idx = pc_q_inc(tq, idx);
	atomic_thread_fence_rel();
    }
	p->tx_frames++;
	mtx_unlock(&p->mtx);
	m_freem(m);
	return (0);
}

static void
agnic_pcinet_qflush(if_t ifp)
{
	(void)ifp;
}

static int
agnic_pcinet_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct agnic_pcinet *p = if_getsoftc(ifp);
	int error = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if (if_getflags(ifp) & IFF_UP) {
			if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
				agnic_pcinet_init(p);
		}
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (error);
}

void
agnic_pcinet_teardown(struct agnic_softc *sc)
{
	struct agnic_pcinet *p = sc->pcinet;

	if (p == NULL)
		return;
	if (p->inited) {
		mtx_lock(&p->mtx);
		p->running = 0;
		if (p->cfg_bar >= 0) {
			pc_cfg_wr(p, PC_CFG_LINK_STATUS, PC_NETIF_STOP);
			pc_cfg_wr(p, PC_CFG_LINK_CHANGE, 1);
		}
		mtx_unlock(&p->mtx);
		callout_drain(&p->link_co);
		callout_drain(&p->rx_co);
		if (p->rx_tq != NULL) {
			taskqueue_drain(p->rx_tq, &p->rx_task);
			taskqueue_free(p->rx_tq);
			p->rx_tq = NULL;
		}
	}
	if (p->ifp != NULL) {
		ether_ifdetach(p->ifp);
		if_free(p->ifp);
	}
	if (p->inited)
		mtx_destroy(&p->mtx);
	agnic_dma_free(&p->txq_bufs); agnic_dma_free(&p->txq_ents);
	agnic_dma_free(&p->txq_ctrl);
	agnic_dma_free(&p->rxq_bufs); agnic_dma_free(&p->rxq_ents);
	agnic_dma_free(&p->rxq_ctrl);
	sc->pcinet = NULL;
	free(p, M_AGNIC_PC);
	device_printf(sc->dev, "P5: mvmgmt0 torn down\n");
}
