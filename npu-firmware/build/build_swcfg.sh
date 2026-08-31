#!/bin/sh
# SPDX-License-Identifier: MIT
# Cross-build dp_swcfg (Marvell 88E6193X / Amethyst switch config tool) for the NPU.
# Runs in Docker: docker run --rm -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_swcfg.sh
#
# Reuses the umsd (Marvell Unified Switch Driver) source:
#   - libMsdDrv.a               : the switch driver (AMETHYST build)
#   - libMRegAccess_mvmdio_uio.a: MDIO register access over /dev/mvmdio-uio
#   - host/src/init.c           : qdInit()/qddev + SMIRead/SMIWrite BSP callbacks
# Links those with tools/dp_swcfg.c into a single static-ish binary for the NPU.
set -e
# ubuntu:20.04 base image ships without make; install the minimal host tools.
if ! command -v make >/dev/null 2>&1; then
  apt-get update -qq && apt-get install -y -qq --no-install-recommends make file >/dev/null 2>&1 || true
fi
W=/w
UMSD=$W/umsd
TC=$W/aarch64--glibc--stable-2018.11-1                 # GCC7.3/glibc2.27, target-exact
CROSS=$TC/bin/aarch64-buildroot-linux-gnu-
export PATH=$TC/bin:$PATH
export CROSS_COMPILE=$CROSS

# --- umsd env (mirror ./setenv: AMETHYST only, static lib) ---
export MSD_ROOT=$UMSD MSD_USER_BASE=$UMSD MSD_PROJ_NAME=msdDrv MSD_TOOL_DIR=$UMSD/tools
export RELEASE=YES OS_RUN=LINUX
export TOPAZ_BUILD=NO PERIDOT_BUILD=NO AGATE_BUILD=NO PEARL_BUILD=NO AMETHYST_BUILD=YES
export STATIC_LIB=1

cd "$UMSD"
echo "### clean"
make -C host/linux clean >/dev/null 2>&1 || true
make clean            >/dev/null 2>&1 || true
rm -f host/linux/libMsdDrv.a host/linux/libMRegAccess_mvmdio_uio.a

echo "### build msd driver library (top make -> src -> library/msdDrv.o) + archive"
make -C host/linux libMsdDrv.a 2>&1 | tail -8

echo "### build MDIO access lib (mvmdio_uio)"
make -C host/linux libMRegAccess_mvmdio_uio.a 2>&1 | tail -6

ls -la host/linux/libMsdDrv.a host/linux/libMRegAccess_mvmdio_uio.a || { echo "LIB_MISSING"; exit 1; }

echo "### compile init.o + dp_swcfg.o + link"
INC="-I$UMSD/include -I$UMSD/include/driver -I$UMSD/include/api -I$UMSD/include/platform \
     -I$UMSD/include/utils -I$UMSD/include/dev -I$UMSD/host/include \
     -I$UMSD/host/linux/libMRegAccess_mvmdio_uio"
DEF="-DLINUX -DUSE_SEMAPHORE -DAMETHYST_BUILD_IN"

${CROSS}gcc $DEF $INC -O2 -c "$UMSD/host/src/init.c" -o "$W/init.o"
${CROSS}gcc $DEF $INC -O2 -c "$W/dp_swcfg.c"          -o "$W/dp_swcfg.o"
${CROSS}gcc -o "$W/dp_swcfg" "$W/dp_swcfg.o" "$W/init.o" \
    host/linux/libMsdDrv.a host/linux/libMRegAccess_mvmdio_uio.a -lpthread

echo "### result"
file "$W/dp_swcfg" || true
ls -la "$W/dp_swcfg"
echo "### undefined symbols (should be only libc/pthread):"
${CROSS}nm -u "$W/dp_swcfg" | head -40
echo "### BUILD_SWCFG_DONE"
