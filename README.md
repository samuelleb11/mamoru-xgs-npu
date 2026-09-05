# mamoru-xgs-npu

An open host driver and a minimal NPU firmware that bring the front ports of a Sophos XGS
appliance up under an OS of your choice, instead of Sophos Firewall OS. IPFire/Linux works
today; pfSense/OPNsense (FreeBSD) requires finishing work.

The XGS is an x86 host plus a Marvell CN9130 "NPU" (ARM) plus an 88E6193X switch, connected
over PCIe. The front jacks are not normal NICs: they hang off the switch, the NPU multiplexes
them onto a single PCIe DMA trunk, and a host driver demultiplexes that trunk back into
per-port interfaces. Forwarding is not offloaded. Every front-panel packet crosses PCIe to the
host, the firewall forwards or filters it, and it crosses back.

The host driver speaks the Marvell AGNIC trunk protocol and exposes `port1..portN` as ordinary
network interfaces the firewall OS can assign. The NPU firmware — a forwarder (`dp_fwd`) plus a
clean-room switch bring-up — makes the NPU do exactly what the driver needs and nothing more.

This is not a full NPU firmware. The XGS's NPU keeps its own vendor base OS and bootloader,
which are already on the box; the kit adds only the thin data-plane layer that makes the NPU
talk to the host driver.

## How it works

```
  front jacks (RJ45 / SFP)
        |
   copper PHYs / SerDes           <- brought up by switch-init/sw-init.sh
        |
   Marvell 88E6193X switch        <- DSA-tagged; per-port VLAN maps
        |
   CN9130 "NPU" (ARM)  ── dp_fwd  <- forwarder: switch <-> single GIU trunk
        |                              (built from forwarder.c against MUSDK)
   ── AGNIC GIU trunk over PCIe ──    (one DMA channel for ALL ports)
        |
   host driver  (Linux mamoru-agnic / FreeBSD if_agnic)
        |                              <- demultiplexes the trunk by a 66-byte
   port1 .. portN  (netdev / ifnet)      per-port prefix into N interfaces
        |
   firewall stack (nftables on IPFire / pf on pfSense)
```

The 8 front ports and the SFP cage share one AGNIC GIU trunk; a 66-byte prefix on each frame
carries the port (`byte0 = 0x81 + port_index`), which the host driver adds on TX and strips on
RX. The NPU is a switch + PHY + port-multiplexer conduit, not the policy engine, so the host
driver is the whole datapath rather than a control channel.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) carries the full model.

## Hardware support

Every measurement in this repository is on an XGS 116.

| Model | Front ports | Switch | State |
|---|---|---|---|
| XGS 116 | 8 copper, plus the SFP cage on switch port 9 | 88E6193X | Reference platform |
| XGS 136 | Unverified; may differ in count | Unverified; may be another SKU, such as an 88E6393X | Unverified. The PCIe endpoint is likely identical; the port map and SFP lanes must be re-derived |
| Other XGS models | Unverified | Unverified | Unverified, as for the 136 |

Run the endpoint check on the target before building anything:

```sh
sudo ./tools/probe.sh
```

The pass criterion is PCI id `11ab:7080` with BARs `1M / 16M / 16M`. If it matches, the host
driver is a near-drop-in. Read [docs/HARDWARE.md](docs/HARDWARE.md) first for any other
outcome, and before running the kit on anything that is not a 116.

The `npu-firmware/switch-init/` loops assume the 116's 8 front ports. On a board with a
different port count they leave ports powered down or mis-mapped, and mis-driving a non-116
board is the main risk this kit carries.

## Status

| Component | State | What that rests on |
|---|---|---|
| Linux host driver | **Measured** | Clean-room and self-contained; builds out-of-tree and carries traffic on the XGS 116 |
| FreeBSD host driver — control plane | **Measured** | bind → BARs → MSI-X → mailbox → mgmt echo, on 116 hardware |
| FreeBSD host driver — RX/TX datapath | **Unverified** | Written; not yet verified end to end |
| NPU firmware (`dp_fwd` + switch-init) | **Measured** | Clean-room switch-init and `dp_fwd` carry real traffic on the 116: front jack → VLAN 30 → host `portN` → DHCP lease |
| NPU rootfs replacement | **Not attempted** | Reaching the NPU over the management link is measured on two XGS 116 units (2026-09-04 and 2026-07-29); no rootfs has been installed through that path end to end |

