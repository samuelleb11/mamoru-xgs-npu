# host-driver-linux — Marvell AGNIC driver for Linux

A clean-room, out-of-tree kernel module: not an in-tree port, and not vendor code. It exposes the
XGS front ports as `port1..portN` netdevs by speaking the AGNIC GIU trunk protocol to the NPU.
Licence: GPL-2.0 OR MIT on the module sources and the `Makefile`, MIT on `tools/npc.sh`, with an
SPDX identifier in every file ([`../LICENSES/`](../LICENSES/)).

On the XGS 116, N is 9. `port1` to `port8` are the copper jacks and `port9` is the SFP cage
(`AGNIC_PPORT_COUNT`, `agnic_abi.h`). That constant is one of three, in three trees, that all change
on a board with a different front-port count; [../docs/HARDWARE.md](../docs/HARDWARE.md) names them
and everything else in this kit that is 116-specific.

## Build

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod mamoru_agnic.ko
dmesg | tail        # P0 bind (11ab:7080) -> P1 BARs/MSI-X -> P2 barmap read
ip link             # port1..portN once the NPU firmware is running
```

The build produces `mamoru_agnic.ko`. For a cross build, or to build against a kernel tree that is
not the running one, `make KDIR=/path/to/kernel-build-tree`.

`attached (P0-P2). barmap + facilities OK.` in `dmesg` is the bind succeeding: the ABI transcribed
from the FreeBSD `if_agnic` driver matched live hardware. Measured on one XGS 116, the reference
unit; the run is undated, and no other model has run this driver.
[../docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) carries the full P0 to P5 phase table with each
phase's log line. [Module parameters](#module-parameters) below lists all five, of which `p3` and
`dp`, both default on, and `mvmgmt`, default off, gate the later phases.

`ip link` shows `port1..portN` only once the NPU firmware is running. P0 to P2 read a contract the
NPU-resident PCIe-endpoint driver `mv_armada_ep` has already published, not anything from `dp_fwd`,
so the driver attaches before `dp_fwd` runs; with no valid barmap cookie in BAR2 the probe fails
with `-ENODEV` (`agnic_read_barmap()` in `agnic_main.c`). The front ports carry frames only once
`dp_fwd` is running on the NPU. Build and deploy that half first: `../npu-firmware/` and
[../docs/BUILD.md](../docs/BUILD.md).

## Build dependencies

Self-contained. The module includes only `<linux/*>` plus the two local headers, `agnic.h` and
`agnic_abi.h`. No firmware blob is loaded: `request_firmware()` is never called. No device tree is
required; the device is found by PCI enumeration alone. No cross-toolchain is required. Nothing
from the rest of the Mamoru firewall (Rust userspace, signing, Buildroot) is needed to build or run
it.

## Kernel target

The 6.12–6.19 KPI era. A current IPFire (6.x) is a close match. On a far-older kernel, expect small
adjustments in `agnic_txrx.c` and `agnic_pport.c`: netdev ops, DMA, and the MSI-X helpers.

## Module parameters

Five, all read at load time. `tx_hdr_mode` stays writable afterwards through sysfs (0644); the
other four are read-only (0444). The appliance builds the module in, and a built-in module takes no
`insmod` arguments, so the defaults below are what ships.

| Parameter | Default | Effect |
|---|---|---|
| `p3` | on | Bring up the P3 GIU management channel: mgmt rings, `HOST_MGMT_READY`, the `DEV_MGMT_READY` wait. `p3=0` stops the driver after P2. |
| `dp` | on | Bring up the P3b/P4 GIU datapath after the mgmt channel. `p3=1 dp=0` is a mgmt-channel diagnostic mode with no datapath. |
| `mvmgmt` | off | Register `mvmgmt0`, the host-to-NPU pcinet management netdev. Off by default so it never reaches `/sys/class/net` as an operator interface; `mvmgmt=1` opts in for host-to-NPU debugging. |
| `mac_src` | 0 | Which per-unit source seeds the front-panel port MACs: 0 = host DMI then the NPU device MAC, 1 = the NPU device MAC first, 2 = the fixed `02:81:00:00:00:0N`. |
| `tx_hdr_mode` | 3 | Fill for the 64-byte hardware header inside the TX prefix: 0 = zeros, 1 = replay the last RX header seen, 3 = `0xC0 + k`, the SFOS-exact header and the FreeBSD default. The current NPU treats that header as opaque. |

## Front-panel port addresses

Every `portN` netdev carries a locally administered unicast address of the shape
`02:81:<24 bits per unit>:<port number>` — nine distinct addresses on the 116, one per front port.
Octet 0 is `0x02`, locally administered: this project owns no OUI, so no address it assigns can
claim global uniqueness. Octet 1 is `0x81`, a mnemonic echo of the pport tag base with no wire
role, since the port tag rides the 66-byte prefix rather than the address. The last octet is the
port number, 1 to 9.

The middle 24 bits are derived per appliance at P4, in `agnic_port_mac_base()` (`agnic_main.c`),
from one of two sources.

- Host DMI. The first usable of `DMI_PRODUCT_UUID`, `DMI_PRODUCT_SERIAL` and `DMI_BOARD_SERIAL`,
  hashed with `crc32_le` and truncated to 24 bits. Reads nothing from the NPU. The UUID is tried
  first because SMBIOS requires it to be unique per unit and it is the field least often stubbed;
  a board with a placeholder serial would otherwise shadow a good UUID. Strings under four
  characters and the factory placeholders are rejected, case-insensitively and by prefix, because
  they repeat across units: `None`, `Default string`, `Not Specified`, `System Serial Number`,
  `To be filled...`, `O.E.M.`, `0123456789`, `Unknown`, `N/A`, and the all-zero and all-Fs UUIDs.
  `tools/probe.sh` prints these three fields, so which one will be used is visible before you
  build.
- The NPU-published device MAC. Its low three octets, read from GIU config_mem at
  `AGNIC_GIU_MAC_OFF` and re-sampled at P4 once `DEV_READY` is set. Probe runs at
  `device_initcall`, seconds into boot, and normally reads that field before the NPU has finished
  booting, so P4 is the first point at which it is worth reading.

`mac_src` picks the order between them. With no source readable at all, the middle bits are zero
and the nine addresses are `02:81:00:00:00:01` through `02:81:00:00:00:09` — byte-identical to the
fixed scheme this driver used before, so the derivation has no worse case than the addressing it
replaces. The derivation exists so that two appliances on one L2 segment do not present the same
nine addresses.

The chosen source is logged at P4:

```
P4: port MAC base 02:81:a3:1f:7c:00 (port in last octet) from host DMI [DEV_READY=1]
```

with `NPU device MAC` in place of `host DMI` when that source is used. When no per-unit source is
readable, or `mac_src=2` asks for the fixed address, the same line is a warning instead and says
`NOT unique to this appliance` — the one outcome that leaves this box sharing its addresses with
every other one is not reported as a success. `DEV_READY` on that line is the GIU status bit that
gates the device-MAC read, so a `0` there explains a device MAC that was never readable rather
than one that was read and rejected.

The address is not load-bearing for the trunk. RX demultiplexes on byte 0 of the 66-byte prefix
alone, the prefix carries no address, and the AGNIC ABI has no MAC-set command, so nothing the
driver sends the NPU depends on it. What it governs is host-side: which frames the host stack
treats as locally destined. `ip link set portN address ...` overrides it per interface
(`.ndo_set_mac_address = eth_mac_addr`, `agnic_pport.c`).

### Per-unit uniqueness of the NPU device MAC

Unverified: whether the device MAC the NPU publishes in GIU config_mem differs between appliances
at all. Nothing in `npu-firmware/` writes that field. Its sibling in the pcinet descriptor,
`PC_CFG_REMOTE_MAC` (`agnic_pcinet.c`), holds the NPU's own address and neither driver reads it;
that address is a firmware constant, read identically as `7c:5a:1c:bc:04:8b` on both measured
XGS 116 units ([../docs/NPU-INSTALL.md](../docs/NPU-INSTALL.md)). A source that is constant across
appliances would be worse than no source, because it looks like identity and is not, so host DMI is
preferred by default.

One comparison settles it. On two units, with the NPU running, load the driver with the device MAC
preferred and read both log lines:

```sh
sudo insmod mamoru_agnic.ko mac_src=1
dmesg | grep -E 'P2: device MAC|P4: port MAC base'
```

`P2: device MAC` prints only when `DEV_READY` was already set at probe, which is a reload against a
running NPU rather than a cold boot; `P4: port MAC base ... (NPU device MAC)` reports the low three
octets the derivation uses. Different values on the two units make the field per-unit and
`mac_src=1` a supportable preference. Identical values confirm it is a firmware constant, and host
DMI stays the only per-unit source.

### Divergence from the FreeBSD driver

The FreeBSD tree was not changed. `if_agnic` attaches each port with the fixed
`02:81:00:00:00:NN` and registers that address with stock NPU firmware over NW_AGENT `mac_set`
before admin-up ([../host-driver-freebsd/README.md](../host-driver-freebsd/README.md)). This driver
registers no address anywhere: it resolves the NW_AGENT window at P2 and sends nothing, and this
kit's `dp_fwd` replaced the Sophos NetAgent that serviced those messages. A known divergence
between the two trees, not a defect in either.

## Link state

With the generic NPU forwarder, the driver forces carrier on. The forwarder exposes no PHY state,
so `agnic_pport.c` asserts carrier when it registers each netdev and never demotes it on an
unverified read. Real per-port link state needs the NW_AGENT path. The FreeBSD tree implements
NW_AGENT (`../host-driver-freebsd/agnic_nwa.c`); it is not wired on Linux. The Linux driver
resolves the NW_AGENT facility window at P2 and does nothing further with it. Per-port link state
on Linux is Not attempted.

## NPU serial console

`tools/npc.sh` is an optional debugging helper. It pipes a command from stdin to the NPU's serial
console on the host's `/dev/ttyS2` and prints whatever the NPU replies within `W` seconds, default
3. It is not part of building or loading the module.
