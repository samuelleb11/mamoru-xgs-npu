# host-driver-freebsd — Marvell AGNIC driver (FreeBSD / pfSense / OPNsense)

Native FreeBSD `if_agnic` driver (newbus + `if(9)`, deliberately not iflib — the
rings are a firmware-fixed ABI). License: **BSD-2-Clause** (SPDX in every file).
The Linux driver was transcribed from this one; the ABI headers here
(`agnic_barmap.h`, `agnic_ctrl.h`, `agnic_giu.h`) are the OS-independent contract.

## Status — read before you build

- **Control plane: proven on XGS 116 hardware** — PCI bind, BAR0/2/4 map, 36-bit
  `bus_dma`, MSI-X (table lives in BAR0, maps cleanly), barmap read-back
  (cookie/version correct), two-way mailbox, and the `CC_PF_MGMT_ECHO` round-trip.
- **Datapath (RX/TX/bpool + per-port demux): written, not yet verified end-to-end.**
  This is the finishing work — the `CC_PF_INIT…ENABLE` sequence, the buffer-pool
  refill, MSI-X kick + poll fallback, and the 66-byte pport tag demux. Cross-check
  against the Linux `agnic_txrx.c`/`agnic_pport.c`, which are traffic-proven.
- `agnic_nwa.c` (NW_AGENT) implements the PHY-control mailbox for **stock NPU
  firmware** (real per-port link). Keep it if your NPU runs stock firmware.

## Build

```sh
make                    # against /usr/src matching YOUR base
sudo kldload ./if_agnic.ko
echo 'if_agnic_load="YES"' >> /boot/loader.conf   # persist
```

**FreeBSD base matters:** pfSense CE 2.7.x is FreeBSD 14.0; pfSense Plus 24/25 and
OPNsense 26 are FreeBSD 15. This targets 15.1; the `if_t`/`bus_dma` APIs shifted
14→15, so build against your exact source and expect minor adjustments on 14.

The `portN` interfaces attach as ordinary Ethernet ifnets you assign/bridge in the
firewall GUI. `mvmgmt0` (`agnic_pcinet.c`) is optional and off by default — skip
it for a firewall deployment.
