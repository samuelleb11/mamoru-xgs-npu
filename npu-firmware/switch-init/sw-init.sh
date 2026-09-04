#!/bin/sh
# SPDX-License-Identifier: MIT
# Native MV88E6193X switch init for the XGS116 — full working config, applied via native SMI
# (swmdio.sh). No libsbsp / mvmdio_uio / platform DB. Ports = raw dev 0x0..0x9, g2 = 0x1c.
# = stock xgs-mvl6193-init (reverse-engineered from libsbsp) + the dp_swctl DSA/VLAN/PHY steps.
# VERIFIED end-to-end: front-jack VLAN30 -> host portN -> DHCP lease.
# write_switch_phy = C22 via Global2(0x1c) SMI-PHY Cmd(0x18)+Data(0x19): Data=val;
#   Cmd=0x9400|(port<<5)|reg (page via reg22 first).  write_switch_dev = C45: Data=addr;
#   Cmd=0x8000|(dev<<5)|devad ; Data=val ; Cmd=0x8400|(dev<<5)|devad.
#
# ============================ MAPS BEFORE PHYs — THE INTERLOCK ============================
# The per-port VLAN maps are written FIRST, before anything powers a PHY, and the PHY steps
# REFUSE TO RUN if the maps could not be verified. This ordering is a safety property, not a
# style choice.
#
# Our topology is hub-and-spoke: every front port may egress ONLY to the CPU port, because all
# forwarding decisions belong to the host firewall. Two front ports sharing a broadcast domain
# bridge the customer's network to itself and produce a loop. On 2026-08-08 that took out a live
# LAN and needed mains power cut to recover.
#
# After a switch reset reg 6 holds the part's default map (all ports), so a port that is POWERED
# and FORWARDING before its map is written is briefly a bridge. Until 2026-09-01 this file did
# exactly that: step (4) powered ports 1-8 and step (5) wrote the maps afterwards. The rule was
# already stated in this file, in the SFP section's comment, and applied there only -- the eight
# copper jacks it walked past did not get it. Dark ports cannot bridge; that is the entire
# argument, and it only holds if the maps land first.
#
# Do not reorder these steps. If you add a port, add it to the map loop, not just to phyup.
# Override the refusal with SW_ISO_ENFORCE=0 only if you know why you are doing it.
# =========================================================================================
SW=${SW:-/opt/dp/swmdio.sh}
ISO_ENFORCE=${SW_ISO_ENFORCE:-1}

w() { sh "$SW" wr "$@" >/dev/null 2>&1; }
r() { sh "$SW" rd "$@" 2>/dev/null; }

# wv DEV REG VAL — write, then READ BACK and compare. Non-zero on transaction failure, on an
# unreadable register, or on a mismatch. An unverifiable write is treated as a FAILED write:
# "I could not check" and "it worked" are different claims and must not collapse into each other.
wv() {
	sh "$SW" wr "$1" "$2" "$3" >/dev/null 2>&1 ||
		{ echo "      port$1 reg$2: WRITE TRANSACTION FAILED"; return 1; }
	_got=$(r "$1" "$2")
	case "$_got" in
	0x*) ;;
	*) echo "      port$1 reg$2: READ-BACK INVALID ('${_got:-empty}') -- cannot verify"; return 1 ;;
	esac
	[ "$((_got))" -eq "$(($3))" ] ||
		{ echo "      port$1 reg$2: MISMATCH wrote $3, reads $_got"; return 1; }
	return 0
}

# phyup PORT: C22 internal-PHY reg0 = 0x9140 (power up + reset), page 0
phyup() { w 0x1c 0x19 0x0000; w 0x1c 0x18 $((0x9416 | ($1<<5))); w 0x1c 0x19 0x9140; w 0x1c 0x18 $((0x9400 | ($1<<5))); }

