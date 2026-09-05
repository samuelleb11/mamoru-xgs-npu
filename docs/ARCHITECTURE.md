# Architecture — front jack to host stack

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

The reference platform for this document is the XGS 116: eight copper front jacks plus one SFP
cage, nine front-panel ports in all. The NPU hangs off switch port 0, the switch's CPU port; the
front jacks land on switch ports 1 to 9 (`npu-firmware/src/portmap.h`). [HARDWARE.md](HARDWARE.md)
covers what has to be re-derived for other models.

## Forwarding

Forwarding is host-side, not offloaded. Every front-panel packet DMAs across PCIe to the host, the
firewall forwards or filters it, and it DMAs back. The NPU is a switch + PHY + port-multiplexer
conduit, not the policy engine, so the host driver is the whole datapath rather than a control
channel beside one: throughput and per-packet correctness are properties of the host.

## Port multiplexing

The eight front ports of the 116 plus the SFP cage are multiplexed onto a single AGNIC GIU trunk.
A 66-byte prefix sits ahead of every L2 frame in both directions and carries the source or
destination port: a two-byte tag whose `byte0 = 0x81 + port_index` and whose byte 1 is zero, then a
64-byte hardware header, then the Ethernet frame. Port1 is tag `0x81`; PortF1, the SFP port, is
`0x89`. The host driver adds the prefix on TX and strips it and demultiplexes on RX, on `byte0`
alone, so one trunk becomes `port1..portN` interfaces.

The prefix is part of the frame budget, not an addition to it. The frame size advertised to the
device at `CC_PF_INIT` is `1500 + 14 + 4 + 66` = 1584 bytes and must include the prefix; a value
that omits it overflows the GIU and full-MTU frames are clipped. Host RX buffers and TX copy slots
are a separate 2048 bytes, the host's own size rather than the NPU-local jumbo ceiling
(`AGNIC_RX_FRAME_SIZE` and `AGNIC_RX_CLSIZE`, `host-driver-linux/agnic_abi.h`).

## Control plane and data plane

Two planes ride the same trunk. The management plane rides the mgmt rings and carries the `CC_PF_*`
commands — `CC_PF_INIT`, `CC_PF_ENABLE` and `CC_PF_PROMISC` — plus the read-only
`CC_GET_CAPABILITIES`. Promiscuous mode is load-bearing rather than a diagnostic convenience:
front-panel frames arrive with a destination MAC of `0x81pp..`, so without `CC_PF_PROMISC` the trunk
drops every tagged frame and RX stays at zero. The data plane rides the RX and TX rings with a
buffer pool that the device DMAs into.

Both planes are described in the ABI headers: `agnic_abi.h` on the Linux side, and
`agnic_barmap.h`, `agnic_ctrl.h` and `agnic_giu.h` on the FreeBSD side, which are the OS-independent
statement of the same contract.

## Front-panel port addresses

Each front-panel interface carries a locally administered unicast address of the shape
`02:81:<24 bits per unit>:<port number>`: nine distinct addresses on the 116, one per port. This
kit owns no OUI, so none of them is globally unique. The `0x81pp` destination MAC the trunk sees is
the prefix's first two bytes, not one of these addresses.

The address is not load-bearing for the trunk. RX demultiplexes on `byte0` of the prefix alone, the
prefix carries no address, and the ABI has no MAC-set command. What it governs is host-side: which
frames the host stack treats as locally destined.

The two drivers differ in where the middle 24 bits come from, and the difference is deliberate.

| Driver | Port address | Registered with the NPU |
|---|---|---|
| Linux `mamoru-agnic` | Derived per appliance from the x86 board's DMI or the NPU-published device MAC, in the order the `mac_src` module parameter selects. With no source readable it falls back to `02:81:00:00:00:0N`, byte for byte ([../host-driver-linux/README.md](../host-driver-linux/README.md)) | No. The ABI has no MAC-set command, and this driver sends no NW_AGENT message |
| FreeBSD `if_agnic` | The fixed `02:81:00:00:00:NN` | Yes, over NW_AGENT `mac_set` against stock firmware (`host-driver-freebsd/agnic_nwa.c`) |

