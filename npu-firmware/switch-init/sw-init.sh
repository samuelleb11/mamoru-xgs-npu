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

echo "(1) per-port VLAN maps FIRST: front ports 1-10 -> CPU only (reg6=0x001); CPU -> all front (0x7fe)"
ISO_FAIL=0
for p in 1 2 3 4 5 6 7 8 9 10; do wv $p 6 0x001 || ISO_FAIL=$((ISO_FAIL + 1)); done
wv 0 6 0x7fe || ISO_FAIL=$((ISO_FAIL + 1))
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
