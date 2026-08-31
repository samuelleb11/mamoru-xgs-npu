/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic Phase 4a: NW_AGENT (nwa) mailbox client -- the PHYSICAL front-panel
 * port control plane. Separate from the GIU data trunk: a shared-memory
 * request/response mailbox in the NW_AGENT facility window (BAR0) that the
 * CN9130 NPU services. This is what SFOS's mv_nwa_host uses to bring the
 * front-panel PHYs up and read per-port link. Without it our GIU trunk links
 * up but every front-panel port stays dark.
 *
 * Clean-room reimplementation for OPNsense/FreeBSD. The mailbox transaction,
 * config-descriptor layout, message envelope and opcodes are interface FACTS
 * transcribed/verified from the SFOS mv_nwa_host.ko disassembly (nwa_pci_cmd_send
 * @0x340, nwa_wq_create_pports @0x1c90, nwa_port_state_set @0x13d0); no GPL/
 * proprietary .c logic is copied.
 *
 * MAILBOX (in the NW_AGENT window, all MMIO on BAR0):
 *   config descriptor @window: [0]=0xCAFEBABE ready magic, [4]=0x34 cfg cookie,
 *     then offset/len fields. Verified live by dumping the window (this file
 *     logs window[0..0x40] on first bring-up so the exact layout is confirmed
 *     on our own hardware, which runs the same NPU firmware as SFOS).
 *   ctrl register block (window-relative nwa_ctrl_off): +0x18 doorbell
 *     (write 1=submit, 2=ack), +0x1c req_len, +0x20 ownership (0=host may write,
 *     1=response ready), +0x24 resp_len.
 *   payload buffer (window-relative nwa_payload_off): request words at [0..],
 *     response words at [roundup4(req_len)..].
 *
 * MESSAGE ENVELOPE (32-byte request):
 *   [0x00]=type (1=DISCOVERY, 3=ATTR-SET, 4=ATTR-GET)
 *   [0x04]=sub_opcode (SET/GET: 0=admin-state, 2=mtu, 3=mac)
 *   [0x08]=port_id (u32)
 *   [0x10]=value (admin state byte / mtu / mac ...)
 * RESPONSE: valid iff resp[0]==0x14 (marker) && resp[4]==0 (status).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/atomic.h>

#include <net/ethernet.h>		/* ETHERMTU */

#include "if_agnic.h"

/*
 * Config-descriptor field offsets (window-relative), verified by live dump +
 * mv_nwa_host relocations. The config descriptor sits at the window base and IS
 * the ctrl-register block (ctx+0x128 == the descriptor pointer). Live layout:
 *   [0x00]=0xCAFEBABE  [0x04]=0x34 (cookie AND cmd-payload offset)
 *   [0x08]=cmd_mbox_len (0x7FCC)   [0x0C]=evt offset (0x8000)  [0x10]=evt_len
 *   [0x18..0x24]=ctrl registers    payload buffer @ window+[0x04] (=0x34)
 */
#define	NWA_CFG_READY_MAGIC_OFF		0x00	/* == 0xCAFEBABE when NPU up */
#define	NWA_CFG_READY_MAGIC		0xCAFEBABEU
#define	NWA_CFG_COOKIE_OFF		0x04	/* == 0x34 when cfg published */
#define	NWA_CFG_COOKIE			0x34U
#define	NWA_CFG_PAYLOAD_OFF_OFF		0x04	/* cmd payload = window + [0x04] */
#define	NWA_CFG_CMDLEN_OFF		0x08	/* cmd_mbox_len (0x7FCC)     */
#define	NWA_CTRL_OFF			0x00	/* ctrl regs live in the header */

/* ctrl register block, relative to nwa_ctrl_off. */
#define	NWA_REG_DOORBELL		0x18	/* write 1=submit, 2=ack */
#define	NWA_REG_REQ_LEN			0x1C
#define	NWA_REG_OWNERSHIP		0x20	/* 0=host may write, 1=resp ready */
#define	NWA_REG_RESP_LEN		0x24
#define	NWA_DBELL_SUBMIT		1
#define	NWA_DBELL_ACK			2