Whether the NPU-published device MAC is per-unit at all is Unverified. Nothing in `npu-firmware/`
writes that field, and the NPU's own management address is a firmware constant that read identically
on both measured XGS 116 units ([NPU-INSTALL.md](NPU-INSTALL.md)). Comparing the `P2: device MAC`
and `P4: port MAC base ... (NPU device MAC)` log lines across two units settles it. The host's DMI
is preferred by default because it assumes nothing about that field.

## Switch programming

The switch is programmed NPU-side. The 88E6193X is configured entirely by the `switch-init/`
scripts under `npu-firmware/`, running on the NPU, and neither host driver drives it: there is no
MDIO, SMI or DSA code in this kit's host half. The FreeBSD network-agent message types include a
raw switch/PHY register operation, `NWA_TYPE_MDIO_OPERATION` at
`host-driver-freebsd/agnic_nwa.c:87`, that nothing in this kit sends.

The shipped `dp_fwd` does carry an NPU-resident switch-register handler. `forwarder.c` includes
`dp_swop.c` as a single translation unit, and a custom-channel message carrying the `SWOP` magic is
serviced there as a switch register read, a register write, or a bulk read of status and VLAN map
for every port device. Writes are allowlisted by (device, register) pair rather than denylisted,
every write is read back, and a write whose register already held the requested value is reported
as unchanged rather than OK, because a read-back that was never going to move proves nothing.
Writes to Global1 and Global2 are refused outright; those devices stay readable. The firmware
publishes a two-word capability slot in the host-visible BAR0 at the NW_AGENT window base, offset
`0x4000`, so a host can tell whether a handler is registered without sending a message to find
out. `npu-firmware/src/dp_swop.h` binds a reader to match that version
exactly and never with `>=`: firmware and driver ship independently, and the NPU rootfs survives
host OTAs, so a `>=` test would let a future firmware that changed the message shape open the gate
for today's driver. The same header leaves the slot's placement inside the window Unverified, and
requires a hardware survey confirming nothing else writes there before a publishing build ships.

Host-driven switch access is Not attempted in this kit: neither `mamoru-agnic` nor `if_agnic` reads
that capability word or sends a switch-register request. The host therefore has no path of its own
to the switch, and reaches switch registers, if at all, only through that NPU-side handler.
[../npu-firmware/forwarder/README.md](../npu-firmware/forwarder/README.md) states the quarantine
rules for `forwarder.c` itself.

## Host-OS independence

`dp_fwd` is host-OS-agnostic. It speaks only the PCIe AGNIC protocol, so the same NPU firmware
serves a Linux or a FreeBSD host unchanged.

## Driver bring-up phases

The Linux driver logs its bring-up as phases P0 to P5. BAR0 carries the GIU config_mem and the
NW_AGENT windows, BAR2 the barmap descriptor and the facility tail, BAR4 the host-to-target
doorbells.

