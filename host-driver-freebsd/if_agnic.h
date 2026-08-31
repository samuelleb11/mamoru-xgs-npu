/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * if_agnic softc. Extended per phase:
 *   P1  BAR0/2/4 resources + bus handles, 36-bit busdma parent tag, MSI-X vectors
 *   P2a locate + validate the NPU barmap descriptor in BAR2
 *   P2b deferred CTRL-facility handshake: resolve facility windows, ctrl cookie +
 *       TRGT_INIT poll, t2h MSI-X doorbell handlers, HOST_INIT, 1 Hz HOST_ALIVE
 *       heartbeat callout, GIU DEV_READY poll + firmware MAC readout
 *   P3a mgmt CMD ring + NOTIF ring: busdma-alloc the two coherent 256x64B rings,
 *       publish their DMA addr + BAR0-relative prod/cons index offsets + len into
 *       config_mem cmd_q/notif_q, set HOST_MGMT_READY, wait DEV_MGMT_READY, latch
 *       the h2t mgmt doorbell from ctrl_map, and round-trip ONE safe command
 *       (CC_PF_MGMT_ECHO) end-to-end.
 *   P3+ datapath rings, if_t, media, locks
 *
 * The handshake is DEFERRED via config_intrhook so a not-ready NPU can never
 * hang boot, and detach tears everything down without ever triggering an FLR.
 */
#ifndef _IF_AGNIC_H_
#define	_IF_AGNIC_H_

#include <sys/_task.h>		/* struct task embedded in softc (P3b RX) */
#include <sys/socket.h>
#include <net/if.h>		/* if_t (embedded in softc, P3b)          */

#include "agnic_barmap.h"
#include "agnic_ctrl.h"
#include "agnic_giu.h"

struct agnic_softc;		/* fwd: agnic_dbell_vec back-points to it   */
struct mbuf;			/* fwd: only pointers used in this header   */

/* BAR index -> PCI rid mapping: 0=BAR0(0x10), 1=BAR2(0x18), 2=BAR4(0x20). */
#define	AGNIC_NBARS	3

/*
 * 36-bit DMA ceiling: the EP's inbound ATU covers only 16 x 4GB = 64GB. Shared
 * by the parent tag (P1) and the per-ring mgmt tags (P3a); defined here so both
 * if_agnic.c and agnic_mgmt.c agree on the exact value.
 */
#define	AGNIC_DMA_LOWADDR	0xFFFFFFFFFULL

/*
 * One target->host doorbell MSI-X vector. For P2b the handler is a stub that
 * only counts interrupts; msix_id == vector index == IRQ rid-1.
 */
struct agnic_dbell_vec {
	struct resource	       *res;	/* SYS_RES_IRQ */
	void		       *tag;	/* bus_setup_intr cookie */
	int			rid;	/* vector index + 1 */
	int			idx;	/* vector index (0..N-1); P3b RX == 1 */
	struct agnic_softc     *sc;	/* back-pointer for the P3b RX kick   */
	volatile uint64_t	count;	/* interrupts seen */
};

/* ------------------------------------------------------------------------- */
/* Phase 3a: management command / notification ring ABI + state.             */
/* ------------------------------------------------------------------------- */

/*
 * The ONE 64-byte descriptor used by BOTH the mgmt cmd ring and the mgmt notif
 * ring (giu_nic_hw.h struct agnic_cmd_desc, #pragma pack(1)). On amd64 LE this
 * maps byte-identically to the device ABI; no swapping. On the cmd side data[]
 * carries serialized params, on the notif side it carries the response
 * (agnic_mgmt_cmd_resp: data[0] == status) or, when cmd_idx == 0xFFFF, an
 * async notification.
 */
struct agnic_cmd_desc {
	uint16_t	cmd_idx;	/* @0x00 correlation tag / 0xFFFF=notif */
	uint16_t	app_code;	/* @0x02 AC_PF_MANAGER on host cmds     */
	uint8_t		cmd_code;	/* @0x04 agnic_cmd_codes / notif_codes  */
	uint8_t		client_id;	/* @0x05 PF/VF id (0 for PF)            */
	uint8_t		client_type;	/* @0x06 CDT_PF=1                        */
	uint8_t		flags;		/* @0x07 BUF_POS/NO_RESP/NUM_EXT bits    */
	uint8_t		data[AGNIC_MGMT_DESC_DATA_LEN];	/* @0x08, 56 bytes */
} __packed;

#ifdef CTASSERT
CTASSERT(sizeof(struct agnic_cmd_desc) == AGNIC_MGMT_DESC_SIZE);	/* 64 */
#endif

/* desc.app_code (enum agnic_app_codes). Host uses AC_PF_MANAGER on outgoing. */
#define	AGNIC_AC_HOST_AGNIC_NETDEV	0x1
#define	AGNIC_AC_PF_MANAGER		0x2

/* desc.client_type (enum agnic_cmd_dest_type). */
#define	AGNIC_CDT_INVALID		0
#define	AGNIC_CDT_PF			1
#define	AGNIC_CDT_VF			2
#define	AGNIC_CDT_CUSTOM		3