/* Message envelope (byte offsets within the 32-byte request). */
#define	NWA_REQ_LEN			0x20	/* every cmd sends 32 bytes */
#define	NWA_MSG_TYPE			0x00
#define	NWA_MSG_SUBOP			0x04
#define	NWA_MSG_PORT_ID			0x08
#define	NWA_MSG_VALUE			0x10
#define	NWA_TYPE_DISCOVERY		1
#define	NWA_TYPE_ATTR_SET		3
#define	NWA_TYPE_ATTR_GET		4
#define	NWA_TYPE_MDIO_OPERATION		65	/* raw switch/PHY register read/write */
#define	NWA_TYPE_ALL_COMB_PORT_INFO	69	/* bulk per-port state+speed+stats */
/* enum nwa_msg_port_attr (mv_nwa_host.h) -- attr code at msg+0x04. */
#define	NWA_SUBOP_ADMIN_STATE		0
#define	NWA_SUBOP_OPER_STATE		1	/* GET: link up/down        */
#define	NWA_SUBOP_MTU			2
#define	NWA_SUBOP_MAC			3
#define	NWA_SUBOP_SPEED			4	/* GET: u32 Mbit/s          */
#define	NWA_SUBOP_DUPLEX		13	/* GET: u8 ethtool DUPLEX_* */
#define	NWA_SUBOP_STATS			14	/* GET: u64 stats[32]       */
#define	NWA_SUBOP_PROMISC		69
#define	NWA_SUBOP_ALLMULTI		70
#define	NWA_SUBOP_MC_ADD		73
#define	NWA_SUBOP_MC_DELETE		74
#define	NWA_SUBOP_UC_ADD		76
#define	NWA_SUBOP_UC_DELETE		77

/*
 * PORT_ATTR_GET returned value. INFERRED from the discovery layout: the value
 * word(s) begin at resp word[2] (byte 0x08), the same slot discovery uses for
 * port_count. *** UNVERIFIED on this attribute set. *** agnic_pport's link task
 * logs the raw response for the first polls so the offset can be confirmed
 * against a live cable toggle, and its link-state gate is fail-safe (ports are
 * born UP and only demoted when hw.agnic.link_gate is on AND a port has first
 * been seen UP), so a wrong offset can never wedge a working port DOWN.
 */
#define	NWA_GET_VALUE_OFF		0x08	/* resp word[2] -- VERIFY */
#define	NWA_PORT_ATTR_RET_WORDS		(2 + NWA_PORT_CNT_MAX * 2) /* hdr+stats */
/* Per-port MIB stats[] indices (NWA_ST_*) live in if_agnic.h (agnic_pport uses). */

/*
 * ALL_COMB_PORT_INFO (type 69) layout, verified against mv_nwa_host.h
 * struct comb_port_info_ret { u8 state@0; u32 speed@4; u64 stats[32]@8; }
 * __aligned(4) => 264 bytes = 66 u32 words per record. The response is
 * { nwa_msg_ret ret (marker@0, status@4); comb_port_info_ret port_info[]@8 }.
 * Request is { u32 type; u32 port_count; {u32 dev_id; u32 port_id}[port_count] }.
 */
#define	NWA_PINFO_REC_WORDS		66	/* 264-byte per-port record */
#define	NWA_PINFO_STATE_W		0	/* record word 0: u8 state (carrier) */
#define	NWA_PINFO_SPEED_W		1	/* record word 1: u32 Mbit/s         */
#define	NWA_PINFO_STATS_W		2	/* record word 2..: u64 stats[32]    */

/*
 * Per-port record flags (u16 at record offset 2, i.e. the upper half of the
 * first record word). mv_nwa_host.c nwa_create_pports(): MNG => a manageable
 * front-panel port (full ext ops), LAG => a link-aggregation port, neither =>
 * an internal port (CPU / inter-switch, no host xmit).
 */
#define	NWA_PORT_FLAG_MNG		0x0001	/* NWA_MNG_PORT_FLAG BIT(0) */
#define	NWA_PORT_FLAG_LAG		0x0002	/* NWA_LAG_PORT_FLAG BIT(1) */

/* Response layout. */
#define	NWA_RESP_MARKER			0x14	/* resp word[0] */
#define	NWA_RESP_STATUS_OFF		0x04	/* resp word[1] == 0 => OK */
#define	NWA_DISC_RESP_MAX		0x7E4	/* discovery response buffer */
#define	NWA_DISC_PORT_COUNT_OFF		0x08	/* resp word[2] */
#define	NWA_DISC_RECORDS_OFF		0x0C	/* 20-byte records start here */
#define	NWA_DISC_REC_SIZE		20
#define	NWA_DISC_VERSION_OFF		0x7E0	/* resp: (v & ~0x10001)==0xAAAA0001 */
#define	NWA_DISC_VERSION_MASK		0xFFFEFFFFU
#define	NWA_DISC_VERSION_VAL		0xAAAA0001U

#define	NWA_STATUS_RETRY		0x205	/* firmware "not ready, retry" */

/* Poll budgets (ms). */
#define	NWA_OWN_IDLE_MS			1000
#define	NWA_RESP_MS			5000
#define	NWA_READY_ROUNDS		12	/* dump+wait rounds for the magic */
#define	NWA_READY_STEP_MS		2000	/* per round -> up to ~24 s total */

