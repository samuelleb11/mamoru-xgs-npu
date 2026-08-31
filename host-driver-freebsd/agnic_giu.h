/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Clean-room ABI for the Marvell AGNIC GIU config_mem region. Field byte
 * offsets, sizes and status bits are interface FACTS transcribed from the
 * GPL-2.0 giu_nic_hw.h (struct agnic_config_mem / agnic_q_hw_info); no GPL
 * .c logic is copied.
 *
 * config_mem is the GIU facility base = BAR0 offset 0 (barmap GIU @ BAR0:0),
 * a 1024-byte device/host handshake block. All accesses are 32-bit little-
 * endian MMIO into BAR0 (byte fields via bus_space_read_1).
 */
#ifndef _AGNIC_GIU_H_
#define	_AGNIC_GIU_H_

/* config_mem.status bits (offset 0x00). */
#define	AGNIC_CFG_STATUS_DEV_READY	0x1U	/* device: BARs/MAC valid       */
#define	AGNIC_CFG_STATUS_HOST_MGMT_READY 0x2U	/* host:   cmd/notif rings set  */
#define	AGNIC_CFG_STATUS_DEV_MGMT_READY	0x4U	/* device: mgmt rings accepted  */

/* struct agnic_config_mem field byte offsets (total 1024 bytes). */
#define	AGNIC_GIU_STATUS_OFF		0x00	/* u32   handshake bitmask      */
#define	AGNIC_GIU_MAC_OFF		0x04	/* u8[6] device MAC (DEV_READY) */
#define	AGNIC_GIU_CMD_Q_OFF		0x10	/* agnic_q_hw_info (host->dev)  */
#define	AGNIC_GIU_NOTIF_Q_OFF		0x28	/* agnic_q_hw_info (dev->host)  */
#define	AGNIC_GIU_BAR0_VF_START_OFF	0x40	/* u64 SR-IOV BAR0 first-VF off */
#define	AGNIC_GIU_BAR2_VF_START_OFF	0x48	/* u64 SR-IOV BAR2 first-VF off */
#define	AGNIC_GIU_DEV_USE_SIZE_OFF	0x58	/* u32 bytes device consumed    */
#define	AGNIC_GIU_MSIX_TBL_OFF		0x5C	/* u32 MSI-X table off in BAR0  */

#define	AGNIC_GIU_CONFIG_SIZE		0x400U	/* sizeof(config_mem) = 1024    */
#define	AGNIC_CONFIG_BAR_SIZE		0x10000U /* usable GIU region on BAR0   */

/* struct agnic_q_hw_info field byte offsets (24 bytes). */
#define	AGNIC_QHW_ADDR_OFF		0x00	/* u64 host DMA addr of ring    */
#define	AGNIC_QHW_PROD_OFF		0x08	/* u32 BAR-rel producer idx off */
#define	AGNIC_QHW_CONS_OFF		0x0C	/* u32 BAR-rel consumer idx off */
#define	AGNIC_QHW_LEN_OFF		0x10	/* u32 descriptor count         */

/* Ring sizing (P3; declared here as ABI facts). */
#define	AGNIC_CMD_Q_LEN			256
#define	AGNIC_NOTIF_Q_LEN		256
#define	AGNIC_MGMT_DESC_SIZE		64
#define	AGNIC_MGMT_DESC_DATA_LEN	56

#endif /* _AGNIC_GIU_H_ */
