#!/bin/sh
# SPDX-License-Identifier: MIT
# Runs ON the host: relay dp_fwd + nmp config to the NPU's writable partition over mvmgmt0.
# Push the two files to the host /tmp first, then run this.
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
echo "=== relay to NPU $DST ==="
$SCP /tmp/dp_fwd            "root@[$NPU]:$DST/dp_fwd"            2>&1 | tail -1
$SCP /tmp/dp-nmp-config.txt "root@[$NPU]:$DST/dp-nmp-config.txt" 2>&1 | tail -1
$SSH "chmod +x $DST/dp_fwd; echo installed:; ls -la $DST/dp_fwd $DST/dp-nmp-config.txt; sha256sum $DST/dp_fwd 2>/dev/null"
echo "### DONEDPLOY"