static MALLOC_DEFINE(M_AGNIC_NWA, "agnic_nwa", "agnic NW_AGENT mailbox");

/* window-relative 32-bit MMIO accessors on the NW_AGENT BAR. */
static __inline uint32_t
nwa_rd(struct agnic_softc *sc, uint32_t rel)
{
	return (AGNIC_RD4(sc, sc->nwa_bar, sc->nwa_win_off + rel));
}
static __inline void
nwa_wr(struct agnic_softc *sc, uint32_t rel, uint32_t v)
{
	AGNIC_WR4(sc, sc->nwa_bar, sc->nwa_win_off + rel, v);
}

/*
 * One mailbox transaction. req/resp are host-order u32 arrays. Returns 0 on a
 * received response (resp filled up to resp_max words), ETIMEDOUT on no
 * response. Does NOT interpret the response marker/status (caller's job).
 * Init-time only for now (single-threaded); a lock is added when runtime
 * per-port up/down lands.
 */
static int
agnic_nwa_cmd(struct agnic_softc *sc, const uint32_t *req, int req_words,
    uint32_t *resp, int resp_max_words)
{
	uint32_t ctrl = sc->nwa_ctrl_off;
	uint32_t pay = sc->nwa_payload_off;
	uint32_t req_len = (uint32_t)req_words * 4;
	uint32_t resp_len, roff;
	int i, rw, error = 0;

	if (sc->nwa_sx_inited)
		sx_xlock(&sc->nwa_sx);

	/* 1. wait ownership==0 (host may write). */
	if (agnic_poll(sc, sc->nwa_bar, sc->nwa_win_off + ctrl + NWA_REG_OWNERSHIP,
	    0xFFFFFFFFU, 0, NWA_OWN_IDLE_MS, "nwa own-idle") != 0) {
		error = ETIMEDOUT;
		goto out;
	}

	/* 2. write request words into the payload buffer, then req_len. */
	for (i = 0; i < req_words; i++)
		nwa_wr(sc, pay + (uint32_t)i * 4, req[i]);
	nwa_wr(sc, ctrl + NWA_REG_REQ_LEN, req_len);
	bus_barrier(sc->bar[sc->nwa_bar], 0, sc->bar_size[sc->nwa_bar],
	    BUS_SPACE_BARRIER_WRITE);

	/* 3. submit. */
	nwa_wr(sc, ctrl + NWA_REG_DOORBELL, NWA_DBELL_SUBMIT);

	/* 4. wait ownership==1 (response ready). */
	if (agnic_poll(sc, sc->nwa_bar, sc->nwa_win_off + ctrl + NWA_REG_OWNERSHIP,
	    0xFFFFFFFFU, 1, NWA_RESP_MS, "nwa resp") != 0) {
		/* leave ownership; do not ack a response we never got. */
		error = ETIMEDOUT;
		goto out;
	}

	/* 5. read resp_len + the response words (response follows the request). */
	resp_len = nwa_rd(sc, ctrl + NWA_REG_RESP_LEN);
	roff = pay + ((req_len + 3u) & ~3u);
	rw = (int)(resp_len / 4);
	if (rw > resp_max_words)
		rw = resp_max_words;
	for (i = 0; i < rw; i++)
		resp[i] = nwa_rd(sc, roff + (uint32_t)i * 4);

	/* 6. ack. */
	nwa_wr(sc, ctrl + NWA_REG_DOORBELL, NWA_DBELL_ACK);
out:
	if (sc->nwa_sx_inited)
		sx_xunlock(&sc->nwa_sx);
	return (error);
}

/*
 * One ATTR-SET transaction (mirrors mv_nwa_host.c nwa_send_req_common): message
 * = [type=ATTR_SET @0][attr @4][port_id @8][dev_id=0 @0xC][param @0x10]. The
 * param bytes are copied verbatim (SFOS fills a union at the same offset:
 * state u8 / mtu u32 / mac_addr[6] / promisc u8 ...). Serialized by nwa_sx (the
 * NW_AGENT mailbox is single-outstanding) once the pport ioctl path can call
 * these concurrently with each other. Accepted iff resp marker 0x14 + status 0.
 */
