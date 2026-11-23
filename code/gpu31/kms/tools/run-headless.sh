#!/bin/sh
# run-headless.sh - GBM + EGL-surfaceless test that works BEFORE card0 exists.
#
# Exercises the userspace half of M2 (Mesa 20.3.5 panfrost, GBM allocation,
# EGL/GLES2 render, deterministic readback checksum) on /dev/dri/renderD128.
# Fails if: GBM/EGL/GL init fails, the checksum changes between frames, or
# the panfrost/DECON dmesg counters move during the run.
#
#   sh run-headless.sh
#   FRAMES=60 sh run-headless.sh
#   MESA_MODE=system sh run-headless.sh          # stock Mesa 26 (reference)
#   EXPECT_RENDERER= sh run-headless.sh          # accept llvmpipe
#
# Must be run on the device. Hold the host lock for the whole invocation:
#   flock /tmp/gpsdev.lock sh run-headless.sh
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/data/kmslogs}
mkdir -p "$OUT" 2>/dev/null || OUT=/tmp/kmslogs
mkdir -p "$OUT"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$OUT/headless-$STAMP.log
KMSFLIP=${KMSFLIP:-$DIR/kmsflip}
NODE=${GBM_NODE:-/dev/dri/renderD128}
FRAMES=${FRAMES:-30}
MODE=${MODE:-720x1280}
EXPECT=${EXPECT_RENDERER-Mali}

say() { printf '%s\n' "$*"; }

[ -x "$KMSFLIP" ] || { say "HEADLESS: FAIL missing binary $KMSFLIP (run build.sh)"; exit 3; }
[ -e "$NODE" ] || { say "HEADLESS: SKIP no render node $NODE"; exit 3; }

MESA_MODE=${MESA_MODE:-egl}
if ! . "$DIR/env.sh" "$MESA_MODE"; then
	say "HEADLESS: FAIL cannot select Mesa stack ($MESA_MODE)"
	exit 3
fi

say "== headless GBM/EGL test $STAMP =="
uname -a
"$DIR/kms-dmesg.sh" snapshot > "$OUT/before-$STAMP.txt"

set -- --headless --node "$NODE" --mode "$MODE" --frames "$FRAMES" \
	--dump "$OUT/headless-$STAMP.ppm"
if [ -n "$EXPECT" ]; then
	set -- "$@" --expect-renderer "$EXPECT"
fi

"$KMSFLIP" "$@" > "$LOG" 2>&1
rc=$?
cat "$LOG"

"$DIR/kms-dmesg.sh" snapshot > "$OUT/after-$STAMP.txt"
STRICT_PF=1 "$DIR/kms-dmesg.sh" delta "$OUT/before-$STAMP.txt" "$OUT/after-$STAMP.txt"
drc=$?

say "HEADLESS: kmsflip_rc=$rc dmesg_rc=$drc log=$LOG"
if [ "$rc" -eq 0 ] && [ "$drc" -eq 0 ]; then
	say "RESULT: PASS"
	exit 0
fi
say "RESULT: FAIL"
exit 1
