/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic Phase 4b: pport demux -- turns the single GIU trunk into per-front-
 * panel-port interfaces (SFOS's mv_pport, "Physical Port" driver). The NPU's
 * generic NIC app forwards every front-panel port's traffic up the trunk with a
 * per-port tag; this layer strips the tag+header and delivers each frame on its
 * own ifnet (port1..port9), so OPNsense sees Port1..Port8 as ordinary,
 * individually-addressable NICs.
 *
 * WIRE FORMAT (confirmed live on hardware):
 *   [ 2-byte tag 0x81pp ][ 64-byte PPORT header ][ ethernet frame ]
 * tag byte0 = 0x81 (Port1) .. 0x89 (PortF1); byte1 = 0x00. Strip 66 bytes on RX;
 * prepend on TX. Clean-room; no GPL/proprietary .c logic copied.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/callout.h>
#include <sys/taskqueue.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <machine/atomic.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/bpf.h>

#include "if_agnic.h"

/* Tag/header geometry. PPORT_TAG_LEN/HDR_LEN/PREFIX now live in if_agnic.h
 * (shared with the CC_PF_INIT frame-size calc in agnic_txrx.c). */
#define	PPORT_TAG0_BASE		0x81		/* tag byte0 for Port1         */
#define	PPORT_COUNT		9		/* Port1..Port8 + PortF1       */
#define	PPORT_MAX_MC		32		/* shadow multicast entries    */

static MALLOC_DEFINE(M_AGNIC_PP, "agnic_pport", "agnic front-panel ports");

struct agnic_pport {
	struct agnic_softc     *sc;
	if_t			ifp[PPORT_COUNT];
	uint8_t			mac[PPORT_COUNT][6];
	uint64_t		rx[PPORT_COUNT];
	uint64_t		tx_drop[PPORT_COUNT];
	int			port_of_ifp_valid;

	/* Per-port control-plane shadow (ioctl -> NW_AGENT attr-set). */
	uint32_t		if_flags_last[PPORT_COUNT];
	uint8_t			mc[PPORT_COUNT][PPORT_MAX_MC][6];
	int			nmc[PPORT_COUNT];

	/* --- P4c: per-port link/media/stats, refreshed by the 1 Hz link poll. */
	int			link_up[PPORT_COUNT];	/* carrier shadow      */
	int			seen_up[PPORT_COUNT];	/* carrier read UP once */
	int			mng[PPORT_COUNT];	/* 1 = NPU-manageable  */
	int			info_ok;	/* ALL_COMB_PORT_INFO usable   */
	int			info_fail;	/* consecutive bulk failures   */
	int			info_empty;	/* consecutive all-zero replies */
	uint32_t		media_active[PPORT_COUNT]; /* IFM_* active word */
	struct ifmedia		media[PPORT_COUNT];
	uint64_t		hwstats[PPORT_COUNT][NWA_PORT_CNT_MAX];
	int			stats_valid[PPORT_COUNT];
	/* Host-side software counters (authoritative for locally-dropped TX). */
	uint64_t		rx_bytes[PPORT_COUNT];
	uint64_t		tx_pkts[PPORT_COUNT];
	uint64_t		tx_bytes[PPORT_COUNT];
	/* Link poll: mtx-bound callout kicks a sleepable task (runs the mailbox). */
	struct mtx		link_mtx;
	struct callout		link_poll;
	struct task		link_task;
	struct taskqueue       *link_tq;
	int			link_dying;	/* set under link_mtx at teardown */
};

/*
 * The NW_AGENT/pport tag for our port index: front-panel physical port N (=
 * index+1) has tag 0x8000 | (N << 8) (GPL port_tag.h: PPORT_REM_PORT_IND |
 * PPORT_REM_PORT_NUM). Inverse of PPORT_REM_PORT_NUM used by agnic_nwa.
 */
static __inline uint16_t
pport_tag(int port)
{

	return ((uint16_t)(0x8000 | ((port + 1) << 8)));
}

/*
 * hw.agnic.link_gate -- how the ALL_COMB_PORT_INFO carrier reading drives link
 * state (ports are always born UP so DHCP/traffic never depend on the read):
 *   0 = never change link state; only log carrier + refresh media/stats caches.
 *   1 = (default) demote a port to DOWN when the NPU reports no carrier, but
 *       ONLY after that port has first been read UP (seen_up) -- detects a cable
 *       UNPLUG while a wrong/absent read can never wedge a working port DOWN.
 *   2 = trust the carrier read fully: reflect real link state at boot too, so an
 *       unplugged port shows DOWN immediately (set this once the state field is
 *       confirmed on hardware: cabled port up, uncabled port down in the logs).
 */
static int agnic_link_gate = 1;
TUNABLE_INT("hw.agnic.link_gate", &agnic_link_gate);

/*
 * Consecutive NW_AGENT GET failures for one attribute on one port before the
 * link poll latches that attribute off for that port (stops retrying, so a
 * firmware that rejects e.g. OPER_STATE(1) does not spam or waste mailbox time;
 * the port simply stays UP on software counters).
 */
#define	NWA_GET_GIVEUP		3

static int	agnic_pport_transmit(if_t ifp, struct mbuf *m);
static void	agnic_pport_qflush(if_t ifp);
static int	agnic_pport_ioctl(if_t ifp, u_long cmd, caddr_t data);
static void	agnic_pport_init(void *xifp);
static int	agnic_pport_media_change(if_t ifp);
static void	agnic_pport_media_status(if_t ifp, struct ifmediareq *ifmr);
static uint64_t	agnic_pport_get_counter(if_t ifp, ift_counter c);
static void	agnic_pport_link_poll(void *xpp);
static void	agnic_pport_link_task(void *ctx, int pending);
static int	agnic_pport_media_word(uint32_t mbps, int full);

/* Map an ifp back to its port index (0..PPORT_COUNT-1), or -1. */
static int
pport_index(struct agnic_pport *pp, if_t ifp)
{
	int i;

	for (i = 0; i < PPORT_COUNT; i++)
		if (pp->ifp[i] == ifp)
			return (i);
	return (-1);
}

int
agnic_pport_bringup(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	struct agnic_pport *pp;
	int i;

	if (sc->pport != NULL)
		return (0);

	pp = malloc(sizeof(*pp), M_AGNIC_PP, M_WAITOK | M_ZERO);
	pp->sc = sc;

	for (i = 0; i < PPORT_COUNT; i++) {
		if_t ifp = if_alloc(IFT_ETHER);

		if (ifp == NULL) {
			device_printf(dev, "P4b: if_alloc port%d failed\n", i + 1);
			continue;
		}
		/* Locally-administered synthetic MAC per port (TODO: real). */
		pp->mac[i][0] = 0x02;
		pp->mac[i][1] = 0x81;
		pp->mac[i][2] = 0x00;
		pp->mac[i][3] = 0x00;
		pp->mac[i][4] = 0x00;
		pp->mac[i][5] = (uint8_t)(i + 1);

		if_setsoftc(ifp, pp);
		if_initname(ifp, "port", i + 1);
		if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
		if_setmtu(ifp, ETHERMTU);
		if_setinitfn(ifp, agnic_pport_init);
		if_setioctlfn(ifp, agnic_pport_ioctl);
		if_settransmitfn(ifp, agnic_pport_transmit);
		if_setqflushfn(ifp, agnic_pport_qflush);
		if_setgetcounterfn(ifp, agnic_pport_get_counter);
		if_setcapabilities(ifp, 0);
		if_setcapenable(ifp, 0);

		/* ifmedia: autoselect only (speed is NPU-driven); the media
		 * status callback reports carrier + the poll-cached speed/duplex. */
		ifmedia_init(&pp->media[i], 0, agnic_pport_media_change,
		    agnic_pport_media_status);
		ifmedia_add(&pp->media[i], IFM_ETHER | IFM_AUTO, 0, NULL);
		ifmedia_set(&pp->media[i], IFM_ETHER | IFM_AUTO);
		pp->media_active[i] = IFM_ETHER | IFM_AUTO;
		pp->mng[i] = (agnic_nwa_port_find(sc, pport_tag(i)) != NULL);

		ether_ifattach(ifp, pp->mac[i]);
		if_setdrvflagbits(ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);
		/*
		 * Born UP: DHCP is already independent of link (the trunk auto-
		 * starts before OPNsense runs), so there is no race to fix by
		 * starting DOWN, and starting DOWN on an unverified carrier read
		 * risks wedging the LAN. The link poll demotes to DOWN only under
		 * hw.agnic.link_gate once a port has been seen UP.
		 */
		if_link_state_change(ifp, LINK_STATE_UP);
		pp->link_up[i] = 1;
		pp->ifp[i] = ifp;
	}

	pp->info_ok = 1;	/* probe ALL_COMB_PORT_INFO; latch off if rejected */

	/*
	 * Link poll: an mtx-bound callout (race-free drain, like rx_poll) that
	 * only enqueues a sleepable task on its own taskqueue. The task runs the
	 * NW_AGENT mailbox (which sleeps on nwa_sx) -- it must NOT share rx_tq
	 * (that would stall RX for up to ~5 s per mailbox call).
	 */
	mtx_init(&pp->link_mtx, "agnic_link", NULL, MTX_DEF);
	callout_init_mtx(&pp->link_poll, &pp->link_mtx, 0);
	TASK_INIT(&pp->link_task, 0, agnic_pport_link_task, pp);
	pp->link_tq = taskqueue_create("agnic_link", M_NOWAIT,
	    taskqueue_thread_enqueue, &pp->link_tq);
	if (pp->link_tq != NULL)
		taskqueue_start_threads(&pp->link_tq, 1, PI_NET, "%s linktq",
		    device_get_nameunit(dev));

	sc->pport = pp;			/* publish BEFORE arming the callout */

	if (pp->link_tq != NULL) {
		mtx_lock(&pp->link_mtx);
		callout_reset(&pp->link_poll, hz, agnic_pport_link_poll, pp);
		mtx_unlock(&pp->link_mtx);
	} else {
		device_printf(dev, "P4c: link taskqueue_create failed; no "
		    "carrier poll (ports remain UP)\n");
	}

	device_printf(dev,
	    "[Phase 4b] pport demux up: port1..port%d created; front-panel "
	    "traffic demuxed by tag; link/media/stats poll @1Hz "
	    "(hw.agnic.link_gate=%d)\n", PPORT_COUNT, agnic_link_gate);
	return (0);
}

/*
 * Demux one raw trunk frame. m starts with [tag][64B hdr][ethernet]. Reads the
 * tag, strips the 66-byte prefix, and delivers on the matching port ifnet.
 * Consumes m (frees on error). Called per-frame from agnic_rx_service.
 */
void
agnic_pport_rx(struct agnic_softc *sc, struct mbuf *m)
{
	struct agnic_pport *pp = sc->pport;
	uint8_t *d;
	int port;

	if (pp == NULL) {
		m_freem(m);
		return;
	}
	if (m->m_pkthdr.len < PPORT_PREFIX + ETHER_HDR_LEN) {
		m_freem(m);
		return;
	}
	/* Ensure the tag byte is in the first mbuf. */
	if (m->m_len < PPORT_TAG_LEN) {
		m = m_pullup(m, PPORT_TAG_LEN);
		if (m == NULL)
			return;
	}
	d = mtod(m, uint8_t *);
	port = (int)d[0] - PPORT_TAG0_BASE;
	if (port < 0 || port >= PPORT_COUNT || pp->ifp[port] == NULL) {
		m_freem(m);
		return;
	}
	/*
	 * Snapshot a real NPU-produced 64-byte pport header (bytes after the 2-byte
	 * tag) so TX can replay the exact format (tx_hdr_mode=1). It's constant, so
	 * one snapshot suffices; benign if raced.
	 */
	if (!sc->rx_last_hdr_valid && m->m_len >= PPORT_PREFIX) {
		memcpy(sc->rx_last_hdr, d + PPORT_TAG_LEN, PPORT_HDR_LEN);
		sc->rx_last_hdr_valid = 1;
	}
	m_adj(m, PPORT_PREFIX);			/* strip tag + header */
	m->m_pkthdr.rcvif = pp->ifp[port];
	pp->rx[port]++;
	pp->rx_bytes[port] += m->m_pkthdr.len;	/* post-strip L2 length */
	if_input(pp->ifp[port], m);
}

static void
agnic_pport_init(void *xifp)
{
	(void)xifp;			/* nothing to (re)init: always live */
}

/*
 * TX: prepend the port's [2-byte meta tag][64-byte PPORT header] and hand the
 * frame to the GIU trunk. The NPU's pport layer reads the tag to pick the egress
 * front-panel port and strips the 66-byte prefix before the wire. Mirrors the RX
 * wire format (tag byte0 = 0x81 + port index, byte1 = 0). The 64-byte header is
 * zero-filled: on RX it carries device-side parse metadata (the a7..a0 pattern),
 * which the egress path does not consume -- only the tag selects the port. If a
 * cold-boot DHCP test shows frames not egressing, the header fill is the first
 * thing to vary. Consumes m in all paths.
 */
static int
agnic_pport_transmit(if_t ifp, struct mbuf *m)
{
	struct agnic_pport *pp = if_getsoftc(ifp);
	int port = pport_index(pp, ifp);
	uint8_t *d;
	unsigned int txlen;
	int error;

	if (port < 0 || pp->sc == NULL) {
		m_freem(m);
		return (ENXIO);
	}

	txlen = m->m_pkthdr.len;		/* clean L2 length, pre-prefix */
	/* BPF output tap on the clean ethernet frame (before the trunk tag), so
	 * tcpdump on portN sees egress. Our TX path bypasses ether_output_frame's
	 * driver tap, so we do it here. */
	if (bpf_peers_present(if_getbpf(ifp)))
		bpf_mtap_if(ifp, m);

	/* Make 66 bytes of contiguous headroom at the front of the frame. */
	M_PREPEND(m, PPORT_PREFIX, M_NOWAIT);
	if (m == NULL) {
		pp->tx_drop[port]++;
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
		return (ENOBUFS);
	}
	if (m->m_len < PPORT_PREFIX) {
		m = m_pullup(m, PPORT_PREFIX);
		if (m == NULL) {
			pp->tx_drop[port]++;
			if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
			return (ENOBUFS);
		}
	}

	d = mtod(m, uint8_t *);
	d[0] = (uint8_t)(PPORT_TAG0_BASE + port);	/* 0x81 + port index */
	d[1] = 0x00;
	/* 64-byte pport header, filled per the runtime-tunable tx_hdr_mode. */
	switch (pp->sc->tx_hdr_mode) {
	case 1:					/* replay a captured real RX header */
		if (pp->sc->rx_last_hdr_valid)
			memcpy(d + PPORT_TAG_LEN, pp->sc->rx_last_hdr,
			    PPORT_HDR_LEN);
		else
			bzero(d + PPORT_TAG_LEN, PPORT_HDR_LEN);
		break;
	case 2: {				/* 8-byte magic (LE) at header[0] */
		uint64_t mg = pp->sc->tx_hdr_magic;
		int k;

		bzero(d + PPORT_TAG_LEN, PPORT_HDR_LEN);
		for (k = 0; k < 8; k++)
			d[PPORT_TAG_LEN + k] = (uint8_t)(mg >> (8 * k));
		break;
	}
	case 3: {				/* EXACT SFOS fill: data[i]=0xC0+i */
		int k;			/* mv_pport pport_dev_hard_start_xmit  */

		for (k = 0; k < PPORT_HDR_LEN; k++)
			d[PPORT_TAG_LEN + k] = (uint8_t)(0xC0 + k);
		break;
	}
	default:				/* 0: all zeros */
		bzero(d + PPORT_TAG_LEN, PPORT_HDR_LEN);
		break;
	}

	error = agnic_giu_tx(pp->sc, m);		/* consumes m */
	if (error != 0) {
		pp->tx_drop[port]++;
		if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
	} else {
		if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
		pp->tx_pkts[port]++;
		pp->tx_bytes[port] += txlen;
	}
	return (error);
}

static void
agnic_pport_qflush(if_t ifp)
{
	(void)ifp;
}

/* Compose an IFM_ETHER media word from an NPU speed (Mbit/s) + duplex. */
static int
agnic_pport_media_word(uint32_t mbps, int full)
{
	int w = IFM_ETHER;

	switch (mbps) {
	case 10:	w |= IFM_10_T;   break;
	case 100:	w |= IFM_100_TX; break;
	case 1000:	w |= IFM_1000_T; break;
	default:	return (IFM_ETHER | IFM_AUTO);	/* unknown -> autoselect */
	}
	w |= full ? IFM_FDX : IFM_HDX;
	return (w);
}

/* SIOCSIFMEDIA: only autoselect is supported (speed is NPU-driven). */
static int
agnic_pport_media_change(if_t ifp)
{
	struct agnic_pport *pp = if_getsoftc(ifp);
	int port = pport_index(pp, ifp);

	if (port < 0)
		return (ENXIO);
	if (IFM_TYPE(pp->media[port].ifm_media) != IFM_ETHER ||
	    IFM_SUBTYPE(pp->media[port].ifm_media) != IFM_AUTO)
		return (EINVAL);
	return (0);
}

/*
 * SIOCGIFMEDIA status. Reports carrier + the poll-cached active speed/duplex.
 * IFM_ACTIVE (what dhclient/GUI gate on) is asserted only when link_up[port].
 * Caches only -- never sleeps, so it is safe under rtsock/ifconfig locks.
 */
static void
agnic_pport_media_status(if_t ifp, struct ifmediareq *ifmr)
{
	struct agnic_pport *pp = if_getsoftc(ifp);
	int port = pport_index(pp, ifp);

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;
	if (port < 0)
		return;
	if (pp->link_up[port]) {
		ifmr->ifm_status |= IFM_ACTIVE;
		ifmr->ifm_active = pp->media_active[port];
	}
}

/*
 * if_get_counter: cached NPU MIB when valid, else host software counters, else
 * the stack default. Never sleeps. Host-side TX drops (tx_drop) never reach the
 * NPU MIB, so they are ADDED to OERRORS in the hw branch too -- otherwise local
 * drops vanish the moment NPU stats go valid. Lock-free reads of the monotonic
 * caches are an accepted benign race (the task publishes stats_valid after the
 * memcpy with a release fence).
 */
static uint64_t
agnic_pport_get_counter(if_t ifp, ift_counter c)
{
	struct agnic_pport *pp = if_getsoftc(ifp);
	int port = pport_index(pp, ifp);
	const uint64_t *s;

	if (port < 0)
		return (if_get_counter_default(ifp, c));

	if (pp->stats_valid[port]) {
		s = pp->hwstats[port];
		switch (c) {
		case IFCOUNTER_IBYTES:
			return (s[NWA_ST_GOOD_OCTETS_RCV]);
		case IFCOUNTER_OBYTES:
			return (s[NWA_ST_GOOD_OCTETS_SENT]);
		case IFCOUNTER_IPACKETS:
			return (s[NWA_ST_GOOD_UC_PKTS_RCV] +
			    s[NWA_ST_MC_PKTS_RCV] + s[NWA_ST_BRDC_PKTS_RCV]);
		case IFCOUNTER_OPACKETS:
			return (s[NWA_ST_GOOD_UC_PKTS_SENT] +
			    s[NWA_ST_MC_PKTS_SENT] + s[NWA_ST_BRDC_PKTS_SENT]);
		case IFCOUNTER_IERRORS:
			return (s[NWA_ST_MAC_RCV_ERR] + s[NWA_ST_BAD_CRC]);
		case IFCOUNTER_OERRORS:
			return (s[NWA_ST_MAC_TX_ERR] +
			    s[NWA_ST_EXCESSIVE_COLL] + pp->tx_drop[port]);
		case IFCOUNTER_COLLISIONS:
			return (s[NWA_ST_COLLISIONS]);
		case IFCOUNTER_IQDROPS:
			return (s[NWA_ST_DROP_EVENTS]);
		case IFCOUNTER_IMCASTS:
			return (s[NWA_ST_MC_PKTS_RCV]);
		case IFCOUNTER_OMCASTS:
			return (s[NWA_ST_MC_PKTS_SENT]);
		default:
			return (if_get_counter_default(ifp, c));
		}
	}

	switch (c) {			/* NPU stats not yet valid: software */
	case IFCOUNTER_IPACKETS:	return (pp->rx[port]);
	case IFCOUNTER_IBYTES:		return (pp->rx_bytes[port]);
	case IFCOUNTER_OPACKETS:	return (pp->tx_pkts[port]);
	case IFCOUNTER_OBYTES:		return (pp->tx_bytes[port]);
	case IFCOUNTER_OERRORS:		return (pp->tx_drop[port]);
	default:			return (if_get_counter_default(ifp, c));
	}
}

/*
 * 1 Hz callout. Runs with link_mtx held (mtx-bound, like rx_poll). Touches NO
 * mailbox -- only enqueues the sleepable link task + reschedules. Checking
 * link_dying under the lock before reschedule is what makes callout_drain
 * race-free.
 */
static void
agnic_pport_link_poll(void *xpp)
{
	struct agnic_pport *pp = xpp;

	mtx_assert(&pp->link_mtx, MA_OWNED);
	if (pp->link_dying)
		return;
	if (pp->link_tq != NULL)
		taskqueue_enqueue(pp->link_tq, &pp->link_task);
	callout_reset(&pp->link_poll, hz, agnic_pport_link_poll, pp);
}

/*
 * Sleepable link task (taskqueue thread). Per manageable port: read carrier /
 * speed / duplex / stats via the mailbox (each takes nwa_sx internally; we hold
 * NO non-sleepable lock). Refresh the media + stats caches, then apply link-
 * state transitions under the hw.agnic.link_gate safety policy: never demote a
 * port that hasn't first been seen UP (so an unverified/broken carrier read
 * cannot wedge a working port). link_dying is re-checked between mailbox calls
 * to bound teardown latency.
 */
static void
agnic_pport_link_task(void *ctx, int pending)
{
	struct agnic_pport *pp = ctx;
	struct agnic_softc *sc = pp->sc;
	/*
	 * Big per-poll buffers kept static: the taskqueue has a single thread and
	 * runs one link_task at a time (never re-entrant), so this is race-free
	 * and keeps ~2.5 KB off the kernel stack.
	 */
	static uint16_t tags[PPORT_COUNT];
	static uint8_t states[PPORT_COUNT];
	static uint32_t speeds[PPORT_COUNT];
	static uint64_t stats[PPORT_COUNT][NWA_PORT_CNT_MAX];
	static int idx[PPORT_COUNT];
	int n, i, j, port, up, any, hasstats;

	(void)pending;
	if (pp->link_dying || !pp->info_ok)
		return;

	/* Build the manageable-port tag list (one bulk query covers them all). */
	n = 0;
	for (port = 0; port < PPORT_COUNT; port++) {
		if (pp->ifp[port] != NULL && pp->mng[port]) {
			tags[n] = pport_tag(port);
			idx[n] = port;
			n++;
		}
	}
	if (n == 0)
		return;

	if (agnic_nwa_ports_info_get(sc, tags, n, states, speeds, stats) != 0) {
		if (++pp->info_fail >= NWA_GET_GIVEUP) {
			pp->info_ok = 0;
			device_printf(sc->dev, "P4c: ALL_COMB_PORT_INFO "
			    "unsupported; ports stay UP, software counters "
			    "(no link-down detection)\n");
		}
		return;
	}
	pp->info_fail = 0;
	if (pp->link_dying)
		return;

	/*
	 * The generic-NIC NPU app ACKs the query but returns EMPTY records (no
	 * PHY carrier/speed/MIB -- confirmed on hardware; only the full SFOS
	 * agent populates them). Detect an all-zero reply and, after a few, latch
	 * the poll off so we stop the wasted 1 Hz mailbox round-trip and keep the
	 * software counters. If a firmware ever DOES populate it, this all works.
	 */
	any = 0;
	for (i = 0; i < n && !any; i++) {
		if (states[i] != 0 || speeds[i] != 0)
			any = 1;
		for (j = 0; j < NWA_PORT_CNT_MAX && !any; j++)
			if (stats[i][j] != 0)
				any = 1;
	}
	if (!any) {
		if (++pp->info_empty >= NWA_GET_GIVEUP) {
			pp->info_ok = 0;
			device_printf(sc->dev, "P4c: NPU returns empty per-port "
			    "info (generic-NIC firmware exposes no PHY carrier/"
			    "MIB); ports stay UP, software counters\n");
		}
		return;
	}
	pp->info_empty = 0;

	for (i = 0; i < n; i++) {
		port = idx[i];
		up = states[i] != 0;

		/* Refresh media (speed; assume full-duplex). */
		pp->media_active[port] = up ?
		    agnic_pport_media_word(speeds[i], 1) : (IFM_ETHER | IFM_AUTO);

		/* Publish NPU MIB only if this port actually has counters. */
		hasstats = 0;
		for (j = 0; j < NWA_PORT_CNT_MAX && !hasstats; j++)
			if (stats[i][j] != 0)
				hasstats = 1;
		if (hasstats) {
			memcpy(pp->hwstats[port], stats[i], sizeof(stats[i]));
			atomic_thread_fence_rel();	/* publish post-memcpy */
			pp->stats_valid[port] = 1;
		}

		/*
		 * Link-state transitions. Promote DOWN->UP on carrier-up (record
		 * seen_up). Demote UP->DOWN when the gate allows: level 2 trusts
		 * the read at boot; level 1 demotes only a port that was first
		 * seen UP (catches an unplug, never wedges a working port); level
		 * 0 never demotes.
		 */
		if (up)
			pp->seen_up[port] = 1;
		if (up && !pp->link_up[port]) {
			pp->link_up[port] = 1;
			if_link_state_change(pp->ifp[port], LINK_STATE_UP);
		} else if (!up && pp->link_up[port] &&
		    (agnic_link_gate >= 2 ||
		    (agnic_link_gate == 1 && pp->seen_up[port]))) {
			pp->link_up[port] = 0;
			if_link_state_change(pp->ifp[port], LINK_STATE_DOWN);
		}
	}
}

/* if_foreach_llmaddr callback: program one multicast MAC into the NPU filter. */
struct pport_mc_ctx {
	struct agnic_pport     *pp;
	int			port;
	uint16_t		tag;
	int			n;
};

static u_int
pport_mc_snap_cb(void *arg, struct sockaddr_dl *sdl, u_int cnt)
{
	struct pport_mc_ctx *c = arg;

	(void)cnt;
	if (c->n >= PPORT_MAX_MC)
		return (0);
	memcpy(c->pp->mc[c->port][c->n], LLADDR(sdl), 6);
	c->n++;
	return (1);
}

/*
 * Reprogram a port's NPU multicast filter to the ifnet's current group set.
 * The NW_AGENT ops are incremental (add/delete) and we can't diff cheaply, so
 * delete the previously-programmed shadow, then add the live set (SFOS relies on
 * the same add/delete pair). No-op on ports the NPU didn't mark manageable.
 *
 * The mailbox sleeps, so it must NOT be touched from the if_foreach_llmaddr
 * callback (that runs under the non-sleepable IF_ADDR lock): snapshot the group
 * set into the shadow first, then issue the add/delete transactions here.
 */
static void
pport_mc_sync(struct agnic_pport *pp, int port)
{
	struct pport_mc_ctx c;
	uint16_t tag = pport_tag(port);
	int i;

	if (agnic_nwa_port_find(pp->sc, tag) == NULL)
		return;
	/* Delete the previously-programmed set before the shadow is overwritten. */
	for (i = 0; i < pp->nmc[port]; i++)
		agnic_nwa_port_mc(pp->sc, tag, pp->mc[port][i], 0);
	/* Snapshot the live group set (no sleeping under the IF_ADDR lock). */
	c.pp = pp;
	c.port = port;
	c.tag = tag;
	c.n = 0;
	if_foreach_llmaddr(pp->ifp[port], pport_mc_snap_cb, &c);
	/* Program the snapshot (mailbox sleeps here; IF_ADDR lock already released). */
	for (i = 0; i < c.n; i++)
		agnic_nwa_port_mc(pp->sc, tag, pp->mc[port][i], 1);
	pp->nmc[port] = c.n;
}

/*
 * ifnet control plane -> NW_AGENT attr-set (mirrors SFOS pport_dev ndo ops via
 * nwa_pport_ext_port_ops): promisc/allmulti on flag deltas, per-port multicast
 * filter, and MTU (clamped to the NPU-advertised max). Admin state + source MAC
 * are already set for manageable ports at discovery; ports the NPU didn't mark
 * manageable fall through as no-ops but still behave as ordinary ifnets.
 */
static int
agnic_pport_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct agnic_pport *pp = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;
	int port, error = 0;
	uint16_t tag;

	if (pp == NULL || pp->sc == NULL)
		return (ENXIO);
	port = pport_index(pp, ifp);
	if (port < 0)
		return (ENXIO);
	tag = pport_tag(port);

	switch (cmd) {
	case SIOCSIFFLAGS: {
		uint32_t now = if_getflags(ifp);
		uint32_t changed = now ^ pp->if_flags_last[port];

		if (agnic_nwa_port_find(pp->sc, tag) != NULL) {
			if (changed & IFF_PROMISC)
				agnic_nwa_port_promisc(pp->sc, tag,
				    (now & IFF_PROMISC) != 0);
			if (changed & IFF_ALLMULTI)
				agnic_nwa_port_allmulti(pp->sc, tag,
				    (now & IFF_ALLMULTI) != 0);
		}
		pp->if_flags_last[port] = now;
		break;
	}
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		pport_mc_sync(pp, port);
		break;
	case SIOCSIFMTU: {
		struct agnic_nwa_port *p = agnic_nwa_port_find(pp->sc, tag);
		int max = p != NULL ? (int)p->max_mtu : ETHERMTU;

		if (ifr->ifr_mtu < ETHERMIN || ifr->ifr_mtu > max) {
			error = EINVAL;
			break;
		}
		if (p != NULL) {
			error = agnic_nwa_port_mtu_set(pp->sc, tag,
			    (uint32_t)ifr->ifr_mtu);
			if (error != 0)
				break;
		}
		if_setmtu(ifp, ifr->ifr_mtu);
		break;
	}
	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
	case SIOCGIFXMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &pp->media[port], cmd);
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (error);
}

