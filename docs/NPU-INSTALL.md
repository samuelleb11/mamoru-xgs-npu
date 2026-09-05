# Installing NPU firmware over the management link (`mvmgmt0` + SSH)

The XGS's NPU is a CN9130 running its own Linux on its own eMMC, with its own bootloader in
its own SPI flash. To put anything of yours on it you have to reach it — and the good news is
that **there is a network link to it inside the chassis, it comes up against factory firmware,
and it carries SSH.** No TFTP, no bootloader gymnastics, no bit-banging files over a serial
console in base64.

This document is how a stranger with an XGS 116 brings that link up, gets a root shell on the
NPU, and replaces the NPU's root filesystem without stranding the box.

> **Read `HARDWARE.md` first if you have anything other than a 116.** Every measurement below
> is on a 116. The management link is the part most likely to be identical across the line —
> it is PCIe and NPU-side software, not the switch — but the slot layout and the addresses are
> things you must read off your own box regardless.

## Status — what is measured and what is not

| | |
|---|---|
| The management link comes up against a **factory** NPU | **Measured** — 2026-09-04, XGS 116 |
| Layer 3 to the NPU works over it (RST + IPv6 ND `REACHABLE`) | **Measured** — 2026-09-04 |
| SSH into a factory NPU with the bundled key | **Measured** — 2026-07-29, a second XGS 116 |
| The NPU's storage/boot model (SPI-NOR + eMMC A/B, no autorollback) | **Measured** — 2026-09-04 |
| The U-Boot console is reachable and the box can be steered from it | **Measured** — 2026-09-04 |
| **A rootfs actually replaced through this path, end to end** | **NOT DONE BY ANYONE YET** |

**Nobody has yet completed a full install this way.** Every step of *reaching* the NPU below has
been run on real hardware; the steps that *write* to it have not. They are written from the
platform's measured behaviour, and they are ordered so that the reversible things happen first.
Treat the write half as a careful plan, not as a trodden path, and read
"[If it does not come up](#9-if-it-does-not-come-up-the-u-boot-console)" before you start rather
than after.

---

## 1. The platform, in one page

Everything in this section was read off a factory XGS 116 on **2026-09-04**.

**Boot firmware is on SPI-NOR, not eMMC.** A 4 MB `mx25l3205d` on the CN9130's SPI bus, two
partitions:

```
/dev/mtd0   0x000000-0x3f0000   "U-Boot"       ~4 MB   TF-A + U-Boot
/dev/mtd1   0x3f0000-0x400000   "U-Boot-env"    64 KB  single copy, no redundancy
```

**The OS is on eMMC**, a 7.28 GiB part with four partitions:

```
mmcblk0p1   rootfs slot   U-Boot has a bootcmd variant for it; we did not look inside
mmcblk0p2   rootfs slot   <- ours ran from here (root=/dev/mmcblk0p2, ext4, mounted ro,
                             1.5 G with ~260 M used)
mmcblk0p3   rootfs slot   <- the standby on our unit; we did not look inside it either
mmcblk0p4   ext4          /persistent (84 MB, ~76 MB free — too small to stage a rootfs)
```

The eMMC hardware boot areas (`mmcblk0boot0` / `boot1`, 4 MiB each) exist but play no part in
this boot chain — the bootloader is in SPI-NOR — and nothing here writes them.

Each rootfs slot is **self-contained**: it carries its own `/boot/Image` and
`/boot/cn9130-Sophos-XGS116.dtb`, and U-Boot `ext4load`s them out of that partition and
`booti`s. There is no separate boot partition and no shared kernel.

**This is why the rootfs path has no brick vector.** Stock U-Boot in SPI-NOR loads whatever
kernel the selected slot contains. Writing a rootfs slot never touches `/dev/mtd0`, so the
bootloader you are relying on to recover is never the thing you are modifying.

**A/B is switched from outside, and there is no automatic rollback.** The environment on our
unit carries ready-made variants:

```
bootcmd_emmc1 / bootargs_emmc1     bootcmd_emmc2 / bootargs_emmc2     bootcmd_emmc3 / bootargs_emmc3
bootcmd  = (a copy of bootcmd_emmc2)     bootargs = (a copy of bootargs_emmc2)
bootdelay=3
```

