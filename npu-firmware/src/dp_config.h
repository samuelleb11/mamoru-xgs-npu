// SPDX-License-Identifier: MIT
/*
 * dp_config.h — compile-time constants for the custom NPU data-plane (dp_app).
 * See ARCHITECTURE.md §A/§B. Values pinned to the frozen host contract.
 */
#ifndef DP_CONFIG_H
#define DP_CONFIG_H

/* Front-panel ports: Port1..Port8 (copper) + PortF1 = 9 (ARCHITECTURE portmap). */
#define DP_PORT_COUNT		9

/* GIU trunk frame budget = 1500 MTU + 14 L2 + 4 CRC + 66 pport prefix = 1584 (host CC_PF_INIT). */
#define DP_GIU_FRAME		1584

/* pp2 packet offset (headroom) — matches stock NMP dflt_pkt_offset. */
#define DP_PKT_OFFSET		64

/* pp2 BM long-pool buffer size: jumbo-capable (10240) so full 1500-MTU + DSA + prefix + offset fit. */
#define DP_PP2_LONG_BUF		10240
#define DP_PP2_SHORT_BUF	2048

/* BM pool depths (multiple of 8; keep topped to low-watermark every loop, ARCHITECTURE §A.3.1). */
#define DP_GIU_BPOOL_BUFS	4096
#define DP_PP2_LONG_BPOOL_BUFS	4096
#define DP_PP2_SHORT_BPOOL_BUFS	1024
#define DP_BPOOL_REFILL_WM	64

/* Forwarder burst + ring sizing. */
#define DP_BURST		64
#define DP_TXQ_SIZE		2048
#define DP_RXQ_SIZE		2048

/* Single TC/queue for M1/M2 (RSS multi-queue deferred to M5). */
#define DP_NUM_TCS		1
#define DP_NUM_QS_PER_TC	1

/* Core affinity (isolcpus=1-3). ctrl/init on 0; forwarder on dp core. */
#define DP_CTRL_CORE		0
#define DP_DP0_CORE		1

/* eth0 = the single pp2 uplink to the 88E6193X switch. */
#define DP_PP2_PPIO_NAME	"eth0"

/* 88E6193X DSA tag length on the eth0 trunk (Marvell 4-byte DSA). */
#define DP_DSA_TAG_LEN		4

#endif /* DP_CONFIG_H */
