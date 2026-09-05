# host-driver-freebsd — the Marvell AGNIC driver for FreeBSD, pfSense and OPNsense

`if_agnic` is a native FreeBSD driver for the Marvell AGNIC PCIe endpoint that the CN9130 NPU
presents to the x86 host. It is built on newbus and `if(9)`, and deliberately not on iflib: the
rings are a firmware-fixed ABI, not a shape the driver gets to choose. Licence: BSD-2-Clause, with
an SPDX identifier in every file ([`../LICENSES/BSD-2-Clause.txt`](../LICENSES/BSD-2-Clause.txt)).

This tree is the original. The Linux driver in [`../host-driver-linux/`](../host-driver-linux/) was
transcribed from it, and the three ABI headers here, `agnic_barmap.h`, `agnic_ctrl.h` and
`agnic_giu.h`, are the OS-independent contract both drivers implement.

## Status

| Half | State | Evidence |
|---|---|---|
| Control plane | **Measured** | PCI bind, BAR0/2/4 map, 36-bit `bus_dma`, MSI-X, barmap read-back, two-way mailbox and the `CC_PF_MGMT_ECHO` round-trip, on XGS 116 hardware |
| RX/TX datapath, buffer pool and per-port demux | **Unverified** | Written; not run end to end |

The MSI-X table lives in BAR0 at 0x1000, where the firmware ABI puts it, and maps cleanly. The
barmap read-back returns cookie `0xd0fac10d` and version `0x00000005`.

Unverified on the datapath: the `CC_PF_INIT…ENABLE` sequence, the buffer-pool refill, the MSI-X
kick and its poll fallback, and the 66-byte pport tag demux. Cross-check each against the Linux
[`agnic_txrx.c`](../host-driver-linux/agnic_txrx.c) and
[`agnic_pport.c`](../host-driver-linux/agnic_pport.c), which are traffic-proven.

The driver labels its log lines by phase: `[Phase 1+2a]` at attach, then `P2b`, `P3a`, `P3b`, `P4a`,
`P4b` and `P4c`. `P5`, the `mvmgmt0` bring-up, runs inside the P2b handshake, so its lines can
appear ahead of the P3 and P4 ones. [../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) says what
each phase does, under the Linux driver's own P0-to-P5 labelling.

## Build

Prerequisite: `/usr/src` matching your exact base. pfSense CE 2.7.x is FreeBSD 14.0;
pfSense Plus 24/25 and OPNsense 26 are FreeBSD 15. `if_agnic` targets 15.1, and the `if_t` and
`bus_dma` APIs shifted between 14 and 15, so build against your own source tree and expect minor
adjustments on 14.

```sh
make                                              # -> if_agnic.ko
sudo kldload ./if_agnic.ko
echo 'if_agnic_load="YES"' >> /boot/loader.conf   # persist across boot
```

Both halves of the kit, host driver and NPU firmware, are built in
[../docs/BUILD.md](../docs/BUILD.md).

## Interfaces

The `portN` interfaces attach as ordinary Ethernet ifnets. Assign or bridge them in the firewall
GUI like any other NIC.

`mvmgmt0`, the management link to the NPU, comes from `agnic_pcinet.c`, and it is not optional.
`if_agnic` calls `agnic_pcinet_bringup()` on the attach path with no tunable, sysctl or flag to
suppress it; there is no FreeBSD equivalent of the Linux `mvmgmt` module parameter. The function
self-guards, returning early when the MGMT_NETDEV facility is absent or its ready pattern never
appears, so a box without the facility gets no `mvmgmt0` and attach carries on.
[../docs/NPU-INSTALL.md](../docs/NPU-INSTALL.md) uses that link to reach the NPU.

## Front-panel port addresses

`if_agnic` attaches every `portN` with the fixed synthetic address `02:81:00:00:00:NN`, NN being
the physical port number (`agnic_pport.c`). At P4a, on each port the NPU reports as manageable, it
registers that same address over NW_AGENT `mac_set` and only then admin-ups the port, the order
SFOS's own driver uses (`agnic_nwa.c`). Neither was changed.

The Linux driver now derives the middle three octets per appliance, from the x86 board's DMI or the
NPU-published device MAC, and falls back to this same fixed address when neither is readable
([../host-driver-linux/README.md](../host-driver-linux/README.md)). Here the address stays fixed,
so two FreeBSD appliances on one L2 segment present the same nine port addresses. That is a known
divergence between the two trees, deliberate and not a defect in either: this side registers its
port address with stock firmware over `mac_set`, and the Linux side registers none, since this
kit's `dp_fwd` replaced the NetAgent that serviced those messages.

## Per-port link state

`agnic_nwa.c` implements NW_AGENT, the PHY-control mailbox, and it targets stock NPU firmware. The
mailbox is a shared-memory window in BAR0 that the NPU services; SFOS's own host driver,
`mv_nwa_host`, is what drives it to bring the front-panel PHYs up and read per-port link, and
`agnic_nwa.c` is the clean-room FreeBSD client for the same transactions. NW_AGENT is the only path
in this tree to real per-port link state. Against stock firmware without it, the GIU trunk links up
and every front-panel port stays dark. Keep `agnic_nwa.c` if your NPU runs stock firmware.
