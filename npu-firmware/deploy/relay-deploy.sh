#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs ON the host: relay the whole deployable kit to the NPU over mvmgmt0.
# Build dp_fwd and put it, with dp-nmp-config.txt, in the host's /tmp first; the switch
# scripts are taken from this checkout.
#
# THE SET HAS TO MATCH WHAT dp-autostart.sh ACTUALLY OPENS, not just dp_fwd. It runs
# `sh "$DP/sw-init.sh"`, and sw-init.sh in turn runs its sibling swmdio.sh -- so a relay
# that copied only dp_fwd and the nmp config left `DP=/tmp/dp sh dp-autostart.sh` to abort
# on a missing sw-init.sh under `set -e`, which is exactly the documented way to deploy to
# a factory NPU (read-only rootfs, /tmp is the only writable place).
#
# >>> Set NPU (your NPU's link-local on mvmgmt0). Discover it, don't guess: bring mvmgmt0 up
#     (`ip link set mvmgmt0 up` -- it is created DOWN), then `ping6 -c2 ff02::1%mvmgmt0` and read
#     `ip -6 neigh show dev mvmgmt0`. Full procedure + the whole install: docs/NPU-INSTALL.md.
#     KEY defaults to the bundled key the factory NPU already trusts (keys/README.md explains
#     what it is and why it is in this repo); set NPU_KEY to use your own once you've added it.
#     DST is where dp_fwd lives on the NPU; it must match dp-autostart.sh's DP dir.
KEY=${NPU_KEY:-"$(dirname "$0")/keys/mvmgt.x86"}
NPU=${NPU_LL:-'fe80::YOUR-NPU-EUI64%mvmgmt0'}
DST=${NPU_DST:-/opt/dp}
ifconfig mvmgmt0 inet6 -ifdisabled auto_linklocal up >/dev/null 2>&1
i=0; while [ $i -lt 8 ]; do ping6 -c1 -W1 "$NPU" >/dev/null 2>&1 && break; sleep 1; i=$((i+1)); done
ping6 -c1 -W1 "$NPU" >/dev/null 2>&1 || { echo NPU_DOWN; echo "### DONEDPLOY"; exit 0; }
SSH="ssh -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 root@$NPU"
SCP="scp -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8"
SWDIR="$(dirname "$0")/../switch-init"
echo "=== relay to NPU $DST ==="
# $DST is not guaranteed to exist: /opt/dp does on a kit rootfs, /tmp/dp does not on any
# box. scp does not create it, and fails per-file if it is missing.
$SSH "mkdir -p $DST" 2>&1 | tail -1
$SCP /tmp/dp_fwd            "root@[$NPU]:$DST/dp_fwd"            2>&1 | tail -1
$SCP /tmp/dp-nmp-config.txt "root@[$NPU]:$DST/dp-nmp-config.txt" 2>&1 | tail -1
for f in sw-init.sh swmdio.sh sfp-init.sh; do
	$SCP "$SWDIR/$f" "root@[$NPU]:$DST/$f" 2>&1 | tail -1
done
$SSH "chmod +x $DST/dp_fwd $DST/*.sh; echo installed:; ls -la $DST; sha256sum $DST/dp_fwd 2>/dev/null"
echo "### DONEDPLOY"
