#!/bin/sh
# SPDX-License-Identifier: MIT
# Native MV88E6193X switch access on the XGS116 NPU — raw orion-SMI @ 0xf212a200,
# multi-chip SMI address 2 indirect protocol. NO libsbsp / mvmdio_uio / kernel rebuild.
# DEV: port0=0x10 .. port10=0x1a, global1=0x1b, global2=0x1c.
# SMI-cmd bits: busy(15) mode-c22(12) op(11:10 read=10/write=01) dev(9:5) reg(4:0).
# Outer orion SMI @ base: data(15:0) phy(20:16) reg(25:21) readop(26) valid(27) busy(28).
SMI=0xf212a200
dm() { busybox devmem "$@"; }

owait() {  # wait outer orion SMI busy (bit28) clear
  i=0; while [ $i -lt 200 ]; do
    r=$(dm $SMI 32); [ $(( r & 0x10000000 )) -eq 0 ] && return 0; i=$((i+1))
  done; echo "owait timeout" >&2; return 1
}
swbusy() {  # wait switch SMI-command busy (phy2 reg0 bit15) clear
  i=0; while [ $i -lt 200 ]; do
    dm $SMI 32 0x04020000 >/dev/null; owait; c=$(dm $SMI 32)
    [ $(( c & 0x8000 )) -eq 0 ] && return 0; i=$((i+1))
  done; echo "swbusy timeout" >&2; return 1
}
swrd() {  # DEV REG -> 0xhhhh
  swbusy
  dm $SMI 32 $(( 0x20000 | 0x9800 | ($1<<5) | $2 )) >/dev/null; owait
  swbusy
  dm $SMI 32 0x04220000 >/dev/null; owait; v=$(dm $SMI 32)
  printf '0x%04x\n' $(( v & 0xffff ))
}
swwr() {  # DEV REG VAL
  swbusy
  dm $SMI 32 $(( 0x220000 | ($3 & 0xffff) )) >/dev/null; owait
  dm $SMI 32 $(( 0x20000 | 0x9400 | ($1<<5) | $2 )) >/dev/null; owait
  swbusy
}
case "$1" in
  rd) swrd $(($2)) $(($3)) ;;
  wr) swwr $(($2)) $(($3)) $(($4)); echo "wrote dev=$2 reg=$3 val=$4" ;;
  id) swrd 0x10 3 ;;
  dump) for p in 0x10 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 0x19 0x1a; do
          printf 'dev %s reg0(status)=%s reg3(id)=%s\n' "$p" "$(swrd $(($p)) 0)" "$(swrd $(($p)) 3)"; done ;;
  *) echo "usage: $0 {rd DEV REG | wr DEV REG VAL | id | dump}  (DEV: port0=0x10..port10=0x1a, g1=0x1b, g2=0x1c)" ;;
esac
