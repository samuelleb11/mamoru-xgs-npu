# The NPU payload — what an XGS 116's coprocessor runs

`mamoru-install --npu-payload` writes these onto the NPU's standby eMMC slot and points
U-Boot at it. Without them an XGS installs, boots, reports success and has **no
front-panel ports**: the CN9130 drives the switch, and until it runs our software the
box has `lo` and nothing else.

They are here, in the kit, because the payload is **platform support** — the same
category as a NIC's firmware blob. An operator installing Mamoru on an XGS must get a
working appliance from one image, with no second stick, no network fetch and no extra
hardware. Anything that has to be supplied separately is not hardware support, it is
homework.

## What each file is

| file | what it is |
|---|---|
| `Image` | the CN9130 kernel (arm64) U-Boot `ext4load`s from the slot |
| `cn9130-Sophos-XGS116.dtb` | the device tree for this board |
| `rootfs-dp.tar` | the root filesystem, carrying `/opt/dp/` — `dp_fwd`, the three UIO/MUSDK modules, and the switch-init scripts — with an `rcS` that starts the data plane on boot |
| `MANIFEST` | `name bytes sha256` for the three above. **This file is the authority**: the installer reads it at flash time and verifies every file against it, and the Mamoru build pins it by hash. |

## Provenance — read this before trusting it

**These are artefacts, not sources, and the sources for most of them do not exist
anywhere.** Stated plainly because a directory of binaries in a repository implies a
build that can reproduce them, and here there is none:

* `Image` — **no build.** No kernel config, no recipe, in this kit or any other.
* `rootfs-dp.tar` — **no build.** Assembled once, 2026-08-01, by a process nobody
  recorded. `platform/sophos-xgs116/scripts/assemble_npu_payload.sh` in the Mamoru
  repository joins it with `/opt/dp` and rewrites `rcS`, but it starts from this tar and
  cannot make one.
* `/opt/dp/modules/*.ko` (inside the tar) — **no build.** The only copies that exist.
* `dp_fwd` (inside the tar) — the **one** component with source here
  (`../../forwarder/forwarder.c`), though the binary shipped was built 2026-07-31 rather
  than rebuilt from it.

So this directory can be VERIFIED byte-for-byte and cannot be REPRODUCED. That is a
known gap, tracked as DEBT #190 in the Mamoru repository, and it is the reason this
README exists rather than a build script.

## Verifying

    cd npu-firmware/deploy/payload
    while read -r name bytes sha _; do
        case "$name" in \#*|"") continue ;; esac
        [ "$(wc -c < "$name")" = "$bytes" ] || echo "SIZE MISMATCH: $name"
        echo "$sha  $name" | sha256sum -c -
    done < MANIFEST
