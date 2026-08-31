# Contributing to mamoru-xgs-npu

Thanks for helping bring Sophos XGS hardware up under open firewalls. This is a
component of the **Mamoru** firewall project, released on its own so you can run
your XGS however you like.

## Before you build
Run `tools/probe.sh` on your target first (see the README and `docs/HARDWARE.md`).
Every proof point here is on an **XGS 116** — if you have a 136 or another model,
the hardware deltas in `docs/HARDWARE.md` are where to start.

## Building & testing
See `docs/BUILD.md`. In short:
- **Linux host driver:** `make -C $KDIR M=$PWD modules` (builds against 6.12–6.19-era
  kernels; verified compiling against 6.19.14).
- **FreeBSD host driver:** `make` against your matching `/usr/src` (control plane is
  hardware-proven; the RX/TX datapath needs finish-and-verify — a great place to help).
- **NPU firmware:** cross-build per `docs/BUILD.md` Part B.

Test on real hardware where you can, and say what you tested (board model, kernel, OS).

## Licensing & sign-off
- Every source file carries an `SPDX-License-Identifier` — keep it. New first-party
  files are `MIT` unless there's a specific reason otherwise. Full map: `docs/LICENSING.md`.
- Do **not** modify the Marvell header on `npu-firmware/forwarder/forwarder.c`, and do
  not use it as clean-room input (see that directory's README).
- Do **not** add vendor/proprietary material or PII — IPs, keys, device link-locals,
  hostnames, home paths. `docs/VENDOR-BITS.md` and `docs/PROVENANCE.md` explain what
  stays on your own box and how to obtain it there.
- Sign off your commits (Developer Certificate of Origin): `git commit -s`.

## Good first contributions
- Finish + verify the FreeBSD RX/TX datapath (`host-driver-freebsd/README.md`).
- Port maps / `switch-init` adaptations for the XGS 136 and other models (`docs/HARDWARE.md`).
- Kernel-version compatibility for newer/older Linux.

## What lives elsewhere
The reverse-engineering artifacts and vendor references are kept in a private archive,
not this repo. `docs/PROVENANCE.md` is the reproducible summary of what was used and how
to obtain it yourself.
