// SPDX-License-Identifier: MIT
/*
 * portmap.h — static front-panel port topology (replaces NetAgent's soc_ports=1 DB).
 *
 * Three coordinate systems and the map between them:
 *   - array index   : 0..8            (dp_app internal)
 *   - pport physport : 1..9           (host <-> NPU tag: byte0 = 0x81 + index)
 *   - 88E6193X DSA port : the switch port each front jack physically lands on
 *
 * The pport side (index/physport/tag) is KNOWN and frozen by the host contract.
 * The DSA switch-port column was **MUST-VERIFY** (ARCHITECTURE §E R7) and is now
 * CONFIRMED (2026-08-27) against the board's own platform descriptor,
 * bsp/opt/sophos/plt/AMDA0208-0001R00.txt: npu0.ethN.port=1:<switch port> gives the
 * identity map 1..8 for the copper jacks and 1:9 for PortF1, and npu0.bp0.port1=1:0
 * puts the CPU/backplane on switch port 0. The linear guess was right.
 *
 * INDEPENDENTLY RE-CONFIRMED ON LIVE HARDWARE 2026-09-01, by reading the switch's own
 * Port Status (reg0) and Port-Based VLAN Map (reg6) over raw SMI and correlating them
 * against link state known from the x86 side. The map is not merely plausible, it is
 * corroborated by two independent observers:
 *   sw0  status=0x0f4d link=1 2500  vlanmap=0x03fe  -> CPU port, sees all nine jacks
 *   sw1  status=0xde0f link=1 1000  vlanmap=0x0001  -> front port1, the live mgmt link
 *   sw2..sw8            link=0      vlanmap=0x0001  -> the dark copper jacks
 *   sw9  status=0x0f4d link=1 2500  vlanmap=0x0001  -> PortF1, the SFP lane (1:9)
 * port9 reporting 2500 rather than 1000 independently corroborates the SerDes note
 * below: it is an SFP lane, not a copper jack behind an Alaska PHY.
 *
 * media is NOT decoration. DP_MEDIA_SFP means the port has no integrated Alaska PHY
 * behind it — it is a Clause-45 SerDes lane (lane number == switch port), so the
 * copper `dp_swctl phyup` path cannot bring it up and a separate `serdesup` must.
 * Missing that is what left PortF1 dark from the day NetAgent was replaced until
 * 2026-08-27. The descriptor also says npu0.eth8.init_speed=1G, so the
 * lane's PCS mode defaults to 1000BASE-X even though it is a 10G-capable lane.
 */
#ifndef DP_PORTMAP_H
#define DP_PORTMAP_H

#include <stdint.h>
#include "dp_config.h"
#include "pport_hdr.h"

enum dp_media { DP_MEDIA_COPPER_1G = 0, DP_MEDIA_SFP = 1 };

struct dp_port {
	uint8_t		index;		/* 0..8                                  */
	uint8_t		physport;	/* 1..9 (pport tag physnum)              */
	uint8_t		tag0;		/* pport tag byte0 = 0x81 + index        */
	uint8_t		dsa_port;	/* 88E6193X switch port  ** MUST-VERIFY ***/
	uint8_t		media;		/* enum dp_media                          */
	const char	*name;		/* "Port1".."Port8","PortF1"             */
};

/*
 * Empirically-derived map (2026-07-31, dp_swctl on the live 88E6193X):
 *   - switch port 0 = CPU port (to the NPU eth0); NOT a front jack.
 *   - switch ports 1..8 = the 8 front copper jacks.  ** label<->port order VERIFY **
 *   - switch port 1 currently carries the lab net + mgmt (ATU: all upstream MACs on port 1).
 * So front physport N maps to switch port N (identity, CPU excluded). The physical-LABEL
 * order (jack "3" -> which switch port) is still confirmed empirically from the DSA src-port
 * field of live RX frames; a wrong label order mis-pairs jacks but still forwards & separates.
 */
#define DP_CPU_DSA_PORT		0	/* 88E6193X CPU port (to NPU) */
static const struct dp_port dp_ports[DP_PORT_COUNT] = {
	{ 0, 1, 0x81, 1, DP_MEDIA_COPPER_1G, "Port1"  },
	{ 1, 2, 0x82, 2, DP_MEDIA_COPPER_1G, "Port2"  },
	{ 2, 3, 0x83, 3, DP_MEDIA_COPPER_1G, "Port3"  },
	{ 3, 4, 0x84, 4, DP_MEDIA_COPPER_1G, "Port4"  },
	{ 4, 5, 0x85, 5, DP_MEDIA_COPPER_1G, "Port5"  },
	{ 5, 6, 0x86, 6, DP_MEDIA_COPPER_1G, "Port6"  },
	{ 6, 7, 0x87, 7, DP_MEDIA_COPPER_1G, "Port7"  },
	{ 7, 8, 0x88, 8, DP_MEDIA_COPPER_1G, "Port8"  },
	{ 8, 9, 0x89, 9, DP_MEDIA_SFP,       "PortF1" },
};

/* pport physport (1..9) -> dp_ports[] index (0..8), or -1. */
static inline int dp_phys_to_index(uint8_t phys)
{
	return (phys >= 1 && phys <= DP_PORT_COUNT) ? (int)(phys - 1) : -1;
}

/* 88E6193X DSA source port -> dp_ports[] index (0..8), or -1. Linear scan (tiny table). */
static inline int dp_dsa_to_index(uint8_t dsa_port)
{
	for (int i = 0; i < DP_PORT_COUNT; i++)
		if (dp_ports[i].dsa_port == dsa_port)
			return i;
	return -1;
}

#endif /* DP_PORTMAP_H */