/* desc.flags byte (offset 7): BUF_POS_SINGLE(0) with/without NO_RESP(bit5). */
#define	AGNIC_DESC_FLAGS_SINGLE_RESP	0x00	/* single desc, response wanted */
#define	AGNIC_DESC_FLAGS_SINGLE_NORESP	0x20	/* single desc, fire-and-forget */

/* Reserved cmd_idx values (never used as a real correlation tag). */
#define	AGNIC_CMD_ID_ILLEGAL		0x0000
#define	AGNIC_CMD_ID_NOTIFICATION	0xFFFF

/* Command opcodes (enum agnic_cmd_codes) exercised by P3a bring-up. */
#define	AGNIC_CC_PF_MGMT_ECHO		0x09	/* simplest safe round-trip     */
#define	AGNIC_CC_GET_CAPABILITIES	0x1E	/* read-only caps query         */

/* agnic_mgmt_cmd_resp.status (data[0] of a response desc). */
#define	AGNIC_NOTIF_STATUS_OK		0
#define	AGNIC_NOTIF_STATUS_FAIL		1

/* cmd_idx correlation/cookie space (AGNIC_CMD_Q_MAX_COOKIE_LEN). */
#define	AGNIC_CMD_COOKIE_COUNT		1024

/* Ring-index arithmetic for the power-of-2 mgmt rings (count == 256). */
#define	AGNIC_RING_INC(i, cnt)		(((i) + 1) & ((cnt) - 1))

/*
 * The mgmt cmd/notif producer & consumer index words live ON BAR0, in the
 * ring_indices_arr immediately after config_mem (base = config_mem.dev_use_size).
 * We claim 4 sequential u32 slots: cmd-prod, cmd-cons, notif-prod, notif-cons.
 */
#define	AGNIC_MGMT_IDX_SLOTS		4
#define	AGNIC_MGMT_SLOT_CMD_PROD	0
#define	AGNIC_MGMT_SLOT_CMD_CONS	1
#define	AGNIC_MGMT_SLOT_NOTIF_PROD	2
#define	AGNIC_MGMT_SLOT_NOTIF_CONS	3

/*
 * The h2t (host->target) doorbell packing (ctrl_map.h2t_dbell_msg[]) advances a
 * global vec_id only over facilities with num_h2t_dbells>0, enum order
 * CONTROL(0),MGMT_NETDEV(0),NW_AGENT(0),RPC(1),GIU(4). Hence index 0 is the RPC
 * mgmt/command doorbell (indices 1..4 = GIU data queues).
 */
#define	AGNIC_H2T_DBELL_MGMT		0

/* A single coherent busdma allocation (descriptor ring on host DRAM). */
struct agnic_dma_mem {
	bus_dma_tag_t		tag;
	bus_dmamap_t		map;
	void		       *vaddr;
	bus_addr_t		paddr;	/* < 64GB, 36-bit safe */
	bus_size_t		size;
};

/*
 * One management ring. cmd ring: host=producer / device=consumer. notif ring:
 * device=producer / host=consumer. The RO index for each side is kept local so
 * we never read it across PCIe: host reads notif-producer + cmd-consumer, host
 * writes cmd-producer + notif-consumer. All four index words are BAR0 MMIO.
 */
struct agnic_mgmt_ring {
	struct agnic_dma_mem	mem;		/* coherent descriptor ring     */
	uint32_t		count;		/* # descriptors (256)          */
	uint32_t		prod_shadow;	/* local producer index         */
	uint32_t		cons_shadow;	/* local consumer index         */
	uint32_t		prod_bar_off;	/* BAR0 byte off of prod idx wd */
	uint32_t		cons_bar_off;	/* BAR0 byte off of cons idx wd */
	uint32_t		pub_prod_off;	/* value published in q_prod_offs*/
	uint32_t		pub_cons_off;	/* value published in q_cons_offs*/
};

/* ------------------------------------------------------------------------- */
/* Phase 3b: ingress (RX) datapath ABI + state.                              */
/* ------------------------------------------------------------------------- */

/*
 * P3b command opcodes (extend the P3a ECHO/CAPS set; enum agnic_cmd_codes).
 * The bring-up order is INIT -> INGRESS_TC_ADD -> INGRESS_DATA_Q_ADD ->
 * INIT_DONE -> ENABLE; DISABLE is the fire-and-forget teardown poke.
 */
#define	AGNIC_CC_PF_INIT		0x01
#define	AGNIC_CC_PF_INIT_DONE		0x02
#define	AGNIC_CC_PF_EGRESS_TC_ADD	0x03
#define	AGNIC_CC_PF_EGRESS_DATA_Q_ADD	0x04
#define	AGNIC_CC_PF_INGRESS_TC_ADD	0x05
#define	AGNIC_CC_PF_INGRESS_DATA_Q_ADD	0x06
#define	AGNIC_CC_PF_ENABLE		0x07
#define	AGNIC_CC_PF_DISABLE		0x08
#define	AGNIC_CC_PF_LINK_STATUS		0x0A	/* resp: u32 link_status @resp[1] */
#define	AGNIC_CC_PF_PROMISC		0x0E	/* params[0]=1 enable / 0 disable */
#define	AGNIC_CC_PF_MTU			0x10
#define	AGNIC_CC_PF_LINK_INFO		0x19	/* resp: link_up,speed,duplex,phy */

