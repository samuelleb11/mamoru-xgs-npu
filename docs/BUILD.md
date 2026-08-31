# Build & deploy

Two independent halves: the **host driver** (on your firewall OS) and the **NPU
firmware** (cross-built, deployed onto the NPU). Do the NPU firmware first so the
trunk is alive when the host driver attaches.

> **Step 0, always:** `sudo ../tools/probe.sh` on the target. Confirm `11ab:7080`
> + BARs `1M/16M/16M` before spending time building. See `HARDWARE.md`.

---

## Part A — host driver

### Linux / IPFire

Prereqs: kernel headers or source matching the **running** kernel (IPFire ships a
recent 6.x; the driver targets the 6.12–6.19 era, so a current IPFire is a close
match). On IPFire, install the kernel headers pakfire package or point `KDIR` at a
matching kernel tree.

```sh
cd host-driver-linux
make -C /lib/modules/$(uname -r)/build M=$PWD modules      # or: make KDIR=/path/to/kernel
sudo insmod mamoru_agnic.ko                                        # module name per Makefile
dmesg | tail                                               # expect P0/P2 attach
ip link                                                     # expect port1..portN
```

If the build errors on a KPI mismatch (netdev ops, page_pool, etc.), you're on a
kernel far from the target era — small adaptations; see the comments in
`agnic_txrx.c`/`agnic_pport.c`. Assign the `portN` interfaces to IPFire zones as
usual. The ports only carry traffic once the NPU firmware (Part B) is running.

### FreeBSD / pfSense / OPNsense

Prereqs: `/usr/src` matching your exact base. **pfSense CE 2.7.x = FreeBSD 14.0;
pfSense Plus 24/25 and OPNsense 26 = FreeBSD 15.** The driver targets 15.1, and
the `if_t`/`bus_dma` APIs shifted 14→15, so build against **your** source.

```sh
cd host-driver-freebsd
make                       # kldxref-friendly out-of-tree build
sudo kldload ./if_agnic.ko
# persist: echo 'if_agnic_load="YES"' >> /boot/loader.conf
```

**Status:** the control plane (bind → BARs → MSI-X → mailbox → mgmt echo) is
proven on 116 hardware; the RX/TX datapath is written but needs finish-and-verify.
Expect to work through the P3b–P5 tail (see the reference notes). If your NPU runs
stock Sophos firmware, `agnic_nwa.c` (the NW_AGENT PHY mailbox) is the path for
real per-port link — the FreeBSD tree implements it; the Linux one forces carrier
on instead.

---

## Part B — NPU firmware (`dp_fwd` + switch bring-up)

The NPU (CN9130) has no on-box compiler, so you cross-build on a Linux host and
relay the binaries to the NPU. See `npu-firmware/build/env.sh` for the pinned
toolchain + MUSDK source.

```sh
cd npu-firmware/build
. ./env.sh                      # sets toolchain paths, MUSDK repo/tag, relay target

# 1. fetch inputs (one-time): the Bootlin aarch64 toolchain ($TC_URL) and MUSDK
#    (MarvellEmbeddedProcessors/musdk-marvell @ the tag env.sh pins) into $BUILD_ROOT.
# 2. build MUSDK (static libmusdk.a with giu+pp2+nmp):
docker run --rm --network host -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_musdk.sh
# 3. build dp_fwd (forwarder.c + our headers, inside the MUSDK app tree):
docker run --rm -v ~/npu-build:/w -w /w ubuntu:20.04 sh /w/build_fwd.sh   # -> /w/dp_fwd
# 4. build the native switch-config helper (optional, for diagnostics):
sh build_swcfg.sh                                                          # -> dp_swcfg
```

Deploy onto the NPU (into `/opt/dp/`): `dp_fwd`, `dp-nmp-config.txt`, the
`switch-init/` scripts, and your box's Marvell UIO modules (see `VENDOR-BITS.md`).
`npu-firmware/deploy/relay-deploy.sh` does this over serial→host→`mvmgmt0`. At NPU
boot, `dp-autostart.sh` loads the UIO modules, runs `sw-init.sh`, and execs
`dp_fwd`.

---

## Order of operations end to end

1. `probe.sh` on the target — confirm the endpoint.
2. Cross-build `dp_fwd`; deploy it + `switch-init/` + your UIO modules to the NPU.
3. Reboot the NPU (or run `dp-autostart.sh`) — the trunk comes alive.
4. Build + load the host driver — `port1..portN` appear.
5. Assign the ports in your firewall; verify a DHCP lease + forwarding.
