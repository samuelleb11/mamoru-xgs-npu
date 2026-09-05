# Licensing map

This kit is a **mixed-license** package. That is normal and fine, but it means
"can I give this to someone / publish it?" has three different answers depending
on the piece. This file is the map. If you redistribute the kit onward, keep this
file with it.

## Tier 1 — ours, permissive (redistribute freely)

| Piece | License |
|---|---|
| `host-driver-linux/` (all `agnic_*.c/.h`, Makefile) | **GPL-2.0 OR MIT** (dual) |
| `host-driver-freebsd/` (all `if_agnic*`, `agnic_*`) | **BSD-2-Clause** |
| `npu-firmware/src/` (wire-contract headers + `dp_app` skeleton) | MIT |
| `npu-firmware/switch-init/` (`sw-init.sh`, `swmdio.sh`, `sfp-init.sh`, `dp_swcfg.c`) | ours |
| `npu-firmware/build/`, `npu-firmware/deploy/`, `tools/` | ours |

These are **clean-room**: written against documented hardware behaviour and
observed traffic. The host drivers transcribe only *interface facts* (register
offsets, the AGNIC ABI) — no third-party `.c` logic. The native switch-init
reverse-engineers the register sequence the proprietary Sophos init performed, so
it needs **no** `libsbsp` and **no** `xgs-mvl6193-init`.

## Tier 2 — GPLv2 (redistributable, but only under the GPL, with source)

| Piece | Origin / terms |
|---|---|
| `npu-firmware/forwarder/forwarder.c` → compiled into `dp_fwd` | Marvell, multi-option; we elect **GPLv2** |
| Marvell **MUSDK** (linked into `dp_fwd`) | GPL-2.0 — **not shipped here**, fetched at build time (see below) |

Rules for `forwarder.c`:
- **Do not relicense it and do not strip its header.** Marvell's terms require the
  copyright notice be preserved on whichever license option is elected.
- It is **quarantined** in its own directory precisely so its license can't be
  lost by proximity to our code. It is **not** clean-room input — do not read it
  and then write host-driver code.
- Because it is compiled into a binary you may ship (`dp_fwd`), it is committed
  here as source. If you distribute `dp_fwd`, you must also make this
  corresponding source available (that's just GPLv2).

**MUSDK is not in this repo.** `npu-firmware/build/env.sh` pins it to Marvell's
public `MarvellEmbeddedProcessors/musdk-marvell` (SDK-10.3.5.0 tag). You fetch it
at build time like any dependency. It is GPL-2.0 and public; we neither ship nor
need to ship it.

## Tier 3 — vendor, NOT ours, NOT shipped, NOT needed from us

None of the following is in this kit, and none of it needs to be — your XGS
already has it. See `VENDOR-BITS.md` for how you use your own box's copies.

- The Sophos BSP (`libsbsp.so`, `xgs-mvl6193-init`, `xgs-platform*`, the
  `opt/sophos/plt/*` platform DB) — Sophos/Marvell proprietary.
- Sophos NetAgent / the stock data-plane binary.
- The NPU's base OS + bootloader (on the NPU's own eMMC).
- `mv_armada_ep` (the PCIe-endpoint driver that publishes the barmap) — blob-locked.
- The Marvell UIO modules `musdk_cma.ko`, `mv_dmax2_uio.ko`.

**Do not redistribute any Tier-3 item.** They are licensed to you as the owner of
your specific appliance; passing them to someone else is not yours to do. The
whole design of this kit is to avoid ever needing to.

### The one deliberate exception: `npu-firmware/deploy/keys/mvmgt.x86`

One vendor artifact *is* in this repository, knowingly: the RSA key the x86 host
uses to SSH into the NPU. It is Sophos's, not ours. It is bundled because it is
**already public** — it ships unencrypted in Sophos's downloadable firmware ISO,
and its public half is already in `/root/.ssh/authorized_keys` on every XGS's NPU
— so publishing it hands nobody anything they did not already have, while leaving
it out would turn a repeatable install into an archaeology exercise. It
authenticates to a coprocessor inside a chassis you must already own root on.

`npu-firmware/deploy/keys/README.md` carries the full justification and two ways
to verify our copy against your own box. **Read it as a single settled exception,
not as a precedent** — nothing else vendor-side ships here, and the rest of Tier 3
stays exactly as above.

## So, can I pass this kit to a friend?

Yes — Tiers 1 and 2 together, with this file and the corresponding source for
`dp_fwd`. Your friend supplies the Tier-3 pieces from **his own** appliance. The
only vendor artifact travelling with the kit is the already-public NPU SSH key
noted above, and it confers nothing his own appliance did not already trust.
