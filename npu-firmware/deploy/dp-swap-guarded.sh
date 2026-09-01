#!/bin/sh
# SPDX-License-Identifier: MIT
#
# dp-swap-guarded.sh — restart dp_fwd under a deadline, restoring the incumbent
# binary automatically if the candidate does not reach readiness.
#
# WHY THIS EXISTS. dp_fwd IS the datapath: the host's front ports (and therefore
# every management path into the appliance) ride the AGNIC trunk it serves. The
# moment it is stopped, the operator loses the x86 host, the NPU console reached
# through that host, and any way of observing the outcome — simultaneously. So the
# decision to keep or roll back CANNOT be made by whoever started the swap. It has
# to be made here, on the NPU, by something that is still running when the observer
# is gone. Same shape as the OTA boot-confirm watchdog, one layer down.
#
# MEASURED 2026-09-01 — READ THIS BEFORE RELYING ON THIS SCRIPT.
# The open question below was tested with an IDENTICAL binary (candidate == incumbent,
# so nothing could be blamed on new code) and the answer is: **THE HOST DOES NOT
# SURVIVE A dp_fwd RESTART.** Both management paths (port1 and port9) stayed dark
# through the watchdog's entire candidate-plus-restore budget, and recovery required
# a mains power-cycle (host back at T+29s, NPU datapath clean: mgmt ECHO OK, GIU
# configured, 9 netdevs registered).
#
# The cause is the one the header already suspected: the host publishes its AGNIC
# management rings ONCE at boot (P3) and the NPU latches them when dp_fwd starts.
# Nothing re-drives that handshake from the host side, so a restarted dp_fwd has no
# path back to a host that is still holding its original publication.
#
# WHAT THAT MEANS FOR THIS SCRIPT, honestly: its rollback logic is largely MOOT
# today. If the host is gone either way, nobody can read RESULT (tmpfs is wiped by
# the power cycle that recovers the box), and the mains cycle performs the rollback
# anyway by wiping /tmp and re-running the untouched incumbent. The script is kept
# because its premise becomes true the moment the host CAN re-establish -- e.g. a
# host-side agnic reset that re-drives P3 -- and because its refusal checks and
# immutable-rollback reasoning stay correct. It is NOT a working deploy path today,
# and must not be cited as one.
#
# PRACTICAL CONSEQUENCE FOR D84: every NPU firmware iteration costs a mains cycle.
# ~30s, proven twice, but it has to be PLANNED rather than discovered mid-test, and
# it means no NPU experiment can report its own result -- the verdict has to be read
# from the host AFTER the cycle, or not at all.
#
# THE ROLLBACK TARGET IS IMMUTABLE, and that is not luck. The NPU rootfs is mounted
# READ-ONLY (ext4 ro), so $DP/dp_fwd cannot be modified, truncated or replaced by
# anything this script does -- or by a failed candidate. A candidate therefore always
# runs from /tmp (tmpfs, the only writable storage), and the known-good binary it falls
# back to is beyond reach of the experiment. A power cycle wipes /tmp and dp-autostart.sh
# runs the untouched incumbent, which is why a mains cycle is a guaranteed recovery.
#
# COROLLARY FOR SHIPPING: because $DP is read-only, there is NO persistent deployment
# path for NPU firmware here. /tmp staging is a TEST mechanism only -- it does not
# survive a reboot by design. Shipping a D84 handler requires updating the NPU's own
# storage (the appliance's built-in NPU flasher), which is a separate mechanism with a
# separate proof. Do not mistake a working /tmp swap for a deployable one.
#
#   usage: setsid nohup sh dp-swap-guarded.sh /tmp/cand/dp_fwd > /dev/null 2>&1 &
#          (detached ON PURPOSE — it must outlive the console that launched it)
#
#   THE CANDIDATE FILE MUST BE NAMED dp_fwd, in its own directory. Stopping is done
#   by process NAME (killall/pidof dp_fwd), so a candidate called dp_fwd.new would be
#   INVISIBLE to the rollback: the script could not stop a failed candidate before
#   restarting the incumbent, and both would run at once. Naming it dp_fwd in a
#   separate directory keeps the name-based stop correct for either binary.
#
#   env: DP        deploy dir holding the incumbent dp_fwd  (default /opt/dp)
#        DEADLINE  seconds to wait for readiness            (default 60)
#        RESULT    where the verdict is written             (default /tmp/dp-swap.result)

