# Provenance and obtaining the inputs

This kit ships our code — the clean-room drivers, the clean-room switch bring-up, and the GPLv2
forwarder source — and none of the proprietary Sophos/Marvell artifacts it was developed against.
Those inputs came from two places: public sources, named below with where to fetch them, and the
appliance itself, named below with the command that extracts them.

Everything here assumes an appliance you own. Extracting a component from your own device to keep
your own device running is repurposing, not redistribution, and nothing proprietary is included in
or redistributed by this kit. The per-directory licence map is [docs/LICENSING.md](LICENSING.md).

## Inputs

| Input | Source | Role | Shipped here |
|---|---|---|---|
| MUSDK (Marvell User-Space Dev Kit, `SDK-10.3.5.0-PR2`) | Public — `MarvellEmbeddedProcessors/musdk-marvell` on GitHub | `dp_fwd` links its giu/pp2/nmp libs | No; fetched at build (GPL-2.0/BSD) |
| Bootlin aarch64 toolchain (`aarch64--glibc--stable-2018.11-1`) | Public — toolchains.bootlin.com | cross-compiles the NPU binaries | No; fetched |
| 88E6193X register knowledge | Public — the Marvell 88E6193X datasheet plus observed traffic | feeds the clean-room `npu-firmware/switch-init/` | N/A; knowledge, not a file |
| Marvell UIO modules (`musdk_cma.ko`, `mv_dmax2_uio.ko`, `uio_pdrv_genirq.ko`) | Your box — the NPU's own rootfs | loaded before `dp_fwd` | No; you extract yours |
| `mv_armada_ep` (PCIe-EP driver; publishes the barmap) and the NPU base OS (Linux 4.14 rootfs) | Your box — resident on the NPU eMMC | brings the NPU up | No; stays on your box |
| `libsbsp.so` / `xgs-mvl6193-init` (Sophos switch init) | Your box (SFOS) — reference only | reverse-engineered, then replaced by `npu-firmware/switch-init/` | No, and not needed |

Which of these you supply from your own appliance, and where each one sits on it, is
[docs/VENDOR-BITS.md](VENDOR-BITS.md).

## Public inputs

- MUSDK: `git clone` the Marvell repo above at the tag recorded in `npu-firmware/build/env.sh`
  (line 27, `SDK-10.3.5.0-PR2`). `MUSDK` there is a path to your checkout, not a mechanical pin:
  `env.sh` records the tag in a comment and does not check it out. It builds to a static
  `libmusdk.a` (giu+pp2+nmp); see [docs/BUILD.md](BUILD.md) Part B.
- Toolchain: download the Bootlin tarball named by `$TC` in `env.sh`, which also derives the
  download URL from it. `$TC` is `aarch64--glibc--stable-2018.11-1` (GCC 7.3.0, glibc 2.27, the
  NPU's own glibc version, and the compiler `libmusdk.a` is built with). `env.sh` is the single
  source for that version: it previously pinned `2018.02-2` while every build script used
  `2018.11-1`, corrected 2026-09-01.
- Datasheet: the Marvell 88E6193X ("Link Street") datasheet documents the switch registers the
  clean-room `npu-firmware/switch-init/` programs.

## Extraction from the appliance

1. Reach the NPU over the serial console. The x86 host's `ttyS2` (COM3 @ 0x3e8) is wired to the
   NPU's UART and lands on a busybox root shell with no login.

   ```sh
   stty -F /dev/ttyS2 115200 raw -echo
   # then read/write /dev/ttyS2; host-driver-linux/tools/npc.sh wraps that:
   echo 'uname -sr' | sh host-driver-linux/tools/npc.sh
   ```

2. Copy the UIO modules off the NPU, into wherever you deploy the kit, e.g. `/opt/dp/`.

   ```sh
   # on the NPU shell, over ttyS2:
   find / -name 'musdk_cma.ko' -o -name 'mv_dmax2_uio.ko' 2>/dev/null   # typically /lib/modules/4.14.*/extra/
   ```

3. Move bulk payloads — the `dp_fwd` binary and the module blobs — over the `mvmgmt0` link with
   `scp`; base64 over `ttyS2` is the fallback. [docs/NPU-INSTALL.md](NPU-INSTALL.md) is the full
   network path: bring the link up, find the NPU, SSH in. `npu-firmware/deploy/relay-deploy.sh`
   wraps the push of `dp_fwd` and `dp-nmp-config.txt`, staged in the host's `/tmp` first. That
   script assumes a FreeBSD host: it brings the link up with
   `ifconfig mvmgmt0 inet6 -ifdisabled auto_linklocal up` and probes with `ping6`, not the Linux
   `ip` forms used elsewhere in these docs.

The NPU base OS and `mv_armada_ep` are not extracted. They stay resident and boot the NPU; the host
driver only reads the barmap they publish.

## Known hazards

- Bringing **`mvmgmt0` up on the NPU** is routine on vendor firmware and a box-wedger on a minimal
  rootfs of your own. Measured 2026-09-04 on a factory XGS 116: the vendor rootfs brings `mvmgmt0`
  up itself at boot, from a static stanza in its own `/etc/network/interfaces`. It is the transport
  Sophos uses, and SSH into a factory NPU over it is Measured (2026-07-29, a second XGS 116;
  [docs/NPU-INSTALL.md](NPU-INSTALL.md)). The hazard is a rootfs you build. An early minimal NPU
  rootfs of ours carried a pcinet build that crashes on open; `ifconfig mvmgmt0 up` on that rootfs
  wedged the NPU console in uninterruptible D-state, and the only recovery was a full appliance
  power-cycle with a ~6-min NPU cold boot. An earlier revision of this file attributed that wedge to
  stock firmware; that was wrong — it was ours. Keep `ttyS2` available as the fallback either way.
- **Never trigger a PF FLR** on the host side. An FLR nukes the live NPU firmware state and the data
  plane with it, which is why neither host driver carries a reset path
  (`host-driver-linux/agnic_main.c`, `host-driver-freebsd/if_agnic.c`).
- The GIU host↔NPU handshake latches host ring addresses once per NPU boot. Sequence a host reboot
  and an NPU reboot with care, or you desync the trunk and need to restart both ends.

## What was reverse-engineered

The one proprietary runtime dependency removed was the Sophos switch init, `libsbsp.so` plus
`xgs-mvl6193-init`. It is userspace CSR/MDIO banging, not a hidden kernel driver, so the register
sequence was recovered by disassembly and reimplemented natively in
`npu-firmware/switch-init/sw-init.sh` and `npu-firmware/switch-init/swmdio.sh`, using raw multi-chip
SMI via `devmem`. That is why this kit needs no `libsbsp` and is redistributable.

The detailed reverse-engineering notes are kept in a private archive. What is published is the
reproducible summary above.
