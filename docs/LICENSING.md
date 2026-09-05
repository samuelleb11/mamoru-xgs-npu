# Licensing map

mamoru-xgs-npu is a mixed-licence package: the answer to "can I redistribute this?" differs per
component. If you redistribute the kit onward, this file travels with it.

| Tier | Contents | Redistribution |
|---|---|---|
| 1 | First-party code | Freely, under the permissive terms named below |
| 2 | Marvell code under the GPL | Only under the GPL, with corresponding source |
| 3 | Vendor components | Not ours, not shipped here, not needed from us |

Full licence texts are in [`LICENSES/`](../LICENSES/). The root [`LICENSE`](../LICENSE) carries the
same component table in short form.

## Tier 1 — first-party code

| Component | SPDX |
|---|---|
| `host-driver-linux/` (all `agnic_*.c/.h`, Makefile) | **GPL-2.0 OR MIT** (dual) |
| `host-driver-freebsd/` (all `if_agnic*`, `agnic_*`) | **BSD-2-Clause** |
| `npu-firmware/src/` (wire-contract headers, `dp_app` skeleton, switch-op handler) | **MIT** |
| `npu-firmware/switch-init/` shell bring-up (`sw-init.sh`, `swmdio.sh`, `sfp-init.sh`) | **MIT** |
| `npu-firmware/switch-init/dp_swcfg.c` | **MIT** |
| `npu-firmware/build/`, `npu-firmware/deploy/` (less `keys/mvmgt.x86`), `tools/` | **MIT** |

Each of these files carries its own `SPDX-License-Identifier`, except
`npu-firmware/deploy/dp-nmp-config.txt`; it and the docs are MIT under the root `LICENSE`.
`npu-firmware/deploy/keys/mvmgt.x86` is Sophos's, and is covered in Tier 3 below.

Tier 1 is clean-room: written against documented hardware behaviour and observed traffic. The host
drivers transcribe only *interface facts*, meaning register offsets and the AGNIC ABI, and contain no
third-party `.c` logic. The shell bring-up in `npu-firmware/switch-init/` reverse-engineers the
register sequence the proprietary Sophos init performed and drives the switch over raw SMI, so the
kit needs no `libsbsp` and no `xgs-mvl6193-init`. That bring-up is the shipping path:
`npu-firmware/deploy/dp-autostart.sh` runs `sw-init.sh`, then execs `dp_fwd`. `dp_swcfg` is on no
runtime path.

`dp_swcfg.c` is the one Tier 1 file that does not stand alone. It is ours and it is MIT, but it
`#include`s `msdApi.h` and links `libMsdDrv.a` and `libMRegAccess_mvmdio_uio.a`, which
`npu-firmware/build/build_swcfg.sh` builds from a Marvell UMSD (Unified Switch Driver) tree it
expects at `$W/umsd`. No UMSD tree is in this repository and no script here fetches one, so
`build_swcfg.sh` run as documented stops at `cd "$UMSD"`: `dp_swcfg` is not buildable from this kit
as it stands. At runtime it opens `/dev/mvmdio-uio`, which the shell bring-up deliberately does not
use.

## Tier 2 — Marvell code under the GPL

| Component | Origin and terms |
|---|---|
| `npu-firmware/forwarder/forwarder.c`, compiled into `dp_fwd` | Marvell, multi-option licence; we elect **GPLv2** |
| Marvell MUSDK, linked into `dp_fwd` | **GPL-2.0**; not shipped here, fetched at build time |

Rules for `forwarder.c`:

- Do not relicense it and do not strip its header. Marvell's terms require the copyright notice be
  preserved on whichever licence option is elected. The file carries no `SPDX-License-Identifier`
  line; that header is its licence statement.
- It is quarantined in its own directory precisely so its licence cannot be lost by proximity to our
  code. The directory's rules are in
  [`npu-firmware/forwarder/README.md`](../npu-firmware/forwarder/README.md).
- It is not clean-room input. Do not read it and then write host-driver code.
- Because it is compiled into a binary you may ship (`dp_fwd`), it is committed here as source.
  Distributing `dp_fwd` obliges you to make the corresponding source available (GPL-2.0 §3).

`npu-firmware/src/dp_swop.c` is MIT and is `#include`d at `forwarder.c:116`: the MUSDK app makefile
builds exactly one `.c` for the example `dp_fwd` is forked from, so the two files compile as a
single translation unit. That unit is not the whole binary. `dp_fwd` is built inside the MUSDK tree
and links MUSDK's app-common objects (`mvapp`, `cli`, `pp2_utils`, `giu_utils`, `nmp_guest_utils`)
and the MUSDK library with it.

MUSDK is not in this repository. `npu-firmware/build/env.sh` names Marvell's public
`MarvellEmbeddedProcessors/musdk-marvell` at tag `SDK-10.3.5.0-PR2`, in a comment on the `MUSDK`
path variable. The build compiles whatever tree is mounted at that path, so the tag records the
intended checkout rather than enforcing one. MUSDK is GPL-2.0 and public; this project neither ships
it nor needs to. The fetch and build steps are in [`BUILD.md`](BUILD.md).

## Tier 3 — vendor components, not distributed

None of the following is in this kit, and none of it needs to be: your XGS already has it.
[`VENDOR-BITS.md`](VENDOR-BITS.md) covers using your own box's copies.

- The Sophos BSP: `libsbsp.so`, `xgs-mvl6193-init`, `xgs-platform*`, and the `opt/sophos/plt/*`
  platform DB. Sophos/Marvell proprietary.
- Sophos NetAgent and the stock data-plane binary.
- The NPU's base OS and bootloader, on the NPU's own eMMC.
- `mv_armada_ep`, the PCIe-endpoint driver that publishes the barmap. Blob-locked.
- The Marvell UIO modules `musdk_cma.ko` and `mv_dmax2_uio.ko`.

Do not redistribute any Tier-3 item. They are licensed to you as the owner of your specific
appliance, and passing them on is not yours to do.

### Bundled exception: `npu-firmware/deploy/keys/mvmgt.x86`

One vendor artefact is in this repository, knowingly: the RSA key the x86 host uses to SSH into the
NPU. It is Sophos's, not ours. It is bundled because it is already public — it ships unencrypted in
Sophos's downloadable firmware ISO, its public half is already in `/root/.ssh/authorized_keys` on
every XGS's NPU, publishing it therefore hands nobody anything they did not already have, and
omitting it would turn a repeatable install into an archaeology exercise. It authenticates to a
coprocessor inside a chassis you must already own root on.

[`npu-firmware/deploy/keys/README.md`](../npu-firmware/deploy/keys/README.md) carries the full
justification and two independent checks of this copy against your own box, one of them recorded
there as measured. This file defers to that record rather than making the claim independently.

No other vendor artefact ships in this repository, and the rest of Tier 3 stands as listed above.

## Redistribution

Tiers 1 and 2 may be redistributed together, accompanied by this file and by the corresponding
source for `dp_fwd`. The recipient supplies the Tier-3 components from their own appliance.