static int
agnic_nwa_attr_set(struct agnic_softc *sc, uint32_t port_id, uint32_t attr,
    const void *param, size_t plen)
{
	uint32_t req[NWA_REQ_LEN / 4];
	uint32_t resp[8];
	int error;

	if (plen > NWA_REQ_LEN - NWA_MSG_VALUE)
		return (EINVAL);
	bzero(req, sizeof(req));
	req[NWA_MSG_TYPE / 4] = NWA_TYPE_ATTR_SET;
	req[NWA_MSG_SUBOP / 4] = attr;
	req[NWA_MSG_PORT_ID / 4] = port_id;
	if (param != NULL && plen > 0)
		memcpy((uint8_t *)req + NWA_MSG_VALUE, param, plen);

	bzero(resp, sizeof(resp));
	error = agnic_nwa_cmd(sc, req, NWA_REQ_LEN / 4, resp, nitems(resp));
	if (error != 0)
		return (error);
	if (resp[0] != NWA_RESP_MARKER || resp[1] != 0) {
		device_printf(sc->dev,
		    "P4a: port %u attr %u set rejected (marker 0x%x status 0x%x)\n",
		    port_id, attr, resp[0], resp[1]);
		return (EIO);
	}
	return (0);
}

/*
 * One ATTR-GET transaction (mirrors mv_nwa_host.c nwa_send_req_resp_common):
 * message = [type=ATTR_GET @0][attr @4][port_id @8][dev_id=0 @0xC][param=0 @0x10].
 * The firmware writes the reply after the request in the shared buffer;
 * agnic_nwa_cmd returns it as host-order words. resp[0] must be the 0x14 marker
 * and resp[1] the status (0=OK); the returned value(s) begin at
 * resp[NWA_GET_VALUE_OFF/4] (= resp[2]). Serialized by nwa_sx inside
 * agnic_nwa_cmd. The caller sizes resp for the attribute (3 words for a scalar,
 * NWA_PORT_ATTR_RET_WORDS for the stats block); resp is left filled on success.
 */
static int
agnic_nwa_attr_get(struct agnic_softc *sc, uint32_t port_id, uint32_t attr,
    uint32_t *resp, int resp_max_words)
{
	uint32_t req[NWA_REQ_LEN / 4];
	int error;

	if (resp_max_words < 3)		/* need marker + status + >=1 value */
		return (EINVAL);
	bzero(req, sizeof(req));
	req[NWA_MSG_TYPE / 4] = NWA_TYPE_ATTR_GET;
	req[NWA_MSG_SUBOP / 4] = attr;
	req[NWA_MSG_PORT_ID / 4] = port_id;

	error = agnic_nwa_cmd(sc, req, NWA_REQ_LEN / 4, resp, resp_max_words);
	if (error != 0)
		return (error);
	if (resp[0] != NWA_RESP_MARKER || resp[1] != 0) {
		static int rej_budget = 24;	/* rate-limit: the poll retries */

		if (rej_budget > 0) {
			rej_budget--;
			device_printf(sc->dev, "P4c: port %u attr %u get rejected "
			    "(marker 0x%x status 0x%x)%s\n", port_id, attr,
			    resp[0], resp[1],
			    rej_budget == 0 ? " [further rejects silenced]" : "");
		}
		return (EIO);
	}
	return (0);
}

/* ATTR-SET admin-state to a port. state 1=up, 0=down. */
static int
agnic_nwa_port_admin(struct agnic_softc *sc, uint32_t port_id, uint8_t state)
{

	return (agnic_nwa_attr_set(sc, port_id, NWA_SUBOP_ADMIN_STATE,
	    &state, sizeof(state)));
}

/*
 * Register the host's source MAC for a port (ATTR-SET attr 3). SFOS's
 * pport_dev_open() does mac_set BEFORE state-up: the NPU/88E6193X needs the
 * host MAC per port to accept egress FROM it (RX works regardless via trunk
 * promisc; host->front egress does not until the MAC is registered).
 */
static int
agnic_nwa_port_mac_set(struct agnic_softc *sc, uint32_t port_id,
    const uint8_t *mac)
{

	return (agnic_nwa_attr_set(sc, port_id, NWA_SUBOP_MAC, mac, 6));
}

/* ---- Per-port ops driven by the pport ifnet ioctl path (agnic_pport.c). ---- */

/* Set the front-panel port MTU (param = u32, host order). */
int
agnic_nwa_port_mtu_set(struct agnic_softc *sc, uint32_t port_id, uint32_t mtu)
{

	return (agnic_nwa_attr_set(sc, port_id, NWA_SUBOP_MTU, &mtu, sizeof(mtu)));
}

/* Enable/disable promiscuous mode on a front-panel port. */
int
agnic_nwa_port_promisc(struct agnic_softc *sc, uint32_t port_id, int on)
{
	uint8_t v = on ? 1 : 0;

	return (agnic_nwa_attr_set(sc, port_id, NWA_SUBOP_PROMISC, &v, 1));
}

/* Enable/disable all-multicast on a front-panel port. */
int
agnic_nwa_port_allmulti(struct agnic_softc *sc, uint32_t port_id, int on)
{
	uint8_t v = on ? 1 : 0;

	return (agnic_nwa_attr_set(sc, port_id, NWA_SUBOP_ALLMULTI, &v, 1));
}

