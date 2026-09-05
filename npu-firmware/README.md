# npu-firmware

The minimal data plane that runs on the NPU: just enough to make the CN9130 speak the host
driver's AGNIC protocol. This is not a full firmware. The NPU keeps its own vendor base OS and
bootloader, which are already on the box, and nothing here replaces them. Every measurement below
is on an XGS 116.

## Directory map

| Path | Contents | Licence |
|---|---|---|
| [`src/`](src/) | Clean-room wire-contract headers (`pport_hdr.h`, `tag_dsa.h`, `portmap.h`, `dp_config.h`), the `dp_app` skeleton (`main.c`), and the `dp_swop` switch-register handler (`dp_swop.h`, `dp_swop.c`) with its host-side unit test (`dp_swop_test.c`) | MIT |
| [`forwarder/`](forwarder/) | `forwarder.c`, built into `dp_fwd`, the forwarder that runs today. Marvell's, kept in its own directory with its licence header intact; the quarantine rules are in [forwarder/README.md](forwarder/README.md) | GPL-2.0 |
| [`switch-init/`](switch-init/) | Native 88E6193X bring-up: `sw-init.sh`, `swmdio.sh`, `sfp-init.sh`, `dp_swcfg.c`. Replaces `libsbsp` and `xgs-mvl6193-init` | MIT |
| [`build/`](build/) | Cross-build: `env.sh` (toolchain and MUSDK source), `build_musdk.sh`, `build_fwd.sh`, `build_app.sh`, `build_swcfg.sh` | MIT |
| [`deploy/`](deploy/) | `dp-autostart.sh` (clean-room launcher), `relay-deploy.sh`, `dp-nmp-config.txt`, `dp-swap-guarded.sh` | MIT |
| [`deploy/keys/`](deploy/keys/) | `mvmgt.x86`, Sophos's already-public NPU management SSH key: the one bundled vendor artefact, justified in [deploy/keys/README.md](deploy/keys/README.md) | — |
| [`tests/`](tests/) | `sw-init-interlock-test.sh`, which proves the maps-before-PHYs interlock in `sw-init.sh`; no hardware, no root | MIT |

Everything here is ours and MIT except two files: `forwarder/forwarder.c`, which is Marvell's
under the GPLv2 election, and `deploy/keys/mvmgt.x86`, which is Sophos's key and carries no
licence of ours. Two files carry no `SPDX-License-Identifier` line of their own:
`forwarder/forwarder.c`, whose Marvell header is its licence statement, and
`deploy/dp-nmp-config.txt`, which is MIT under the root `LICENSE`.
[../docs/LICENSING.md](../docs/LICENSING.md) is the per-component map.

## The two forwarders

`dp_fwd`, built from `forwarder/forwarder.c` (GPLv2), is what actually runs today. It links
Marvell MUSDK, which is fetched at build time and is not shipped in this repository
([../docs/LICENSING.md](../docs/LICENSING.md)).

`dp_app` (`src/main.c`) is a cleaner, ours-only rewrite skeleton: the path toward a fully
permissive forwarder, not a drop-in replacement for `dp_fwd`. It is not complete. As committed it
performs init, the host handshake and NMP scheduling only, so the GIU comes up and the host sees
the ports while no traffic moves. It also still links MUSDK, so it is not yet the permissive
endpoint either.

## Switch-register handler

`dp_fwd` carries `dp_swop`, an NPU-side handler for 88E6193X register access over the AGNIC
custom channel. Its write allowlist, the refusal of Global1 and Global2, and the read-back after
every write are stated in [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

`forwarder.c` includes `dp_swop.c` as a single translation unit, so `build_fwd.sh` stages both
`dp_swop.c` and `dp_swop.h` with the headers; if either is missing the build fails at the
`#include` rather than producing a `dp_fwd` without the handler. `src/dp_swop_test.c` exercises
the request/response envelope on any host with a C compiler and does not test the SMI transaction
itself. The SMI transaction is proven only through the shell reference: Measured 2026-09-01 on
one unit, `swmdio.sh` read port status and VLAN maps that matched known link state (`port1` up at
1000, `port9` up at 2500, ports 2-8 down, CPU port 0 VLAN map `0x03fe`).

The host half is Not attempted. Neither `mamoru-agnic` nor `if_agnic` sends a swop request or
reads the capability word `dp_fwd` publishes, so no swop operation has run from the host end to
end. Measured 2026-09-03, with the custom channel pumped only from `ctrl_cb`: the first custom
send and four swop reads returned -110 while the PF handshake stayed clean and the host's health
word still read `ok`. `forwarder.c` now drives the guest channel from `mng_pump_thread`, and no
host-side swop result after that change is recorded. Unverified: where the capability slot sits
inside the NW_AGENT window. `src/dp_swop.h` requires a hardware survey of that window before a
publishing build ships.

## Build and deploy

The procedure is [../docs/BUILD.md](../docs/BUILD.md), Part B. The order is: fetch the toolchain
and MUSDK, build MUSDK, build `dp_fwd`, deploy. What lands on the NPU in `/opt/dp/` is `dp_fwd`,
the `switch-init/` scripts, and your own box's Marvell UIO modules, which are not shipped here
([../docs/VENDOR-BITS.md](../docs/VENDOR-BITS.md)). At boot, `dp-autostart.sh` loads those
modules, runs `sw-init.sh`, and execs `dp_fwd`.

That drop-in lands only on a rootfs of your own. Measured 2026-09-04 on one factory unit: the
rootfs slot it ran from mounts read-only (`root=/dev/mmcblk0p2`, ext4, ro), so `/opt/dp/dp_fwd`
cannot be replaced there, and `/tmp` staging is a test mechanism that does not survive a reboot by
design. A persistent deployment therefore needs a rootfs of your own
([../docs/NPU-INSTALL.md](../docs/NPU-INSTALL.md)).

`dp-swap-guarded.sh` restarts `dp_fwd` under a deadline and restores the incumbent binary if the
candidate does not reach readiness. Measured 2026-09-01 on one unit, with a candidate identical to
the incumbent: the host does not survive a `dp_fwd` restart, and recovery required a mains
power-cycle, with the host back at T+29s. Its rollback logic is therefore moot today and it is not
a working deploy path; [../docs/BUILD.md](../docs/BUILD.md) carries the full result.

`dp_swcfg.c` does not build from this kit as it stands. `build_swcfg.sh` compiles it against a
Marvell UMSD (Unified Switch Driver) source tree at `$W/umsd`, no UMSD tree is in this
repository, and the script stops at `cd "$UMSD"`. Nothing in the boot path runs the resulting
binary.

## Board-specific tuning

The `switch-init/` scripts are tuned for the 116. In `sw-init.sh`, `ISOLATE_PORTS` covers switch
ports 1-10 and `CPU_EGRESS_PORTS` ports 1-9, the phyup loop walks the 8 copper ports, and
`SFP_LANE` defaults to lane 9. On a board with a different port count, or a cage on another lane,
those loops leave ports powered down or mis-mapped. Adapt them before running the kit on a 136 or
any other model; [../docs/HARDWARE.md](../docs/HARDWARE.md) lists what to change.

Add a new port to `ISOLATE_PORTS`, not only to the phyup loop. `sw-init.sh` writes the per-port
VLAN maps before it powers any PHY and, unless `SW_ISO_ENFORCE=0` is set, holds the front PHYs
dark when a map does not verify by read-back. A port powered before its map is written egresses to
every other front port and bridges the network to itself.