and **no `bootcount`, no `bootlimit`, no `altbootcmd`, no `upgrade_available`** — we grepped
the whole environment for them and got zero hits. U-Boot boots what `bootcmd` says, forever. A
slot that panics is rebooted into the same panic indefinitely. **The switch is an explicit
`fw_setenv`, and so is the rollback** — which means the decision to roll back is yours, not the
NPU's, and you need a way to reach it. That is what section 9 is for.

**Do not write `/dev/mtd0`.** Nothing in this document needs to. It is single-copy with no
redundant environment, and it is the one write on this board that can leave you with a CN9130
that does not boot at all. Updating the boot chain is a different job with a different risk
profile; the rootfs path deliberately stays away from it.

---

## 2. Bring up `mvmgmt0` on the host

`mvmgmt0` is a PCIe management netdev between the x86 host and the NPU. Both ends have one; the
host end is created by this kit's driver, the NPU end by the NPU's own kernel.

**Load the host driver with the datapath half switched off:**

```sh
sudo insmod mamoru_agnic.ko mvmgmt=1 p3=0 dp=0        # Linux
```

- `mvmgmt=1` — create `mvmgmt0`. It is **off by default**.
- `p3=0 dp=0` — do **not** bring up the GIU management channel or the datapath. Those talk to
  `dp_fwd`, and a factory NPU has nothing there to answer them. Left on against factory firmware
  the driver sits in `P3: still awaiting the NPU dp_fwd (~Ns)...`, or logs `P3: bad dev_use_size
  0x0` repeatedly. Noisy rather than harmful, but there is no reason to drive GIU state into
  firmware you do not own while you are still just trying to talk to the thing.

On **FreeBSD**, `if_agnic` brings the management link up at attach unconditionally (it
self-guards if the facility is absent), so `kldload ./if_agnic.ko` is the whole step.

You should see the driver walk P0 → P5:

```
P0: Marvell AGNIC GIU-NIC PF 11ab:7080 (rev 00)
P1: BAR0 1024K, BAR2 16M, BAR4 16M mapped; 36-bit DMA; busmaster on
P2: barmap OK @BAR2+0xffe000 (cookie 0xd0fac10d, version 0x00000005)
P5: pcinet MGMT_NETDEV @BAR2+0xefd000
P5: pcinet ready pattern seen; acking
P5: mvmgmt0 created (MAC 00:00:12:13:14:15). `ip link set mvmgmt0 up`, then reach the NPU over it.
```

The MAC on `mvmgmt0` is the host end's and is assigned by the driver — it is not a property of
your box, and it is not the address you will connect to. `P5: MGMT_NETDEV facility absent` here
instead means the NPU has not published the facility: it is most likely still booting.

### Then bring the interface up. This step is not optional and it is easy to miss.

```sh
sudo ip link set mvmgmt0 up
```

**`mvmgmt0` is created administratively DOWN** — `state DOWN`, `qdisc noop` — and the link
handshake does not complete until you bring it up. The driver's own log line says so
(*"`ip link set mvmgmt0 up`, then reach the NPU over it"*), and it is still the single easiest
way to get a false negative here: run only `ip link show mvmgmt0`, read `DOWN`, and conclude
the link does not work. **It inverted a result for us on 2026-09-04.** The step is host-side
only — it writes nothing to the NPU.

Now it should read:

```
3: mvmgmt0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP
    inet6 fe80::200:12ff:fe13:1415/64 scope link
P5: mvmgmt0 link ESTABLISHED
```

**Check the packet counters, not just the carrier flag.** `carrier 1` says the driver believes
the link is up; `/sys/class/net/mvmgmt0/statistics/{rx,tx}_packets` moving says bytes actually
crossed to the NPU and came back. Ours read 10 each way on a bare bring-up.

If `mvmgmt0` never appears at all, the NPU may still be booting — it boots independently of the
x86 host and a cold NPU boot takes minutes. Give it time before concluding anything.

---

## 3. Find your NPU's address

The NPU end has a **static** IPv6 link-local, configured in the NPU's own
`/etc/network/interfaces` (not autoconfigured, not from a router advertisement). Discover it
rather than assume it:

```sh
ping -6 -c2 ff02::1%mvmgmt0            # Linux;  ping6 -c2 ff02::1%mvmgmt0 on FreeBSD
ip -6 neigh show dev mvmgmt0
  fe80::7e5a:1cff:febc:48b lladdr 7c:5a:1c:bc:04:8b ref 1 used 0/0/0 probes 1 REACHABLE
```

**`REACHABLE` is the result you want, and it is a strong one.** IPv6 neighbour discovery only
reaches that state after a bidirectional exchange — the NPU answered a solicitation with its
link-layer address. It cannot be produced by a one-way transmit.

> **Both of our XGS 116s read `fe80::7e5a:1cff:febc:48b` (EUI-64 of MAC `7c:5a:1c:bc:04:8b`).**
> That is two units, measured separately — one by pinging it, one by reading its
> `/etc/network/interfaces` — so if yours matches, that is a useful cross-check. It is still
> two units of one model on one firmware version, so **discover yours; do not paste ours.**

Neither end runs anything you can browse. Ours answered a TCP connect with an RST
(`Connection refused`) and did not answer TFTP at all — which is exactly right: the RST proves
an IP stack is there, and the absence of a TFTP server is a reminder that **there is no TFTP
anywhere in this story.** See "[Two things that are not paths](#two-things-that-are-not-paths)".

---

## 4. SSH in

The factory NPU runs OpenSSH as root (`sshd: /usr/sbin/sshd [listener]`, from a Buildroot
2018.11 userland on kernel 4.14). It authenticates root by key, and the key it trusts ships in
Sophos's publicly downloadable firmware ISO. **This kit bundles it** —
`npu-firmware/deploy/keys/mvmgt.x86`. Read
[`npu-firmware/deploy/keys/README.md`](../npu-firmware/deploy/keys/README.md) for what that key
is, why it is not ours, and why publishing it gives nobody anything they did not already have.

```sh
NPU='fe80::YOUR-NPU-EUI64%mvmgmt0'          # <-- yours, from step 3 (ours: fe80::7e5a:1cff:febc:48b)
KEY=npu-firmware/deploy/keys/mvmgt.x86
chmod 600 "$KEY"

ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    "root@[$NPU]" 'uname -a'
  Linux marvell 4.14.207-10.22.03 ... aarch64 GNU/Linux
```

`StrictHostKeyChecking=no` + `UserKnownHostsFile=/dev/null` are there because the NPU
regenerates host keys when its rootfs changes, which is precisely what you are about to do.

**That banner is how you confirm you are on vendor firmware**: hostname `marvell`, kernel
4.14.207, and an `/opt/sophos` tree beside it (`ls -d /opt/sophos`). Those are the things that
stop being true once your own rootfs is the one running.

**On client algorithm policy:** the key is RSA-2048 and the server is from 2018, so a modern
OpenSSH client refusing `ssh-rsa` (SHA-1) is a plausible failure. We have not hit it — 49
scripted `ssh`/`scp` invocations from a FreeBSD host needed no algorithm options at all — but
if your client refuses the key, add `-o PubkeyAcceptedAlgorithms=+ssh-rsa` (older clients:
`PubkeyAcceptedKeyTypes`).

**Once you are in, add your own key** to `/root/.ssh/authorized_keys` and stop depending on
Sophos's. You will have to remount root read-write to do it (`mount -o remount,rw /`), and it
will not survive replacing the rootfs — so the durable version of this is to put your own key
into the rootfs image you are about to build.

`npu-firmware/deploy/relay-deploy.sh` is this same connection wrapped up for pushing `dp_fwd`
and its config. It defaults to the bundled key and takes `NPU_LL` / `NPU_KEY` / `NPU_DST` from
the environment.

---

## 5. Look before you write

Four questions, all read-only, all answered from the SSH session. **Answer all four before you
write anything.**