CAND=$1
DP=${DP:-/opt/dp}
DEADLINE=${DEADLINE:-60}
RESULT=${RESULT:-/tmp/dp-swap.result}
READY='GIU pkt-echo is started'
ARGS='-g 2 -i eth0 -c 1 -a 1 -f dp-nmp-config.txt --no-stat'

say() { echo "[$(cut -d' ' -f1 /proc/uptime)] $*" >> "$RESULT"; }

: > "$RESULT"
say "swap starting: candidate=$CAND deploy=$DP deadline=${DEADLINE}s"

# --- refuse early, while refusing is still free -------------------------------
if [ -z "$CAND" ] || [ ! -f "$CAND" ]; then
	say "VERDICT=REFUSED reason=candidate-missing"
	exit 1
fi
chmod +x "$CAND" 2>/dev/null
if [ ! -x "$CAND" ]; then
	say "VERDICT=REFUSED reason=candidate-not-executable"
	exit 1
fi
if [ ! -x "$DP/dp_fwd" ]; then
	say "VERDICT=REFUSED reason=no-incumbent-to-restore"
	exit 1
fi
# A candidate identical to the incumbent is a same-binary restart. That is a LEGITIMATE
# and deliberately-supported experiment (it answers whether the host survives a restart
# at all, independently of any code change), so it is allowed — but it is recorded, so
# a later reader cannot mistake it for a test of new code.
CMD5=$(md5sum "$CAND" 2>/dev/null | cut -d' ' -f1)
IMD5=$(md5sum "$DP/dp_fwd" 2>/dev/null | cut -d' ' -f1)
say "candidate md5=$CMD5"
say "incumbent md5=$IMD5"
[ "$CMD5" = "$IMD5" ] && say "NOTE: candidate == incumbent (same-binary restart, not a code test)"

# --- stop the incumbent -------------------------------------------------------
# By exact name, never a pattern: `pkill -f dp_fwd` would match THIS script's own
# command line and kill the watchdog that is supposed to do the restoring.
say "stopping incumbent"
killall dp_fwd 2>/dev/null || kill -TERM "$(pidof dp_fwd 2>/dev/null)" 2>/dev/null
i=0
while [ $i -lt 20 ]; do
	pidof dp_fwd >/dev/null 2>&1 || break
	i=$((i + 1))
done
pidof dp_fwd >/dev/null 2>&1 && { killall -9 dp_fwd 2>/dev/null; }

start_and_wait() {   # $1 = binary, $2 = label -> 0 ready, 1 not
	_bin=$1; _lab=$2; _log=/tmp/dp-$_lab.log
	: > "$_log"
	# cd into $DP: dp_fwd resolves dp-nmp-config.txt relative to its cwd.
	( cd "$DP" && env LD_LIBRARY_PATH=/lib:/usr/lib "$_bin" $ARGS >> "$_log" 2>&1 ) &
	_pid=$!
	say "$_lab started pid=$_pid log=$_log"
	_t=0
	while [ $_t -lt "$DEADLINE" ]; do
		if grep -q "$READY" "$_log" 2>/dev/null; then
			# Readiness is necessary but not sufficient — the process must also
			# still BE there. A binary that prints the marker and then dies is a
			# failure, and without this check it would read as a success.
			sleep 2
			if kill -0 "$_pid" 2>/dev/null; then
				say "$_lab READY and alive after marker"
				return 0
			fi
			say "$_lab printed ready marker then EXITED"
			return 1
		fi
		kill -0 "$_pid" 2>/dev/null || { say "$_lab exited before readiness"; return 1; }
		sleep 1
		_t=$((_t + 1))
	done
	say "$_lab TIMEOUT after ${DEADLINE}s without '$READY'"
	return 1
}

# --- try the candidate --------------------------------------------------------
if start_and_wait "$CAND" candidate; then
	say "VERDICT=CANDIDATE_OK"
	say "NOTE: host connectivity is a SEPARATE question - see header"
	exit 0
fi

# --- candidate failed: restore the incumbent ----------------------------------
say "candidate failed - restoring incumbent"
killall -9 dp_fwd 2>/dev/null
i=0
while [ $i -lt 20 ]; do pidof dp_fwd >/dev/null 2>&1 || break; i=$((i + 1)); done

if start_and_wait "$DP/dp_fwd" incumbent; then
	say "VERDICT=REVERTED_OK candidate failed, incumbent restored"
	exit 2
fi

# Both failed. Say so LOUDLY and unambiguously: this is the state that needs a
# mains cycle, and it must never be confused with a quiet success.
say "VERDICT=BOTH_FAILED - datapath is DOWN - mains power-cycle required"
exit 3
