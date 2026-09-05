# Vendor-derived NPU source

Files in this directory are not ours. They carry a third-party copyright and third-party licence
terms, and they live in their own directory so that neither fact can be lost by proximity to code
that is ours.

| File | Copyright | Terms |
|---|---|---|
| `forwarder.c` | Marvell International Ltd. and its affiliates | Marvell multi-option header, four alternatives: Marvell Commercial, **GPLv2**, LGPL-2.1 with a FreeRTOS exception, or Marvell BSD |

We elect GPLv2, and the rules below are what that election requires of this file. Distributing a
binary built from it obliges you to make its corresponding source available. The election does not
settle all of `dp_fwd`: `../build/build_fwd.sh` builds that binary inside Marvell's MUSDK tree, and
MUSDK carries GPL-2.0 terms of its own.

The file is committed here as source, rather than recorded as a reference, because it is compiled
into `dp_fwd`, a binary built from this kit and shipped by whoever builds it. A build input fetched
at build time is not. A checksum in a manifest is the right record for something merely referenced;
it is the wrong record for something that ends up in the binary. So the file is committed,
unmodified in its licence header, and quarantined.

## Rules

- Do not relicense it, strip its header, or add an `SPDX-License-Identifier` line to it. Marvell's
  terms require the copyright notice be preserved on whichever alternative is elected, and that
  header, which carries no SPDX line, is the file's licence statement.
- Do not let it spread. Nothing outside this directory should be a derived work of it. The rest of
  `../src/` is ours and stays that way. The one crossing runs the other way: `forwarder.c:116`
  includes `dp_swop.c`, this kit's MIT-licensed `../src/dp_swop.c`, which `../build/build_fwd.sh:17`
  copies onto the build's include path so the two compile as a single translation unit.
- This file is not clean-room input. Do not read it and then write host-driver code. The host
  drivers in this kit are clean-room, written against documented behaviour and observed traffic;
  contamination from this file is exactly what that claim exists to exclude.

## Position in the licence map

The rest of the kit is MIT; the host drivers are GPL-2.0 OR MIT on Linux and BSD-2-Clause on
FreeBSD. This directory is the stated GPL-2.0 exception. The texts of the licences this kit elects
are in [`../../LICENSES/`](../../LICENSES/), the short component table is in
[`../../LICENSE`](../../LICENSE), and the per-component map, including everything `dp_fwd` takes in
beyond this file, is [`../../docs/LICENSING.md`](../../docs/LICENSING.md).
