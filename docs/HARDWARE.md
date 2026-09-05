# Hardware — what is 116-specific and what to re-derive

Every proof point in this kit is on a Sophos XGS 116. Other models in the line, the XGS 136 among
them, are expected to match at the PCIe and NPU level and to differ at the port map, the SFP lanes
and possibly the switch SKU.

The switch-init scripts named here run on the NPU. Getting a shell there is
[NPU-INSTALL.md](NPU-INSTALL.md).

## Endpoint check

Run on the target, from the repository root, before building anything:

```sh
sudo ./tools/probe.sh
```

| Result | Reading |
|---|---|
| `11ab:7080`, BARs `1M/16M/16M` | The AGNIC endpoint is the same family. The host driver is a near-drop-in; continue with [BUILD.md](BUILD.md). |
| id present, BARs differ | The barmap layout may differ, so the host driver cannot be assumed to attach. Characterize the endpoint before building. |
| id absent | The NPU endpoint is not enumerating. Check that the NPU booted and that its `mv_armada_ep` driver is up on the NPU's own OS ([VENDOR-BITS.md](VENDOR-BITS.md)). |

Nothing below matters on an endpoint the host driver cannot attach to. If the id or the BARs
differ, characterizing the endpoint is the first work, not the port map.

The AGNIC PCIe ABI is very likely identical across the XGS line, because the line shares one CN9130
NPU firmware family. That is an expectation, not a measurement: this kit carries no probe result
from any model other than the 116. Unverified.

## The 116 assumptions in this kit

| Assumption | Where | Risk on another model |
|---|---|---|
| Eight front copper jacks, on switch ports 1-8 | `npu-firmware/switch-init/sw-init.sh`: `ISOLATE_PORTS`, `CPU_EGRESS_PORTS`, the reg4 loop in step (2), the phyup loop `1 2 3 4 5 6 7 8` in step (5) | **High** — loops that do not match the board leave ports powered-down or mis-mapped |
| Switch port 9 is the SFP cage, PortF1: a C45 SerDes lane whose lane number equals its port number | `npu-firmware/switch-init/sw-init.sh` step (6), `SFP_LANE=9` | **High** — another model may carry more cages, or cages on other lanes |
| Nine host-visible ports, Port1..Port8 plus PortF1 | `AGNIC_PPORT_COUNT` in `host-driver-linux/agnic_abi.h`, `PPORT_COUNT` in `host-driver-freebsd/agnic_pport.c`, `DP_PORT_COUNT` in `npu-firmware/src/dp_config.h` | **High** — three separate constants with no shared header; a different front-port count changes all three |
| 88E6193X "Amethyst" switch, multi-chip SMI address 2, orion SMI at `0xf212a200` | `npu-firmware/switch-init/swmdio.sh` | **Medium** — a different switch SKU, such as an 88E6393X, shifts both the SMI access method and the register map |
| The DSA port map: which switch port carries which host `portN` | `npu-firmware/src/portmap.h` | **Medium** — re-derive for the board's layout |

`npu-firmware/deploy/dp-autostart.sh` holds no port count. Read end to end, it insmods three UIO
modules, runs `npu-firmware/switch-init/sw-init.sh` and execs `dp_fwd`; its only per-port content is
a comment pointing back at that script. Nothing in it changes with the front-port count.

SFP EEPROM addressing is a known hazard with no code in this kit. A module's EEPROM can sit at a
shifted offset — the shifted-EEPROM trap — and different cage wiring on another model moves it
again, which makes it a medium risk wherever it is read. Nothing in this repository reads one:
`npu-firmware/switch-init/sfp-init.sh` names a module EEPROM in its comments only, and both bring-up
paths drive the SerDes over SMI. Not attempted here; the hazard lands on whatever tooling you add.

## Ordering: VLAN maps before PHY up

A front port powered before its VLAN map is written egresses to every other front port, bridging the
customer's networks inside the switch. On 2026-08-08 that looped a live LAN on the reference 116 and
needed mains power cut to recover. After a switch reset reg 6 holds the part's default all-ports
map, so a port that is powered and forwarding before its map is written is briefly a bridge. Dark
ports cannot bridge; that is the entire argument, and it holds only if the maps land first.

`npu-firmware/switch-init/sw-init.sh` writes every per-port map in step (1), reads each write back
and compares it (`wv()`), and holds the front PHYs dark if any map is unverified: steps (5) and (6)
are skipped, the run prints `ISOLATION UNVERIFIED`, and it exits 1 as `DONE (DEGRADED)`. An
unverifiable write counts as a failed write. "I could not check" and "it worked" are different
claims. The CPU port is exempt from the gate, because it is the only port the isolation maps point
at and powering it cannot create a front-to-front path; the CPU trunk comes up, so the box stays
reachable for diagnosis. `SW_ISO_ENFORCE=0` overrides the refusal.

Keep that ordering when adapting to another model, and add a new port to `ISOLATE_PORTS` rather than
only to the phyup loop. Port 9 was found holding `reg6 = 0x05ff` on the live box while every other
front port held `0x001`, because it is the port every bring-up loop skips.