# THESE TWO WRITES ARE ABSOLUTE, AND THAT IS LOAD-BEARING FAR OUTSIDE THIS SCRIPT.
#
# Setting reg 6 to a constant rather than read-modify-writing it is what makes a MAINS CYCLE a
# complete and deterministic undo for ANY reg-6 value the box is left holding — including one put
# there by D84's guarded VLAN-map write over the host mailbox. That is the abort path for the whole
# write half of D84, and it is the ONLY one: the host-side never-widen guard refuses to restore a
# narrowed map (restoring is itself a widening), so the mailbox cannot undo its own writes, and the
# NPU console can only help while the management port's map is intact enough to reach the host.
#
# So a change here that looks locally reasonable — preserving existing bits, merging with what is
# already there, "not clobbering the operator's map" — SILENTLY DELETES D84's abort path, and no
# test anywhere goes red. If these writes ever need to stop being absolute, the write half of D84
# needs a different abort first, and that is a decision for whoever owns it, not a side effect of
# tidying this loop.
# DEBT #151: THE CPU EGRESS MAP IS DERIVED, NOT TYPED, because three hand-written counts of the
# same thing disagreed and the write-verify passed on the wrong one.
#
# The board has eleven switch ports. The descriptor assigns port 0 -> the CN9131 backplane (CPU)
# and port 9 -> the SFP cage, and assigns port 10 NOTHING — there is no `port=1:10` anywhere in the
# tree — while the AGNIC driver independently sets AGNIC_PPORT_COUNT = 9. So the CPU port's egress
# map covers ports 1-9, which is 0x3fe. This line wrote 0x7fe, adding bit 10 — a real 10G-capable
# SerDes lane with no assigned role — to the CPU's egress map at every boot, VERIFIED, and
# dp-autostart.sh narrowed it a few lines later. It corrected; it did not corrupt. The defect was
# upstream of the correction.
#
# WHY THIS IS SAFE WITHOUT ANSWERING "IS PORT 10 WIRED?", which is still unanswered and must not be
# read as answered here: `npu_switch_ports` at diag=0 reads `sw=10 link=0` — DARK, WHICH IS NOT
# PROOF; a lane can be cabled and dark. What licenses the change is a different, MEASURED fact:
# `sw=0 vlanmap=0x03fe` on the running box. dp-autostart.sh's narrowing is unconditional, so
# 0x3fe has been this appliance's steady state for its entire uptime. Narrowing here changes only
# the TRANSIENT window between this line and that one; it cannot regress anything that is not
# already excluded seconds later on every boot.
#
# TWO LISTS, DELIBERATELY. Port 10 keeps its own CPU-only isolation write — isolating a lane with
# no role costs nothing and an UNWRITTEN map is the hazard (a powered front port with the part's
# default map bridges the network to itself, 2026-08-08). What it does NOT get is a bit in the
# CPU's egress map. Narrowing the isolation loop instead would have been the unsafe half of the
# same edit.
ISOLATE_PORTS="1 2 3 4 5 6 7 8 9 10"      # every front port: CPU-only, role or no role
CPU_EGRESS_PORTS="1 2 3 4 5 6 7 8 9"      # ports with an assigned role (descriptor + PPORT_COUNT)
CPU_MAP=0
for p in $CPU_EGRESS_PORTS; do CPU_MAP=$((CPU_MAP | (1 << p))); done
CPU_MAP_HEX=$(printf '0x%03x' "$CPU_MAP")

echo "(1) per-port VLAN maps FIRST: front ports $ISOLATE_PORTS -> CPU only (reg6=0x001);"
echo "    CPU -> ports $CPU_EGRESS_PORTS ($CPU_MAP_HEX)"
ISO_FAIL=0
for p in $ISOLATE_PORTS; do wv $p 6 0x001 || ISO_FAIL=$((ISO_FAIL + 1)); done
wv 0 6 "$CPU_MAP" || ISO_FAIL=$((ISO_FAIL + 1))
if [ "$ISO_FAIL" -eq 0 ]; then
	echo "      isolation VERIFIED by read-back on all 11 ports"
else
	echo "      *** ISOLATION UNVERIFIED on $ISO_FAIL port(s) -- front ports are NOT isolated ***"
fi

echo "(2) port control: ports 0-9 reg4 = 0x7f (forwarding)"
for d in 0 1 2 3 4 5 6 7 8 9; do w $d 4 0x7f; done

echo "(3) CPU port 0 -> DSA mode: reg4 = 0x17f (FrameMode=DSA; switch tags by src port)"
w 0 4 0x17f

echo "(4) CPU-port 10G SERDES bring-up: C45 port0 MMD4 0xf002=0x804d, 0x2000=0x9140"
w 0x1c 0x19 0xf002; w 0x1c 0x18 0x8004; w 0x1c 0x19 0x804d; w 0x1c 0x18 0x8404
w 0x1c 0x19 0x2000; w 0x1c 0x18 0x8004; w 0x1c 0x19 0x9140; w 0x1c 0x18 0x8404

