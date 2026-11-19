#!/bin/sh
# Self-tests for render-node-probe v2 (host model, no hardware).
#
# Usage: ./run-probe-tests.sh [probe-binary]
#        PROBE_RUNNER="qemu-aarch64 -L /usr/aarch64-linux-gnu" ./run-probe-tests.sh ./render-node-probe
#
# The host model: /dev/dri/renderD* (if present) is a NON-panfrost DRM driver
# (nvidia-drm on this build host), so it must classify NEITHER/exit 4 — the
# same shape the j7elte shows today (kbase misc node, no DRM node at all).
# Tests assert only deterministic properties, so they pass on any host:
# missing/regular/null nodes, text/JSON exit agreement, JSON schema, and
# run-to-run determinism on the real host render node (whatever its verdict).
#
# Prints PASS:/FAIL: lines; exits 0 only if all tests hold, nonzero on FAIL.
# POSIX sh only (no bashisms).

PROBE=${1:-./render-node-probe}
FAIL=0
N=0

pass() { N=$((N+1)); printf 'PASS: %s\n' "$1"; }
fail() { N=$((N+1)); printf 'FAIL: %s\n' "$1"; FAIL=1; }

run() { ${PROBE_RUNNER} "$PROBE" "$@"; }

need_probe() {
	if [ ! -x "$PROBE" ] && [ -z "$PROBE_RUNNER" ]; then
		echo "FAIL: probe binary '$PROBE' not found (run make first)"
		exit 2
	fi
	if [ ! -e "$PROBE" ]; then
		echo "FAIL: probe binary '$PROBE' not found (run make first)"
		exit 2
	fi
}

need_probe

TMPF=./selftest-regular.tmp
printf 'not-a-device' >"$TMPF"

# --- T1: missing node -> exit 2 NO_DEVICE ---
OUT=$(run /nonexistent-node-xyz-abc 2>&1); RC=$?
if [ "$RC" -eq 2 ]; then pass "T1 missing node exits 2 (got $RC)"; else fail "T1 missing node exits 2 (got $RC: $OUT)"; fi
OUTJ=$(run --json /nonexistent-node-xyz-abc 2>&1); RCJ=$?
if [ "$RCJ" -eq 2 ]; then pass "T1b missing node --json exits 2"; else fail "T1b missing node --json exits 2 (got $RCJ)"; fi
case "$OUTJ" in
	*'"verdict":"NO_DEVICE"'*'"exit":2'*|*'"exit":2'*'"verdict":"NO_DEVICE"'*)
		# order is fixed (verdict then exit), but accept either
		pass "T1c missing node JSON verdict NO_DEVICE exit 2" ;;
	*) fail "T1c missing node JSON verdict ($OUTJ)" ;;
esac
case "$OUTJ" in
	*'"exists":false'*'"open":false'*) pass "T1d missing node JSON exists:false open:false" ;;
	*) fail "T1d missing node JSON availability flags ($OUTJ)" ;;
esac

# --- T2: regular file opens but is neither path -> exit 4 NEITHER ---
OUT=$(run "$TMPF" 2>&1); RC=$?
if [ "$RC" -eq 4 ]; then pass "T2 regular file exits 4 NEITHER"; else fail "T2 regular file exits 4 (got $RC: $OUT)"; fi
OUTJ=$(run --json "$TMPF" 2>&1); RCJ=$?
if [ "$RCJ" -eq 4 ]; then pass "T2b regular file --json exits 4"; else fail "T2b regular file --json exits 4 (got $RCJ)"; fi
case "$OUTJ" in
	*'"verdict":"NEITHER"'*) pass "T2c regular file JSON verdict NEITHER" ;;
	*) fail "T2c regular file JSON verdict ($OUTJ)" ;;
esac
case "$OUTJ" in
	*'"exists":true'*'"chardev":false'*) pass "T2d regular file JSON exists:true chardev:false" ;;
	*) fail "T2d regular file JSON availability flags ($OUTJ)" ;;
esac

# --- T3: /dev/null opens, is a chardev, speaks neither path -> exit 4 ---
if [ -e /dev/null ]; then
	OUT=$(run /dev/null 2>&1); RC=$?
	if [ "$RC" -eq 4 ]; then pass "T3 /dev/null exits 4 NEITHER"; else fail "T3 /dev/null exits 4 (got $RC: $OUT)"; fi
	OUTJ=$(run --json /dev/null 2>&1); RCJ=$?
	case "$OUTJ" in
		*'"chardev":true'*'"open":true'*) pass "T3b /dev/null JSON chardev:true open:true" ;;
		*) fail "T3b /dev/null JSON flags ($OUTJ)" ;;
	esac
