#!/bin/sh
# SPDX-License-Identifier: MIT
# Cross-build libmusdk.a inside a container (autotools present); run via:
#   docker run --rm --network host -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_musdk.sh
# Toolchain + musdk source are mounted at /w. Produces /w/musdk/**/libmusdk.a (static).
set -e
export DEBIAN_FRONTEND=noninteractive
if ! command -v autoconf >/dev/null 2>&1; then
  apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    autoconf automake libtool m4 make pkg-config >/dev/null
fi
TC=/w/aarch64--glibc--stable-2018.11-1                 # GCC 7.3.0 / glibc 2.27 (target-exact)
export CROSS_COMPILE=$TC/bin/aarch64-buildroot-linux-gnu-
cd /w/musdk
[ -f configure ] || ./bootstrap
# giu + pp2 + nmp static lib, 64-bit DMA addrs (32 GiB host window, >32-bit phys), CMA (not hugepage)
# giu/nmp default OFF -> must enable explicitly (else only pp2 lands in libmusdk.a)
rm -f config.status
./configure --host=aarch64-linux-gnu \
  CC=${CROSS_COMPILE}gcc AR=${CROSS_COMPILE}ar RANLIB=${CROSS_COMPILE}ranlib LD=${CROSS_COMPILE}ld \
  --enable-static --disable-shared --enable-dma-addr=64 \
  --enable-pp2 --enable-giu --enable-nmp --enable-sam=no --enable-neta=no
make -j4
echo "=== libmusdk artifacts ==="
find /w/musdk -name "libmusdk*.a" -exec ls -la {} \;
echo "=== symbols sanity (giu/pp2/nmp present?) ==="
A=$(find /w/musdk -name "libmusdk.a" | head -1)
${CROSS_COMPILE}nm "$A" 2>/dev/null | grep -cE " T (giu_gpio_recv|pp2_ppio_recv|nmp_init|nmp_schedule)" | xargs echo "core-symbol hits:"
echo "MUSDK_BUILD_DONE"
