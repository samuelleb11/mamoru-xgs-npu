/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Clean-room ABI for the Marvell AGNIC (Armada GIU-NIC) NPU as presented by the
 * Sophos XGS 116 CN9130 co-processor over PCIe (PF 11ab:7080).
 *
 * Constants, byte offsets and struct layouts here are interface FACTS: transcribed
 * from the BSD-2 FreeBSD if_agnic driver (itself transcribed from the GPL-2.0
 * Marvell headers barmap.h / facility_conf.h / giu_nic_hw.h). No control-path logic
 * is copied — only the wire contract the NPU firmware already publishes.
 *
 * The NPU is the PCIe endpoint; the x86 host is the root complex. The host NEVER
 * resets/FLRs the device — an FLR nukes the live NPU firmware and the data plane.
 * All MMIO into the BARs is 32-bit little-endian (ioread32/iowrite32).
 */
#ifndef _AGNIC_ABI_H_
#define _AGNIC_ABI_H_

#include <linux/types.h>

/* PCI identity. */
#define AGNIC_VENDOR_MARVELL	0x11ab
#define AGNIC_DEVICE_PF		0x7080
#define AGNIC_DEVICE_VF		0x7081	/* SR-IOV VF: not used by this driver */

/* ---- barmap facility descriptor (published by the NPU in BAR2 tail) ---------- */

#define AGNIC_BARMAP_COOKIE	0xD0FAC10DU
/* NPU_BARMAP_VERSION = (MAJOR<<16)|MINOR, MAJOR=0 MINOR=5 -> 0x00000005 */
#define AGNIC_BARMAP_VERSION	0x00000005U

/*
 * BAR2 (16 MB) tail layout, facilities block at the TOP of BAR2:
 *   CTRL 4K @0 | MGMT_NETDEV 4K @0x1000 | RPC 1M @0x2000 |
 *   npu_barmap 4K | PCI_BOOTCMD 4K (last page)
 * => the npu_barmap descriptor is the second-to-last 4K page of BAR2.
 */
#define AGNIC_BAR2_PCI_BOOTCMD_SIZE	0x1000U
#define AGNIC_BAR2_BARMAP_SIZE		0x1000U
#define AGNIC_BARMAP_OFF(bar2_size) \
	((u32)(bar2_size) - AGNIC_BAR2_PCI_BOOTCMD_SIZE - AGNIC_BAR2_BARMAP_SIZE)

/* Facility types (GPL enum order). */
enum agnic_facility {
	AGNIC_FAC_CONTROL = 0,
	AGNIC_FAC_MGMT_NETDEV,
	AGNIC_FAC_NW_AGENT,
	AGNIC_FAC_RPC,
	AGNIC_FAC_GIU,
	AGNIC_FAC_COUNT
};

/* Which BAR a facility region lives in. */
enum agnic_shm_bar { AGNIC_SHM_BAR0 = 0, AGNIC_SHM_BAR2 = 1 };

/* One facility's window (16 bytes, LE). */
struct agnic_facility_bar_map {
	__le32 bar;	/* enum agnic_shm_bar */
	__le32 type;	/* enum agnic_facility */
	__le32 offset;	/* offset within BAR0/2 (see AGNIC_BAR2_TAIL_OFF for BAR2) */
	__le32 size;
} __packed;

/* Descriptor published by the NPU at BAR2 + AGNIC_BARMAP_OFF(). */
struct agnic_npu_bar_map {
	__le32 version;
	__le32 cookie;
	struct agnic_facility_bar_map facility_map[AGNIC_FAC_COUNT];
} __packed;

#define AGNIC_BARMAP_VERSION_OFF	0
#define AGNIC_BARMAP_COOKIE_OFF		4
#define AGNIC_BARMAP_FACMAP_OFF		8
#define AGNIC_FACMAP_ENTRY_SIZE		16

/*
 * The facilities block sits at the TOP of BAR2; a BAR2 facility's
 * facility_map[].offset is RELATIVE to this tail base:
 *   tail_base = bar2_size - (PCI_BOOTCMD 0x1000 + BAR2_TOTAL 0x103000) = 0xEFC000
 * for the 16 MB BAR2. A BAR0 facility's offset is absolute in BAR0.
 */
