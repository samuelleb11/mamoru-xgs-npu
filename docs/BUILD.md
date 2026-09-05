# Build and deploy

The kit is two independent halves: the host driver, built on the firewall OS, and the NPU
firmware, cross-built on a Linux host and deployed onto the NPU. Build and deploy the NPU firmware
(Part B) first, so the trunk is alive when the host driver (Part A) attaches.

The end-to-end order is: confirm the endpoint on the target; cross-build `dp_fwd` and deploy it
with the `switch-init/` scripts and your appliance's own Marvell UIO modules; reboot the NPU, or
run `dp-autostart.sh`, so the trunk comes alive; build and load the host driver, at which point
`port1..portN` appear; assign those ports in the firewall and verify a DHCP lease and forwarding
through them.

Prerequisite: a confirmed AGNIC endpoint. `sudo ./tools/probe.sh`, run on the target from the
repository root, must report `11ab:7080` with BARs `1M/16M/16M`. The id present with different BARs
means the barmap layout may differ, so the host driver cannot be assumed to attach.
[HARDWARE.md](HARDWARE.md) carries that reading and the one for an absent id.

## Part B — NPU firmware and switch bring-up

Prerequisite: a root shell on the NPU over `mvmgmt0` ([NPU-INSTALL.md](NPU-INSTALL.md)). That
document brings the management link up, finds the NPU's address, and covers replacing the NPU's
whole root filesystem. On a factory NPU the rootfs is read-only, so the `/opt/dp/` drop-in below is
a test mechanism and the rootfs replacement is the only persistent path. Persistence and restart,
below, records the measurement.

### Cross-build

The CN9130 has no on-box compiler, so the firmware is cross-built on a Linux host and the binaries
relayed to the NPU. `npu-firmware/build/env.sh` sets the build inputs: toolchain paths, the MUSDK
source directory, and the relay target. It pins the toolchain,
`TC=aarch64--glibc--stable-2018.11-1`. It does not pin MUSDK: `MUSDK` is a path, and the repository
and tag appear only in a comment on that line, so checking the tree out at that tag is a manual
step.

```sh
cd npu-firmware/build
. ./env.sh                      # toolchain paths, MUSDK repo/tag, relay target

# 1. fetch inputs (one-time) into $BUILD_ROOT: the Bootlin aarch64 toolchain ($TC_URL)
#    and MUSDK (MarvellEmbeddedProcessors/musdk-marvell at the tag env.sh names).
# 2. build MUSDK: a static libmusdk.a with giu+pp2+nmp.
docker run --rm --network host -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_musdk.sh
# 3. build dp_fwd from forwarder.c + the kit's headers, inside the MUSDK app tree.
docker run --rm -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_fwd.sh   # -> /w/dp_fwd
```

Step 3 reads the kit's sources from `/w/dp-src/`, so `npu-firmware/src/` and
`npu-firmware/forwarder/forwarder.c` must be staged into the build root before it runs.

Step 4, the native switch-config helper `dp_swcfg`, does not build from this kit as it stands. It
is diagnostics only; nothing in the datapath needs it. `build_swcfg.sh` runs inside the same
ubuntu:20.04 container with the build root mounted at `/w`, not as `sh build_swcfg.sh` on the build
host, and it compiles against a Marvell UMSD (Unified Switch Driver) source tree at `$W/umsd`, from
which it builds `libMsdDrv.a` and `libMRegAccess_mvmdio_uio.a`. No UMSD tree is in this repository,
so the script stops at `cd "$UMSD"`. `dp_swcfg.c` also `#include`s `msdApi.h` and opens
`/dev/mvmdio-uio`, so it is not part of the dependency-free shell bring-up: the three
`switch-init/` shell scripts need no vendor library, this one C file needs the UMSD tree.

### Deployment onto the NPU

The NPU needs, in `/opt/dp/`: `dp_fwd`, `dp-nmp-config.txt`, the `switch-init/` scripts, and your
appliance's own Marvell UIO modules. The UIO modules are not shipped in this repository and come off
your own box — see [VENDOR-BITS.md](VENDOR-BITS.md). `/opt/dp/` is read-only on a factory NPU, so
this drop-in lands only on a rootfs of your own.

`npu-firmware/deploy/relay-deploy.sh` copies two of those files over `mvmgmt0` with `scp`:
`/tmp/dp_fwd` and `/tmp/dp-nmp-config.txt` from the host, into `$DST` on the NPU (default
`/opt/dp`). Stage both in the host's `/tmp` first. The script then chmods and sha256sums `dp_fwd`
on the NPU. It touches nothing else, so `swmdio.sh`, `sw-init.sh`, `dp-autostart.sh` and the UIO
modules have to be copied separately. It assumes a FreeBSD host: it brings the link up with
`ifconfig mvmgmt0 inet6 -ifdisabled auto_linklocal up` and probes with `ping6`, not the Linux `ip`
forms used elsewhere in these docs. The fourth `switch-init/` script, `sfp-init.sh`, has no caller
anywhere in the tree and is a bench tool rather than a deployment payload
([HARDWARE.md](HARDWARE.md)).

