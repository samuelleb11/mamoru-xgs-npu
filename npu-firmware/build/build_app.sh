#!/bin/sh
# SPDX-License-Identifier: MIT
# Cross-build dp_app against the prebuilt libmusdk.a, inside the build container:
#   scp npu-dataplane/src/* -> build host ~/npu-build/dp-src/
#   docker run --rm -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_app.sh
# Uses the SAME toolchain + MUSDK tree as libmusdk (so mv_autogen_comp_flags.h config matches).
set -e
TC=/w/aarch64--glibc--stable-2018.11-1                 # GCC 7.3.0 / glibc 2.27 (== libmusdk's)
GCC=$TC/bin/aarch64-buildroot-linux-gnu-gcc
OBJDUMP=$TC/bin/aarch64-buildroot-linux-gnu-objdump
MUSDK=/w/musdk
LIB=$MUSDK/src/.libs/libmusdk.a
INC="-I$MUSDK/src/include -I$MUSDK/src/include/drivers -I$MUSDK/apps/include -I/w/dp-src"
# MUST match the exact MVCONF -D flags libmusdk.a was built with (config.status CFLAGS) —
# these change public type/struct layouts (phys_addr_t size, types visibility).
MVCONF="-DMVCONF_DBG_LEVEL=6 -DMVCONF_SYS_DMA_UIO -DMVCONF_TYPES_PUBLIC -DMVCONF_DMA_PHYS_ADDR_T_PUBLIC -DMVCONF_DMA_PHYS_ADDR_T_SIZE=64"

# sanity: confirm libmusdk was built 64-bit-DMA (must match our struct layouts)
echo "=== MVCONF (must show DMA_PHYS_ADDR_T_SIZE 64) ==="
grep -E "DMA_PHYS_ADDR_T_SIZE|MVCONF_ARCH|SYS_DMA_UIO" $MUSDK/src/include/env/mv_autogen_comp_flags.h 2>/dev/null | head

echo "=== compile dp_app ($(ls /w/dp-src/*.c | wc -l) src) ==="
$GCC -O2 -Wall -std=gnu99 $MVCONF $INC /w/dp-src/*.c $LIB -lpthread -lrt -lm -o /w/dp_app
echo "BUILD_OK"
file /w/dp_app
echo -n "GLIBC floor: "; $OBJDUMP -T /w/dp_app 2>/dev/null | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1
echo "DP_APP_BUILD_DONE"