#define AGNIC_BAR2_FACILITY_TAIL_TOTAL	0x104000U	/* 0x1000 + 0x103000 */
#define AGNIC_BAR2_TAIL_OFF(bar2_size) \
	((u32)(bar2_size) - AGNIC_BAR2_FACILITY_TAIL_TOTAL)

/* ---- CONTROL facility mailbox (BAR2 tail @ CTRL offset 0) -------------------- */

#define AGNIC_FACILITY_COOKIE	0xAFACAFACU	/* ctrl_map.cookie */

/* ctrl_map.handshake bits (u32 at offset 0x04), shared read-modify-write. */
#define CTRL_FCLT_TRGT_INIT		0x1U	/* target: init done          */
#define CTRL_FCLT_HOST_INIT		0x2U	/* host:   dbell init done    */
#define CTRL_FCLT_TRGT_H2T_DBELL	0x4U	/* target: h2t dbells written */
#define CTRL_FCLT_HOST_ALIVE		0x8U	/* host:   heartbeat          */

#define AGNIC_CTRL_COOKIE_OFF		0x00	/* u32 == AGNIC_FACILITY_COOKIE */
#define AGNIC_CTRL_HANDSHAKE_OFF	0x04	/* u32 CTRL_FCLT_* bitfield     */
#define AGNIC_CTRL_H2T_DBELL_CNT_OFF	0x08	/* u32 valid h2t dbell entries  */
#define AGNIC_CTRL_H2T_DBELL_MSG_OFF	0x10	/* struct dbell_msg[] base      */

/* struct dbell_msg: 16-byte stride (u64 addr + u32 data + u32 pad, NOT packed). */
#define AGNIC_DBELL_MSG_STRIDE		0x10
#define AGNIC_DBELL_MSG_ADDR_OFF	0x00	/* u64 MSI addr / h2t reg offset */
#define AGNIC_DBELL_MSG_DATA_OFF	0x08	/* u32 MSI data to write         */
#define AGNIC_CTRL_H2T_DBELL_MSG(i) \
	(AGNIC_CTRL_H2T_DBELL_MSG_OFF + (i) * AGNIC_DBELL_MSG_STRIDE)

#define AGNIC_N_T2H_DBELLS	5	/* MGMT_NETDEV(1) + GIU(4) */
#define AGNIC_N_H2T_DBELLS	5	/* RPC(1)         + GIU(4) */

/* ---- GIU config_mem (GIU facility base = BAR0 @ facility offset) ------------- */

/* config_mem.status bits (offset 0x00). */
#define AGNIC_CFG_STATUS_DEV_READY		0x1U	/* device: BARs/MAC valid      */
#define AGNIC_CFG_STATUS_HOST_MGMT_READY	0x2U	/* host:   cmd/notif rings set */
#define AGNIC_CFG_STATUS_DEV_MGMT_READY		0x4U	/* device: mgmt rings accepted */

/* struct agnic_config_mem field byte offsets (total 1024 bytes). */
#define AGNIC_GIU_STATUS_OFF		0x00	/* u32   handshake bitmask      */
#define AGNIC_GIU_MAC_OFF		0x04	/* u8[6] device MAC (DEV_READY) */
#define AGNIC_GIU_CMD_Q_OFF		0x10	/* agnic_q_hw_info (host->dev)  */
#define AGNIC_GIU_NOTIF_Q_OFF		0x28	/* agnic_q_hw_info (dev->host)  */
#define AGNIC_GIU_BAR0_VF_START_OFF	0x40	/* u64 SR-IOV BAR0 first-VF off */
#define AGNIC_GIU_BAR2_VF_START_OFF	0x48	/* u64 SR-IOV BAR2 first-VF off */
#define AGNIC_GIU_DEV_USE_SIZE_OFF	0x58	/* u32 bytes device consumed    */
#define AGNIC_GIU_MSIX_TBL_OFF		0x5C	/* u32 MSI-X table off in BAR0  */

#define AGNIC_GIU_CONFIG_SIZE		0x400U	/* sizeof(config_mem) = 1024 */
#define AGNIC_CONFIG_BAR_SIZE		0x10000U /* usable GIU region on BAR0 */

/* struct agnic_q_hw_info field byte offsets (24 bytes). */
#define AGNIC_QHW_ADDR_OFF		0x00	/* u64 host DMA addr of ring    */
#define AGNIC_QHW_PROD_OFF		0x08	/* u32 BAR-rel producer idx off */
#define AGNIC_QHW_CONS_OFF		0x0C	/* u32 BAR-rel consumer idx off */
#define AGNIC_QHW_LEN_OFF		0x10	/* u32 descriptor count         */