/* Async notification codes (desc.cmd_code when cmd_idx == 0xFFFF). */
#define	AGNIC_NC_PF_LINK_CHANGE		0x01	/* data[0] = u32 link_status */
#define	AGNIC_NC_PF_KEEP_ALIVE		0x02	/* no ack; resets host watchdog */

/* enum agnic_egress_sched (pf_init.egress_sched, u8 @ params[12]). */
#define	AGNIC_ES_STRICT_SCHED		0x1
#define	AGNIC_ES_WRR_SCHED		0x2

/* enum agnic_ingress_hash_type (pf_ingress_tc_add.hash_type, u8 @ params[12]). */
#define	AGNIC_ING_HASH_TYPE_NONE	0x0
#define	AGNIC_ING_HASH_TYPE_2_TUPLE	0x1
#define	AGNIC_ING_HASH_TYPE_5_TUPLE	0x2

/* q_add response q_inf / bpool_q_inf low-bit status (data[1]/data[9]). */
#define	AGNIC_Q_INF_STATUS_OK		0
#define	AGNIC_Q_INF_STATUS_ERR		1

/* CC_GET_CAPABILITIES resp.flags. */
#define	AGNIC_CAPABILITIES_SG		(1U << 0)

/*
 * Params are serialized starting at desc->data[0]; the GPL caller always
 * transmits sizeof(union agnic_mgmt_cmd_params) == 48 bytes for every
 * param-bearing command (largest member is pf_ingress_data_q_add). We zero the
 * 48-byte area, fill the used prefix, and send all 48 so the tail is
 * deterministic 0 (the GPL code leaks stack garbage there; we do not).
 */
#define	AGNIC_MGMT_PARAMS_LEN		48

/* Datapath descriptor sizes (distinct from the 64B mgmt desc). */
#define	AGNIC_RXD_SIZE			32	/* sizeof(agnic_rx_desc)    */
#define	AGNIC_TXD_SIZE			32	/* sizeof(agnic_tx_desc)    */
#define	AGNIC_BPD_SIZE			16	/* sizeof(agnic_bpool_desc) */

/*
 * One ingress TC + one RX queue, AND one egress TC + one TX queue. The NPU
 * refuses CC_PF_ENABLE for an RX-only config (it accepts egress_tc=0 at INIT
 * and INIT_DONE, but will not bring the port live without a registered egress
 * queue). We therefore register a minimal egress TC/queue during bring-up even
 * though the TX datapath itself lands in P4. egress_dma_engines is 1 (from
 * CC_GET_CAPABILITIES), so num_queues == num_queues_per_dma == 1.
 */
#define	AGNIC_INGRESS_TCS		1
#define	AGNIC_EGRESS_TCS		1
#define	AGNIC_QS_PER_TC			1

/* Power-of-2 ring depths (RX ring depth <= bpool buffer count). */
#define	AGNIC_RX_RING_LEN		256
#define	AGNIC_TX_RING_LEN		256
#define	AGNIC_BP_RING_LEN		256

/*
 * BAR0 ring_indices_arr slot assignment. Mgmt already owns slots 0..3
 * (cmd-prod, cmd-cons, notif-prod, notif-cons); the datapath claims 6 more.
 * Device WRITES rx-prod + bpool-cons + tx-cons; host WRITES rx-cons +
 * bpool-prod + tx-prod (host is the TX producer, device the TX consumer).
 */
#define	AGNIC_DATA_SLOT_RX_PROD		4
#define	AGNIC_DATA_SLOT_RX_CONS		5
#define	AGNIC_DATA_SLOT_BP_PROD		6
#define	AGNIC_DATA_SLOT_BP_CONS		7
#define	AGNIC_DATA_SLOT_TX_PROD		8
#define	AGNIC_DATA_SLOT_TX_CONS		9
#define	AGNIC_DATA_IDX_SLOTS		6

/*
 * RX queue -> GIU t2h MSI-X doorbell vector index. The 4 GIU vectors are
 * dbell[1..4]; we map the single RX queue to vector index 1. NOTE: the GPL
 * source passes a GLOBAL msi id (mv_get_msi_id), a translation we have NOT
 * extracted -- so this value may be wrong and the RX MSI-X may never fire. The
 * 1 Hz poll callout is the reliable RX path; the doorbell is a bonus.
 */
#define	AGNIC_RX_DBELL_ID		1

/* Host headroom reserved before frame data in each buffer (0 for bring-up). */
#define	AGNIC_RX_HEADROOM		0

/*
 * DMA-reordering sentinels the GPL device/driver use to poison stale descs.
 * Cheap defensive guards in the RX reap loop; not required for correctness.
 */
#define	AGNIC_COOKIE_DEVICE_WATERMARK	0xcafecafeULL
#define	AGNIC_COOKIE_DRIVER_WATERMARK	0xdeaddeadULL

/*
 * agnic_rx_desc (giu_nic_hw.h, #pragma pack(1)): DEVICE-producer completion
 * descriptor written into the host RX ring. LE amd64 maps byte-identically.
 */