The NPU firmware is not turnkey for another board. The `npu-firmware/switch-init/` scripts are
116-tuned: 8-port loops, SFP on lane 9. There is known repo-vs-appliance drift upstream.
Adapting the scripts is the porting work [docs/HARDWARE.md](docs/HARDWARE.md) sets out.

## Requirements

### On the appliance

Root on the x86 host, and a shell on the NPU: most of the kit assumes you can already reach the
CN9130. There is a PCIe management link inside the chassis, it comes up against factory
firmware, and the factory firmware answers SSH on it.
[docs/NPU-INSTALL.md](docs/NPU-INSTALL.md) brings that link up, finds the NPU, gets a root
shell, and replaces the NPU's root filesystem without stranding the box, recording for each step
whether it is Measured or Unverified. No TFTP is involved anywhere in this kit; the NPU's
bootloader has no network at all.

### Linux host driver

Kernel headers or source matching the running kernel. The driver targets the 6.12–6.19 KPI era,
which a current IPFire matches closely. IPFire is the fastest path: no cross-toolchain, an
ordinary out-of-tree build against the running kernel, and `insmod` to load it. See
[docs/BUILD.md](docs/BUILD.md).

### FreeBSD host driver

`/usr/src` matching your exact base. pfSense CE 2.7.x is FreeBSD 14.0; pfSense Plus 24/25 and
OPNsense 26 are FreeBSD 15. `if_agnic` targets 15.1, and the `if_t` and `bus_dma` APIs shifted
14→15, so build against your own source. See [docs/BUILD.md](docs/BUILD.md).

### NPU firmware

A separate Linux cross-build host with Docker. The CN9130 has no on-box compiler, so the builds
run in an `ubuntu:20.04` container. It needs the Bootlin aarch64 toolchain, pinned as `$TC` in
`npu-firmware/build/env.sh`; a MUSDK checkout, which you fetch into `$MUSDK` at the tag that file
names; and the Marvell UIO modules `musdk_cma.ko`, `mv_dmax2_uio.ko` and `uio_pdrv_genirq.ko`,
which come off your own box ([docs/VENDOR-BITS.md](docs/VENDOR-BITS.md)). The NPU firmware is
identical for both host OSes: `dp_fwd` speaks only the PCIe AGNIC protocol.

## Quickstart

1. `sudo ./tools/probe.sh` on the target. Confirm the endpoint ([docs/HARDWARE.md](docs/HARDWARE.md)).
2. Cross-build `dp_fwd`, and deploy it with the `npu-firmware/switch-init/` scripts and your own box's UIO modules to the NPU ([docs/BUILD.md](docs/BUILD.md) Part B, [docs/VENDOR-BITS.md](docs/VENDOR-BITS.md)).
3. Reboot the NPU, or run `npu-firmware/deploy/dp-autostart.sh` there: it loads the UIO modules, runs the switch bring-up, and execs `dp_fwd`. The trunk comes alive.
4. Build and load the host driver. `port1..portN` appear ([docs/BUILD.md](docs/BUILD.md) Part A).
5. Assign the ports in your firewall; verify a DHCP lease and forwarding.

Step 2 has a measured limit. The NPU's stock rootfs mounts read-only (`root=/dev/mmcblk0p2`,
ext4, ro), so `/opt/dp/dp_fwd` cannot be written on a factory NPU, and `/tmp` staging is a test
mechanism that does not survive a reboot. A persistent NPU deployment therefore requires a
rootfs of your own, which is the rootfs replacement in
[docs/NPU-INSTALL.md](docs/NPU-INSTALL.md) that nobody has completed end to end.

## Repository layout