/* Ring sizing (P3 ABI facts). */
#define AGNIC_CMD_Q_LEN			256
#define AGNIC_NOTIF_Q_LEN		256
#define AGNIC_MGMT_DESC_SIZE		64
#define AGNIC_MGMT_DESC_DATA_LEN	56

/* ---- GIU management command protocol (P3) ----------------------------------- */

/* One 64-byte mgmt descriptor (cmd ring: host->dev; notif ring: dev->host). LE, packed. */
struct agnic_cmd_desc {
	__le16	cmd_idx;	/* @0x00 correlation tag; 0xFFFF = async notif */
	__le16	app_code;	/* @0x02 AC_PF_MANAGER on host commands        */
	u8	cmd_code;	/* @0x04 CC_PF_* / NC_PF_*                     */
	u8	client_id;	/* @0x05 0 for PF                              */
	u8	client_type;	/* @0x06 CDT_PF = 1                            */
	u8	flags;		/* @0x07 DESC_FLAGS_SINGLE_*                   */
	u8	data[AGNIC_MGMT_DESC_DATA_LEN];	/* @0x08, 56 bytes     */
} __packed;
static_assert(sizeof(struct agnic_cmd_desc) == AGNIC_MGMT_DESC_SIZE, "cmd_desc must be 64 bytes");

#define AGNIC_AC_PF_MANAGER		0x2	/* app_code on host commands */
#define AGNIC_CDT_PF			1	/* client_type = PF          */
#define AGNIC_DESC_FLAGS_SINGLE_RESP	0x00	/* single desc, response wanted   */
#define AGNIC_DESC_FLAGS_SINGLE_NORESP	0x20	/* single desc, fire-and-forget   */
#define AGNIC_CMD_ID_NOTIFICATION	0xFFFF	/* cmd_idx of an async notif      */
#define AGNIC_CMD_COOKIE_COUNT		1024	/* cmd_idx rolls 1..1023          */
#define AGNIC_NOTIF_STATUS_OK		0	/* resp data[0] == 0 on success   */
#define AGNIC_RING_INC(i, cnt)		(((i) + 1) & ((cnt) - 1))

/* cmd_code (CC_PF_*) — the host->device manager commands. */
#define AGNIC_CC_PF_INIT		0x01
#define AGNIC_CC_PF_INIT_DONE		0x02
#define AGNIC_CC_PF_EGRESS_TC_ADD	0x03
#define AGNIC_CC_PF_EGRESS_DATA_Q_ADD	0x04
#define AGNIC_CC_PF_INGRESS_TC_ADD	0x05
#define AGNIC_CC_PF_INGRESS_DATA_Q_ADD	0x06
#define AGNIC_CC_PF_ENABLE		0x07
#define AGNIC_CC_PF_DISABLE		0x08
#define AGNIC_CC_PF_MGMT_ECHO		0x09	/* simplest safe round-trip */
#define AGNIC_CC_PF_LINK_STATUS		0x0A
#define AGNIC_CC_PF_PROMISC		0x0E
#define AGNIC_CC_PF_MTU			0x10
#define AGNIC_CC_GET_CAPABILITIES	0x1E	/* read-only caps query */
#define AGNIC_CAPABILITIES_SG		(1U << 0)

/* notif cmd_code (NC_PF_*) — async device->host notifications. */
#define AGNIC_NC_PF_LINK_CHANGE		0x01	/* data[0] = u32 link_status */
#define AGNIC_NC_PF_KEEP_ALIVE		0x02	/* no ack; resets watchdog   */

/* The ring index words the device places on BAR0 at config_mem.dev_use_size. */
#define AGNIC_MGMT_IDX_SLOTS		4
#define AGNIC_MGMT_SLOT_CMD_PROD	0
#define AGNIC_MGMT_SLOT_CMD_CONS	1
#define AGNIC_MGMT_SLOT_NOTIF_PROD	2
#define AGNIC_MGMT_SLOT_NOTIF_CONS	3

/* h2t doorbell index 0 = the RPC mgmt channel (ctrl_map.h2t_dbell_msg[0]). */
#define AGNIC_H2T_DBELL_MGMT		0