/* Add/remove a multicast MAC filter entry on a front-panel port. */
int
agnic_nwa_port_mc(struct agnic_softc *sc, uint32_t port_id, const uint8_t *mac,
    int add)
{

	return (agnic_nwa_attr_set(sc, port_id,
	    add ? NWA_SUBOP_MC_ADD : NWA_SUBOP_MC_DELETE, mac, 6));
}

/* Add/remove a secondary unicast MAC filter entry on a front-panel port. */
int
agnic_nwa_port_uc(struct agnic_softc *sc, uint32_t port_id, const uint8_t *mac,
    int add)
{

	return (agnic_nwa_attr_set(sc, port_id,
	    add ? NWA_SUBOP_UC_ADD : NWA_SUBOP_UC_DELETE, mac, 6));
}

/* ---- Per-port GET ops driven by the pport link poll (agnic_pport.c). ---- */

/*
 * GET the front-panel operational (carrier) state via OPER_STATE(1): u8 @0x08.
 * This is LINK/oper state, NOT admin-state -- discovery admin-up'd every
 * manageable port, so ADMIN_STATE(0) would read 1 unconditionally and never
 * reflect a dead PHY. The value offset is unverified (see NWA_GET_VALUE_OFF);
 * the caller logs the raw reply on first polls and gates link changes safely.
 */
int
agnic_nwa_port_oper_get(struct agnic_softc *sc, uint32_t port_id, int *up)
{
	static int log_budget = 20;	/* ~2 poll rounds of 9 ports */
	uint32_t resp[3];
	int error;

	bzero(resp, sizeof(resp));
	error = agnic_nwa_attr_get(sc, port_id, NWA_SUBOP_OPER_STATE, resp,
	    nitems(resp));
	if (error != 0)
		return (error);
	if (log_budget > 0) {
		log_budget--;
		device_printf(sc->dev, "P4c: oper GET port 0x%04x resp[0..2]="
		    "%08x %08x %08x => up=%d (VERIFY offset vs a cable toggle)\n",
		    port_id, resp[0], resp[1], resp[2],
		    (resp[NWA_GET_VALUE_OFF / 4] & 0xFF) != 0);
	}
	*up = (resp[NWA_GET_VALUE_OFF / 4] & 0xFF) != 0;
	return (0);
}

/* GET link speed in Mbit/s (SPEED(4), u32 @0x08; 0/0xFFFFFFFF => unknown). */
int
agnic_nwa_port_speed_get(struct agnic_softc *sc, uint32_t port_id,
    uint32_t *mbps)
{
	uint32_t resp[3];
	int error;

	bzero(resp, sizeof(resp));
	error = agnic_nwa_attr_get(sc, port_id, NWA_SUBOP_SPEED, resp,
	    nitems(resp));
	if (error != 0)
		return (error);
	*mbps = resp[NWA_GET_VALUE_OFF / 4];
	return (0);
}

/* GET duplex (DUPLEX(13), u8 @0x08: 0=half, 1=full, 0xFF=unknown). */
int
agnic_nwa_port_duplex_get(struct agnic_softc *sc, uint32_t port_id, int *full)
{
	uint32_t resp[3];
	int error;

	bzero(resp, sizeof(resp));
	error = agnic_nwa_attr_get(sc, port_id, NWA_SUBOP_DUPLEX, resp,
	    nitems(resp));
	if (error != 0)
		return (error);
	*full = ((resp[NWA_GET_VALUE_OFF / 4] & 0xFF) == 0x01);
	return (0);
}

/*
 * GET the 32-entry per-port MIB block (STATS(14)). Reply: 8-byte header then
 * u64 stats[32] starting at @0x08, i.e. resp[2 + 2*i] (low) |
 * resp[3 + 2*i]<<32 (high). agnic_nwa_cmd returns each 32-bit LE word host-order.
 */
int
agnic_nwa_port_stats_get(struct agnic_softc *sc, uint32_t port_id,
    uint64_t stats[NWA_PORT_CNT_MAX])
{
	uint32_t resp[NWA_PORT_ATTR_RET_WORDS];
	int error, i, base = NWA_GET_VALUE_OFF / 4;	/* = 2 */

	bzero(resp, sizeof(resp));
	error = agnic_nwa_attr_get(sc, port_id, NWA_SUBOP_STATS, resp,
	    nitems(resp));
	if (error != 0)
		return (error);
	for (i = 0; i < NWA_PORT_CNT_MAX; i++)
		stats[i] = (uint64_t)resp[base + 2 * i] |
		    ((uint64_t)resp[base + 2 * i + 1] << 32);
	return (0);
}

