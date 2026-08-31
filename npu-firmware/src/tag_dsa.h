// SPDX-License-Identifier: MIT
/*
 * tag_dsa.h — the frame transform at the heart of the forwarder.
 *
 * Two tagging domains meet here (ARCHITECTURE §A.3):
 *   host side  : pport prefix  [0x81+idx][0x00][64B agnic_pport_md] + ethernet   (66 B prefix)
 *   switch side: Marvell 4-byte DSA tag inserted after the 12-byte MAC header
 *                [dst6][src6][DSA4][ethertype ...]
 *
 * The forwarder is the ONLY translator between them. Both transforms are done in-place
 * inside the buffer's headroom via a 12-byte memmove of the MAC header (near-zero-copy):
 * the 66-byte pport prefix region provides the room the 4-byte DSA insert/strip needs.
 *
 * ** EMPIRICAL-VERIFY (ARCHITECTURE §E R7, portmap.h) **: DSA_DEV, the front-jack<->switch
 * port map (portmap dsa_port), and whether the switch emits plain 4-byte DSA (vs EDSA) are
 * confirmed against the live 88E6193X + the DSA byte0 of real TO_CPU frames on first deploy.
 * The stock mvpp2_eth0_dsa.conf used dsa_mode=dsa (4-byte), so 4-byte DSA is the starting point.
 */
#ifndef DP_TAG_DSA_H
#define DP_TAG_DSA_H

#include <string.h>
#include <stdint.h>
#include "pport_hdr.h"
#include "portmap.h"

#define DP_DSA_DEV		2	/* switch device number in the DSA tag. VERIFIED from live TO_CPU
					 * frames: byte0=0xc2 -> src_dev = 0xc2 & 0x1f = 2 (matches SMI addr 2).
					 * FROM_CPU frames MUST target dev 2 or the switch drops them (was 0 -> the
					 * host->front direction was silently dropped -> mgmt TX dead). */
#define DSA_LEN			4
#define ETH_MAC_LEN		12	/* dst(6)+src(6) before the DSA insertion point */

/* Marvell 4-byte DSA tag commands (bits 7:6 of byte0). */
#define DSA_CMD_TO_CPU		0x0
#define DSA_CMD_FROM_CPU	0x1
#define DSA_CMD_TO_SNIFFER	0x2
#define DSA_CMD_FORWARD		0x3

/* Build a FROM_CPU DSA tag (host->switch) targeting switch port `dsa_port`. Untagged frame. */
static inline void dsa_build_from_cpu(uint8_t *t, uint8_t dsa_port)
{
	t[0] = (DSA_CMD_FROM_CPU << 6) | (DP_DSA_DEV & 0x1f);	/* 0x40 | dev            */
	t[1] = (uint8_t)((dsa_port & 0x1f) << 3);		/* target port in 7:3    */
	t[2] = 0x00;
	t[3] = 0x00;
}

/* Parse a DSA tag; return the source switch port and command. */
static inline uint8_t dsa_src_port(const uint8_t *t) { return (uint8_t)((t[1] >> 3) & 0x1f); }
static inline uint8_t dsa_cmd(const uint8_t *t)      { return (uint8_t)((t[0] >> 6) & 0x3); }

/*
 * ** SWITCH MODE **: the 88E6193X CPU port (switch port 0) is put in Marvell DSA mode
 * (sw-init.sh: w 0 4 0x17f). Every frame egressing the CPU port then carries a
 * 4-byte TO_CPU DSA tag whose src field is the ingress switch port; frames we inject FROM_CPU
 * carry a 4-byte FROM_CPU tag selecting the egress switch port. This is what gives true
 * per-front-port separation: dp_fwd maps switch-port <-> pport index via portmap.h.
 *
 * Buffer math (in place, near-zero-copy; the 66B pport headroom absorbs the 4B DSA delta):
 *   t2h  in : [dst6][src6][DSA4][payload]        (from pp2, len)
 *        out: [tag0][00][md64][dst6][src6][payload]   (66B prefix + eth, len+62), out=frame-62
 *   h2t  in : [tag0][00][md64][dst6][src6][payload]   (from giu, len)
 *        out: [dst6][src6][DSA4][payload]        (to pp2, len-62), out=frame+62
 */