/* pport data-plane prefix — 66 bytes ahead of every L2 frame, BOTH directions:
 *   [byte0 = 0x81 + port_index][byte1 = 0x00][64-byte HW header][ethernet]
 * Demux is purely on byte0: port_index = byte0 - 0x81 (Port1=0x81 .. PortF1=0x89).
 * (The device does NOT read the redundant src/dst_port inside the 64-byte header.) */
#define AGNIC_PPORT_COUNT		9	/* Port1..Port8 (copper) + PortF1 (SFP) */
#define AGNIC_PPORT_TAG_LEN		2	/* the 2-byte tag               */
#define AGNIC_PPORT_HDR_LEN		64	/* HW header after the tag      */
#define AGNIC_PPORT_PREFIX_LEN		66	/* tag(2) + hdr(64)             */
#define AGNIC_PPORT_TAG0_BASE		0x81	/* byte0 = base + port_index    */
#define AGNIC_PPORT_TAG0(idx)		((u8)(AGNIC_PPORT_TAG0_BASE + (idx)))
#define AGNIC_PPORT_IDX_FROM_TAG0(b0)	((int)(u8)(b0) - AGNIC_PPORT_TAG0_BASE)

/* ---- GIU datapath: LIF config params + descriptor rings (P3b/P4) ------------ *
 * Transcribed from the FreeBSD if_agnic.h (itself from GPL giu_nic_hw.h). The
 * datapath descriptors are DISTINCT from the 64-byte mgmt descriptor. The CC_PF_*
 * LIF-config commands (codes above) carry these params serialized at desc->data[0].
 */

/* Every param-bearing CC_PF_* command transmits exactly 48 bytes (largest member
 * is ingress_data_q_add); zero the area, fill the used prefix, send all 48. */
#define AGNIC_MGMT_PARAMS_LEN		48

/* q_add response status: low bit of the q_inf/bpool_q_inf field (0 OK, 1 ERR). */
#define AGNIC_Q_INF_STATUS_OK		0
#define AGNIC_Q_INF_STATUS_ERR		1

/* One ingress TC + one RX queue, one egress TC + one TX queue. The NPU REFUSES
 * CC_PF_ENABLE for an RX-only config (accepts egress_tc=0 at INIT/INIT_DONE, but
 * won't bring the port live without a registered egress queue) — so a minimal
 * egress TC/queue is registered during bring-up. egress_dma_engines is 1 (from
 * CC_GET_CAPABILITIES) => num_queues == num_queues_per_dma == 1. */
#define AGNIC_INGRESS_TCS		1
#define AGNIC_EGRESS_TCS		1
#define AGNIC_QS_PER_TC			1

/* Power-of-2 datapath ring depths (RX ring depth <= bpool buffer count). 1024
 * gives burst headroom for the poll-driven datapath; the NPU nmp config allows up
 * to qs_size 2048 / 4096 buffers, so 1024 is well within its limits. */
#define AGNIC_RX_RING_LEN		1024
#define AGNIC_TX_RING_LEN		1024
#define AGNIC_BP_RING_LEN		1024

/* CC_PF_INIT mtu/mru override — the max frame the GIU carries. MUST include the
 * 66-byte pport prefix (else full-MTU frames overflow the GIU and are clipped). */
#define AGNIC_RX_FRAME_SIZE	(1500 + 14 + 4 + AGNIC_PPORT_PREFIX_LEN)	/* 1584 */

/* Host RX buffer / TX copy-slot size, and the q_buf_size advertised to the device.
 * The host advertises its OWN size (2048, >= frame 1584), NOT the NPU-local jumbo
 * ceiling (9304 in dp-nmp-config.txt). Must be >= AGNIC_RX_FRAME_SIZE and pow2-ish. */
#define AGNIC_RX_CLSIZE		2048

/* CC_PF_INIT egress_sched: strict priority. */
#define AGNIC_ES_STRICT_SCHED	0x1
/* CC_PF_INGRESS_TC_ADD hash_type: none (single queue, no RSS). */
#define AGNIC_ING_HASH_TYPE_NONE	0x0
/* CC_PF_INGRESS_TC_ADD pkt_offset: FreeBSD-proven 0 (RX path trims per-frame via
 * rx_desc.pkt_offset), despite dp-nmp-config.txt declaring dflt_pkt_offset=64. */
