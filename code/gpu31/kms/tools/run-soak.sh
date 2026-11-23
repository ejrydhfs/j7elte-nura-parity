#!/bin/sh
# run-soak.sh - 300-flip KMS soak (M1/M2 acceptance).
#
# Flips a deterministic pattern 300 times at the panel's vblank, verifies the
# mapped buffer checksum after every flip, and fails on any flip timeout,
# checksum change, or dmesg underrun/timeout/DSIM counter movement. Also
# records max RSS, wall time, FPS and the thermal ceiling so leak/underflow
# regressions show up.
#
#   sh run-soak.sh
#   FRAMES=1000 PATTERN=noise sh run-soak.sh
#
# On the device only; hold the host lock: flock /tmp/gpsdev.lock sh run-soak.sh
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/data/kmslogs}
mkdir -p "$OUT" 2>/dev/null || OUT=/tmp/kmslogs
mkdir -p "$OUT"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$OUT/soak-$STAMP.log
KMSFLIP=${KMSFLIP:-$DIR/kmsflip}
DEV=${KMS_DEV:-/dev/dri/card0}
FRAMES=${FRAMES:-300}
PATTERN=${PATTERN:-bars}
TIMEOUT_MS=${TIMEOUT_MS:-200}

say() { printf '%s\n' "$*"; }

max_temp() {
	cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1
}

[ -x "$KMSFLIP" ] || { say "SOAK: FAIL missing binary $KMSFLIP"; exit 3; }
if [ ! -e "$DEV" ]; then
	say "SOAK: SKIP no $DEV"
	exit 3
fi

. "$DIR/env.sh" kms >/dev/null 2>&1 || true

say "== KMS soak $STAMP frames=$FRAMES pattern=$PATTERN =="
uname -a
MEM_BEFORE=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo)
T_BEFORE=$(max_temp)
"$DIR/kms-dmesg.sh" snapshot > "$OUT/before-$STAMP.txt"

"$KMSFLIP" --device "$DEV" --frames "$FRAMES" --pattern "$PATTERN" \
	--timeout-ms "$TIMEOUT_MS" --dump "$OUT/soak-$STAMP.ppm" > "$LOG" 2>&1
rc=$?
cat "$LOG"

MEM_AFTER=$(sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo)
T_AFTER=$(max_temp)
"$DIR/kms-dmesg.sh" snapshot > "$OUT/after-$STAMP.txt"
"$DIR/kms-dmesg.sh" delta "$OUT/before-$STAMP.txt" "$OUT/after-$STAMP.txt"
drc=$?

say "SOAK: rc=$rc dmesg_rc=$drc mem_available_kb=${MEM_BEFORE:-?}->${MEM_AFTER:-?} temp_mC=${T_BEFORE:-?}->${T_AFTER:-?}"
if [ "$rc" -eq 0 ] && [ "$drc" -eq 0 ]; then
	say "RESULT: PASS ($FRAMES flips, no underrun/timeout)"
	exit 0
fi
say "RESULT: FAIL"
exit 1
