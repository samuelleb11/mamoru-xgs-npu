#!/bin/sh
# SPDX-License-Identifier: MIT
# dp-autostart.sh — clean-room NPU data-plane launcher for the Sophos XGS.
#
# Runs on the CN9130 NPU at boot. Brings the Marvell 88E6193X switch up natively
# (no libsbsp, no xgs-mvl6193-init, no platform DB), then execs dp_fwd, which
# fans the front ports over the single AGNIC PCIe trunk to the host driver
# (Linux mamoru-agnic / FreeBSD if_agnic).
#
# This is the CLEAN-ROOM path only. The stock appliance launcher also had a
# vendor branch (handing off to dp_launch.sh -> xgs-mvl6193-init + dp_swctl,
# which link the proprietary libsbsp); that branch is intentionally NOT shipped
# here. Everything below is ours, or your own box's Marvell UIO modules.
#
# >>> XGS 136 ADAPTATION: the switch-init loops assume the 116's 8 front ports.
#     If your board has more/fewer ports, edit sw-init.sh (see docs/HARDWARE.md).
set -e

DP=${DP:-/opt/dp}          # where this kit is deployed on the NPU
cd "$DP"

# --- 1. Marvell UIO modules. These are GPL-2.0 Marvell kernel modules ALREADY on
#        your XGS's NPU rootfs (lib/modules/.../extra/) — we ship nothing here; we
#        load your box's own copies. See docs/VENDOR-BITS.md. ---
insmod musdk_cma.ko    2>/dev/null || echo "[dp-autostart] musdk_cma.ko: already loaded or not found"
insmod mv_dmax2_uio.ko 2>/dev/null || echo "[dp-autostart] mv_dmax2_uio.ko: already loaded or not found"
insmod uio_pdrv_genirq.ko of_id=generic-uio 2>/dev/null || true

# --- 2. Native switch bring-up (clean-room; replaces xgs-mvl6193-init). ---
sh "$DP/sw-init.sh"

# --- 3. Hand the datapath to dp_fwd (built from forwarder.c against MUSDK).
#        -g 2 GIU id, -i eth0 host-trunk netdev, -f nmp config. ---
exec env LD_LIBRARY_PATH=/lib:/usr/lib ./dp_fwd -g 2 -i eth0 -c 1 -a 1 -f dp-nmp-config.txt --no-stat