struct agnic_rx_desc {
	uint32_t	flags;			/* @0x00 l3/l4/csum bitfields   */
	uint8_t		pkt_offset;		/* @0x04 extra offset to data   */
	uint8_t		info;			/* @0x05 vlan/l2/l3 info        */
	uint16_t	byte_cnt;		/* @0x06 received frame length  */
	uint16_t	port_num;		/* @0x08                        */
	uint16_t	num_sg_ent;		/* @0x0A                        */
	uint32_t	timestamp_hashkey;	/* @0x0C RSS hash               */
	uint64_t	buffer_addr;		/* @0x10 bus addr we posted     */
	uint64_t	cookie;			/* @0x18 buff_cookie we posted  */
} __packed;

/*
 * agnic_bpool_desc (giu_nic_hw.h, #pragma pack(1)): HOST-producer free-buffer
 * descriptor pushed into the bpool ring to hand RX buffers to the device.
 */
struct agnic_bpool_desc {
	uint64_t	buff_addr_phys;		/* @0x00 bus addr for device DMA */
	uint64_t	buff_cookie;		/* @0x08 echoed into rx_desc.cookie */
} __packed;

/*
 * agnic_tx_desc (giu_nic_hw.h, #pragma pack(1)): HOST-producer egress descriptor
 * the device consumes to transmit. LE amd64 maps byte-identically.
 */
struct agnic_tx_desc {
	uint32_t	flags;			/* @0x00 format/csum bitfields  */
	uint8_t		pkt_offset;		/* @0x04 offset to frame data   */
	uint8_t		info;			/* @0x05 vlan/res               */
	uint16_t	byte_cnt;		/* @0x06 frame length           */
	uint16_t	res5;			/* @0x08                        */
	uint16_t	num_seg_ent;		/* @0x0A                        */
	uint32_t	res6;			/* @0x0C                        */
	uint64_t	buffer_addr;		/* @0x10 frame buffer bus addr  */
	uint64_t	cookie;			/* @0x18                        */
} __packed;

/* desc.flags: 3<<28 marks a single-segment (non-SG) packet. */
#define	AGNIC_TXD_FLAGS_SG_SINGLE_ENTRY	(3U << 28)
/*
 * csum-generation control (giu_nic_hw.h). Set BOTH to tell the device NOT to
 * regenerate checksums: we advertise no TX offload and the host already
 * checksummed the frame. Leaving them clear commands csum generation (flags==0
 * encodes "generate L3+L4"), which rewrites frame bytes and stalls the GIU.
 */
#define	AGNIC_TXD_FLAGS_GEN_L4_CSUM_NOT		(1U << 14)
#define	AGNIC_TXD_FLAGS_GEN_IPV4_CSUM_DIS	(1U << 15)

#ifdef CTASSERT
CTASSERT(sizeof(struct agnic_rx_desc) == AGNIC_RXD_SIZE);
CTASSERT(sizeof(struct agnic_bpool_desc) == AGNIC_BPD_SIZE);
CTASSERT(sizeof(struct agnic_tx_desc) == AGNIC_TXD_SIZE);
#endif

/*
 * Serialized CC_PF_INGRESS_DATA_Q_ADD (0x6) parameters. Field ORDER is fixed by
 * the ABI: q_phys, q_prod, q_cons, bpool_phys, bpool_prod, bpool_cons, q_len,
 * msix_id, tc, q_buf_size -- the bpool block sits BETWEEN the data-q offsets and
 * q_len/msix/tc. Passed to agnic_ingress_data_q_add_params() as host-order.
 */
struct agnic_ingress_q_cfg {
	uint64_t	q_phys;			/* RX ring desc-body host DMA phys */
	uint32_t	q_prod_offs;		/* BAR0-rel RX producer idx off */
	uint32_t	q_cons_offs;		/* BAR0-rel RX consumer idx off */
	uint64_t	bpool_phys;		/* bpool ring desc-body host DMA */
	uint32_t	bpool_prod_offs;	/* BAR0-rel bpool producer off  */
	uint32_t	bpool_cons_offs;	/* BAR0-rel bpool consumer off  */
	uint32_t	q_len;			/* # RX descriptors             */
	uint32_t	msix_id;		/* per-queue MSI-X id (0 = none) */
	uint32_t	tc;			/* traffic class                */
	uint32_t	q_buf_size;		/* bpool per-buffer byte size   */
};

/*
 * Serialized CC_PF_EGRESS_DATA_Q_ADD (0x4) parameters (also the BP/Tx queue
 * shape). Field ORDER is fixed by the ABI: q_phys, q_prod, q_cons, q_len,
 * q_wrr_weight, tc, msix_id -- NOTE this differs from the ingress q_add order
 * (no bpool block; q_len precedes wrr/tc/msix). Passed host-order.
 */
struct agnic_egress_q_cfg {
	uint64_t	q_phys;			/* TX ring desc-body host DMA phys */
	uint32_t	q_prod_offs;		/* BAR0-rel TX producer idx off */
	uint32_t	q_cons_offs;		/* BAR0-rel TX consumer idx off */
	uint32_t	q_len;			/* # TX descriptors             */
	uint32_t	q_wrr_weight;		/* 0 == strict prio             */
	uint32_t	tc;			/* traffic class                */
	uint32_t	msix_id;		/* per-queue MSI-X id (0 = none) */
};

