# host-driver-linux — Marvell AGNIC driver (Linux)

Clean-room out-of-tree kernel module. Exposes the XGS front ports as
`port1..portN` netdevs by speaking the AGNIC GIU trunk protocol to the NPU.
License: **GPL-2.0 OR MIT** (SPDX in every file).

## Build

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD modules
# or, cross/explicit kernel:  make KDIR=/path/to/kernel-build-tree
sudo insmod mamoru_agnic.ko
dmesg | tail        # P0 bind (11ab:7080) -> P1 BARs/MSI-X -> P2 barmap read
ip link             # port1..portN once the NPU firmware is running
```

Self-contained: only `<linux/*>` plus the two local headers. No firmware blob
(`request_firmware()` is never called), no device tree (pure PCI enumeration), no
cross-toolchain. Nothing from the rest of the Mamoru firewall (Rust userspace, signing,
Buildroot) is required.

## Notes

- **Kernel version.** Targets the 6.12–6.19 KPI era. A current IPFire (6.x) is a
  close match. On a far-older kernel, expect small adjustments in
  `agnic_txrx.c`/`agnic_pport.c` (netdev ops, DMA, MSI-X helpers).
- **No traffic without the NPU side.** The driver attaches (P0/P2) on its own, but
  the front ports only carry frames once `dp_fwd` is running on the NPU. See
  `../npu-firmware/` and `../docs/BUILD.md`.
- **Link state.** With the generic NPU forwarder the driver forces carrier on
  (the forwarder exposes no PHY state); real per-port link needs the NW_AGENT path
  (the FreeBSD tree implements it; not wired on Linux).
- `tools/npc.sh` is an optional NPU serial-poke helper for debugging.