| Phase | What the driver does | Representative log line |
|---|---|---|
| P0 | Bind the `11ab:7080` PF and print its identity. No reset and no FLR: an FLR takes down the live NPU firmware and the front-panel data plane, so `pci_reset_function()` and `pcie_flr()` appear nowhere in the driver. | `P0: Marvell AGNIC GIU-NIC PF 11ab:7080 (rev 00)` |
| P1 | Enable the device, map BAR0/2/4, set a 36-bit DMA mask, set busmaster. | `P1: BAR0 1024K, BAR2 16M, BAR4 16M mapped; 36-bit DMA; busmaster on` |
| P2 | Read the barmap descriptor the NPU publishes in the BAR2 tail, verify cookie `0xd0fac10d` and version `0x00000005`, resolve the five facility windows (CONTROL, MGMT_NETDEV, NW_AGENT, RPC, GIU), verify the CTRL cookie, read GIU config_mem status and the device MAC. MMIO reads only, with no writes to the NPU. | `P2: barmap OK @BAR2+0xffe000 (cookie 0xd0fac10d, version 0x00000005)`, then `attached (P0-P2). barmap + facilities OK.` |
| P3 | Publish the mgmt CMD and NOTIF rings into config_mem, assert `HOST_MGMT_READY`, wait for `DEV_MGMT_READY`, latch the h2t doorbell, then run a `CC_PF_MGMT_ECHO` round-trip and `CC_GET_CAPABILITIES`. | `P3: HOST_MGMT_READY set; awaiting DEV_MGMT_READY (needs dp_fwd/nmp on the NPU)`, then `P3: DEV_MGMT_READY — mgmt rings live` |
| P3b | Allocate the RX, buffer-pool and TX descriptor rings, then run the `CC_PF_INIT` … `CC_PF_ENABLE` sequence. | `P3b: GIU datapath configured (rings up; port DOWN pending ENABLE)`, then `P3b: CC_PF_ENABLE OK` |
| P4 | Set the trunk promiscuous, derive the per-unit front-panel MAC base, register the front-panel netdevs, arm the MSI-X t2h doorbells, start the RX reaper. A 10 ms poll is the fallback when MSI-X is unavailable. | `P4: port MAC base 02:81:xx:xx:xx:NN (host DMI)`, then `P4: 9 front-panel netdevs registered (port1..port9, born UP)` |
| P5 | Bring up `mvmgmt0`, the pcinet management netdev to the NPU. Off by default on Linux. | `P5: mvmgmt0 created (MAC 00:00:12:13:14:15)`, or `P5: MGMT_NETDEV facility absent; no mvmgmt0` |

P0 to P2 need nothing from the NPU; they read a contract that whatever firmware is already running
has published. P3 onward needs `dp_fwd` answering on the NPU. The driver probes at
`device_initcall`, seconds into boot, and can attach before the NPU has finished booting, so the
handshake is not a one-shot wait: `HOST_MGMT_READY` is asserted once and a worker retries every
second until `DEV_MGMT_READY` appears, in any host/NPU boot order. Once the mgmt rings are
published, the NPU latches their physical addresses whenever `dp_fwd` starts and there is no
unpublish path, since unpublishing would mean an FLR.

P5 is attempted in probe immediately after P3 publishes, so its lines can appear ahead of the P3b
and P4 lines, which wait on the NPU. Three module parameters gate the phases on Linux: `p3`
(default on) the mgmt channel, `dp` (default on) the datapath, and `mvmgmt` (default off) the
`mvmgmt0` netdev; `p3=1 dp=0` is a mgmt-channel diagnostic mode with no datapath. Two more change
behaviour inside a phase rather than gating one: `mac_src` selects the front-panel port MAC source
at P4, and `tx_hdr_mode` the TX hardware-header fill. All five are listed with their defaults in
[../host-driver-linux/README.md](../host-driver-linux/README.md#module-parameters).

The FreeBSD `if_agnic` driver follows the same sequence. Measured: its control plane on XGS 116
hardware. Unverified: its datapath end to end
([../host-driver-freebsd/README.md](../host-driver-freebsd/README.md)).

## Design origin and scope

The appliance was built to run one x86 firewall behind a smart switch fronted by an NPU. The
single-trunk, per-port-tag design is the vendor's, not this kit's: a fixed hardware contract rather
than a choice. This kit reimplements only the two ends it needs around that contract, the host
driver and a minimal NPU forwarder plus switch bring-up.

[BUILD.md](BUILD.md) builds both halves. [NPU-INSTALL.md](NPU-INSTALL.md) covers reaching the NPU
and replacing its rootfs.
