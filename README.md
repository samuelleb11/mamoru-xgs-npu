# mamoru-xgs-npu

**Mamoru's** open driver + minimal NPU firmware to bring the **front ports of a Sophos XGS
appliance** up under an OS of your choice (IPFire/Linux today, pfSense/OPNsense
FreeBSD with finishing work), instead of Sophos Firewall OS.

The Sophos XGS is an x86 host plus a **Marvell CN9130 "NPU"** (ARM) and an
**88E6193X switch**, connected over PCIe. The front jacks are not normal NICs —
they hang off the switch, are multiplexed onto a *single* PCIe DMA trunk by the
NPU, and demultiplexed back into per-port interfaces by a host driver. This kit
is the two halves that make that work:

1. a **host driver** that speaks the Marvell **AGNIC** trunk protocol and exposes
   `port1..portN` as ordinary network interfaces your firewall OS can assign; and
2. the **minimal NPU firmware** — a forwarder (`dp_fwd`) plus a clean-room switch
   bring-up — that makes the NPU do exactly what the driver needs and nothing more.

> **This is not a full NPU firmware.** Your XGS's NPU keeps its own vendor base OS
> and bootloader (already on the box). We add only the thin data-plane layer that
> makes the NPU talk to our driver.

## Run this first

Every proof point here is on an **XGS 116**. If you have a **136** (or anything
else), confirm the endpoint matches before building:

```sh
sudo ./tools/probe.sh
```

You want `11ab:7080` with BARs `1M / 16M / 16M`. If it matches, the host driver is
a near-drop-in. If it differs, read `docs/HARDWARE.md` first — the 136 may have a
different port count or switch SKU, and mis-driving it is the main risk. **The
switch-init loops in this kit assume the 116's 8 front ports.**

## Layout

| Path | What | License |
|---|---|---|
| `host-driver-linux/` | Linux kernel module (IPFire, or any Linux) | GPL-2.0 OR MIT |
| `host-driver-freebsd/` | FreeBSD `if_agnic` driver (pfSense/OPNsense) — control plane proven, datapath finishing | BSD-2-Clause |
| `npu-firmware/src/` | Clean-room wire-contract headers + `dp_app` skeleton | MIT |
| `npu-firmware/forwarder/` | `dp_fwd` source (`forwarder.c`) | **GPL-2.0** (Marvell — see the dir's README) |
| `npu-firmware/switch-init/` | Native 88E6193X bring-up (replaces the proprietary init) | ours |
| `npu-firmware/build/` | Cross-build pipeline (toolchain + MUSDK + dp_fwd) | ours |
| `npu-firmware/deploy/` | Launcher + relay-to-NPU tooling | ours |
| `npu-firmware/deploy/keys/` | Sophos's already-public NPU SSH key + why it's here | **not ours** |
| `tools/probe.sh` | The hardware check above | ours |
| `docs/` | Build, hardware, NPU install, licensing, provenance, architecture | — |

## Which OS?

- **IPFire (Linux) — fastest path.** The host driver builds as a normal
  out-of-tree module against IPFire's kernel and loads with `insmod`. This is the
  "get it going" route. See `docs/BUILD.md`.
- **pfSense / OPNsense (FreeBSD) — real, but finishing work.** A native FreeBSD
  driver already exists here; its control plane is hardware-proven, and the packet
  datapath is written but needs finish-and-verify against your exact FreeBSD base.
  See `docs/BUILD.md`.

Either way the **NPU firmware is the same** — `dp_fwd` is host-OS-agnostic.

## Getting onto the NPU

Most of this kit assumes you can already reach the CN9130. If you cannot, start with
**`docs/NPU-INSTALL.md`**: there is a PCIe management link inside the chassis, it comes up
against factory firmware, and the factory firmware answers SSH on it. That doc brings the link
up, finds the NPU, gets you a root shell, and — with an honest account of what is measured and
what is not — replaces the NPU's root filesystem without stranding the box. **No TFTP is
involved anywhere**; the NPU's bootloader has no network at all, which that doc also explains
so you do not go looking.

## Licensing in one line

Everything we wrote is permissive (MIT/BSD) or GPL-dual. `dp_fwd` is Marvell
**GPLv2** (shipped with source). The pieces that are *not* ours to give you — the
NPU's base OS, the `mv_armada_ep` endpoint driver, the Marvell UIO modules — you
already own on your XGS and extract from your own box. **One deliberate exception**
travels with the kit: `npu-firmware/deploy/keys/mvmgt.x86`, Sophos's NPU management
SSH key. It is not ours; it is bundled because it is already public in Sophos's own
downloadable firmware ISO and already trusted by every XGS's NPU, so it confers
nothing on anyone — and without it the install path stops being repeatable. The
README beside it says all of that at length. Full detail: `docs/LICENSING.md`,
`docs/VENDOR-BITS.md`, and `docs/PROVENANCE.md`.

## Status (be honest with yourself before you start)

- **Host driver, Linux:** clean-room, self-contained, builds out-of-tree. Proven
  on the 116.
- **Host driver, FreeBSD:** control plane (bind → BARs → MSI-X → mailbox → mgmt
  echo) proven on 116 hardware; **RX/TX datapath written but not yet verified
  end-to-end** — expect finishing work.
- **NPU firmware:** the clean-room switch-init + `dp_fwd` carry real traffic on
  the 116. The scripts are **116-tuned** (8-port loops) and there is known
  repo-vs-appliance drift upstream — treat these as a correct, working *starting
  point* you adapt to your board, not turnkey.


## About Mamoru

This kit is one component of **Mamoru**, an open-source Rust firewall firmware for
commodity and repurposed network appliances. The host driver and NPU firmware here
are Mamoru's own clean-room work to run the Sophos XGS's Marvell CN9130 NPU under
any OS — the same code Mamoru itself runs on this hardware. It's carved out and
shared on its own so you can bring your XGS up today, whatever firewall you put on
it (IPFire, pfSense/OPNsense, plain Linux, or Mamoru itself).

Mamoru is heading open-source; when its repository is public, add the link here so
recipients of this kit can find the rest of the project.