/* Per-buffer host tracking, one per bpool slot (indexed by buff_cookie). */
struct agnic_rxbuf {
	struct mbuf	       *m;	/* cluster posted to device; NULL when free */
	bus_dmamap_t		map;	/* streaming map for the cluster            */
	bus_addr_t		paddr;	/* loaded segment addr                      */
};

/*
 * One datapath ring: the RX data ring (agnic_rx_desc[count], device-producer)
 * OR the bpool ring (agnic_bpool_desc[count], host-producer). buf_tag/rxb are
 * used by the bpool ring only.
 */
struct agnic_data_ring {
	struct agnic_dma_mem	mem;		/* coherent descriptor body     */
	uint32_t		count;		/* # descriptors (power of 2)   */
	uint32_t		prod_shadow;	/* local producer index         */
	uint32_t		cons_shadow;	/* local consumer index         */
	uint32_t		prod_bar_off;	/* BAR0 MMIO off of prod idx wd */
	uint32_t		cons_bar_off;	/* BAR0 MMIO off of cons idx wd */
	uint32_t		pub_prod_off;	/* config_mem-rel prod off (cmd)*/
	uint32_t		pub_cons_off;	/* config_mem-rel cons off (cmd)*/

	/* bpool ring only: */
	bus_dma_tag_t		buf_tag;	/* 36-bit, 1-seg, cluster-max   */
	struct agnic_rxbuf     *rxb;		/* rxb[count]                   */
	uint32_t		buf_size;	/* advertised q_buf_size        */
};

/*
 * One front-panel port descriptor, distilled from a discovery record
 * (SFOS nwa_msg_port / nwa_dev_port): tag is the NW_AGENT/pport port_id
 * (host order, e.g. 0x8100 = front Port1), portnum = PPORT_REM_PORT_NUM(tag)
 * = (tag & 0x3F00) >> 8. mng = manageable front-panel port (SFOS ext ops:
 * state/mac/mtu/promisc/filters); internal ports are read-only (int ops) and
 * are never admin-up'd or mac-set from the host.
 */
#define	AGNIC_MAX_PORTS		16
#define	PPORT_REM_PORT_NUM(t)	(((t) & 0x3F00) >> 8)	/* GPL port_tag.h  */

/*
 * pport trunk wire framing: every host<->NPU frame on the GIU is
 *   [ 2-byte tag 0x81pp ][ 64-byte PPORT header ][ ethernet frame ]
 * so the GIU frame carries PPORT_PREFIX (66) bytes ahead of the L2 frame.
 * The GIU frame size advertised at CC_PF_INIT must include this prefix, or a
 * full 1500-MTU frame (1514/1518 on the wire) + 66 overflows the GIU limit and
 * is clipped/wedged -- only sub-(limit-66) frames survive. (Shared by the TX/RX
 * demux in agnic_pport.c and the frame-size macro in agnic_txrx.c.)
 */
#define	PPORT_TAG_LEN		2		/* [0x81pp]                    */
#define	PPORT_HDR_LEN		64		/* opaque PPORT header         */
#define	PPORT_PREFIX		(PPORT_TAG_LEN + PPORT_HDR_LEN)	/* 66  */

struct agnic_nwa_port {
	uint16_t		tag;		/* NW_AGENT port_id (host order)   */
	uint16_t		flags;		/* NWA_PORT_FLAG_* bitmap          */
	uint16_t		max_mtu;	/* NPU-advertised max L2 MTU       */
	uint8_t			portnum;	/* physical front-panel number     */
	uint8_t			mng;		/* 1 = manageable front-panel port */
};

struct agnic_softc {
	device_t		dev;

	/* --- Phase 1: resource acquisition --- */
	struct resource	       *bar[AGNIC_NBARS];
	int			bar_rid[AGNIC_NBARS];
	bus_space_tag_t		bar_bt[AGNIC_NBARS];
	bus_space_handle_t	bar_bh[AGNIC_NBARS];
	bus_size_t		bar_size[AGNIC_NBARS];

	bus_dma_tag_t		parent_dmat;	/* 36-bit ceiling (EP iATU) */

	int			msix_count;	/* vectors allocated (0 = none) */

	/* --- Phase 2b: deferred NPU facility handshake --- */
	struct intr_config_hook	config_hook;	/* defer handshake off boot */
	int			config_hook_established;
	int			config_hook_done;
	int			p2b_inited;	/* hb_mtx/callout constructed */

	/* Facility windows resolved from the barmap: offset within OUR mapping. */
	int			fac_bar[AGNIC_FAC_COUNT];  /* AGNIC_BAR0/2, -1 absent */
	uint32_t		fac_off[AGNIC_FAC_COUNT];  /* byte offset in mapping  */

	/* Convenience copies for the two facilities P2b touches. */
	int			ctrl_bar;	/* CONTROL facility BAR index */
	uint32_t		ctrl_off;	/* CONTROL facility BAR2 offset */
	int			giu_bar;	/* GIU facility BAR index */
	uint32_t		giu_off;	/* GIU config_mem BAR0 offset */

	/* t2h doorbell MSI-X vectors (stub counters for now). */
	struct agnic_dbell_vec	dbell[AGNIC_N_T2H_DBELLS];
	int			dbell_nvec;	/* handlers actually armed */

