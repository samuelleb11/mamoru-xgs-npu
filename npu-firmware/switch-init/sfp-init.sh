#!/bin/sh
# SPDX-License-Identifier: MIT
# PortF1 SFP bring-up — switch port 9 / SerDes lane 9 on the 88E6193X.
#
# WHY THIS IS A SEPARATE SCRIPT. dp-autostart.sh does its per-port work in `for p in 1..8` and
# ends with `exec dp_fwd`, so it cannot be re-run to test a change — a second forwarder crashes
# the NPU. This stanza is idempotent and safe to run by hand against a live switch, which is how
# it gets proven before it ever runs at boot.
#
# CALL IT AFTER THE VLAN MAPS ARE WRITTEN. A port brought up before its map can egress to every
# other front port and bridge the customer's networks inside the switch; that looped a live
# LAN on 2026-08-08. Not hypothetical here: port 9 was found holding reg6 = 0x05ff on the live
# box while every other front port held 0x001, because it is the port every loop skips.
#
# Port 9 has NO integrated Alaska PHY — the C22 `reg0 = 0x9140` that powers copper ports 1-8
# cannot reach it. It is a Clause-45 SerDes lane (lane number == port number), like the CPU port.
#
# WHAT IS PROVEN, AND WHAT IS NOT (measured on the appliance 2026-08-27):
#   PROVEN  lane 9 boots POWERED DOWN — BMCR read 0x1940 vs lane 0's 0x1140, bit 11 set — and its
#           POC read 0x0078, byte-identical to UNPOPULATED lane 10, i.e. never configured at all.
#           These writes clear that: readback POC 0x004d, BMCR 0x1140, cmode 0xd, matching the
#           live CPU trunk in every field.
#   PROVEN  reg6 = 0x001 isolation, readback confirmed.
#   NOT     that the port then LINKS. It does not, with a 10G passive DAC (SFP-H10GB-CU1M) to a
#           Nami CS110-24. All four rate x autoneg combinations were tried and all stay down.
#           The far end is the suspect: the CS110's DTS gives only sfp0/`lan25` an `i2c-bus`, and
#           says a cage without one "registers but stays down". Bringing this lane up is
#           necessary, not sufficient.
#
# ORDER MATTERS: POC before BMCR. A BMCR write to a lane still powered down at the POC level is
# silently swallowed by the unclocked PCS — measured, the write did not take.
#
# Overridable: SFP_POC 0x8048 = 1000BASE-X (the board's declared init_speed=1G) with SFP_CMODE
# 0x9; the defaults below are 10GBASE-R, which is what the DAC's EEPROM (byte 0x0c = 0x67 =
# 10.3 GBd) and the CS110's `phy-mode = "10gbase-r"` both call for.

S=${S:-/opt/dp/swmdio.sh}
LANE=${SFP_LANE:-9}
POC=${SFP_POC:-0x804d}
CMODE=${SFP_CMODE:-0xd}
BMCR=${SFP_BMCR:-0x9140}

w() { sh "$S" wr "$@" >/dev/null 2>&1; }
r() { sh "$S" rd "$@" 2>/dev/null; }

# 1. ISOLATE FIRST, then allow forwarding. Never the other way round.
w "$LANE" 6 0x001
w "$LANE" 4 0x7f

# 2. Port cmode (reg 0x00 bits[3:0]) must AGREE with the SerDes PCS mode. Different registers:
#    setting the POC alone leaves the port at its old cmode and nothing links. Read-modify-write.
sts=$(r "$LANE" 0)
case "$sts" in
0x*) w "$LANE" 0 $(( (sts & 0xfff0) | CMODE )) ;;
*)   echo "[sfp-init] WARN: could not read port$LANE reg0; leaving cmode alone" ;;
esac

# 3. SerDes lane, C45 via Global2: address phase, then write phase.
A=$(( 0x8000 | (LANE << 5) | 4 ))
W=$(( 0x8400 | (LANE << 5) | 4 ))
w 0x1c 0x19 0xf002; w 0x1c 0x18 "$A"; w 0x1c 0x19 "$POC";  w 0x1c 0x18 "$W"
w 0x1c 0x19 0x2000; w 0x1c 0x18 "$A"; w 0x1c 0x19 "$BMCR"; w 0x1c 0x18 "$W"

echo "[sfp-init] port$LANE reg0=$(r "$LANE" 0) reg6=$(r "$LANE" 6) (want reg0 low nibble $CMODE, reg6 0x0001)"