void
agnic_pport_teardown(struct agnic_softc *sc)
{
	struct agnic_pport *pp = sc->pport;
	int i;

	if (pp == NULL)
		return;

	/*
	 * Quiesce the link poll FIRST: stop the callout re-arm under link_mtx
	 * (race-free like rx_poll), drain the in-flight task, free the tq. Only
	 * then NULL sc->pport and detach the ifnets the task touches. sc->nwa_sx
	 * outlives us (if_agnic.c destroys it after this), so an in-flight
	 * mailbox call in the drained task is safe.
	 */
	if (mtx_initialized(&pp->link_mtx)) {
		mtx_lock(&pp->link_mtx);
		pp->link_dying = 1;
		callout_stop(&pp->link_poll);
		mtx_unlock(&pp->link_mtx);
		callout_drain(&pp->link_poll);
		if (pp->link_tq != NULL) {
			taskqueue_drain(pp->link_tq, &pp->link_task);
			taskqueue_free(pp->link_tq);
			pp->link_tq = NULL;
		}
		mtx_destroy(&pp->link_mtx);
	}

	sc->pport = NULL;		/* stop the RX path from using it */
	for (i = 0; i < PPORT_COUNT; i++) {
		if (pp->ifp[i] != NULL) {
			ether_ifdetach(pp->ifp[i]);
			ifmedia_removeall(&pp->media[i]);
			if_free(pp->ifp[i]);
		}
	}
	free(pp, M_AGNIC_PP);
	device_printf(sc->dev, "P4b: pport demux torn down\n");
}
