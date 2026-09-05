# NPU install over the management link

The XGS's NPU is a CN9130 running its own Linux on its own eMMC, with its own bootloader in its
own SPI flash. A PCIe management link to it exists inside the chassis, comes up against factory
firmware, and carries SSH. File transfer over it is ordinary `ssh` and `scp`.

This document covers bringing that link up, obtaining a root shell on the NPU, and replacing the
NPU's root filesystem in the standby eMMC slot without stranding the box.

Every measurement in this document is on an XGS 116. For any other model, read
[HARDWARE.md](HARDWARE.md) first. The management link is the part most likely to be identical
across the product line, because it is PCIe and NPU-side software rather than the switch. Slot
layout and addresses must still be read off your own box.

## Status

| Claim | Status | Evidence |
|---|---|---|
| The management link comes up against a factory NPU | Measured | 2026-09-04, XGS 116 |
| Layer 3 to the NPU over it (RST + IPv6 ND `REACHABLE`) | Measured | 2026-09-04, XGS 116 |
| SSH into a factory NPU with the bundled key | Measured | 2026-07-29, a second XGS 116 |
| The NPU's storage and boot model (SPI-NOR + eMMC A/B, no autorollback) | Measured | 2026-09-04, XGS 116 |
| The U-Boot console is reachable and the box can be steered from it | Measured | 2026-09-04, XGS 116 |
| A rootfs replaced through this path, end to end | Not attempted | — |

