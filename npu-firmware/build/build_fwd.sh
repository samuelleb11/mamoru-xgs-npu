#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the forwarder (forked giu_pkt_echo + our framing) inside the MUSDK tree, so all the
# app-common helpers (mvapp/cli/pp2_utils/giu_utils/nmp_guest_utils) link automatically.
# Output: musdk_giu_pkt_echo == our dp_fwd.
#   docker run --rm -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_fwd.sh
set -e
export DEBIAN_FRONTEND=noninteractive
command -v make >/dev/null 2>&1 || { apt-get update -qq && apt-get install -y -qq make >/dev/null; }
MUSDK=/w/musdk
PE=$MUSDK/apps/examples/giu/pkt_echo
cp /w/dp-src/forwarder.c $PE/pkt_echo.c
cp /w/dp-src/tag_dsa.h /w/dp-src/pport_hdr.h /w/dp-src/portmap.h /w/dp-src/dp_config.h $MUSDK/apps/include/
cd $MUSDK
echo "=== make (incremental: rebuild pkt_echo w/ framing) ==="
make 2>&1 | grep -viE "^make\[|Nothing to be done|Entering|Leaving" | tail -30
echo "=== result ==="
B=$(find $MUSDK -name musdk_giu_pkt_echo -type f 2>/dev/null | head -1)
if [ -n "$B" ]; then cp "$B" /w/dp_fwd; ls -la /w/dp_fwd; echo "FWD_BUILD_OK"; else echo "FWD_BUILD_FAIL (no binary)"; fi
