#!/bin/sh
# SPDX-License-Identifier: MIT
# probe.sh — RUN THIS FIRST, on the target XGS, before building anything.
#
# Every proof point for this kit is on a Sophos XGS 116. Your board is a 136
# ("same hardware mostly"). This confirms the one thing that must be true for the
# host driver to be a near-drop-in: the Marvell AGNIC endpoint enumerates with
# the expected PCI id and BAR sizes. If it does, proceed. If it differs, the 136
# delta is the first thing to characterize (see docs/HARDWARE.md).
#
# Works on Linux (IPFire) and FreeBSD (pfSense). Run as root.

echo "== mamoru-xgs-npu hardware probe =="
echo

want_id="11ab:7080"          # Marvell AGNIC PF (VF is 7081)
want_bars="BAR0=1M  BAR2=16M  BAR4=16M"

if command -v lspci >/dev/null 2>&1; then
    echo "[Linux] AGNIC endpoint (expecting $want_id):"
    lspci -nn | grep -i "11ab:70" || echo "  NOT FOUND — no 11ab:70xx device. This may not be an AGNIC XGS, or the NPU EP is not up."
    echo
    echo "[Linux] BAR layout (expecting $want_bars):"
    dev=$(lspci -Dn | awk -F' ' '/11ab:7080/{print $1}' | head -1)
    if [ -n "$dev" ]; then lspci -vs "$dev" | grep -iE "Region|Memory at"; else echo "  (no 7080 PF found)"; fi
elif command -v pciconf >/dev/null 2>&1; then
    echo "[FreeBSD] AGNIC endpoint (expecting vendor=0x11ab device=0x7080):"
    pciconf -l | grep -i "chip=0x708011ab" || pciconf -l | grep -i "0x11ab" || echo "  NOT FOUND — no Marvell 0x11ab device."
    echo
    echo "[FreeBSD] BAR layout (expecting $want_bars):"
    sel=$(pciconf -l | awk -F'@| ' '/chip=0x708011ab/{print $1}' | head -1)
    if [ -n "$sel" ]; then pciconf -br "$sel" 2>/dev/null || pciconf -lbv "$sel" | grep -iE "bar|memory"; else echo "  (no 7080 PF found)"; fi
else
    echo "Neither lspci nor pciconf found. Install pciutils (Linux) or use base pciconf (FreeBSD)."
    exit 1
fi

echo
echo "== interpretation =="
echo " - id 11ab:7080 present + BARs 1M/16M/16M  -> host driver is a near-drop-in; proceed to docs/BUILD.md"
echo " - id present, BARs differ                 -> characterize the 136 endpoint before building"
echo " - id absent                               -> the NPU endpoint is not enumerating; check that the"
echo "                                              NPU booted and its EP driver (mv_armada_ep) is up"
echo "                                              on the NPU's own OS (see docs/VENDOR-BITS.md)"
