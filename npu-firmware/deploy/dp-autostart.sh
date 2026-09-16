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
#
# Look for each module in $DP first, then on the NPU's own rootfs. A deploy staged in
# /tmp (the only writable place on a factory NPU) contains dp_fwd and the kit's scripts
# but NOT the .ko files -- those are the vendor's, they are already installed under
# /lib/modules, and we deliberately ship none. Loading only from the current directory
# therefore worked for /opt/dp and silently failed everywhere else.
load_uio() {
	_m=$1; shift
	if [ -f "$DP/$_m" ] && insmod "$DP/$_m" "$@" 2>/dev/null; then return 0; fi
	_p=$(find /lib/modules -name "$_m" 2>/dev/null | head -1)
	if [ -n "$_p" ] && insmod "$_p" "$@" 2>/dev/null; then return 0; fi
	echo "[dp-autostart] $_m: already loaded or not found"
}
load_uio musdk_cma.ko
load_uio mv_dmax2_uio.ko
load_uio uio_pdrv_genirq.ko of_id=generic-uio

# --- 2. Native switch bring-up (clean-room; replaces xgs-mvl6193-init). ---
#
# sw-init.sh writes the front-port isolation maps FIRST and refuses to power the front PHYs if
# it cannot verify them (see its header). It applies isolation ONCE, and that is deliberate:
# this path execs dp_fwd directly, so after sw-init.sh returns nothing else on the NPU touches
# the switch. There is no reprogrammer to race.
#
# THE APPLIANCE PATH IS DIFFERENT AND NEEDS MORE. The stock launcher hands off to armada, whose
# UMSD_NPU comes up LATE -- well after the forwarder -- and reprograms the switch, restoring
# stock bridging. A one-shot write there is silently undone; that is what looped a live LAN on
# 2026-08-08. The appliance therefore runs a background re-assertion sweep
# (platform/sophos-xgs116/npu/scripts/dp-autostart.sh in the mamoru tree) which re-reads reg 6,
# re-asserts, and reads back forever.
#
# IF YOU ADD ANYTHING TO THIS PATH THAT CAN REPROGRAM THE SWITCH -- a vendor init, UMSD, a
# second forwarder, your own tooling -- THE ONE-SHOT IS NO LONGER SUFFICIENT and you must port
# that sweep. The absence of a sweep here is a consequence of this path's simplicity, not a
# judgement that re-assertion is unnecessary.
sh "$DP/sw-init.sh"

# --- 3. Hand the datapath to dp_fwd (built from forwarder.c against MUSDK).
#        -g 2 GIU id, -i eth0 host-trunk netdev, -f nmp config. ---
#
# TWO THINGS THIS SCRIPT CANNOT DO FOR YOU, both of which apply when you are taking a
# FACTORY NPU over rather than booting a rootfs that already runs this kit:
#
#   1. The vendor data plane owns eth0 and the GIU. If it is still running, dp_fwd cannot
#      bind and exits. Stop it before you get here. (Its process name is the vendor's, not
#      ours, so this script does not guess at it and will not kill something on your
#      appliance by name-matching.)
#
#   2. The HOST must re-drive its side afterwards. The host publishes its AGNIC management
#      rings ONCE, at P3, and the NPU latches them when dp_fwd starts -- so a dp_fwd that
#      starts after the host driver is already up has no path back to it. Reload the host
#      driver once this is running: `kldunload if_agnic; kldload if_agnic` on FreeBSD,
#      `rmmod mamoru_agnic; insmod mamoru_agnic.ko` on Linux. See dp-swap-guarded.sh for
#      the measurement behind this.
echo "[dp-autostart] starting dp_fwd from $DP; now reload the host driver so it re-drives P3"
exec env LD_LIBRARY_PATH=/lib:/usr/lib ./dp_fwd -g 2 -i eth0 -c 1 -a 1 -f dp-nmp-config.txt --no-stat
