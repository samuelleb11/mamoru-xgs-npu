# Provenance & obtaining the inputs

This kit ships **our** code (clean-room drivers, the clean-room switch bring-up, the
GPLv2 forwarder source) and **none** of the proprietary Sophos/Marvell artifacts it
was developed against. This doc records, for reproducibility: what those inputs were,
where to get them yourself, and how to extract the ones that live on your own box.

> Everything here assumes **your own appliance**. You own the XGS; the vendor firmware
> shipped on it is licensed to you as its owner. Extracting a component from your own
> device to keep your own device running is repurposing, not redistribution — and
> nothing proprietary is included in or redistributed by this kit.

## Inputs at a glance

| Input | Source | Role | Ship it? |
|---|---|---|---|
| **MUSDK** (Marvell User-Space Dev Kit, SDK-10.3.5.0) | **Public** — `MarvellEmbeddedProcessors/musdk-marvell` on GitHub | `dp_fwd` links its giu/pp2/nmp libs | No — fetched at build (GPL-2.0/BSD) |
| **Bootlin aarch64 toolchain** (glibc stable 2018.02) | **Public** — toolchains.bootlin.com | cross-compiles the NPU binaries | No — fetched |
| **88E6193X register knowledge** | **Public** — the Marvell 88E6193X datasheet + observed traffic | the clean-room `switch-init/` | N/A — knowledge, not a file |
| **Marvell UIO modules** (`musdk_cma.ko`, `mv_dmax2_uio.ko`, `uio_pdrv_genirq.ko`) | **Your box** — the NPU's own rootfs | loaded before `dp_fwd` | No — you extract yours |
| **`mv_armada_ep`** (PCIe-EP driver; publishes the barmap) + the **NPU base OS** (Linux 4.14 rootfs) | **Your box** — resident on the NPU eMMC | brings the NPU up | No — stays on your box |
| **`libsbsp.so` / `xgs-mvl6193-init`** (Sophos switch init) | **Your box** (SFOS) — reference only | reverse-engineered, then **replaced** by `switch-init/` | No — and **not needed** |

## Fetching the public inputs

- **MUSDK:** `git clone` the Marvell repo above at the SDK-10.3.5.0 tag; `npu-firmware/build/env.sh`
  pins it. It builds to a static `libmusdk.a` (giu+pp2+nmp) — see `docs/BUILD.md` Part B.
- **Toolchain:** download the Bootlin `aarch64--glibc--stable-2018.02` tarball (URL in `env.sh`).
- **Datasheet:** the Marvell 88E6193X ("Link Street") datasheet documents the switch registers the
  clean-room `switch-init/` programs.

## Extracting the your-own-box inputs

The pieces above marked "your box" are already on **your** appliance from Sophos. You copy your own
copies; we ship nothing.

**1. Reach the NPU over the serial console.** The x86 host's **`ttyS2` (COM3 @ 0x3e8) is wired to the
NPU's UART** and lands on a busybox root shell (no login):

```sh
stty -F /dev/ttyS2 115200 raw -echo
# then read/write /dev/ttyS2; the helper host-driver-linux/tools/npc.sh wraps this:
echo 'uname -sr' | sh host-driver-linux/tools/npc.sh
```

**2. Copy the UIO modules off the NPU** (into wherever you deploy the kit, e.g. `/opt/dp/`):

```sh
# on the NPU shell (via ttyS2):
find / -name 'musdk_cma.ko' -o -name 'mv_dmax2_uio.ko' 2>/dev/null   # typically /lib/modules/4.14.*/extra/
```

**3. Bulk transfer** (the `dp_fwd` binary, module blobs) over `ttyS2` with base64, or over a plain
data pipe. `npu-firmware/deploy/relay-deploy.sh` is a reference for the network path once it's safe.

The NPU base OS and `mv_armada_ep` you do **not** extract — they stay resident and boot the NPU; the
host driver just reads the barmap they publish.

## Safety warnings (learned the hard way)

- **Do NOT `ifconfig mvmgmt0 up` on the NPU with stock firmware.** The stock p3 boot keeps it down on
  purpose (a TX-open crash); bringing it up wedges the NPU console in uninterruptible D-state, and the
  only recovery is a full appliance **power-cycle** (~6-min NPU cold boot). Use `ttyS2` for control and
  bulk transfer instead.
- **Never trigger a PF FLR** on the host side — it nukes the NPU firmware state.
- The GIU host↔NPU handshake latches host ring addresses once per NPU boot; sequence a host reboot and
  an NPU reboot with care, or you'll desync the trunk and need to restart both ends.

## The reverse-engineering, briefly

The one proprietary runtime dependency we removed was the Sophos switch init (`libsbsp.so` +
`xgs-mvl6193-init`). It's userspace CSR/MDIO banging, not a hidden kernel driver, so the register
sequence was recovered by disassembly and reimplemented natively in `switch-init/sw-init.sh` +
`swmdio.sh` (raw multi-chip SMI via `devmem`). That is why this kit needs **no** `libsbsp` and is
redistributable. The detailed RE notes are kept in a private archive; this doc is the reproducible
summary.