`npu-firmware/tests/sw-init-interlock-test.sh` tests the property against a stub switch in five
controlled arms: no hardware, no root, any POSIX shell. It fails on the pre-2026-09-01 ordering. Run
it after editing the loops.

## Retargeting checklist

1. Port count. In `npu-firmware/switch-init/sw-init.sh`, edit `ISOLATE_PORTS`, `CPU_EGRESS_PORTS`,
   the reg4 loop in step (2) and the phyup loop `1 2 3 4 5 6 7 8` in step (5). `ISOLATE_PORTS` must
   be a superset of every physical port on the part, roled or not: isolating a lane with no assigned
   role costs nothing, and an unwritten map is the hazard. `CPU_EGRESS_PORTS` lists only the ports
   with an assigned role, and the CPU port's egress map is derived from it rather than typed,
   `0x3fe` on the 116, because three hand-written counts of the same thing disagreed and the
   write-verify passed on the wrong one. This is the highest-risk change in this list.
2. SFP lanes. If the board carries more or other cages, replicate step (6) of
   `npu-firmware/switch-init/sw-init.sh`, the boot path, for each cage's SerDes lane through
   `SFP_LANE`, and set `SFP_POC` and `SFP_CMODE` per module, DAC or optical. A passive DAC has no
   PCS of its own, so the rate must match the far-end switch port: cmode `0x9` with POC `0x8048` for
   1000BASE-X, cmode `0xd` with POC `0x804d` for 10GBASE-R. POC first, then BMCR: a BMCR write to a
   lane still powered down at the POC level is swallowed silently by the unclocked PCS, and
   measured, the write did not take.
3. Switch SKU. If the probe or register reads show a different part, re-verify the SMI base address
   and the register offsets in `npu-firmware/switch-init/swmdio.sh` against that part's datasheet. A
   wrong device base fails silently: on 2026-09-01 a port base of `0x10`, which is the single-chip
   PHY-address convention and wrong for the SMI-address-2 indirect protocol, returned `0x0000` from
   every port register with the SMI VALID bit set. Reg 3 (Switch ID) cannot discriminate, because it
   reads `0x1930` at any device address. Correlate register values against independently-known link
   state before trusting a map.
4. Port map. Re-derive `npu-firmware/src/portmap.h` from the board. Its comments document two
   methods, both available on your own appliance: the board's platform descriptor, on the 116
   `bsp/opt/sophos/plt/AMDA0208-0001R00.txt`, whose `npu0.ethN.port=1:<switch port>` lines give the
   switch port behind each front jack; and live register reads with
   `npu-firmware/switch-init/swmdio.sh` on the NPU, `rd <port> 0` (Port Status) and `rd <port> 6`
   (Port-Based VLAN Map), correlated against link state known from the x86 side. On the 116 the two
   agree: the identity map 1..8 for the copper jacks, `1:9` for PortF1, and the CPU/backplane on
   switch port 0. Confirmed against the descriptor 2026-08-27, re-confirmed by register reads
   2026-09-01. `dp_swcfg` (`npu-firmware/switch-init/dp_swcfg.c`) is not one of those methods: it
   links the Marvell UMSD tree, which is not in this kit ([BUILD.md](BUILD.md)). The physical-label
   order, which jack is silkscreened "3", comes from the DSA source-port field of live RX frames. A
   wrong label order mis-pairs jacks but still forwards and separates them.
5. Host port count. `AGNIC_PPORT_COUNT`, `PPORT_COUNT` and `DP_PORT_COUNT` all read 9 on the 116
   and must move together. The prefix encoding is `byte0 = 0x81 + port_index`, so indices stay
   contiguous from 0 ([ARCHITECTURE.md](ARCHITECTURE.md)).

## Two SFP implementations

Step (6) of `npu-firmware/switch-init/sw-init.sh` is the boot path. It defaults to `SFP_POC=0x8048`
and `SFP_CMODE=0x9` (1000BASE-X), matching the board descriptor's declared `init_speed=1G` for the
cage. `npu-firmware/switch-init/sfp-init.sh` is a hand-run, idempotent bench version of the same
sequence, with defaults `0x804d` and `0xd` (10GBASE-R) and an extra `SFP_BMCR`. It has no caller
anywhere in the tree, so editing it changes nothing at boot. Retarget the boot path; use the bench
script to prove a change against a live switch first.

What the 116 proves about that cage, measured 2026-08-27 on the reference unit: lane 9 boots powered
down, BMCR `0x1940` against lane 0's `0x1140` — identical but for bit 11, the MII power-down bit —
with a POC of `0x0078`, byte-identical to unpopulated lane 10, meaning never configured at all. The
bring-up clears that, reading back POC `0x004d`, BMCR `0x1140` and cmode `0xd`, matching the live
CPU trunk in every field. A link through the cage is unverified. With a 10G passive DAC
(SFP-H10GB-CU1M) into a Nami CS110-24 the port stays down across all four rate and autoneg
combinations, with the far end the suspect. Bringing the lane up is necessary, not sufficient.
