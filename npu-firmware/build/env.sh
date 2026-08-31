#!/bin/sh
# SPDX-License-Identifier: MIT
# Cross-build environment for the NPU data-plane (config as code; repeatable).
# The NPU (CN9130, SDK 10.22.03, glibc 2.27) has NO on-box compiler (cc1/as/ld absent),
# so cross-build on a Linux build host and relay the binaries to the NPU (serial -> host -> mvmgmt0).
#
# Toolchain: Bootlin aarch64--glibc--stable-2018.02-2 (GCC 6.4.0, glibc 2.26 -> runs on NPU 2.27).
#
# >>> EDIT THESE for your setup before sourcing. The NPU link-local is derived from YOUR NPU's mgmt
#     MAC and differs per box: discover it with `ping6 -c2 ff02::1%mvmgmt0` on the host and note the
#     responder, or read it off the NPU console. The ssh key is whatever key the NPU trusts for root.
BUILD_HOST=${BUILD_HOST:-youruser@your-build-host}
BUILD_ROOT=${BUILD_ROOT:-$HOME/npu-build}
TC=aarch64--glibc--stable-2018.02-2
TC_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/${TC}.tar.bz2
XCROSS=${BUILD_ROOT}/${TC}/bin/aarch64-buildroot-linux-gnu-
XGCC=${XCROSS}gcc
SYSROOT=${BUILD_ROOT}/${TC}/aarch64-buildroot-linux-gnu/sysroot
MUSDK=${BUILD_ROOT}/musdk                     # MarvellEmbeddedProcessors/musdk-marvell @ SDK-10.3.5.0-PR2

# NPU relay target (from the host) — set to YOUR NPU's link-local on mvmgmt0 and your ssh key path.
NPU_LL=${NPU_LL:-'fe80::YOUR-NPU-EUI64%mvmgmt0'}
NPU_KEY=${NPU_KEY:-$HOME/.ssh/npu_mgmt_key}

export BUILD_HOST BUILD_ROOT TC TC_URL XCROSS XGCC SYSROOT MUSDK NPU_LL NPU_KEY
