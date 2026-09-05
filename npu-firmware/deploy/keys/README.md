# `mvmgt.x86` — Sophos's NPU management key, and why a private key is in this repo

**This key is not ours.** It is Sophos's, it is in this repository deliberately, and the
paragraph below is the whole justification. If you found an unexplained private key in a
repository you would be right to assume the worst — so here is exactly what it is.

## What it is

`mvmgt.x86` is the RSA key the **x86 host** uses to SSH into the **CN9130 NPU** inside your
XGS. Sophos ships it, unencrypted, in the initrd of their publicly downloadable firmware ISO
(ours came out of `HW-22.0.1_MR-1-490.iso`). Its public half is already sitting in
`/root/.ssh/authorized_keys` on the NPU of every XGS — put there by Sophos, before you bought
the box.

```
2048-bit RSA, unencrypted PEM, 1679 bytes
SHA256:Oy+mLeV8oSla8puHQ7jYphUZ363Kinf4/0wfGNBn+Rk
file sha256 1c16ceed3201d32e59d1fb04cd32dd2f9e6ebdadd918025a14543ea2ac2edbbf
```

## Why it is here rather than "extract it from your own box"

Every other vendor file this kit needs, you supply yourself (`docs/VENDOR-BITS.md`). This one
is the exception, and it is a deliberate one: **it is the difference between a repeatable
install and an evening of transferring bytes over a serial console in base64.** The whole point
of `docs/NPU-INSTALL.md` is that someone putting open firmware on their own XGS gets a normal
`ssh`/`scp` workflow. Making them go and dig this out of an ISO first would keep the hard part
and drop the easy part.

## Why publishing it gives nobody anything

**It confers nothing on anyone who does not already have it.**

- It is already public — it ships in an ISO Sophos hands to anyone who asks.
- It authenticates **to a coprocessor inside a chassis**. `mvmgmt0` is a PCIe link between the
  x86 host and the NPU on the same board; it is not on any network, and there is no route to
  it from outside the box. To use this key against an NPU you must already have root on the
  x86 host that NPU is bolted to — at which point you own the appliance anyway.
- It is not a signing key, not a trust root, and not a credential for any Sophos service. It
  unlocks one thing: the ARM coprocessor in an appliance you are holding.

**It is still Sophos's key.** Do not read this as permission to use it against hardware that
is not yours.

## Verify our copy against your own

You do not have to take our word that this is the same key your box already trusts. Two
independent checks, either of which is sufficient:

```sh
# 1. Against your own NPU: does its authorized_keys hold this key's public half?
ssh-keygen -y -f mvmgt.x86                       # -> ssh-rsa AAAAB3Nza...sophos@sophos
ssh-keygen -lf mvmgt.x86                         # -> SHA256:Oy+mLeV8oSla8puHQ7jYphUZ363Kinf4/0wfGNBn+Rk
#    compare with /root/.ssh/authorized_keys on your NPU (reachable over the serial
#    console at ttyS2 — see docs/PROVENANCE.md — before you have SSH working)

# 2. Against Sophos's ISO: mount your own firmware ISO, find keys/mvmgt.x86 in the
#    initrd, and compare the sha256 above.
```

**We ran check 1.** The public half derived from this file is byte-identical to
`/root/.ssh/authorized_keys` as captured from a factory XGS 116 NPU on 2026-09-04 — same
fingerprint, `SHA256:Oy+mLeV8oSla8puHQ7jYphUZ363Kinf4/0wfGNBn+Rk`. That is what makes it usable
rather than merely plausible.

## If you would rather not use it

Nothing here depends on it. Once you can reach the NPU at all — over this key, or over the
serial console — you can append your own key to the NPU's `authorized_keys` and never touch
this one again. That is the better long-term arrangement for a box you intend to keep, and
`docs/NPU-INSTALL.md` says where to do it.