At NPU boot, `dp-autostart.sh` loads the UIO modules, runs `sw-init.sh`, and execs `dp_fwd`, in
that order.

### Persistence and restart

Measured 2026-09-01 on the reference XGS 116: the stock NPU rootfs mounts read-only
(`root=/dev/mmcblk0p2`, ext4, ro), so `/opt/dp/dp_fwd` cannot be written, truncated or replaced on
a factory NPU. `/tmp` is tmpfs, the only writable storage, and staging there is a test mechanism
that does not survive a reboot by design. A persistent NPU deployment therefore requires a rootfs
of your own, which is the rootfs replacement in [NPU-INSTALL.md](NPU-INSTALL.md). The appliance's
own built-in NPU flasher is a separate mechanism with a separate proof. Not attempted.

Measured 2026-09-01 on the reference XGS 116: the host does not survive a `dp_fwd` restart. The
test ran a candidate binary identical to the incumbent, so nothing could be blamed on new code;
both management paths, `port1` and `port9`, stayed dark through the watchdog's entire
candidate-plus-restore budget, and recovery required a mains power-cycle, with the host back at
T+29s. The mechanism is the handshake: the host publishes its AGNIC management rings once at boot
(P3) and the NPU latches them when `dp_fwd` starts, and nothing re-drives that publication from the
host side. Every NPU firmware iteration therefore costs a mains cycle, and no NPU-side experiment
can report its own result — the verdict is read from the host after the cycle.

## Part A — host driver

### Linux / IPFire

Prerequisite: kernel headers or source matching the running kernel. IPFire ships a recent 6.x
kernel and the driver targets the 6.12–6.19 era, so a current IPFire is a close match. On IPFire,
install the kernel headers pakfire package, or point `KDIR` at a matching kernel tree.

```sh
cd host-driver-linux
make -C /lib/modules/$(uname -r)/build M=$PWD modules      # or: make KDIR=/path/to/kernel
sudo insmod mamoru_agnic.ko
dmesg | tail                                               # expect P0/P2 attach
ip link                                                    # expect port1..portN
```

A build error on a kernel API (KPI) mismatch means the running kernel is far from the target era.
What shifts across that range is the netdev ops, `page_pool`, and the DMA and MSI-X helpers; the
adjustments land in `agnic_txrx.c` and `agnic_pport.c`.

Assign the `portN` interfaces to IPFire zones. They carry traffic only once the NPU firmware
(Part B) is running.

IPFire keys its zone assignment to the interface MAC address, and this driver derives the
front-panel addresses per appliance rather than using one fixed set
([host-driver-linux/README.md](../host-driver-linux/README.md)). Two consequences follow. On a
first install there is nothing to do. On a box already running an earlier build of this driver,
the addresses change on the first boot after the upgrade, so the existing zone bindings no longer
match and the zone assignment has to be redone. `mac_src=2` restores the previous fixed
`02:81:00:00:00:0N` if you would rather keep the old bindings; the driver logs the base it chose
and warns when the address is not unique to the appliance.

### FreeBSD / pfSense / OPNsense

Prerequisite: `/usr/src` matching your exact base. The driver targets 15.1, and the `if_t` and
`bus_dma` APIs shifted between 14 and 15, so build against your own source.

| Firewall release | FreeBSD base |
|---|---|
| pfSense CE 2.7.x | FreeBSD 14.0 |
| pfSense Plus 24/25 | FreeBSD 15 |
| OPNsense 26 | FreeBSD 15 |

```sh
cd host-driver-freebsd
make                       # kldxref-friendly out-of-tree build
sudo kldload ./if_agnic.ko

# persist across reboots:
echo 'if_agnic_load="YES"' >> /boot/loader.conf
```

Measured on 116 hardware: the control plane, bind → BARs → MSI-X → mailbox → mgmt echo. Unverified:
the RX/TX datapath, which is written but not finished and not verified. The remaining work is the
P3b–P5 tail, the driver's own bring-up stages, which it prints as it attaches and which
[ARCHITECTURE.md](ARCHITECTURE.md) lists.

If the NPU runs stock Sophos firmware, `agnic_nwa.c`, the NW_AGENT PHY mailbox, is the path to real
per-port link state. The FreeBSD tree implements it; the Linux tree asserts carrier unconditionally
instead.
