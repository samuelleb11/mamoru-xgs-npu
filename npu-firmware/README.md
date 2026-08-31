# npu-firmware — the minimal data-plane that runs ON the NPU

Just enough to make the CN9130 NPU speak our driver's AGNIC protocol. **Not a full
firmware** — the NPU keeps its own vendor base OS/bootloader (already on your box).

## What's here

| Dir | What | License |
|---|---|---|
| `src/` | Clean-room wire-contract headers (`pport_hdr.h`, `tag_dsa.h`, `portmap.h`, `dp_config.h`) + `dp_app` skeleton (`main.c`) | MIT (ours) |
| `forwarder/` | `forwarder.c` → built into `dp_fwd`, the running forwarder | **GPL-2.0** (Marvell — keep the header; see the dir's README) |
| `switch-init/` | Native 88E6193X bring-up: `sw-init.sh`, `swmdio.sh`, `sfp-init.sh`, `dp_swcfg.c` — replaces `libsbsp`/`xgs-mvl6193-init` | ours |
| `build/` | Cross-build: `env.sh` (toolchain + MUSDK source), `build_musdk.sh`, `build_fwd.sh`, `build_swcfg.sh` | ours |
| `deploy/` | `dp-autostart.sh` (clean-room launcher), `relay-deploy.sh`, `dp-nmp-config.txt` | ours |

## The two forwarders

- **`dp_fwd`** (from `forwarder/forwarder.c`, GPLv2) is what actually runs today.
  It links Marvell **MUSDK** (fetched at build, not shipped — see `../docs/LICENSING.md`).
- **`dp_app`** (`src/main.c`) is a cleaner, ours-only rewrite skeleton — not yet
  complete, and it still links MUSDK. It's here as the path toward a fully-permissive
  forwarder, not a drop-in replacement yet.

## Build & deploy

See `../docs/BUILD.md` (Part B). In short: fetch the toolchain + MUSDK, build MUSDK,
build `dp_fwd`, deploy `dp_fwd` + `switch-init/` + your box's Marvell UIO modules
(`../docs/VENDOR-BITS.md`) into `/opt/dp/` on the NPU. `dp-autostart.sh` wires it up
at boot.

## 136 note

The `switch-init/` scripts are **116-tuned** (8-port loops, SFP on lane 9). Adapt
them for your board — `../docs/HARDWARE.md` lists exactly what.
