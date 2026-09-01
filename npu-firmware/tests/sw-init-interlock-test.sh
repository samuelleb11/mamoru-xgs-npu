#!/bin/sh
# SPDX-License-Identifier: MIT
# sw-init-interlock-test.sh — proves the maps-before-PHYs interlock in sw-init.sh.
#
# The property under test is a SAFETY property: a front PHY must never be powered while its
# VLAN map is unwritten or unverified, because a powered port with the part's default map
# bridges the customer's network to itself (live-LAN loop, 2026-08-08).
#
# Until 2026-09-01 sw-init.sh powered ports 1-8 BEFORE writing the maps. Test A is the
# regression test for exactly that: it fails on the old ordering and passes on the new one.
#
# Every arm is controlled. Test E is the one that makes the others mean anything: with the
# override set, the SAME failing stub must still power the PHYs -- which proves D's skip is
# caused by the interlock and not by the stub being broken in some general way. Without E, a
# stub that simply crashed early would produce a passing D.
#
# Runs anywhere with a POSIX shell; no hardware, no root.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SWINIT=${SWINIT:-$HERE/../switch-init/sw-init.sh}
TMP=${TMPDIR:-/tmp}/swinit-interlock.$$
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  ok   — $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL — $1"; }

# --- the fake switch -------------------------------------------------------------------
# Emulates swmdio.sh {rd,wr}. Logs every call so ORDER can be asserted, which is the point.
mkstub() {
	cat > "$TMP/swmdio.sh" <<'STUB'
#!/bin/sh
LOG=$STUB_LOG; ST=$STUB_STORE; MODE=${STUB_MODE:-ok}
op=$1; dev=$(($2)); reg=$(($3))
case "$op" in
wr)
	val=$(($4))
	echo "wr $dev $reg $val" >> "$LOG"
	if [ "$MODE" = wrfail ] && [ "$reg" -eq 6 ] && [ "$dev" -ne 0 ]; then
		echo "WRITE FAILED" >&2; exit 1
	fi
	echo "$val" > "$ST/$dev.$reg"
	echo "wrote dev=$2 reg=$3 val=$4"
	;;
rd)
	echo "rd $dev $reg" >> "$LOG"
	if [ "$MODE" = invalid ] && [ "$reg" -eq 6 ]; then echo INVALID; exit 1; fi
	if [ "$MODE" = mismatch ] && [ "$reg" -eq 6 ] && [ "$dev" -ne 0 ]; then
		printf '0x%04x\n' 1023; exit 0     # 0x3ff = the part's default all-ports map
	fi
	if [ -f "$ST/$dev.$reg" ]; then printf '0x%04x\n' "$(cat "$ST/$dev.$reg")"
	else printf '0x%04x\n' 0; fi
	;;
esac
STUB
	chmod +x "$TMP/swmdio.sh"
}

run() {  # run MODE [ENFORCE] -> sets RC, LOG
	rm -rf "$TMP/store" "$TMP/log"; mkdir -p "$TMP/store"; : > "$TMP/log"
	STUB_LOG=$TMP/log STUB_STORE=$TMP/store STUB_MODE=$1 \
	SW=$TMP/swmdio.sh SW_ISO_ENFORCE=${2:-1} \
		sh "$SWINIT" > "$TMP/out" 2>&1
	RC=$?
}

# first line number in the log matching a pattern (empty = absent)
firstln() { grep -n "$1" "$TMP/log" 2>/dev/null | head -1 | cut -d: -f1; }