| Path | What | SPDX |
|---|---|---|
| [`host-driver-linux/`](host-driver-linux/) | Linux kernel module (IPFire, or any Linux) | GPL-2.0 OR MIT |
| [`host-driver-linux/tools/`](host-driver-linux/tools/) | `npc.sh`, the NPU serial-console helper on the host's `ttyS2` | MIT |
| [`host-driver-freebsd/`](host-driver-freebsd/) | FreeBSD `if_agnic` driver (pfSense/OPNsense); control plane Measured, datapath Unverified | BSD-2-Clause |
| [`npu-firmware/src/`](npu-firmware/src/) | Clean-room wire-contract headers, the `dp_app` skeleton, and the `dp_swop` switch-register handler with its host-side unit test | MIT |
| [`npu-firmware/forwarder/`](npu-firmware/forwarder/) | `dp_fwd` source (`forwarder.c`); Marvell, with the quarantine rules in [that directory's README](npu-firmware/forwarder/README.md) | GPL-2.0 |
| [`npu-firmware/switch-init/`](npu-firmware/switch-init/) | Native 88E6193X bring-up; replaces the proprietary init | MIT |
| [`npu-firmware/build/`](npu-firmware/build/) | Cross-build pipeline (toolchain + MUSDK + dp_fwd) | MIT |
| [`npu-firmware/deploy/`](npu-firmware/deploy/) | Launcher + relay-to-NPU tooling | MIT |
| [`npu-firmware/deploy/keys/`](npu-firmware/deploy/keys/) | Sophos's already-public NPU SSH key, and why it is here | — |
| [`npu-firmware/tests/`](npu-firmware/tests/) | `sw-init-interlock-test.sh`, which proves the maps-before-PHYs interlock; no hardware, no root | MIT |
| [`tools/probe.sh`](tools/probe.sh) | The endpoint check | MIT |
| [`docs/`](docs/) | Architecture, hardware, NPU install, build, vendor bits, provenance, licensing | MIT |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Patch rules, sign-off, wanted work | MIT |
| [`LICENSE`](LICENSE), [`LICENSES/`](LICENSES/) | Component-to-SPDX map, and the full licence texts | — |

The key under `npu-firmware/deploy/keys/` is the one item here that is not ours and carries no
SPDX identifier of ours. It is Sophos's, and it is the single stated exception set out under
[Licensing](#licensing).

## Documentation

| Document | What it answers |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How a packet gets from a front jack to the firewall stack: the single trunk, the 66-byte per-port prefix, the management and data planes |
| [docs/HARDWARE.md](docs/HARDWARE.md) | What is 116-specific, and what to re-derive for another model |
| [docs/NPU-INSTALL.md](docs/NPU-INSTALL.md) | Reaching the NPU over the management link, and replacing its root filesystem |
| [docs/BUILD.md](docs/BUILD.md) | Building and deploying both halves |
| [docs/VENDOR-BITS.md](docs/VENDOR-BITS.md) | Which vendor files you supply from your own appliance, and where they sit on it |
| [docs/PROVENANCE.md](docs/PROVENANCE.md) | Where every input came from, and how to obtain it yourself |
| [docs/LICENSING.md](docs/LICENSING.md) | The per-component licence map and the redistribution rules |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Patch rules, DCO sign-off, and where help is wanted |

## Licensing

Everything first-party is permissive (MIT or BSD-2-Clause) or GPL-dual; `dp_fwd` is Marvell
GPLv2 and ships with its source. Three pieces are not ours to redistribute: the NPU's base OS,
the `mv_armada_ep` endpoint driver, and the Marvell UIO modules. They are not here and are not
needed from us. You already own them on your own XGS, and you extract them from your own box.

One vendor artefact ships deliberately: `npu-firmware/deploy/keys/mvmgt.x86`, Sophos's NPU
management SSH key. It is already public in Sophos's own downloadable firmware ISO, it is
already trusted by every XGS's NPU, so it confers nothing on anyone, and without it the install
path stops being repeatable.

Full detail: [LICENSE](LICENSE), [LICENSES/](LICENSES/), [docs/LICENSING.md](docs/LICENSING.md),
[docs/VENDOR-BITS.md](docs/VENDOR-BITS.md), [docs/PROVENANCE.md](docs/PROVENANCE.md), and
[npu-firmware/deploy/keys/README.md](npu-firmware/deploy/keys/README.md) for the key itself.

## Contributing

The two open items that carry the most weight are finishing and verifying the FreeBSD RX/TX
datapath ([host-driver-freebsd/README.md](host-driver-freebsd/README.md)), and port maps plus
`switch-init` adaptations for the XGS 136 and other models
([docs/HARDWARE.md](docs/HARDWARE.md)). Commits need a Developer Certificate of Origin sign-off
(`git commit -s`). [CONTRIBUTING.md](CONTRIBUTING.md) has the build, test and licence rules.

## About Mamoru

This kit is the XGS hardware-enablement component of Mamoru, an open-source Rust firewall
firmware for commodity and repurposed network appliances. The host driver and NPU firmware here
are Mamoru's own clean-room work, and are the same code Mamoru itself runs on this hardware.
Mamoru's own repository is not yet public.
