# Architecture — how a packet gets from a front jack to your firewall

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
   your firewall stack (nftables on IPFire / pf on pfSense)
```

## The key facts

- **Forwarding is host-side, not offloaded.** Every front-panel packet DMAs
  across PCIe to the host, your firewall forwards/filters it, and it DMAs back.
  The NPU is a **switch + PHY + port-multiplexer conduit**, not the policy engine.
  So the host driver is the *whole* datapath, not just a control channel.

- **One trunk, many ports.** The 8 (116) front ports plus the SFP cage are
  multiplexed onto a single AGNIC GIU trunk. A 66-byte prefix on each frame
  carries the source/destination port (`byte0 = 0x81 + port_index`); the host
  driver adds it on TX and strips+demuxes it on RX, so one trunk becomes
  `port1..portN` interfaces.

- **Two planes over the same trunk.** A management plane (`CC_PF_*` commands:
  init, enable, promisc, capabilities) rides mgmt rings; the datapath rides RX/TX
  rings with a buffer pool the device DMAs into. Both are described in the ABI
  headers (`agnic_abi.h` / `agnic_barmap.h` etc.).

- **`dp_fwd` is host-OS-agnostic.** It speaks only the PCIe AGNIC protocol, so the
  same NPU firmware serves a Linux or a FreeBSD host unchanged.

- **The switch is programmed NPU-side only.** No host-side MDIO/SMI/DSA code — the
  88E6193X is configured entirely by `switch-init/` running on the NPU. A host
  driver never touches the switch.

## Why this shape

The appliance was built to run one x86 firewall behind a smart switch fronted by
an NPU. The single-trunk + per-port-tag design is the vendor's; this kit
reimplements the two ends we need (host driver + minimal NPU forwarder/switch
bring-up) around that fixed hardware contract.
