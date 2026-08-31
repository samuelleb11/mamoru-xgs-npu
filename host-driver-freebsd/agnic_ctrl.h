/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Clean-room ABI for the Marvell AGNIC CONTROL-facility mailbox (ctrl_map) and
 * the target<->host doorbell handshake. Struct layouts, byte offsets and magic
 * constants are interface FACTS transcribed from the GPL-2.0 facility_conf.h;
 * no GPL .c control logic is copied.
 *
 * The CONTROL facility is a shared-memory mailbox at the start of the BAR2
 * "tail" facility region (CTRL @ BAR2-tail offset 0). Both sides read-modify-
 * write a single 32-bit handshake word; each side only OR-sets its own bits
 * (the target additionally CLEARS HOST_ALIVE each heartbeat check).
 */
#ifndef _AGNIC_CTRL_H_
#define	_AGNIC_CTRL_H_

/* ctrl_map.cookie value the target stamps before the handshake is valid. */
#define	AGNIC_FACILITY_COOKIE	0xAFACAFACU

/* Handshake bits (ctrl_map.handshake, a u32 bitfield at offset 0x04). */
#define	CTRL_FCLT_TRGT_INIT		0x1U	/* target: init done          */
#define	CTRL_FCLT_HOST_INIT		0x2U	/* host:   dbell init done    */
#define	CTRL_FCLT_TRGT_H2T_DBELL	0x4U	/* target: h2t dbells written */
#define	CTRL_FCLT_HOST_ALIVE		0x8U	/* host:   heartbeat          */

/*
 * ctrl_map header is exactly 16 bytes: three u32 followed by 4 implicit pad
 * bytes, because struct dbell_msg is 8-byte aligned (it leads with a u64).
 * The flexible h2t_dbell_msg[] therefore begins at offset 0x10, NOT 0x0C.
 */
#define	AGNIC_CTRL_COOKIE_OFF		0x00	/* u32 == AGNIC_FACILITY_COOKIE */
#define	AGNIC_CTRL_HANDSHAKE_OFF	0x04	/* u32 CTRL_FCLT_* bitfield     */
#define	AGNIC_CTRL_H2T_DBELL_CNT_OFF	0x08	/* u32 valid h2t dbell entries  */
#define	AGNIC_CTRL_H2T_DBELL_MSG_OFF	0x10	/* struct dbell_msg[] base      */

/* struct dbell_msg element layout (16-byte stride). */
#define	AGNIC_DBELL_MSG_STRIDE		0x10
#define	AGNIC_DBELL_MSG_ADDR_OFF	0x00	/* u64 MSI addr / h2t reg offset */
#define	AGNIC_DBELL_MSG_DATA_OFF	0x08	/* u32 MSI data to writel        */

/* h2t_dbell_msg[i] byte offset within ctrl_map. */
#define	AGNIC_CTRL_H2T_DBELL_MSG(i) \
	(AGNIC_CTRL_H2T_DBELL_MSG_OFF + (i) * AGNIC_DBELL_MSG_STRIDE)

/*
 * BAR2 "tail" facility base. The facilities block sits at the TOP of BAR2:
 *   base = bar2_size - (PCI_BOOTCMD 0x1000 + BAR2_TOTAL 0x103000) = 0xEFC000
 * for the 16 MB BAR2. barmap facility_map[].offset for a BAR2 facility is
 * relative to THIS base; CTRL lives at base + 0.
 */
#define	AGNIC_BAR2_FACILITY_TAIL_TOTAL	0x104000U	/* 0x1000 + 0x103000 */
#define	AGNIC_BAR2_TAIL_OFF(bar2_size) \
	((uint32_t)(bar2_size) - AGNIC_BAR2_FACILITY_TAIL_TOTAL)

/*
 * Doorbell accounting (facility_conf.h facilities[] table, transcribed as ABL
 * FACTS). t2h (target->host) = MGMT_NETDEV(1, the MSI zero-id workaround) +
 * GIU(4); h2t (host->target) = RPC(1) + GIU(4). msix_id == vector index, with
 * MGMT consuming vector 0 so the four real GIU data-queue vectors are 1..4.
 */
#define	AGNIC_N_T2H_DBELLS	5	/* MGMT_NETDEV(1) + GIU(4) */
#define	AGNIC_N_H2T_DBELLS	5	/* RPC(1)         + GIU(4) */

/*
 * Transcribed struct layouts (for reference / static assertion only; the
 * driver accesses these fields via bus_space using the *_OFF macros above,
 * never by dereferencing these types over a BAR mapping).
 *
 * NOTE: dbell_msg is deliberately NOT __packed -- the target uses natural
 * (u64) alignment, so the u32 'data' is followed by 4 pad bytes (16-byte
 * element). Packing would corrupt the array stride.
 */
struct agnic_dbell_msg {
	uint64_t	address;	/* MSI msg addr (t2h) / reg offset (h2t) */
	uint32_t	data;		/* MSI msg data                          */
	uint32_t	_pad;		/* explicit tail pad -> 16-byte element  */
};

struct agnic_ctrl_map {
	uint32_t	cookie;		/* == AGNIC_FACILITY_COOKIE           */
	uint32_t	handshake;	/* CTRL_FCLT_* bitfield (shared RMW)  */
	uint32_t	h2t_dbell_cnt;	/* target-filled valid entry count    */
	uint32_t	_pad;		/* aligns array to 8 (dbell_msg u64)  */
	struct agnic_dbell_msg	h2t_dbell_msg[];
};

#ifdef CTASSERT
CTASSERT(sizeof(struct agnic_dbell_msg) == 16);
CTASSERT(__offsetof(struct agnic_ctrl_map, handshake) == AGNIC_CTRL_HANDSHAKE_OFF);
CTASSERT(__offsetof(struct agnic_ctrl_map, h2t_dbell_cnt) == AGNIC_CTRL_H2T_DBELL_CNT_OFF);
CTASSERT(__offsetof(struct agnic_ctrl_map, h2t_dbell_msg) == AGNIC_CTRL_H2T_DBELL_MSG_OFF);
#endif

#endif /* _AGNIC_CTRL_H_ */
