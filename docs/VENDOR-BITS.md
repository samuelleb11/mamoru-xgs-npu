# Vendor bits — what you supply from your own appliance

This kit deliberately ships **none** of the proprietary Marvell/Sophos material
(see `LICENSING.md`, Tier 3). A few of those pieces are still needed for the NPU
data plane to run — but they are **already on your XGS**, put there by Sophos, and
licensed to you as the owner of that box. You use your own copies; nobody ships
them to you.

The principle: repurposing hardware you own is your right. Extracting a vendor
file from **your own device** to keep **your own device** working is not
redistribution.

## What you need from your box, and where it lives

All of these live on the **NPU** (the CN9130), not the x86 host, on the NPU's own
root filesystem (a stock Marvell/Sophos Linux, kernel 4.14.x on eMMC):

| Piece | Typical location on the NPU | Used by |
|---|---|---|
| `musdk_cma.ko` | `/lib/modules/4.14.*/extra/` (or `/opt/dp/`) | `dp-autostart.sh` insmods it before `dp_fwd` |
| `mv_dmax2_uio.ko` | same | same |
| `uio_pdrv_genirq.ko` | in-tree (`/lib/modules/.../kernel/drivers/uio/`) | same |
| The base OS + bootloader + `mv_armada_ep` | already resident; you don't touch them | brings the NPU up + publishes the PCIe barmap the host driver reads |

You do **not** need `libsbsp`, `xgs-mvl6193-init`, or the Sophos platform DB — the
clean-room `switch-init/` replaces them.

## How to get them

1. Get a shell on the NPU. Two ways:
   - **Network (easier):** the NPU is reachable over the internal `mvmgmt0` PCIe
     link from the host, and its factory firmware runs `sshd`. Full procedure in
     `NPU-INSTALL.md`; `npu-firmware/deploy/relay-deploy.sh` wraps it.
   - **Serial:** the NPU has a console on the host's `ttyS2`. Slower, but it needs
     nothing to be working first — keep it as the fallback.
2. Find the UIO modules: `find / -name 'musdk_cma.ko' -o -name 'mv_dmax2_uio.ko'`.
3. Copy them alongside the kit's deploy payload (into `/opt/dp/` on the NPU), so
   `dp-autostart.sh` can `insmod` them.

## What you must not do

Do not pass any of these Tier-3 files to anyone else. They came with *your*
appliance and stay with it. If a second person wants to run this kit, they pull
the equivalent files from *their* own box.

**One vendor file is an exception and it is already in this repo:** Sophos's NPU
SSH key, `npu-firmware/deploy/keys/mvmgt.x86`. It is public in Sophos's own
downloadable ISO and already trusted by every XGS's NPU, so bundling it gives
nobody anything — see `LICENSING.md` and the README beside the key. It is the only
one; everything in the table above still comes off your own box.

---

For **where each input comes from and how to obtain/extract it yourself** (public
Marvell/Bootlin sources + your-own-box extraction over `ttyS2`), see `PROVENANCE.md`.