Nobody has completed a full install this way. The steps that *reach* the NPU (sections 2 to 5, and
section 9's console access) have been run on real hardware; the steps that *write* to it
(sections 6 to 8) have not. The write steps are derived from the platform's measured behaviour and
ordered so that the reversible work happens first.

## Prerequisites

- Root on the x86 host, with this kit's host driver built for it ([BUILD.md](BUILD.md)).
- Host access to `/dev/ttyS2`, which is the NPU's serial console and its U-Boot console. Recovery
  from a boot that does not come up runs through it; that procedure is
  [section 9](#9-recovery-the-u-boot-console).
- For sections 6 to 8, a rootfs image of your own carrying `/boot/Image`,
  `/boot/cn9130-Sophos-XGS116.dtb`, an SSH daemon and your key.

---

## 1. Platform reference

Everything in this section was read off a factory XGS 116 on 2026-09-04.

Boot firmware is on SPI-NOR, not eMMC: a 4 MB `mx25l3205d` on the CN9130's SPI bus, two partitions.

```
/dev/mtd0   0x000000-0x3f0000   "U-Boot"       ~4 MB   TF-A + U-Boot
/dev/mtd1   0x3f0000-0x400000   "U-Boot-env"    64 KB  single copy, no redundancy
```

The OS is on eMMC, a 7.28 GiB part with four partitions.

```
mmcblk0p1   rootfs slot   U-Boot carries a bootcmd variant for it; contents not inspected
mmcblk0p2   rootfs slot   the measured unit ran from here (root=/dev/mmcblk0p2, ext4,
                          mounted ro, 1.5 G with ~260 M used)
mmcblk0p3   rootfs slot   the standby on the measured unit; contents not inspected
mmcblk0p4   ext4          /persistent (84 MB, ~76 MB free — too small to stage a rootfs)
```

The eMMC hardware boot areas (`mmcblk0boot0` / `boot1`, 4 MiB each) exist but play no part in this
boot chain, because the bootloader is in SPI-NOR. Nothing in this procedure writes them.

Each rootfs slot is self-contained: it carries its own `/boot/Image` and
`/boot/cn9130-Sophos-XGS116.dtb`, and U-Boot `ext4load`s them out of that partition and `booti`s
them. There is no separate boot partition and no shared kernel.

That is why the rootfs path has no brick vector. Stock U-Boot in SPI-NOR loads whatever kernel the
selected slot contains, and writing a rootfs slot never touches `/dev/mtd0`, so the bootloader you
are relying on to recover is never the thing you are modifying.

A/B is switched from outside, and there is no automatic rollback. The environment on the measured
unit carries ready-made variants:

```
bootcmd_emmc1 / bootargs_emmc1     bootcmd_emmc2 / bootargs_emmc2     bootcmd_emmc3 / bootargs_emmc3
bootcmd  = (a copy of bootcmd_emmc2)     bootargs = (a copy of bootargs_emmc2)
bootdelay=3
```

and no `bootcount`, no `bootlimit`, no `altbootcmd`, no `upgrade_available` — the whole environment
was grepped for them, zero hits. U-Boot boots what `bootcmd` says, forever. A slot that panics is
rebooted into the same panic indefinitely. The A/B switch is an explicit `fw_setenv`, and so is the
rollback: the decision to roll back is yours, and it needs an out-of-band way to reach the box,
which is what section 9 is for.

> Do not write `/dev/mtd0`. It is single-copy with no redundant environment, and it is the one
> write on this board that can leave a CN9130 that does not boot at all. Nothing in this document
> needs to touch it; updating the boot chain is a separate job with a different risk profile, and
> the rootfs path deliberately stays away from it.

---

## 2. Host-side `mvmgmt0` bring-up

`mvmgmt0` is a PCIe management netdev between the x86 host and the NPU. Both ends have one: the
host end created by this kit's driver, the NPU end by the NPU's own kernel.

Load the host driver with the datapath half switched off.

```sh
sudo insmod mamoru_agnic.ko mvmgmt=1 p3=0 dp=0        # Linux
```

- `mvmgmt=1` creates `mvmgmt0`. It is off by default.
- `p3=0 dp=0` leaves the GIU management channel and the datapath down. Both talk to `dp_fwd`, and a
  factory NPU has nothing there to answer them. Left on against factory firmware, the driver sits
  in `P3: still awaiting the NPU dp_fwd (~Ns)...` or repeatedly logs `P3: bad dev_use_size 0x0`.
  Noisy rather than harmful.

On FreeBSD, `if_agnic` brings the management link up at attach unconditionally, self-guarding when
the facility is absent, so `kldload ./if_agnic.ko` is the whole step.

The driver then walks P0 to P5; the phases are defined in [ARCHITECTURE.md](ARCHITECTURE.md).

```
P0: Marvell AGNIC GIU-NIC PF 11ab:7080 (rev 00)
P1: BAR0 1024K, BAR2 16M, BAR4 16M mapped; 36-bit DMA; busmaster on
P2: barmap OK @BAR2+0xffe000 (cookie 0xd0fac10d, version 0x00000005)
P5: pcinet MGMT_NETDEV @BAR2+0xefd000
P5: pcinet ready pattern seen; acking
P5: mvmgmt0 created (MAC 00:00:12:13:14:15). `ip link set mvmgmt0 up`, then reach the NPU over it.
```

The MAC on `mvmgmt0` is the host end's, assigned by the driver. It is not a property of your box
and not the address you connect to. `P5: MGMT_NETDEV facility absent` in place of those lines means
the NPU has not published the facility, most likely because it is still booting.

### Interface bring-up

Loading the driver is not the whole step. The interface must then be brought up.

```sh
sudo ip link set mvmgmt0 up
```

`mvmgmt0` is created administratively DOWN — `state DOWN`, `qdisc noop` — and the link handshake
does not complete until it is up. The driver's own log line says so: "`ip link set mvmgmt0 up`,
then reach the NPU over it". Running only `ip link show mvmgmt0`, reading `DOWN` and concluding the
link does not work is a false negative; it inverted a result on 2026-09-04. Bringing the interface
up is host-side only and writes nothing to the NPU.

It should then read:

```
3: mvmgmt0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP
    inet6 fe80::200:12ff:fe13:1415/64 scope link
P5: mvmgmt0 link ESTABLISHED
```

Check the packet counters, not just the carrier flag. `carrier 1` says the driver believes the link
is up; `/sys/class/net/mvmgmt0/statistics/{rx,tx}_packets` moving says bytes crossed to the NPU and
came back. The measured unit read 10 packets each way on a bare bring-up.

If `mvmgmt0` never appears at all, the NPU may still be booting. It boots independently of the x86
host, and a cold NPU boot takes minutes.

---

## 3. The NPU's link-local address

The NPU end has a static IPv6 link-local, configured in the NPU's own `/etc/network/interfaces`.
It is not autoconfigured and does not come from a router advertisement. Discover it rather than
assume it.

```sh
ping -6 -c2 ff02::1%mvmgmt0            # Linux;  ping6 -c2 ff02::1%mvmgmt0 on FreeBSD
ip -6 neigh show dev mvmgmt0
  fe80::7e5a:1cff:febc:48b lladdr 7c:5a:1c:bc:04:8b ref 1 used 0/0/0 probes 1 REACHABLE
```

IPv6 neighbour discovery reaches `REACHABLE` only after a bidirectional exchange: the NPU answered
a solicitation with its link-layer address. A one-way transmit cannot produce that state.

Both measured XGS 116s read `fe80::7e5a:1cff:febc:48b`, the EUI-64 of MAC `7c:5a:1c:bc:04:8b`. That
is two units, measured separately, one by pinging it and one by reading its
`/etc/network/interfaces`, so a match on your unit is a useful cross-check. It is still two units
of one model on one firmware version: discover yours, do not paste theirs.

Neither end runs anything browsable. The measured unit answered a TCP connect with an RST
(`Connection refused`) and did not answer TFTP at all. The RST proves an IP stack is there; the
absence of a TFTP server confirms there is no TFTP anywhere in this story. See
[Dead ends: U-Boot networking and `/dev/mtd0`](#dead-ends-u-boot-networking-and-devmtd0).

---

## 4. SSH access

The factory NPU runs OpenSSH as root (`sshd: /usr/sbin/sshd [listener]`) from a Buildroot 2018.11
userland on kernel 4.14. It authenticates root by key, and the key it trusts ships in Sophos's
publicly downloadable firmware ISO. This kit bundles that key at
`npu-firmware/deploy/keys/mvmgt.x86`;
[`npu-firmware/deploy/keys/README.md`](../npu-firmware/deploy/keys/README.md) states what it is and
why it is in this repository.

```sh
NPU='fe80::YOUR-NPU-EUI64%mvmgmt0'          # <-- yours, from step 3 (ours: fe80::7e5a:1cff:febc:48b)
KEY=npu-firmware/deploy/keys/mvmgt.x86
chmod 600 "$KEY"

ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    "root@[$NPU]" 'uname -a'
  Linux marvell 4.14.207-10.22.03 ... aarch64 GNU/Linux
```

`StrictHostKeyChecking=no` and `UserKnownHostsFile=/dev/null` are required because the NPU
regenerates host keys when its rootfs changes, which is what sections 6 to 8 do.

That banner is the confirmation you are on vendor firmware: hostname `marvell`, kernel 4.14.207,
and an `/opt/sophos` tree beside it (`ls -d /opt/sophos`). All three stop being true once your own
rootfs is the one running.

Client algorithm policy: the bundled key is RSA-2048 and the server is from 2018, so a modern
OpenSSH client refusing `ssh-rsa` (SHA-1) is a plausible failure. Measured: 49 scripted `ssh`/`scp`
invocations from a FreeBSD host needed no algorithm options at all. Unverified for other clients;
if yours refuses the key, add `-o PubkeyAcceptedAlgorithms=+ssh-rsa` (older clients:
`PubkeyAcceptedKeyTypes`).

Once in, add your own key to `/root/.ssh/authorized_keys` and stop depending on Sophos's. That
requires remounting root read-write (`mount -o remount,rw /`), and the change does not survive
replacing the rootfs. The durable version is putting your own key into the rootfs image you build.

`npu-firmware/deploy/relay-deploy.sh` is this same connection wrapped for pushing `dp_fwd` and its
config. It defaults to the bundled key and takes `NPU_LL`, `NPU_KEY` and `NPU_DST` from the
environment.

---

## 5. Pre-write checks

Four read-only checks, all answered from the SSH session, all before anything is written. Section 7
writes the standby slot; identifying the running slot wrongly destroys the system you are running.

```sh
# a) Which slot is RUNNING? Never write this one.
cat /proc/cmdline                       # -> ... root=/dev/mmcblk0p2 ...
#    The measured unit ran from p2, making p3 its standby. YOURS MAY DIFFER: the environment
#    carries ready-made emmc1/2/3 variants, so a unit that has taken a field update can arrive
#    running from p3, making p2 the standby. Read it; do not assume.

# b) What is in the standby slot?
mkdir -p /tmp/standby && mount -o ro /dev/mmcblk0p3 /tmp/standby && ls /tmp/standby/boot
#    A populated slot has /boot/Image + /boot/cn9130-Sophos-XGS116.dtb. An empty or absent one
#    is fine; it means you have one vendor copy, not two (see section 6).

# c) Does this box have the tools the procedure needs?
for t in tar mke2fs fsck.ext4 fw_printenv fw_setenv md5sum; do
        command -v "$t" >/dev/null && echo "have $t" || echo "MISSING $t"
done
#    Only fw_printenv was ever exercised on the measured unit. Anything MISSING must be worked
#    around before you start, not discovered halfway through: a busybox without mke2fs means
#    you reuse the existing filesystem rather than recreating it.

# d) How much room is there?
df -h                                   # the measured slots were ~1.5 G with ~260 M used
```

---

## 6. Standby-slot backup

> Overwriting the standby slot is **irreversible**: it holds a vendor rootfs, and there is no known
> published source for the NPU's root filesystem, Sophos's downloadable firmware ISO being the x86
> side. It takes you from two vendor copies to one, and that remaining copy is your entire fallback
> once you flip the boot.

Take the backup before anything else. It streams straight to the host over the management link,
with no staging on the NPU, whose `/persistent` is far too small for it.

```sh
# on the HOST, with the standby mounted read-only on the NPU as in step 5(b)
ssh -i "$KEY" "root@[$NPU]" 'tar -C /tmp/standby -cf - .' > npu-slot3-vendor.tar
ls -l npu-slot3-vendor.tar          # sanity-check the size against `df` on the NPU
```

Keep the backup off the appliance, with notes on which slot it came from. If you have a second XGS,
take the same backup from it before touching that one: the two units' standby slots can be
different vendor versions.

From here on, recovery from a failed boot runs through the NPU console; see
[section 9](#9-recovery-the-u-boot-console).

---

## 7. Standby-slot write

Never format or unpack over the running root. Step 5(a) is what identifies it.

```sh
# on the NPU, standby unmounted
umount /tmp/standby
mke2fs -t ext4 -L npu-root /dev/mmcblk0p3       # skip if step 5(c) reported MISSING mke2fs;
                                                # in that case mount it and clear it instead
mount /dev/mmcblk0p3 /tmp/standby
```

Then stream the rootfs in from the host, again with no staging.

```sh
# on the HOST
ssh -i "$KEY" "root@[$NPU]" 'tar -C /tmp/standby -xf -' < my-npu-rootfs.tar
```

Your rootfs must contain `/boot/Image` and `/boot/cn9130-Sophos-XGS116.dtb`, because that is what
U-Boot loads out of the slot. A rootfs without them is a slot that will not boot.

> Keep `sshd` and a key in the image you install. The transport you arrived on is a property of the
> vendor's rootfs, not of the hardware: an early minimal NPU rootfs built for this kit had no `sshd`
> at all, and every subsequent transfer to it had to go over the serial console. Put your own
> `authorized_keys` in the image and leave an SSH daemon running in it.

---

## 8. Verification and boot switch

Until `bootcmd` changes, nothing done so far affects the next boot.

```sh
# on the NPU
umount /tmp/standby
fsck.ext4 -fn /dev/mmcblk0p3                   # must come back clean
mount -o ro /dev/mmcblk0p3 /tmp/standby
ls -l /tmp/standby/boot/Image /tmp/standby/boot/cn9130-Sophos-XGS116.dtb
md5sum /tmp/standby/boot/Image                 # compare against the file you built
umount /tmp/standby
```

Only once all of that passes, point the boot at the slot. Record the current values first: that
record is your rollback.

```sh
fw_printenv bootcmd bootargs                   # record the CURRENT values first
fw_setenv bootcmd  "$(fw_printenv -n bootcmd_emmc3)"
fw_setenv bootargs "$(fw_printenv -n bootargs_emmc3)"
fw_printenv bootcmd bootargs                   # read it back; confirm it now says p3
```

`bootargs` carries `root=/dev/mmcblk0pN` and `bootcmd` carries the matching `ext4load mmc 0:N`.
They travel together: setting one and not the other gives a kernel from one slot running against a
root filesystem from the other.

Then reboot the NPU, in this order.

1. Attach to the console. The NPU console is the x86 host's `ttyS2`:
   `stty -F /dev/ttyS2 115200 raw -echo`, then read it. See [PROVENANCE.md](PROVENANCE.md).
2. Reset the NPU, from the SSH session that the reset drops.

```sh
# on the NPU, over the SSH session you are about to lose
echo 1 > /proc/sys/kernel/sysrq; sync; echo b > /proc/sysrq-trigger
```

There is no automatic rollback: a slot that panics reboots into the same panic indefinitely, and
the console is the only place this is visible. The SSH session does not survive the reboot it
triggers; the console is what carries you across the gap.

Expect to lose `mvmgmt0` across the reboot. The host-to-NPU handshake latches host ring addresses
once per NPU boot, so a host driver attached to the old NPU boot is desynced from the new one.
Reload the host driver, or reboot the host, to get the management link back.

---

## 9. Recovery: the U-Boot console

U-Boot's own console is on the same `ttyS2` line, and `bootdelay=3` provides an interrupt window.
Recovery needs no physical access beyond the host you already have.

Stream keystrokes continuously while the NPU resets, to interrupt `bootdelay`. On the measured
unit, streaming spaces at it across a reset caught the prompt.

```
Marvell>> printenv bootcmd bootargs
```

From there, put `bootcmd` and `bootargs` back to the values recorded in step 8, and boot. This path
works whether or not the slot you flipped to has a working userland, because U-Boot is in SPI-NOR
and was never touched.

Exercised at this prompt: `printenv`, `mii`, and `boot`, the last to return the box to its vendor
OS. Not run on this board: `setenv` and `saveenv`. They are ordinary U-Boot commands and the
obvious way to do the rollback, but they are unverified here. The U-Boot environment is a single
copy with no redundancy: losing power in the middle of a `saveenv` is the one way to make a bad
situation worse.

If the flip has not been made permanent, the safest recovery is not to save at all. Boot the good
slot for this boot only, get back into Linux, and do the `fw_setenv` from a real shell.

---

## Dead ends: U-Boot networking and `/dev/mtd0`

### U-Boot networking

U-Boot cannot TFTP, despite `tftpboot`, `dhcp`, `ping`, `bootp` and `nfs` all being present in the
command table.

```
Marvell>> printenv ethact
## Error: "ethact" not defined
Marvell>> net list
Unknown command 'net' - try 'help'
Marvell>> mii device
MII devices: 'mdio@12a200'
Current device: 'mdio@12a200'
```

An MDIO bus with no MAC driver bound to it. There is no network in this bootloader. The management
link of section 2 exists only once Linux is running on both ends.

### `/dev/mtd0`

`/dev/mtd0` is not part of this procedure, and it is the one write that can take the box away from
you. Section 1 states the mechanism.

---

## Unverified

- A rootfs installed through this path end to end: not attempted. Sections 2 to 5 and section 9's
  console access are measured; the writes in sections 6 to 8 are not.
- Whether your NPU has `mke2fs`, `tar`, `fsck.ext4` and `fw_setenv`. Step 5(c) is a real check, not
  a formality; only `fw_printenv` was ever run.
- `setenv` and `saveenv` at the U-Boot prompt: not run on this board.
- Whether the NPU link-local is the same across the product line. It matched on both XGS 116 units,
  on the same vendor firmware version. That is not a guarantee; step 3 discovers it.
- Whether a modern OpenSSH client needs legacy algorithm options. It did not from FreeBSD, across
  49 invocations; yours might.

## Reporting

Report a completed install, or a failure this document did not predict, per
[CONTRIBUTING.md](../CONTRIBUTING.md): the board model, the slot you were running from, and what
the NPU's busybox provided.