	/* 1 Hz HOST_ALIVE heartbeat. */
	struct mtx		hb_mtx;
	struct callout		heartbeat;
	int			heartbeat_running;

	/* --- Phase 3a: management rings + one-shot command round-trip --- */
	struct sx		mgmt_lock;	/* serializes send+reap (sleepable:
						 * the reap poll loop sleeps, so this
						 * MUST be an sx, not a mtx) */
	int			mgmt_inited;	/* mgmt_lock constructed */
	int			mgmt_ready;	/* DEV_MGMT_READY + echo OK */
	uint32_t		dev_use_size;	/* config_mem.dev_use_size (BAR0) */
	struct agnic_mgmt_ring	cmd_ring;	/* host->device commands */
	struct agnic_mgmt_ring	notif_ring;	/* device->host responses/notifs */
	uint16_t		cmd_idx_gen;	/* rolling cmd_idx (1..1023) */

	/* h2t mgmt doorbell latched from ctrl_map (index 0 = RPC/mgmt). */
	int			h2t_dbell_valid;
	uint32_t		h2t_mgmt_bar4_off;	/* BAR4 byte offset */
	uint32_t		h2t_mgmt_data;		/* MSI data to writel */

	/* --- P3b: continuous notif-ring drainer (GPL MGMT_POLL equivalent) --- */
	struct mtx		mgmt_poll_mtx;	/* guards drain + the one waiter   */
	struct callout		mgmt_poll;	/* periodic notif drain            */
	int			mgmt_poll_inited;
	uint16_t		mgmt_wait_idx;	/* cmd_idx awaited (0 = none)      */
	int			mgmt_wait_done;	/* set by drainer on match         */
	uint8_t			mgmt_wait_status;
	uint8_t		       *mgmt_wait_buf;	/* caller response buffer          */
	size_t			mgmt_wait_len;
	uint64_t		mgmt_keepalive;	/* async keep-alives consumed      */

	/* --- Phase 3b: ingress (RX) datapath + ifnet --- */
	int			txrx_inited;	/* rx_mtx/callout/tq constructed */
	if_t			ifp;		/* the FreeBSD ethernet interface */
	uint8_t			mac[6];		/* firmware MAC (GIU, at DEV_READY) */
	int			if_running;	/* port ENABLEd + RX servicing on  */
	struct mtx		rx_mtx;		/* guards RX drain + bpool refill  */
	struct callout		rx_poll;	/* 1 Hz poll-mode RX servicing     */
	struct taskqueue       *rx_tq;		/* MSI-X-mode RX servicing         */
	struct task		rx_task;	/* enqueued by the RX doorbell     */
	struct agnic_data_ring	rx_ring;	/* device->host RX completions     */
	struct agnic_data_ring	bp_ring;	/* host->device free buffers       */
	struct agnic_data_ring	tx_ring;	/* host->device egress (P4 TX)     */
	struct agnic_dma_mem	tx_bufs;	/* coherent TX copy buffers        */
	struct mtx		tx_mtx;		/* serialises trunk TX             */
	int			tx_inited;	/* tx_mtx constructed              */
	uint32_t		max_buf_size;	/* CC_GET_CAPABILITIES.max_buf_size */
	uint16_t		host_headroom;	/* bytes before frame data (0)     */
	uint64_t		rx_frames;	/* frames handed to the stack      */
	uint64_t		rx_dropped;	/* torn/anomalous descs skipped    */
	int			rx_first_done;	/* first-frame detail logged       */
	uint64_t		tx_packets;	/* frames enqueued on the trunk    */
	uint64_t		tx_dropped;	/* TX ring-full / oversized drops  */
	uint64_t		rx_dbell_count;	/* RX MSI-X doorbells serviced     */
	int			tx_dbg;		/* remaining first-TX dumps to log */
	int			rx_dbg;		/* remaining large-RX dumps to log */

	/*
	 * pport TX format experimentation (runtime-tunable via sysctl). The NPU
	 * drops our egress frames -> the 64-byte pport header content matters.
	 * rx_last_hdr snapshots a real NPU-produced RX header so TX can replay the
	 * exact bytes. tx_hdr_mode: 0=zeros, 1=copy rx_last_hdr, 2=tx_hdr_magic@0.
	 * tx_pkt_offset: descriptor pkt_offset (0 default; try 64 = LIF dflt).
	 */
	uint8_t			rx_last_hdr[64];/* last 64B pport header seen on RX */
	int			rx_last_hdr_valid;
	int			tx_hdr_mode;	/* 0=zeros 1=copy-RX 2=magic       */
	uint64_t		tx_hdr_magic;	/* 8B written LE at header[0]       */
	int			tx_pkt_offset;	/* descriptor pkt_offset override  */

	/* Link state from NC_PF_LINK_CHANGE / CC_PF_LINK_STATUS (diagnostic). */
	int			link_up;	/* last reported link state        */
	uint64_t		link_changes;	/* NC_PF_LINK_CHANGE notifs seen   */