/*
 * Bulk ALL_COMB_PORT_INFO (type 69): one transaction returns carrier state +
 * link speed + the 32-entry MIB for EVERY requested port. This is the exact
 * periodic source SFOS uses (nwa_get_ports_comb_port_info / pors_info_task): a
 * request { type=69, port_count, {dev_id=0, port_id=tag}[nports] } and a reply
 * { ret(marker,status), comb_port_info_ret[nports] } with each 264-byte record
 * = { u8 state(non-zero=link up); u32 speed(Mbit/s); u64 stats[32] }. Fills the
 * caller's per-port arrays (indexed by request order). Serialized by nwa_sx in
 * agnic_nwa_cmd. Preferred over the per-attr GETs (one txn, and OPER_STATE(1) is
 * rejected by the generic-NIC NPU firmware).
 */
int
agnic_nwa_ports_info_get(struct agnic_softc *sc, const uint16_t *tags,
    int nports, uint8_t *states, uint32_t *speeds,
    uint64_t stats[][NWA_PORT_CNT_MAX])
{
	static int rej_budget = 8;
	uint32_t req[2 + AGNIC_MAX_PORTS * 2];
	uint32_t *resp;
	int error, i, j, base, req_words, resp_words;

	if (nports <= 0 || nports > AGNIC_MAX_PORTS)
		return (EINVAL);
	req_words = 2 + nports * 2;
	resp_words = 2 + nports * NWA_PINFO_REC_WORDS;

	bzero(req, sizeof(req));
	req[0] = NWA_TYPE_ALL_COMB_PORT_INFO;
	req[1] = (uint32_t)nports;
	for (i = 0; i < nports; i++) {
		req[2 + i * 2] = 0;			/* dev_id */
		req[2 + i * 2 + 1] = tags[i];		/* port_id (tag) */
	}

	resp = malloc((size_t)resp_words * 4, M_AGNIC_NWA, M_WAITOK | M_ZERO);
	error = agnic_nwa_cmd(sc, req, req_words, resp, resp_words);
	if (error != 0)
		goto out;
	if (resp[0] != NWA_RESP_MARKER || resp[1] != 0) {
		if (rej_budget > 0) {
			rej_budget--;
			device_printf(sc->dev, "P4c: ALL_COMB_PORT_INFO rejected "
			    "(marker 0x%x status 0x%x)%s\n", resp[0], resp[1],
			    rej_budget == 0 ? " [silenced]" : "");
		}
		error = EIO;
		goto out;
	}
	for (i = 0; i < nports; i++) {
		base = 2 + i * NWA_PINFO_REC_WORDS;
		states[i] = (uint8_t)(resp[base + NWA_PINFO_STATE_W] & 0xFF);
		speeds[i] = resp[base + NWA_PINFO_SPEED_W];
		for (j = 0; j < NWA_PORT_CNT_MAX; j++)
			stats[i][j] =
			    (uint64_t)resp[base + NWA_PINFO_STATS_W + 2 * j] |
			    ((uint64_t)resp[base + NWA_PINFO_STATS_W + 2 * j + 1]
			    << 32);
	}
out:
	free(resp, M_AGNIC_NWA);
	return (error);
}

/*
 * Bring up the NW_AGENT mailbox: confirm the window, run discovery, and admin-up
 * every discovered port so the front-panel PHYs light. Heavily logged: the first
 * successful run confirms the config-descriptor layout on our own hardware.
 */