/*
 * host -> NPU -> front (giu RX buffer -> pp2 TX buffer), in place. Read the pport tag to pick
 * the target switch port, drop the 66B prefix, and insert a 4-byte FROM_CPU DSA tag after the
 * 12-byte MAC header. OUT: *out = [MAC][DSA4][payload]; *out_len = len - 62. <0 => drop.
 */
static inline int dp_h2t_transform(uint8_t *frame, uint16_t len, uint8_t **out, uint16_t *out_len)
{
	if (len < PPORT_PREFIX + ETH_MAC_LEN)
		return -1;
	uint8_t phys = PPORT_TAG_TO_PHYS(frame[0]);	/* 0x81->1 .. 0x89->9 */
	int idx = dp_phys_to_index(phys);
	if (idx < 0)
		return -2;				/* unknown host port -> drop */
	uint8_t dsa_port = dp_ports[idx].dsa_port;

	uint8_t *eth = frame + PPORT_PREFIX;		/* [dst6][src6][payload] */
	uint8_t *mac_dst = eth - DSA_LEN;		/* shift the 12B MAC left by 4 */
	memmove(mac_dst, eth, ETH_MAC_LEN);
	dsa_build_from_cpu(mac_dst + ETH_MAC_LEN, dsa_port);	/* DSA4 right after the MAC */

	*out = mac_dst;					/* = frame + PPORT_PREFIX - DSA_LEN = frame+62 */
	*out_len = (uint16_t)(len - PPORT_PREFIX + DSA_LEN);
	return 0;
}

/*
 * front -> NPU -> host (pp2 RX buffer -> giu TX buffer), in place. Parse the TO_CPU DSA tag to
 * learn the source switch port, strip the 4-byte DSA, and prepend the 66B pport prefix whose
 * tag byte0 selects the matching host port. `headroom` = bytes before `frame` (>= 62). <0 => drop.
 */
static inline int dp_t2h_transform(uint8_t *frame, uint16_t len, uint16_t headroom,
				   const struct agnic_pport_md *md_src,
				   uint8_t **out, uint16_t *out_len)
{
	(void)md_src;
	if (len < ETH_MAC_LEN + DSA_LEN)
		return -1;
	if (headroom < PPORT_PREFIX - DSA_LEN)		/* need >= 62 bytes of headroom */
		return -3;

	const uint8_t *dsa = frame + ETH_MAC_LEN;	/* tag sits right after dst+src */
	uint8_t src = dsa_src_port(dsa);
	int idx = dp_dsa_to_index(src);
	if (idx < 0)
		return -4;	/* FAIL-CLOSED: unknown src switch port -> DROP, counted by the
				 * caller as dp_t2h_badport. This used to fail OPEN, delivering the
				 * frame to host Port1 "to keep mgmt/traffic alive" — but Port1 is
				 * the management LAN, so the effect was to inject an UNATTRIBUTED
				 * frame into the most sensitive segment, and silently: the aggregate
				 * drop counter never moved, so nothing distinguished it from normal
				 * traffic. It cannot fire in normal operation — dp_ports covers every
				 * front jack (switch ports 1..9), a TO_CPU tag's src is the ingress
				 * port and so is never the CPU port 0, and port 10 is unpopulated on
				 * this board — so reaching it means a malformed tag or a switch port
				 * we do not model. Neither is something to hand to the LAN. */

	/* slide the 12B MAC right by 4 (over the DSA tag); ethernet now at frame+4, len-4 bytes */
	memmove(frame + DSA_LEN, frame, ETH_MAC_LEN);
	uint8_t *eth = frame + DSA_LEN;
	uint16_t eth_len = (uint16_t)(len - DSA_LEN);

	uint8_t *pfx = eth - PPORT_PREFIX;		/* = frame + 4 - 66 = frame - 62 */
	pfx[0] = dp_ports[idx].tag0;			/* 0x81 + idx -> host PortN */
	pfx[1] = 0x00;
	struct agnic_pport_md *md = (struct agnic_pport_md *)(pfx + PPORT_TAG_LEN);
	memset(md, 0, sizeof(*md));
	md->md_magic = AGNIC_MD_MAGIC;
	md->md_version = AGNIC_MD_VERSION;
	md->md_dir_flags = MD_DIR_T2H;
	md->src_port = dp_ports[idx].physport;
	md->orig_l2_len = eth_len;

	*out = pfx;
	*out_len = (uint16_t)(PPORT_PREFIX + eth_len);
	return 0;
}

#endif /* DP_TAG_DSA_H */
