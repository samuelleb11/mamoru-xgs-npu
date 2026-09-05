# `mvmgt.x86` — Sophos's NPU management key

This key is Sophos's, not the project's, and it is in this repository deliberately. Sophos
publishes it, every XGS NPU already trusts it, and it authenticates to nothing outside the
appliance chassis.

## What it is

`mvmgt.x86` is the RSA key the x86 host uses to SSH into the CN9130 NPU inside the XGS. Sophos
ships it, unencrypted, in the initrd of their publicly downloadable firmware ISO; this copy came
out of `HW-22.0.1_MR-1-490.iso`. Its public half is already in `/root/.ssh/authorized_keys` on the
NPU of every XGS, placed there by Sophos before the box was bought.

```
2048-bit RSA, unencrypted PEM, 1679 bytes
SHA256:Oy+mLeV8oSla8puHQ7jYphUZ363Kinf4/0wfGNBn+Rk
file sha256 1c16ceed3201d32e59d1fb04cd32dd2f9e6ebdadd918025a14543ea2ac2edbbf
```

## The bundled exception

Every other vendor file this kit needs, you supply from your own appliance
([`docs/VENDOR-BITS.md`](../../../docs/VENDOR-BITS.md)). This key is the exception, and a
deliberate one. It is the difference between a repeatable install and an evening of transferring
bytes over a serial console in base64. Without it, the bootstrap transfer runs over the host's
`ttyS2` console at 115200 baud, base64-encoded, until you have reached the NPU that way and
appended a key of your own. With it, the install is an `ssh`/`scp` sequence that repeats.
[`docs/NPU-INSTALL.md`](../../../docs/NPU-INSTALL.md) exists to give that normal `ssh`/`scp`
workflow. Requiring ISO extraction first would keep the hard part and drop the easy part.

## Exposure

It confers nothing on anyone who does not already have it.

- It is already public. It ships in an ISO Sophos hands to anyone who asks.
- It authenticates to a coprocessor inside a chassis. `mvmgmt0` is a PCIe link between the x86 host
  and the NPU on the same board; it is not on any network, and there is no route to it from outside
  the box. To use this key against an NPU you must already have root on the x86 host that NPU is
  bolted to — at which point you own the appliance anyway.
- It is not a signing key, not a trust root, and not a credential for any Sophos service. It
  unlocks exactly one thing: the ARM coprocessor in an appliance you are holding.

It is still Sophos's key. That is not permission to use it against hardware that is not yours.

## Verification

Two independent checks establish that this copy is the key your own NPU already trusts. Either one
is sufficient.

Check 1, against your own NPU:

```sh
ssh-keygen -y -f mvmgt.x86        # -> ssh-rsa AAAAB3Nza...
ssh-keygen -lf mvmgt.x86          # -> 2048 SHA256:Oy+mLeV8oSla8puHQ7jYphUZ363Kinf4/0wfGNBn+Rk no comment (RSA)
```

Compare the derived public half against `/root/.ssh/authorized_keys` on your own NPU. Before SSH
works, the NPU is reachable over the host's serial console at `ttyS2`
([`docs/PROVENANCE.md`](../../../docs/PROVENANCE.md)).

Check 2, against Sophos's ISO: mount your own firmware ISO, find `keys/mvmgt.x86` in the initrd,
and compare the file sha256 above.

Measured: check 1, on one factory XGS 116 NPU, 2026-09-04. The public half derived from this file
is byte-identical to that NPU's `/root/.ssh/authorized_keys`, same fingerprint
`SHA256:Oy+mLeV8oSla8puHQ7jYphUZ363Kinf4/0wfGNBn+Rk`.

Not attempted: check 2. It is offered as an independent route to the same conclusion, not reported
as a result of this project.

The committed key carries no comment. That is the `no comment (RSA)` in the output above, and it is
why the derived public half ends at the base64 blob. An earlier revision of this file gave the
`ssh-keygen -y` output as `ssh-rsa AAAAB3Nza...sophos@sophos`; this file cannot produce that
trailing identity.

## Key replacement

A key appended to the running NPU's `authorized_keys` does not survive rootfs replacement. A key
built into the rootfs image you install does. Nothing in this kit depends on the bundled key either
way.

Once you can reach the NPU at all — over this key or over the serial console — append your own key
to the NPU's `authorized_keys` and never touch this one again. The factory rootfs mounts read-only,
so that append needs `mount -o remount,rw /` first.
[`docs/NPU-INSTALL.md`](../../../docs/NPU-INSTALL.md) says where in the install to do the swap and
where to put the key so it survives the new rootfs. The only file in this kit that reads the
bundled key is [`../relay-deploy.sh`](../relay-deploy.sh), as its `NPU_KEY` default.
