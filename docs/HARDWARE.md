# Hardware — XGS 116 vs your 136, and what to adapt

**Every proof point in this kit is on a Sophos XGS 116.** Your friend's box is a
**136**. "Same hardware mostly" is probably true at the PCIe/NPU level and
probably *not* true at the port-map level. This file is what to check and what to
change.

## First: confirm the endpoint (this gates everything)

```sh
sudo ./tools/probe.sh
```

- **`11ab:7080`, BARs `1M/16M/16M`** → the AGNIC endpoint is the same family; the
  host driver is a near-drop-in. Proceed.
- **id present, BARs differ** → characterize the 136's endpoint before building
  (barmap layout may differ).
- **id absent** → the NPU EP isn't enumerating: check the NPU booted and its
  `mv_armada_ep` driver is up on the NPU's own OS.

The AGNIC PCIe ABI is very likely identical across the XGS line (same CN9130 NPU
firmware family) — that's the optimistic, and probable, case.

## The 116 assumptions baked into this kit

| Assumption | Where | 136 risk |
|---|---|---|
| **8 front copper ports** (loops `1..8`) | `switch-init/sw-init.sh` steps (4)(5); `deploy/dp-autostart.sh` | **High** — a 136 likely has a different port count; the loops must match, or ports boot powered-down / mis-mapped |
| **Port 9 = the SFP cage (PortF1)**, C45 SerDes lane 9 | `sw-init.sh` step (6), `SFP_LANE=9` | **High** — a 136 may have more/different SFP cages at other lanes |
| **88E6193X "Amethyst" switch**, multi-chip SMI addr 2, orion SMI @ `0xf212a200` | `switch-init/swmdio.sh` | **Medium** — a 136 may use a different switch SKU (e.g. 88E6393X); the SMI access + register map would shift |
| **DSA port map** (which switch port = which host `portN`) | `npu-firmware/src/portmap.h` | **Medium** — re-derive for the 136's layout |
| **SFP EEPROM addressing** (the "shifted-EEPROM trap") | SFP init / `sfp-init.sh` | **Medium** — SFP module EEPROM can sit at a shifted offset; a different cage wiring on the 136 can move it again |

## What to change for a 136

1. **Port count.** Edit the `1 2 3 4 5 6 7 8` loops in `sw-init.sh` (steps 4 and
   5) and the per-port work in `dp-autostart.sh` to your actual front-port count.
   This is the single most likely thing to bite.
2. **SFP lanes.** If the 136 has more/other SFP cages, replicate step (6) for each
   cage's SerDes lane (`SFP_LANE`), and set `SFP_POC`/`SFP_CMODE` per the module
   (DAC vs optical; 1000BASE-X `0x9` vs 10GBASE-R `0xd`).
3. **Switch SKU.** If `probe`/register reads show a different switch, re-verify the
   SMI base and the register offsets in `swmdio.sh` against that part's datasheet.
4. **Port map.** Re-derive `portmap.h` from the 136 (the empirical method is in
   that header's comments — `dp_swcfg`/register reads on the live switch).

## The "phyup after VLAN maps" safety rule (don't skip)

`sw-init.sh` brings PHYs up **after** writing per-port VLAN maps, deliberately. A
port brought up before its VLAN map can egress to every other front port,
bridging the customer's networks inside the switch. Keep that ordering when you
edit for the 136.