	/* --- Phase 4a: NW_AGENT mailbox (physical front-panel port control) --- */
	int			nwa_ready;	/* mailbox up + ports discovered   */
	int			nwa_bar;	/* facility BAR (== BAR0)          */
	uint32_t		nwa_win_off;	/* NW_AGENT window base on the BAR */
	uint32_t		nwa_ctrl_off;	/* window-rel ctrl register block  */
	uint32_t		nwa_payload_off;/* window-rel request/resp payload */
	uint32_t		nwa_cmd_len;	/* cmd_mbox_len from cfg descriptor*/

	/*
	 * NW_AGENT mailbox is single-outstanding (SFOS nwa_mailbox.cmd_mtx):
	 * agnic_nwa_cmd sleeps in agnic_poll, so serialize with a sleepable sx.
	 * nwa_sx_inited gates locking during early bring-up (before sx_init).
	 */
	struct sx		nwa_sx;		/* serializes agnic_nwa_cmd         */
	int			nwa_sx_inited;
	struct agnic_nwa_port	nwa_ports[AGNIC_MAX_PORTS];
	int			nwa_nports;	/* # manageable front-panel ports  */

	/* --- Phase 5: mvmgmt0 management link (opaque state) --- */
	void		       *pcinet;		/* struct agnic_pcinet *          */

	/* --- Phase 4b: pport demux -> per-front-panel-port ifnets --- */
	void		       *pport;		/* struct agnic_pport *           */
};

/* NW_AGENT facility window size (GPL barmap.h: 64 KB). */
#define	NPU_NWA_WIN_SIZE	(64u * 1024u)

/* Convenience register accessors (BAR-relative, 32-bit little-endian MMIO). */
#define	AGNIC_RD4(sc, b, o)	bus_space_read_4((sc)->bar_bt[b], (sc)->bar_bh[b], (o))
#define	AGNIC_WR4(sc, b, o, v)	bus_space_write_4((sc)->bar_bt[b], (sc)->bar_bh[b], (o), (v))
#define	AGNIC_RD1(sc, b, o)	bus_space_read_1((sc)->bar_bt[b], (sc)->bar_bh[b], (o))
#define	AGNIC_BAR0	0
#define	AGNIC_BAR2	1
#define	AGNIC_BAR4	2

/* ------------------------------------------------------------------------- */
/* Cross-file prototypes.                                                    */
/* ------------------------------------------------------------------------- */

/* if_agnic.c: shared bounded MMIO poll (0 = matched, ETIMEDOUT on timeout). */
int	agnic_poll(struct agnic_softc *sc, int bar, bus_size_t off,
	    uint32_t mask, uint32_t want, int timeout_ms, const char *what);

/* agnic_mgmt.c: P3a bring-up and teardown. */
int	agnic_mgmt_bringup(struct agnic_softc *sc);
void	agnic_mgmt_teardown(struct agnic_softc *sc);

/*
 * agnic_mgmt.c: shared coherent-DMA helpers (used by P3a rings and P3b datapath
 * rings) and the generic single-descriptor command round-trip.
 */
int	agnic_dma_alloc(struct agnic_softc *sc, struct agnic_dma_mem *m,
	    bus_size_t size, const char *name);
void	agnic_dma_free(struct agnic_dma_mem *m);

/*
 * Serialize one command (single-descriptor), publish it, ring the h2t doorbell,
 * and reap its response. Returns 0 when a response was received (its status byte
 * is stored via *out_status); ETIMEDOUT on no response. Takes mgmt_lock itself.
 */
int	agnic_mgmt_cmd(struct agnic_softc *sc, uint8_t cmd_code,
	    const void *params, size_t plen, uint8_t *resp, size_t resp_len,
	    uint8_t *out_status);
/* Fire-and-forget command (NO_RESP): used by CC_PF_DISABLE at teardown. */
void	agnic_mgmt_cmd_noresp(struct agnic_softc *sc, uint8_t cmd_code,
	    const void *params, size_t plen);

/* agnic_mgmt.c: param serializers (fill a >= AGNIC_MGMT_PARAMS_LEN buffer). */
void	agnic_pf_init_params(uint8_t *buf, uint32_t num_egress_tc,
	    uint32_t num_ingress_tc, uint16_t mtu_override, uint16_t mru_override,
	    uint8_t egress_sched);
void	agnic_ingress_tc_add_params(uint8_t *buf, uint32_t tc,
	    uint32_t num_queues, uint32_t pkt_offset, uint8_t hash_type);
void	agnic_ingress_data_q_add_params(uint8_t *buf,
	    const struct agnic_ingress_q_cfg *c);
void	agnic_egress_tc_add_params(uint8_t *buf, uint32_t tc,
	    uint32_t num_queues, uint32_t num_queues_per_dma);
void	agnic_egress_data_q_add_params(uint8_t *buf,
	    const struct agnic_egress_q_cfg *c);

/* agnic_nwa.c: P4a NW_AGENT mailbox -- physical front-panel port control. */
int	agnic_nwa_bringup(struct agnic_softc *sc);

/*
 * Per-front-panel-port control ops (attr-set transactions), driven by the pport
 * ifnet ioctl path. port_id is the NW_AGENT tag (host order, e.g. 0x8100).
 */
int	agnic_nwa_port_mtu_set(struct agnic_softc *sc, uint32_t port_id,
	    uint32_t mtu);