#define AGNIC_INGRESS_PKT_OFFSET	0
/* RX queue -> GIU t2h MSI-X vector (dbell[1..4]); poll is the reliable RX path. */
#define AGNIC_RX_DBELL_ID	1
/* CC_PF_ENABLE bounded retry (a custom NPU dp app may still be initializing). */
#define AGNIC_ENABLE_RETRIES	20

/*
 * BAR0 ring index-array slots. Mgmt already owns slots 0..3 (cmd-prod, cmd-cons,
 * notif-prod, notif-cons); the datapath claims 6 more. Device WRITES rx-prod +
 * bpool-cons + tx-cons; host WRITES rx-cons + bpool-prod + tx-prod (host is the
 * TX producer, device the TX consumer). Slot byte offset = dev_use_size + slot*4.
 */
#define AGNIC_DATA_SLOT_RX_PROD		4
#define AGNIC_DATA_SLOT_RX_CONS		5
#define AGNIC_DATA_SLOT_BP_PROD		6
#define AGNIC_DATA_SLOT_BP_CONS		7
#define AGNIC_DATA_SLOT_TX_PROD		8
#define AGNIC_DATA_SLOT_TX_CONS		9
#define AGNIC_DATA_IDX_SLOTS		6

/* Datapath descriptor sizes. */
#define AGNIC_RXD_SIZE			32
#define AGNIC_TXD_SIZE			32
#define AGNIC_BPD_SIZE			16

/* agnic_rx_desc: DEVICE-producer RX completion written into the host RX ring. */
struct agnic_rx_desc {
	__le32	flags;			/* @0x00 l3/l4/csum bitfields   */
	u8	pkt_offset;		/* @0x04 extra offset to data   */
	u8	info;			/* @0x05 vlan/l2/l3 info        */
	__le16	byte_cnt;		/* @0x06 received frame length  */
	__le16	port_num;		/* @0x08                        */
	__le16	num_sg_ent;		/* @0x0A                        */
	__le32	timestamp_hashkey;	/* @0x0C RSS hash               */
	__le64	buffer_addr;		/* @0x10 bus addr we posted     */
	__le64	cookie;			/* @0x18 buff_cookie we posted  */
} __packed;
static_assert(sizeof(struct agnic_rx_desc) == AGNIC_RXD_SIZE, "rx_desc must be 32 bytes");

/* agnic_bpool_desc: HOST-producer free-buffer descriptor handing RX buffers to the device. */
struct agnic_bpool_desc {
	__le64	buff_addr_phys;		/* @0x00 bus addr for device DMA    */
	__le64	buff_cookie;		/* @0x08 echoed into rx_desc.cookie */
} __packed;
static_assert(sizeof(struct agnic_bpool_desc) == AGNIC_BPD_SIZE, "bpool_desc must be 16 bytes");

/* agnic_tx_desc: HOST-producer egress descriptor the device consumes to transmit. */
struct agnic_tx_desc {
	__le32	flags;			/* @0x00 format/csum bitfields  */
	u8	pkt_offset;		/* @0x04 offset to frame data   */
	u8	info;			/* @0x05 vlan/res               */
	__le16	byte_cnt;		/* @0x06 frame length           */
	__le16	res5;			/* @0x08                        */
	__le16	num_seg_ent;		/* @0x0A                        */
	__le32	res6;			/* @0x0C                        */
	__le64	buffer_addr;		/* @0x10 frame buffer bus addr  */
	__le64	cookie;			/* @0x18                        */
} __packed;
static_assert(sizeof(struct agnic_tx_desc) == AGNIC_TXD_SIZE, "tx_desc must be 32 bytes");

/* tx_desc.flags: mark a single-segment packet + tell the device NOT to regenerate
 * checksums (host already checksummed; clearing these commands csum-gen, which
 * rewrites frame bytes and stalls the GIU). */
#define AGNIC_TXD_FLAGS_SG_SINGLE_ENTRY		(3U << 28)
#define AGNIC_TXD_FLAGS_GEN_L4_CSUM_NOT		(1U << 14)
#define AGNIC_TXD_FLAGS_GEN_IPV4_CSUM_DIS	(1U << 15)

/* The 36-bit DMA ceiling the NPU's GIE addresses through. */
#define AGNIC_DMA_BITS			36

static_assert(sizeof(struct agnic_facility_bar_map) == AGNIC_FACMAP_ENTRY_SIZE,
	      "facility_bar_map must be 16 bytes");

#endif /* _AGNIC_ABI_H_ */
