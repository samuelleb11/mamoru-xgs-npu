// SPDX-License-Identifier: MIT
/*
 * pport_hdr.h — the 66-byte host<->NPU trunk prefix and the 64-byte in-band
 * offload-metadata header (struct agnic_pport_md).
 *
 * SINGLE SOURCE OF TRUTH: compiled into BOTH the NPU data-plane app (dp_app,
 * aarch64/glibc, little-endian) AND the FreeBSD host driver if_agnic
 * (freebsd-driver/, little-endian). Keep the two copies byte-identical.
 *
 * Design (ARCHITECTURE.md §B): the GIU trunk descriptor stays *verbatim*
 * (flags = 0x3000C000, csum-gen bits clear) — never ask the GIU LIF to
 * checksum (that stalled the stock GIU). ALL offload intent (h2t) and results
 * (t2h) ride IN-BAND here, because the forwarder re-injects each frame into a
 * fresh pp2 descriptor in a different BM pool and cannot rely on the GIU
 * descriptor propagating to pp2.
 *
 * Wire prefix on the GIU trunk (both directions), 66 bytes:
 *   byte 0      : 0x81 + port_index   (= high byte of htons(0x8000|(physport<<8)))
 *   byte 1      : 0x00
 *   byte 2..65  : struct agnic_pport_md (this file), 64 bytes
 *   byte 66..   : ethernet frame
 */
#ifndef AGNIC_PPORT_HDR_H
#define AGNIC_PPORT_HDR_H

#if defined(_KERNEL) || defined(__FreeBSD__)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* ---- trunk prefix geometry (must match agnic_pport.c / forwarder) ---- */
#define PPORT_TAG_LEN		2	/* [0x81pp][0x00]                 */
#define PPORT_HDR_LEN		64	/* struct agnic_pport_md          */
#define PPORT_PREFIX		(PPORT_TAG_LEN + PPORT_HDR_LEN)	/* 66 */
#define PPORT_TAG0_BASE		0x81	/* tag byte0 for front Port1      */
#define PPORT_TAG_TO_PHYS(t0)	((uint8_t)((t0) - PPORT_TAG0_BASE + 1))	/* 0x81->1 .. 0x89->9 */
#define PPORT_PHYS_TO_TAG0(p)	((uint8_t)(PPORT_TAG0_BASE + (p) - 1))

/* ---- md_magic / version ---- */
#define AGNIC_MD_MAGIC		0xA6D0	/* "our metadata present"; first byte 0xC0 = stock fill = NONE */
#define AGNIC_MD_VERSION	0x01

/* md_dir_flags */
#define MD_DIR_T2H		0x01	/* bit0: 0 = h2t (TX cmd), 1 = t2h (RX result) */

/* csum_cmd (h2t: forwarder -> pp2 TX desc) */
#define CMD_L3_CSUM		(1u << 0)	/* generate/insert IPv4 header checksum */
#define CMD_L4_CSUM		(1u << 1)	/* generate/insert TCP/UDP checksum     */
#define CMD_TSO			(1u << 2)	/* segment using tso_mss                 */
#define CMD_VLAN_INS		(1u << 3)	/* insert vlan_tci                       */

/* csum_res (t2h: pp2 RX desc -> forwarder). L3/L4_OK set only after full A.3 Flow2 qualification. */
#define RES_L3_CHECKED		(1u << 0)
#define RES_L3_OK		(1u << 1)
#define RES_L4_CHECKED		(1u << 2)
#define RES_L4_OK		(1u << 3)
#define RES_L3_IPV4		(1u << 4)
#define RES_L3_IPV6		(1u << 5)
#define RES_L4_TCP		(1u << 6)
#define RES_L4_UDP		(1u << 7)
#define RES_VLAN_STRIPPED	(1u << 8)

/* l3_proto / l4_proto small enums (match GIU/pp2 desc L3_INFO / L4_TYPE encodings) */
#define MD_L3_OTHER		0
#define MD_L3_IPV4		1
#define MD_L3_IPV6		2
#define MD_L4_OTHER		0
#define MD_L4_TCP		1
#define MD_L4_UDP		2

/* vlan_flags */
#define MD_VLAN_PRESENT		(1u << 0)
#define MD_VLAN_INSERT		(1u << 1)	/* h2t */
#define MD_VLAN_STRIPPED	(1u << 1)	/* t2h (same bit, dir-dependent meaning) */
#define MD_VLAN_QINQ		(1u << 2)

/* tso_flags */
#define MD_TSO_ENABLE		(1u << 0)

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif

/*
 * 64-byte, little-endian, packed. Offsets are asserted below; do not reorder.
 */
struct agnic_pport_md {
	uint16_t	md_magic;	/* 0x00  AGNIC_MD_MAGIC                                  */
	uint8_t		md_version;	/* 0x02  AGNIC_MD_VERSION                               */
	uint8_t		md_dir_flags;	/* 0x03  MD_DIR_*                                       */
	uint16_t	src_port;	/* 0x04  t2h: ingress front physport 1..9 (authoritative)*/
	uint16_t	dst_port;	/* 0x06  h2t: egress  front physport 1..9 (authoritative)*/
	uint8_t		l3_offset;	/* 0x08  L2-relative bytes to L3 (14/18/22)             */
	uint8_t		l3_proto;	/* 0x09  MD_L3_*                                        */
	uint8_t		ip_hdr_len;	/* 0x0A  IPv4 IHL in 32-bit words (5..15)               */
	uint8_t		l4_proto;	/* 0x0B  MD_L4_*                                        */
	uint16_t	l4_offset;	/* 0x0C  L2-relative bytes to L4                        */
	uint16_t	l4_hdr_len;	/* 0x0E  h2t TSO L4 hdr len; else 0                     */
	uint32_t	csum_cmd;	/* 0x10  h2t CMD_* bitmap                               */
	uint32_t	csum_res;	/* 0x14  t2h RES_* bitmap                               */
	uint32_t	rss_hash;	/* 0x18  t2h pp2 RSS hash -> m_pkthdr.flowid            */
	uint16_t	vlan_tci;	/* 0x1C  802.1Q TCI (h2t insert / t2h stripped)        */
	uint16_t	vlan_flags;	/* 0x1E  MD_VLAN_*                                      */
	uint16_t	tso_mss;	/* 0x20  h2t TSO MSS (0 = none)                         */
	uint16_t	tso_flags;	/* 0x22  MD_TSO_*                                       */
	uint16_t	orig_l2_len;	/* 0x24  full L2 length (sanity vs byte_cnt-66)         */
	uint16_t	reserved0;	/* 0x26                                                 */
	uint64_t	hw_timestamp;	/* 0x28  t2h optional; 0 if unused                     */
	uint8_t		reserved1[16];	/* 0x30  future: tunnel/inner offsets, flow-id, fw mark */
}
#if !defined(_MSC_VER)
__attribute__((packed))
#endif
;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif

/* Compile-time layout guarantees (C11 _Static_assert; available in GCC + FreeBSD kernel). */
#ifndef AGNIC_STATIC_ASSERT
#define AGNIC_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
AGNIC_STATIC_ASSERT(sizeof(struct agnic_pport_md) == 64, "agnic_pport_md must be exactly 64 bytes");
AGNIC_STATIC_ASSERT(PPORT_PREFIX == 66, "pport prefix must be 66 bytes");

#endif /* AGNIC_PPORT_HDR_H */
