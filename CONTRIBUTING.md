# Contributing to mamoru-xgs-npu

This repository is a component of the Mamoru firewall project, released on its own. Patches are
taken against three components: the Linux host driver, the FreeBSD host driver, and the NPU
firmware. Each patch carries a Developer Certificate of Origin sign-off, an `SPDX-License-Identifier`
on any new first-party file, and a statement of what was tested on what hardware.

## Hardware scope

Every proof point in the project is on an XGS 116. Run `sudo ./tools/probe.sh` on the target before
building anything; [README.md](README.md) gives the pass criterion and
[docs/HARDWARE.md](docs/HARDWARE.md) interprets the other outcomes. For an XGS 136 or another model,
the hardware deltas in [docs/HARDWARE.md](docs/HARDWARE.md) are the starting point.

## Build and test

[docs/BUILD.md](docs/BUILD.md) is the build reference for all three components.

- Linux host driver: `make -C $KDIR M=$PWD modules`, with `$KDIR` naming the kernel build directory.
  It targets the 6.12–6.19 kernel era. Compilation against 6.19.14 is Measured; running on that
  version is Unverified.
- FreeBSD host driver: `make` against the `/usr/src` matching your base. The control plane is
  Measured on XGS 116 hardware; the RX/TX datapath is written and Unverified end to end.
  [host-driver-freebsd/README.md](host-driver-freebsd/README.md) names the proven steps and the
  unfinished ones.
- NPU firmware: cross-build per [docs/BUILD.md](docs/BUILD.md) Part B.

Two tests need no hardware. `npu-firmware/src/dp_swop_test.c` covers the request/response envelope
of `dp_swop`, the NPU-side handler for 88E6193X register access
([npu-firmware/README.md](npu-firmware/README.md)). The test leaves the SMI window that reaches the
switch unmapped and issues no SMI transaction, so a green run is evidence about the envelope, not
about switch access. Build it from `npu-firmware/src/`:
`cc -Wall -Wextra -Werror -DDP_SWOP_TEST -o dp_swop_test dp_swop_test.c dp_swop.c`.
`npu-firmware/tests/sw-init-interlock-test.sh` runs under any POSIX shell as an unprivileged user
and asserts that `npu-firmware/switch-init/sw-init.sh` writes and verifies the VLAN maps before it
powers a front PHY. Run the relevant one before sending a patch that touches
`npu-firmware/src/dp_swop.c` or `npu-firmware/switch-init/sw-init.sh`.

Test on real hardware where you can, and state what you tested: board model, kernel, OS.

NPU firmware work requires a shell on the NPU itself.
[docs/NPU-INSTALL.md](docs/NPU-INSTALL.md) is the management-link and SSH route to that shell, and
states which of its steps have been run on hardware and which have not.

## Licensing and sign-off

- Every first-party source file carries an `SPDX-License-Identifier`; keep it. New first-party files
  are `MIT` unless there is a specific reason otherwise. [docs/LICENSING.md](docs/LICENSING.md) is
  the full map.
- `npu-firmware/forwarder/forwarder.c` is the one source file without an SPDX line: it carries
  Marvell's original multi-option header instead. Do not modify that header, and do not add an SPDX
  line to that file.
- `npu-firmware/forwarder/forwarder.c` is not clean-room input. The host drivers were written
  against documented behaviour and observed traffic; driver code written after reading that file is
  contaminated and cannot be taken. The quarantine rules are in
  [npu-firmware/forwarder/README.md](npu-firmware/forwarder/README.md).
- Do not add vendor or proprietary material, and do not commit PII: IPs, keys, device link-locals,
  hostnames, home paths. [docs/VENDOR-BITS.md](docs/VENDOR-BITS.md) and
  [docs/PROVENANCE.md](docs/PROVENANCE.md) cover what stays on your own box and how to obtain it
  there. `npu-firmware/deploy/keys/mvmgt.x86` is the single settled exception
  ([npu-firmware/deploy/keys/README.md](npu-firmware/deploy/keys/README.md),
  [docs/LICENSING.md](docs/LICENSING.md)); no other vendor key or artefact ships in this repository.
- Sign off every commit under the Developer Certificate of Origin: `git commit -s`.

## Open tasks

- Finish and verify the FreeBSD RX/TX datapath
  ([host-driver-freebsd/README.md](host-driver-freebsd/README.md)).
- Port maps and `switch-init` adaptations for the XGS 136 and other models
  ([docs/HARDWARE.md](docs/HARDWARE.md)).
- Kernel-version compatibility for newer and older Linux.
- An NPU rootfs replacement carried out end to end over the management link. That path is Not
  attempted: no rootfs has been replaced through it, and
  [docs/NPU-INSTALL.md](docs/NPU-INSTALL.md) marks its write steps accordingly. Report the board,
  the slot you ran from, and what the NPU's busybox carried.

## Material kept outside this repository

The reverse-engineering artifacts and vendor references are kept in a private archive, not in this
repository. [docs/PROVENANCE.md](docs/PROVENANCE.md) is the reproducible summary of what was used
and how to obtain it yourself.
