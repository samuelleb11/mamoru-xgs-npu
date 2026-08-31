# Vendor-derived NPU source

Files here are **not ours**. They carry a third-party copyright and third-party licence terms, and
they live in their own directory so that neither fact can be lost by proximity to code that is ours.

| File | Copyright | Terms |
|---|---|---|
| `forwarder.c` | Marvell International Ltd. and its affiliates | Multi-option: Marvell Commercial, **GPLv2**, or Marvell BSD. We rely on the **GPLv2** election. |

## Why it is here rather than fetched

Unlike the tarballs in `../../vendor/` — which are build inputs recorded by checksum and never
committed — this file is *compiled into firmware we ship*. A checksum in a manifest is the right
record for something we merely referenced; it is the wrong record for something that ends up in the
binary. So it is committed, unmodified in its licence header, and quarantined.

## Rules

- **Do not relicense it, and do not strip the header.** Marvell's terms explicitly require the
  copyright notice be preserved on whichever alternative is elected.
- **Do not let it spread.** Nothing outside this directory should be a derived work of it. The rest
  of `../src/` is ours and stays that way.
- **It is not clean-room input.** The host drivers in this kit are clean-room — written against
  documented behaviour and observed traffic. Nobody should read this file and then go and write host
  driver code — that is exactly the contamination the clean-room claim exists to exclude.
- The rest of this kit is MIT (the host drivers are GPL-2.0 OR MIT and BSD-2-Clause). This directory
  is the stated GPL-2.0 exception, quarantined so its license can never be lost by proximity to code
  that is ours.