# first_front_power — line number of the first write that powers a FRONT port, or empty.
#
# THIS DETECTOR USED TO BE `grep -n "^wr 28 "` AND IT PRODUCED A FALSE FAIL. Device 28 (0x1c)
# is Global2, the indirect path for EVERY PHY/SerDes access -- including the CPU port's own
# 10G SerDes in step (4), which is EXEMPT from the interlock (the CPU port is the only port
# isolation points at, so powering it cannot create a front-to-front path) and correctly runs
# before the gate. The broad pattern could not tell the exempt port from the gated ones, so it
# reported "PHYs POWERED despite unverified isolation" against code that had refused exactly as
# designed -- rc=1, front ports dark. A detector that fires on everything convicts the innocent;
# it is the same vacuity as one that fires on nothing, and it costs more, because the "fix" it
# invites is a change to working code.
#
# The port is encoded in the command word's bits[9:5], so the arms are separable:
#   C22 copper phyup : reg 0x18, val 0x9xxx, port 1..8
#   C45 SFP lane     : reg 0x18, val 0x8xxx, port 9
#   C45 CPU SerDes   : reg 0x18, val 0x8xxx, port 0   <-- exempt, must NOT match
first_front_power() {
	awk '$1=="wr" && $2==28 && $3==24 {
		v=$4; op=int(v/4096); p=int(v/32)%32
		if ((op==9 && p>=1 && p<=8) || (op==8 && p==9)) { print NR; exit }
	}' "$TMP/log"
}

mkstub
echo "sw-init interlock tests"
echo "== A: maps are written BEFORE any front PHY is powered (regression, 2026-09-01) =="
run ok
last_map=$(grep -n '^wr [1-9][0-9]* 6 1$' "$TMP/log" | tail -1 | cut -d: -f1)
first_phy=$(first_front_power)      # front ports only; the CPU SerDes is exempt
if [ -z "$last_map" ]; then bad "A: no reg-6 map writes at all — stub or script broken"
elif [ -z "$first_phy" ]; then bad "A: no FRONT-port power writes at all — phyup never ran, test is vacuous"
elif [ "$last_map" -lt "$first_phy" ]; then
	ok "A: last map write (line $last_map) precedes first PHY write (line $first_phy)"
else
	bad "A: PHY powered at line $first_phy BEFORE map write at line $last_map — INTERLOCK VIOLATED"
fi

echo "== B: a healthy run verifies isolation and completes =="
run ok
[ "$RC" -eq 0 ] && ok "B: exit 0" || bad "B: exit $RC, expected 0"
grep -q "isolation VERIFIED by read-back on all 11 ports" "$TMP/out" &&
	ok "B: reported VERIFIED" || bad "B: did not report VERIFIED"
[ -n "$(first_front_power)" ] && ok "B: front PHYs were powered" || bad "B: PHYs never powered"

echo "== C: all eleven ports are actually covered (a short loop is a silent gap) =="
run ok
n=$(grep -c '^wr [0-9]* 6 ' "$TMP/log")
[ "$n" -eq 11 ] && ok "C: 11 reg-6 writes (ports 0-10)" || bad "C: $n reg-6 writes, expected 11"
for p in 9 10; do
	grep -q "^wr $p 6 1$" "$TMP/log" && ok "C: port $p isolated (the port bring-up loops skip)" ||
		bad "C: port $p NEVER isolated"
done

echo "== D: NEGATIVE CONTROLS — an unverifiable map must hold the PHYs dark =="
for mode in mismatch wrfail invalid; do
	run "$mode"
	phy=$(first_front_power)
	if [ "$RC" -eq 0 ]; then bad "D/$mode: exit 0 — failure not reported"
	elif [ -n "$phy" ]; then bad "D/$mode: PHYs POWERED at line $phy despite unverified isolation"
	else ok "D/$mode: refused, exit $RC, no PHY powered"
	fi
	grep -q "ISOLATION UNVERIFIED" "$TMP/out" && ok "D/$mode: said so out loud" ||
		bad "D/$mode: silent about it"
done

echo "== E: CONTROL ON THE CONTROL — override must still power the PHYs =="
# If this fails, D's passes prove nothing: they would be consistent with the stub simply
# breaking the script before it reached phyup.
run mismatch 0
phy=$(first_front_power)
[ -n "$phy" ] && ok "E: SW_ISO_ENFORCE=0 powered PHYs at line $phy — D's skip is the gate's doing" ||
	bad "E: PHYs dark even with the override — D proves nothing, stub breaks the run"

echo
echo "passed $PASS, failed $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