# The CPU port is exempt from the interlock above: it is the ONLY port isolation points AT, so
# powering it cannot create a front-to-front path. Steps (5) and (6) are the gated ones.
if [ "$ISO_FAIL" -ne 0 ] && [ "$ISO_ENFORCE" = 1 ]; then
	echo "(5) SKIPPED and (6) SKIPPED -- front PHYs held DARK because isolation is unverified."
	echo "      A powered front port with an unwritten VLAN map bridges the network to itself."
	echo "      The CPU trunk is up, so the box stays reachable: diagnose with '$SW rd <port> 6'."
	echo "      Override deliberately with SW_ISO_ENFORCE=0 if you accept the loop risk."
	echo "DONE (DEGRADED). eth0 should link; front jacks are intentionally dark."
	exit 1
fi

echo "(5) power up all front copper PHYs (ports 1-8)"
for p in 1 2 3 4 5 6 7 8; do phyup $p; done

# Port 9 = PortF1, the SFP cage (platform DB AMDA0208-0001: npu0.phy8.type=SFP,
# npu0.eth8.port=1:9, init_speed=1G). It has NO integrated copper PHY, so step (5)'s phyup
# cannot reach it -- it is a C45 SerDes lane exactly like the CPU port and needs step (4)'s
# treatment with its own lane number. Without this the cage is DARK: measured on the appliance
# 2026-08-27, lane 9's BMCR read 0x1940 against lane 0's 0x1140 -- identical but for bit 11,
# the MII power-down bit -- and its POC was byte-identical to UNPOPULATED lane 10, i.e. never
# configured at all. port9 had rx_packets=0 for its whole life.
#
# THIS RUNS AFTER THE VLAN MAPS DELIBERATELY, and is gated on them above. A port brought up
# before its VLAN map is written can egress to every other front port, bridging the customer's
# networks inside the switch; that is what looped a live LAN on 2026-08-08
# (NPU-SWITCH-BRINGUP.md: "phyup must come strictly AFTER the VLAN maps"). It is not
# hypothetical for THIS port: port 9 was found holding reg6 = 0x05ff on the live box while
# every other front port held 0x001, because it is the port every bring-up loop skips.
#
# POC mode field: 5 = 10GBASE-R (what the vendor writes to lane 0), 0 = 1000BASE-X. The board
# declares init_speed=1G so the default is 0x8048; SFP_POC=0x804d selects 10GBASE-R. A PASSIVE
# DAC has no PCS of its own, so this must MATCH the far-end switch port's rate, and the port's
# own cmode (reg 0x00 bits[3:0]) must agree with it -- 0x9 = 1000BASE-X, 0xd = 10GBASE-R.
# ORDER MATTERS: POC first, then BMCR. Writing BMCR while the lane is powered down at the POC
# level is silently swallowed by the unclocked PCS -- measured, the write did not take.
SFP_LANE=${SFP_LANE:-9}
SFP_POC=${SFP_POC:-0x8048}
SFP_CMODE=${SFP_CMODE:-0x9}
echo "(6) SFP PortF1 SERDES bring-up: C45 lane $SFP_LANE MMD4 0xf002=$SFP_POC, 0x2000=0x9140"
sfp_cmd_addr=$(( 0x8000 | ($SFP_LANE << 5) | 4 ))
sfp_cmd_wr=$((   0x8400 | ($SFP_LANE << 5) | 4 ))
# port cmode: read-modify-write the low nibble of port reg 0x00 so it agrees with the POC.
sfp_sts=$(sh "$SW" rd $SFP_LANE 0 2>/dev/null)
case "$sfp_sts" in
0x*) w $SFP_LANE 0 $(( ($sfp_sts & 0xfff0) | $SFP_CMODE )) ;;
*)   echo "      (could not read port$SFP_LANE reg0; leaving cmode alone)" ;;
esac
w 0x1c 0x19 0xf002; w 0x1c 0x18 $sfp_cmd_addr; w 0x1c 0x19 $SFP_POC; w 0x1c 0x18 $sfp_cmd_wr
w 0x1c 0x19 0x2000; w 0x1c 0x18 $sfp_cmd_addr; w 0x1c 0x19 0x9140;   w 0x1c 0x18 $sfp_cmd_wr

echo "DONE. (eth0 should show 'Link is Up - 10Gbps'; front jacks link when cabled)"
echo "      PortF1/SFP: verify the port-9 SerDes trains and the link comes up (switch port-9 status)."