int	agnic_nwa_port_promisc(struct agnic_softc *sc, uint32_t port_id, int on);
int	agnic_nwa_port_allmulti(struct agnic_softc *sc, uint32_t port_id, int on);
int	agnic_nwa_port_mc(struct agnic_softc *sc, uint32_t port_id,
	    const uint8_t *mac, int add);
int	agnic_nwa_port_uc(struct agnic_softc *sc, uint32_t port_id,
	    const uint8_t *mac, int add);

/* Number of per-port MIB counters in the NW_AGENT STATS(14) block. */
#define	NWA_PORT_CNT_MAX	32

/* stats[] indices in the per-port MIB block (SFOS enum, verbatim values). */
#define	NWA_ST_GOOD_OCTETS_RCV		0
#define	NWA_ST_MAC_TX_ERR		2
#define	NWA_ST_BRDC_PKTS_RCV		3
#define	NWA_ST_MC_PKTS_RCV		4
#define	NWA_ST_EXCESSIVE_COLL		11
#define	NWA_ST_MC_PKTS_SENT		12
#define	NWA_ST_BRDC_PKTS_SENT		13
#define	NWA_ST_DROP_EVENTS		16
#define	NWA_ST_MAC_RCV_ERR		21
#define	NWA_ST_BAD_CRC			22
#define	NWA_ST_COLLISIONS		23
#define	NWA_ST_GOOD_UC_PKTS_RCV		25
#define	NWA_ST_GOOD_UC_PKTS_SENT	26
#define	NWA_ST_GOOD_OCTETS_SENT		31

/*
 * Per-front-panel-port GET ops (PORT_ATTR_GET transactions), driven by the P4c
 * link poll. port_id is the NW_AGENT tag (host order). Each sleeps on nwa_sx via
 * agnic_nwa_cmd, so they must only be called from a sleepable context (the pport
 * link taskqueue), never a callout/ISR. Return 0 and fill the out-param on an
 * accepted reply; non-zero on mailbox error or NPU rejection.
 */
int	agnic_nwa_port_oper_get(struct agnic_softc *sc, uint32_t port_id, int *up);
int	agnic_nwa_port_speed_get(struct agnic_softc *sc, uint32_t port_id,
	    uint32_t *mbps);
int	agnic_nwa_port_duplex_get(struct agnic_softc *sc, uint32_t port_id,
	    int *full);
int	agnic_nwa_port_stats_get(struct agnic_softc *sc, uint32_t port_id,
	    uint64_t stats[NWA_PORT_CNT_MAX]);
/*
 * Bulk carrier+speed+stats for all requested ports in one mailbox transaction
 * (SFOS ALL_COMB_PORT_INFO). states[i]!=0 means link up; fills speeds[] (Mbit/s)
 * and stats[i][] (per-port MIB) in request order. Preferred carrier source.
 */
int	agnic_nwa_ports_info_get(struct agnic_softc *sc, const uint16_t *tags,
	    int nports, uint8_t *states, uint32_t *speeds,
	    uint64_t stats[][NWA_PORT_CNT_MAX]);

/* Look up a discovered port by its NW_AGENT tag; NULL if not manageable. */
struct agnic_nwa_port *agnic_nwa_port_find(struct agnic_softc *sc, uint16_t tag);

/* agnic_pcinet.c: P5 mvmgmt0 -- PCIe management netdev / IPv6 link to the NPU. */
int	agnic_pcinet_bringup(struct agnic_softc *sc);
void	agnic_pcinet_teardown(struct agnic_softc *sc);

/* agnic_pport.c: P4b per-front-panel-port demux (port1..port9). */
int	agnic_pport_bringup(struct agnic_softc *sc);
void	agnic_pport_rx(struct agnic_softc *sc, struct mbuf *m);
void	agnic_pport_teardown(struct agnic_softc *sc);

/* agnic_txrx.c: P3b ingress datapath (rings only; the trunk has no OS ifnet). */
int	agnic_txrx_bringup(struct agnic_softc *sc);

/*
 * Auto-start the trunk datapath: CC_PF_ENABLE + RX servicing + trunk promisc,
 * then NW_AGENT front-panel ports, mvmgmt0, and the pport demux. Called once
 * from the config hook (the trunk has no if_init). Sleepable, idempotent.
 */
void	agnic_datapath_start(struct agnic_softc *sc);

/*
 * Quiesce RX + DISABLE the port (if_running=0, drain callout/task). Idempotent.
 * agnic_detach calls it before agnic_pport_teardown so the pport ifnets can be
 * detached (stopping agnic_giu_tx callers) before agnic_txrx_teardown frees the
 * TX rings/mutex.
 */
void	agnic_stop(struct agnic_softc *sc);

/*
 * agnic_txrx.c: transmit one mbuf on the GIU trunk egress ring (copy into a
 * coherent TX buffer, fill the descriptor, advance the producer). Consumes m.
 * Used by the pport TX (after prepending the port tag+header) and raw trunk TX.
 * Returns 0 on enqueue, ENOBUFS if the ring is full (m freed either way).
 */
int	agnic_giu_tx(struct agnic_softc *sc, struct mbuf *m);
void	agnic_txrx_teardown(struct agnic_softc *sc);
void	agnic_txrx_dbell_kick(struct agnic_softc *sc);	/* from dbell[1] ISR */

#endif /* _IF_AGNIC_H_ */