else
	echo "SKIP: T3 /dev/null absent on this host"
fi

# --- T4: text and JSON exits agree on every node probed ---
for ND in "$TMPF" /dev/null /nonexistent-node-xyz-abc; do
	[ -e "$ND" ] || [ "$ND" = /nonexistent-node-xyz-abc ] || continue
	run "$ND" >/dev/null 2>&1; A=$?
	run --json "$ND" >/dev/null 2>&1; B=$?
	if [ "$A" -eq "$B" ]; then pass "T4 text/json exit agree on $ND ($A)"; else fail "T4 text/json exit agree on $ND (text $A vs json $B)"; fi
done

# --- T5: JSON exit field matches the process exit code ---
for ND in "$TMPF" /nonexistent-node-xyz-abc; do
	OUTJ=$(run --json "$ND" 2>&1); RCJ=$?
	EXJ=$(printf '%s' "$OUTJ" | grep -o '"exit":[0-9]*' | grep -o '[0-9]*')
	if [ "$EXJ" = "$RCJ" ]; then pass "T5 JSON exit field matches process exit on $ND ($RCJ)"; else fail "T5 JSON exit field ($EXJ) vs process exit ($RCJ) on $ND"; fi
done

# --- T6: JSON parses (python3) and has the harness schema ---
if command -v python3 >/dev/null 2>&1; then
	OUTJ=$(run --json "$TMPF" 2>&1)
	if printf '%s' "$OUTJ" | python3 -c "
import json,sys
d=json.load(sys.stdin)
assert set(['node','exists','chardev','open','open_errno','drm','driver','driver_desc','h1','h2','t720','params','kbase','verdict','exit']) <= set(d), sorted(d)
assert isinstance(d['exists'],bool) and isinstance(d['h1'],bool) and isinstance(d['h2'],bool)
assert isinstance(d['params'],dict) and isinstance(d['kbase'],dict)
assert d['verdict'] in ('H1','H1+H2','H2','NEITHER','NO_DEVICE')
assert d['exit'] in (0,1,2,3,4)
assert set(['answered','major','minor','too_old','errno','ret']) <= set(d['kbase'])
" 2>/dev/null; then
		pass "T6 JSON parses with harness schema"
	else
		fail "T6 JSON parses with harness schema"
	fi
else
	echo "SKIP: T6 python3 absent, JSON parse check skipped"
fi

# --- T7: bad flag -> exit 1 ERROR, usage on stderr ---
OUT=$(run --bogus-flag-xyz "$TMPF" 2>&1); RC=$?
if [ "$RC" -eq 1 ]; then pass "T7 bad flag exits 1 ERROR"; else fail "T7 bad flag exits 1 (got $RC: $OUT)"; fi

# --- T8: too many positional args -> exit 1 ---
OUT=$(run "$TMPF" "$TMPF" 2>&1); RC=$?
if [ "$RC" -eq 1 ]; then pass "T8 two nodes exits 1 ERROR"; else fail "T8 two nodes exits 1 (got $RC: $OUT)"; fi

# --- T9: host render node (if any) classifies deterministically ---
RENDER=""
for cand in /dev/dri/renderD128 /dev/dri/renderD129; do
	if [ -e "$cand" ]; then RENDER=$cand; break; fi
done
if [ -n "$RENDER" ]; then
	run "$RENDER" >/dev/null 2>&1; A=$?
	run "$RENDER" >/dev/null 2>&1; B=$?
	if [ "$A" -eq "$B" ]; then pass "T9 host $RENDER deterministic (exit $A)"; else fail "T9 host $RENDER deterministic ($A vs $B)"; fi
	case "$A" in
		0|3|4) pass "T9b host $RENDER exit in {0,3,4} (got $A)" ;;
		*) fail "T9b host $RENDER exit in {0,3,4} (got $A)" ;;
	esac
	OUTJ=$(run --json "$RENDER" 2>&1); RCJ=$?
	if [ "$RCJ" -eq "$A" ]; then pass "T9c host $RENDER text/json agree ($A)"; else fail "T9c host $RENDER text/json agree (text $A vs json $RCJ)"; fi
else
	echo "SKIP: T9 no /dev/dri/renderD* on this host"
fi

rm -f "$TMPF"

if [ "$FAIL" -ne 0 ]; then
	echo "PROBE SELFTEST: FAIL ($N tests, failures present)"
	exit 1
fi
echo "PROBE SELFTEST: PASS ($N tests)"
exit 0
