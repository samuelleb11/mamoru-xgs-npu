# Vendor bits — what the appliance supplies

This kit deliberately ships none of the proprietary Marvell/Sophos material
([LICENSING.md](LICENSING.md), Tier 3). A few of those pieces are still required for the NPU
data plane to run. They are already on your XGS, put there by Sophos and licensed to you as the
owner of that box, so you use your own copies. Extracting a vendor file from your own device to
keep that device working is not redistribution.

Every measurement in this document is on an XGS 116. [HARDWARE.md](HARDWARE.md) covers what has to
be re-derived on another model.

## What the NPU needs, and where it lives

All of it lives on the NPU (the CN9130), not on the x86 host, and on the NPU's own root
filesystem: a stock Marvell/Sophos Linux, kernel 4.14.x, on eMMC.

| Piece | Typical location on the NPU | Used by |
|---|---|---|
| `musdk_cma.ko` | `/lib/modules/4.14.*/extra/` (or `/opt/dp/`) | `dp-autostart.sh` insmods it before `dp_fwd` |
| `mv_dmax2_uio.ko` | same | same |
| `uio_pdrv_genirq.ko` | in-tree, `/lib/modules/.../kernel/drivers/uio/` | same, with `of_id=generic-uio` |
| The base OS + bootloader + `mv_armada_ep` | already resident; you do not touch them | brings the NPU up and publishes the PCIe barmap the host driver reads |

`dp-autostart.sh` fixes the order: it `cd`s to `$DP` (default `/opt/dp`), insmods the three
modules by bare filename, runs `sw-init.sh`, then execs `dp_fwd`. All three are loaded before
`dp_fwd` starts.

You do not need `libsbsp`, `xgs-mvl6193-init`, or the Sophos platform DB. The clean-room
`switch-init/` replaces them. The platform DB is a reference input rather than a runtime one: the
board's platform descriptor on the appliance is one of the two methods that fix the 116's
switch-port map in `npu-firmware/src/portmap.h`, and re-deriving that map for another model is
covered in [HARDWARE.md](HARDWARE.md).

## Extraction

1. Get a shell on the NPU.
   - Network. The NPU is reachable over the internal `mvmgmt0` PCIe link from the host, and its
     factory firmware runs `sshd`. It is the faster route, and it needs the host driver loaded and
     that link up first. Full procedure in [NPU-INSTALL.md](NPU-INSTALL.md);
     `npu-firmware/deploy/relay-deploy.sh` wraps it.
   - Serial. The NPU has a console on the host's `ttyS2`. It is slower, but it needs nothing else
     working first. Keep it as the fallback ([PROVENANCE.md](PROVENANCE.md)).
2. Locate the modules: `find / -name 'musdk_cma.ko' -o -name 'mv_dmax2_uio.ko'`.
3. Copy them into `/opt/dp/` on the NPU, alongside the kit's deploy payload, so
   `dp-autostart.sh` can `insmod` them. That directory must match `dp-autostart.sh`'s `DP` dir.
   `relay-deploy.sh` carries neither the modules nor the `switch-init/` scripts, so those go over
   separately ([BUILD.md](BUILD.md)).

   Measured 2026-09-04 on one factory unit: the NPU's root filesystem mounts read-only, so this
   step presupposes a rootfs of your own ([NPU-INSTALL.md](NPU-INSTALL.md)). What that costs the
   deploy path, and why `/tmp` staging is a test mechanism rather than a deployment, is in
   [BUILD.md](BUILD.md). Not attempted: no rootfs has been installed through that path end to end,
   so the persistent path for these modules is unproven.

## Redistribution limits

Do not pass any of these Tier-3 files to anyone else. They are licensed to the owner of the
appliance they arrived on, and a second person running this kit pulls the equivalent files from
their own box.

One vendor artefact ships in this repository: Sophos's NPU SSH key,
`npu-firmware/deploy/keys/mvmgt.x86`. It is public in Sophos's own downloadable firmware ISO and
its public half is already in `/root/.ssh/authorized_keys` on every XGS's NPU, so shipping it
hands nobody anything ([LICENSING.md](LICENSING.md), and
[the README beside the key](../npu-firmware/deploy/keys/README.md)). It is the only one; every
item in the table above still comes off your own box.

## Provenance of the inputs

For where each input came from and how to obtain or extract it (public Marvell and Bootlin
sources, plus extraction from your own box over `ttyS2`), see [PROVENANCE.md](PROVENANCE.md).