int
agnic_nwa_bringup(struct agnic_softc *sc)
{
	device_t dev = sc->dev;
	uint32_t *resp;
	uint32_t magic, cookie, port_count;
	int i, error;

	sc->nwa_bar = sc->fac_bar[AGNIC_FAC_NW_AGENT];
	sc->nwa_win_off = sc->fac_off[AGNIC_FAC_NW_AGENT];
	if (sc->nwa_bar < 0) {
		device_printf(dev, "P4a: NW_AGENT facility not present; "
		    "front-panel ports unavailable\n");
		return (ENXIO);
	}
	device_printf(dev, "P4a: NW_AGENT @BAR%d+0x%x\n", sc->nwa_bar,
	    sc->nwa_win_off);

	/*
	 * Wait for the device-ready magic, re-dumping the window header each
	 * round so we can watch the NPU publish the mailbox (it may take a few
	 * seconds after the trunk is enabled).
	 */
	for (i = 0; i < NWA_READY_ROUNDS; i++) {
		int j;

		device_printf(dev, "P4a: window[%d]:", i);
		for (j = 0; j < 16; j++)
			printf(" %08x", nwa_rd(sc, (uint32_t)j * 4));
		printf("\n");
		if (nwa_rd(sc, NWA_CFG_READY_MAGIC_OFF) == NWA_CFG_READY_MAGIC)
			break;
		pause("nwardy", NWA_READY_STEP_MS * hz / 1000);
	}
	if (nwa_rd(sc, NWA_CFG_READY_MAGIC_OFF) != NWA_CFG_READY_MAGIC) {
		magic = nwa_rd(sc, NWA_CFG_READY_MAGIC_OFF);
		device_printf(dev, "P4a: no CAFEBABE ready magic after %d s "
		    "(got 0x%08x); abort\n",
		    (NWA_READY_ROUNDS * NWA_READY_STEP_MS) / 1000, magic);
		return (ENXIO);
	}
	if (agnic_poll(sc, sc->nwa_bar,
	    sc->nwa_win_off + NWA_CFG_COOKIE_OFF, 0xFFU, NWA_CFG_COOKIE,
	    NWA_READY_STEP_MS, "nwa cfg cookie") != 0)
		device_printf(dev, "P4a: WARN cfg cookie != 0x34 (got 0x%08x); "
		    "continuing with dumped layout\n",
		    nwa_rd(sc, NWA_CFG_COOKIE_OFF));

	magic = nwa_rd(sc, NWA_CFG_READY_MAGIC_OFF);
	cookie = nwa_rd(sc, NWA_CFG_COOKIE_OFF);
	sc->nwa_ctrl_off = NWA_CTRL_OFF;
	sc->nwa_cmd_len = nwa_rd(sc, NWA_CFG_CMDLEN_OFF);
	sc->nwa_payload_off = nwa_rd(sc, NWA_CFG_PAYLOAD_OFF_OFF);
	device_printf(dev,
	    "P4a: cfg magic 0x%08x cookie 0x%x => ctrl_off 0x%x cmd_len 0x%x "
	    "payload_off 0x%x\n", magic, cookie, sc->nwa_ctrl_off,
	    sc->nwa_cmd_len, sc->nwa_payload_off);
	if (sc->nwa_payload_off == 0 ||
	    sc->nwa_payload_off >= NPU_NWA_WIN_SIZE) {
		device_printf(dev, "P4a: implausible mailbox layout; abort "
		    "(fix offsets from the window dump above)\n");
		return (EINVAL);
	}

	/* Discovery: req = {u32 type=1}. */
	resp = malloc(NWA_DISC_RESP_MAX + 16, M_AGNIC_NWA, M_WAITOK | M_ZERO);
	{
		uint32_t req = NWA_TYPE_DISCOVERY;

		error = agnic_nwa_cmd(sc, &req, 1, resp,
		    (NWA_DISC_RESP_MAX / 4));
	}
	if (error != 0) {
		device_printf(dev, "P4a: discovery: no response (err %d)\n",
		    error);
		free(resp, M_AGNIC_NWA);
		return (error);
	}
	device_printf(dev,
	    "P4a: discovery resp[0..3]=%08x %08x %08x %08x version[0x7e0]=%08x\n",
	    resp[0], resp[1], resp[2], resp[3],
	    resp[NWA_DISC_VERSION_OFF / 4]);
	if (resp[0] != NWA_RESP_MARKER || resp[1] != 0) {
		device_printf(dev, "P4a: discovery rejected (marker 0x%x "
		    "status 0x%x)\n", resp[0], resp[1]);
		free(resp, M_AGNIC_NWA);
		return (EIO);
	}

	port_count = resp[NWA_DISC_PORT_COUNT_OFF / 4];
	device_printf(dev, "P4a: NPU reports %u front-panel port(s)\n",
	    port_count);
	if (port_count == 0 || port_count > 32) {
		device_printf(dev, "P4a: implausible port_count; abort\n");
		free(resp, M_AGNIC_NWA);
		return (EINVAL);
	}

	/*
	 * Walk the discovery records (SFOS struct nwa_msg_port, 20 bytes each):
	 *   [+0] tag(u16)  [+2] flags(u16)  [+4] max_mtu(u16)  ...
	 * Classify by flags exactly as SFOS nwa_create_pports(): LAG -> lag ops,
	 * MNG -> ext (full mgmt) ops, else internal (read-only) ops. Only MNG
	 * front-panel ports get a host MAC + admin-up; internal/LAG ports have no
	 * state_set/mac_set in SFOS, so admin-up'ing them was our bug. Cache the
	 * manageable ports so the pport ioctl path can drive per-port mtu/promisc/
	 * filters against the real tag + NPU max_mtu.
	 */
	sc->nwa_nports = 0;
	for (i = 0; i < (int)port_count; i++) {
		uint32_t recoff = (NWA_DISC_RECORDS_OFF +
		    (uint32_t)i * NWA_DISC_REC_SIZE);
		uint32_t w0 = resp[recoff / 4];
		uint32_t w1 = resp[recoff / 4 + 1];
		uint16_t tag = (uint16_t)(w0 & 0xFFFF);
		uint16_t flags = (uint16_t)((w0 >> 16) & 0xFFFF);
		uint16_t max_mtu = (uint16_t)(w1 & 0xFFFF);
		uint8_t portnum = (uint8_t)PPORT_REM_PORT_NUM(tag);
		int is_lag = (flags & NWA_PORT_FLAG_LAG) != 0;
		int is_mng = (flags & NWA_PORT_FLAG_MNG) && !is_lag;

		device_printf(dev,
		    "P4a:  port[%d] tag=0x%04x flags=0x%04x portnum=%u "
		    "max_mtu=%u => %s\n", i, tag, flags, portnum, max_mtu,
		    is_lag ? "LAG" : is_mng ? "front-panel(mng)" : "internal");

		if (is_mng && sc->nwa_nports < AGNIC_MAX_PORTS) {
			struct agnic_nwa_port *p = &sc->nwa_ports[sc->nwa_nports++];

			p->tag = tag;
			p->flags = flags;
			p->max_mtu = max_mtu ? max_mtu : ETHERMTU;
			p->portnum = portnum;
			p->mng = 1;
		}

		if (!is_mng)
			continue;	/* internal/LAG: no host state_set/mac_set */

		/*
		 * SFOS pport_dev_open() registers the host source MAC BEFORE
		 * state-up so the NPU/88E6193X switch accepts host->front egress
		 * from it. Use the synthetic per-port MAC 02:81:00:00:00:NN that
		 * agnic_pport TX sources (NN = physical port number).
		 */
		{
			uint8_t mac[6] = { 0x02, 0x81, 0x00, 0x00, 0x00,
			    portnum };

			if (agnic_nwa_port_mac_set(sc, tag, mac) == 0)
				device_printf(dev, "P4a:  Port%u (tag 0x%04x) "
				    "mac-set 02:81:00:00:00:%02x OK\n",
				    portnum, tag, portnum);
		}

		error = agnic_nwa_port_admin(sc, tag, 1);
		if (error == 0)
			device_printf(dev, "P4a:  Port%u (tag 0x%04x) admin UP "
			    "OK\n", portnum, tag);
	}

	/*
	 * P4c PROBE (one-shot): the NPU switch manager (UMSD_NPU/NetAgent) IS
	 * running and answers STATE(0)-GET via umsd_port_link_status_get -> Port
	 * Status bit 11. But its sw_port() decode treats a tag-formatted port_id
	 * (bit15 set, our 0x81pp) as "obsolete" and mis-resolves it, while a RAW
	 * physical port number (0..10) resolves directly. STATE(0) with the tag
	 * returned 0 for the cabled port1; try it with the RAW physnum too. If the
	 * raw form returns link=1 for a cabled port (resp[2]), the fix is purely
	 * driver-side: query link by physnum, not tag.
	 */
	for (i = 0; i < sc->nwa_nports; i++) {
		uint32_t rt[4], rp[4];
		uint16_t tag = sc->nwa_ports[i].tag;
		uint8_t pn = sc->nwa_ports[i].portnum;

		bzero(rt, sizeof(rt));
		bzero(rp, sizeof(rp));
		(void)agnic_nwa_attr_get(sc, tag, 0 /*STATE*/, rt, nitems(rt));
		(void)agnic_nwa_attr_get(sc, pn, 0 /*STATE*/, rp, nitems(rp));
		device_printf(dev, "P4c PROBE Port%u STATE(0) via tag0x%04x="
		    "%08x %08x %08x | via raw%u=%08x %08x %08x\n", pn, tag,
		    rt[0], rt[1], rt[2], pn, rp[0], rp[1], rp[2]);
	}

	sc->nwa_ready = 1;
	device_printf(dev, "[Phase 4a] NW_AGENT up; %d of %u port(s) manageable "
	    "front-panel, admin-up -- check front-panel LEDs\n",
	    sc->nwa_nports, port_count);
	free(resp, M_AGNIC_NWA);
	return (0);
}

/*
 * Look up a discovered manageable port by its NW_AGENT tag (host order). Used by
 * the pport ioctl path to validate a port and read its NPU-advertised max_mtu.
 */
struct agnic_nwa_port *
agnic_nwa_port_find(struct agnic_softc *sc, uint16_t tag)
{
	int i;

	for (i = 0; i < sc->nwa_nports; i++)
		if (sc->nwa_ports[i].tag == tag)
			return (&sc->nwa_ports[i]);
	return (NULL);
}
