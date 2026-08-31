/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Clean-room ABI for the Marvell AGNIC NPU "barmap"/facility handshake.
 * Constants, offsets and struct layouts are interface FACTS transcribed from
 * the GPL-2.0 barmap.h / facility_conf.h; no GPL logic is copied.
 *
 * The NPU (CN9130 firmware) publishes a descriptor in BAR2 that tells the host
 * where each "facility" (CTRL mailbox, GIU config, mgmt-netdev, RPC, NW-agent)
 * lives inside BAR0/BAR2. The host finds it via a fixed cookie + version.
 */
#ifndef _AGNIC_BARMAP_H_
#define	_AGNIC_BARMAP_H_

#define	AGNIC_BARMAP_COOKIE	0xD0FAC10DU
/* NPU_BARMAP_VERSION = (MAJOR<<16)|MINOR, MAJOR=0 MINOR=5 -> 0x00000005 */
#define	AGNIC_BARMAP_VERSION	0x00000005U

/*
 * BAR2 (16 MB) tail layout (the facilities block sits at the TOP of BAR2):
 *   CTRL 4K @0 | MGMT_NETDEV 4K @0x1000 | RPC 1M @0x2000 |
 *   npu_barmap 4K | PCI_BOOTCMD 4K (last page)
 * => npu_barmap descriptor = second-to-last 4K page of BAR2.
 */
#define	AGNIC_BAR2_PCI_BOOTCMD_SIZE	0x1000U
#define	AGNIC_BAR2_BARMAP_SIZE		0x1000U
#define	AGNIC_BARMAP_OFF(bar2_size) \
	((uint32_t)(bar2_size) - AGNIC_BAR2_PCI_BOOTCMD_SIZE - AGNIC_BAR2_BARMAP_SIZE)

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
	uint32_t	bar;	/* enum agnic_shm_bar */
	uint32_t	type;	/* enum agnic_facility */
	uint32_t	offset;	/* offset within BAR0/2 */
	uint32_t	size;
} __packed;

/* Descriptor published by the NPU at BAR2 + AGNIC_BARMAP_OFF(). */
struct agnic_npu_bar_map {
	uint32_t	version;
	uint32_t	cookie;
	struct agnic_facility_bar_map facility_map[AGNIC_FAC_COUNT];
} __packed;

/* Field byte-offsets within the descriptor (for bus_space reads). */
#define	AGNIC_BARMAP_VERSION_OFF	0
#define	AGNIC_BARMAP_COOKIE_OFF		4
#define	AGNIC_BARMAP_FACMAP_OFF		8
#define	AGNIC_FACMAP_ENTRY_SIZE		16

#endif /* _AGNIC_BARMAP_H_ */