```sh
# a) Which slot am I RUNNING from? Never write this one.
cat /proc/cmdline                       # -> ... root=/dev/mmcblk0p2 ...
#    Ours ran from p2, so p3 was the standby. YOURS MAY DIFFER — the environment carries
#    ready-made emmc1/2/3 variants, so a unit that has taken a field update can perfectly
#    well arrive running from p3, making p2 the standby. Read it; do not assume.

# b) What is actually IN the standby slot?
mkdir -p /tmp/standby && mount -o ro /dev/mmcblk0p3 /tmp/standby && ls /tmp/standby/boot
#    A populated slot has /boot/Image + /boot/cn9130-Sophos-XGS116.dtb. An empty or absent
#    one is fine too — it just means you have one vendor copy, not two (see section 6).

# c) Does this box have the tools the procedure needs?
for t in tar mke2fs fsck.ext4 fw_printenv fw_setenv md5sum; do
        command -v "$t" >/dev/null && echo "have $t" || echo "MISSING $t"
done
#    We only ever exercised fw_printenv on ours. Anything MISSING here you must work around
#    before you start, not discover halfway through — and a busybox that is missing mke2fs
#    means you reuse the existing filesystem rather than recreating it.

# d) How much room is there?
df -h                                   # our slots were ~1.5 G with ~260 M used
```

---

## 6. Back up the slot you are about to destroy — this is the irreversible step

**The standby slot holds a vendor rootfs, and you should assume it is irreplaceable.** We know of
no published source for the NPU's root filesystem — Sophos's downloadable firmware ISO is the x86
side. Once you overwrite that slot it is gone permanently, and you are down from two vendor copies
to one. When you later flip the boot to your own slot, that remaining copy is your entire
fallback.

**So take the backup, and take it before anything else.** It streams straight to the host over
the link you just brought up — no staging on the NPU, whose `/persistent` is far too small
anyway:

```sh
# on the HOST, with the standby mounted read-only on the NPU as in step 5(b)
ssh -i "$KEY" "root@[$NPU]" 'tar -C /tmp/standby -cf - .' > npu-slot3-vendor.tar
ls -l npu-slot3-vendor.tar          # sanity-check the size against `df` on the NPU
```

Keep it off the appliance, alongside your notes on which slot it came from. If you have a
second XGS, take the same backup from it before you touch that one too — the two units'
standby slots can be different vendor versions.

---

## 7. Write the standby slot

**Never format or unpack over the running root.** You verified which one that is in step 5(a).

```sh
# on the NPU, standby unmounted
umount /tmp/standby
mke2fs -t ext4 -L npu-root /dev/mmcblk0p3       # skip if step 5(c) said MISSING mke2fs;
                                                # in that case mount it and clear it instead
mount /dev/mmcblk0p3 /tmp/standby
```

Then stream your rootfs in from the host — again with no staging:

```sh
# on the HOST
ssh -i "$KEY" "root@[$NPU]" 'tar -C /tmp/standby -xf -' < my-npu-rootfs.tar
```

Your rootfs **must** contain `/boot/Image` and `/boot/cn9130-Sophos-XGS116.dtb`, because that
is what U-Boot loads out of the slot. A rootfs without them is a slot that will not boot.

> **Keep `sshd` and a key in the image you install.** The transport you arrived on is a property
> of the *vendor's* rootfs, not of the hardware — replace the rootfs with something minimal and
> you can lose the way back in. We have done exactly that to ourselves: an early minimal NPU
> rootfs of ours had no `sshd` at all, and every subsequent transfer had to go over the serial
> console. Put your own `authorized_keys` in the image and leave an SSH daemon running in it.

---

## 8. Verify, and only then flip

**The verify step is the whole reason this ordering is safe.** Until you change `bootcmd`,
nothing you have done can affect the next boot.

```sh
# on the NPU
umount /tmp/standby
fsck.ext4 -fn /dev/mmcblk0p3                   # must come back clean
mount -o ro /dev/mmcblk0p3 /tmp/standby
ls -l /tmp/standby/boot/Image /tmp/standby/boot/cn9130-Sophos-XGS116.dtb
md5sum /tmp/standby/boot/Image                 # compare against the file you built
umount /tmp/standby
```

Only once all of that passes, point the boot at it — **both variables, they travel together**:

```sh
fw_printenv bootcmd bootargs                   # record the CURRENT values first; this is
                                               # your rollback, and it takes two seconds
fw_setenv bootcmd  "$(fw_printenv -n bootcmd_emmc3)"
fw_setenv bootargs "$(fw_printenv -n bootargs_emmc3)"
fw_printenv bootcmd bootargs                   # read it back; confirm it now says p3
```

`bootargs` carries `root=/dev/mmcblk0pN` and `bootcmd` carries the matching
`ext4load mmc 0:N`. **Setting one and not the other gives you a kernel from one slot with a root
filesystem from the other**, which is a confusing way to fail.

Then reboot the NPU — **and be on its console before you do, not after.** The NPU console is
the x86 host's `ttyS2` (`stty -F /dev/ttyS2 115200 raw -echo`, then read it; see
`PROVENANCE.md`). Start reading it first, then reset the NPU only:

```sh
# on the NPU, over the SSH session you are about to lose
echo 1 > /proc/sys/kernel/sysrq; sync; echo b > /proc/sysrq-trigger
```

**Watch the boot; do not assume it.** There is no automatic rollback, so a slot that panics
will sit there rebooting into the same panic until you intervene — and the console is the only
place that will tell you.

**Expect to lose `mvmgmt0` across the reboot.** The host↔NPU handshake latches host ring
addresses once per NPU boot, so a host driver that was attached to the *old* NPU boot is
desynced from the new one. Reload the host driver (or reboot the host) to get the management
link back. Plan for that: the SSH session you did all of this from does not survive the reboot
you are about to trigger, and the console is what carries you across the gap.

---

## 9. If it does not come up: the U-Boot console

**You are not locked out, and you do not need physical access beyond the host you already
have.** U-Boot's own console is on the same `ttyS2` line, and `bootdelay=3` gives you a window
to interrupt it.

Send keystrokes continuously while the NPU resets — we caught the prompt by streaming spaces at
it across a reset:

```
Marvell>> printenv bootcmd bootargs
```

From there, put `bootcmd` and `bootargs` back to the values you recorded in step 8 and boot.
This is the out-of-band path, and it works whether or not the slot you flipped to has a working
userland — U-Boot is in SPI-NOR and you never touched it.

**Honestly, about what we exercised at that prompt:** we reached it, ran `printenv` and `mii`,
and used `boot` to return the box to its vendor OS. `setenv` and `saveenv` are ordinary U-Boot
commands and are the obvious way to do the rollback, but **we did not run them on this board.**
Note also that the environment is a **single copy with no redundancy** — losing power in the
middle of a `saveenv` is the one way to make a bad situation worse.

If the flip has not been made permanent yet, the very safest recovery is simply not to save:
boot the good slot for this boot only, get back into Linux, and do the `fw_setenv` from there
where you have a real shell.

---

## Two things that are not paths

**U-Boot cannot TFTP.** `tftpboot`, `dhcp`, `ping`, `bootp` and `nfs` are all in the command
table, which makes this look available. It is not:

```
Marvell>> printenv ethact
## Error: "ethact" not defined
Marvell>> net list
Unknown command 'net' - try 'help'
Marvell>> mii device
MII devices: 'mdio@12a200'
Current device: 'mdio@12a200'
```

An MDIO bus with no MAC driver bound to it. There is no network in this bootloader. **We spent
an evening on this so you do not have to.** The management link in section 2 exists only once
Linux is running on both ends.

**`/dev/mtd0` is not part of this.** Covered in section 1; repeated here because it is the one
write that can take the box away from you.

---

## What is still unproven

Stated plainly, because a procedure that hides its gaps is worse than one that does not exist:

- **No rootfs has been installed through this path end to end.** Sections 2–5 and 9's console
  access are measured; sections 6–8's writes are not.
- **Whether your NPU has `mke2fs`, `tar`, `fsck.ext4` and `fw_setenv`** — step 5(c) is a real
  check, not a formality. We only ever ran `fw_printenv`.
- **`setenv` / `saveenv` at the U-Boot prompt** were not exercised on this board.
- **Whether the NPU link-local is the same across the product line.** It matched on both of our
  XGS 116s, on the same vendor firmware version. That is not a guarantee; step 3 discovers it.
- **Whether a modern OpenSSH client needs legacy algorithm options.** Ours did not, from
  FreeBSD, across 49 invocations. Yours might.

If you complete an install this way — or if it fails somewhere this document did not predict —
that is exactly the contribution `CONTRIBUTING.md` is asking for. Say which board, which slot
you were running from, and what your NPU's busybox actually had on it.
