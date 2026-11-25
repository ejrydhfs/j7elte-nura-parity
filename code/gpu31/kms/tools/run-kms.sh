#!/bin/sh
# run-kms.sh - card0 acceptance test (atomic dumb-buffer flips + dma-buf
#              import flips; modetest; optional kmscube).
#
# Preflight: card0 must exist, else exits 3 ("SKIP").  On int49-class
# render-only images only /dev/dri/renderD128 exists; use
# "kmsflip --probe" there (kms-flip-tool.md).
#
#   sh run-kms.sh
#   SKIP_KMSCUBE=1 sh run-kms.sh        # M1 gate only (no GPU scanout yet)
#   FRAMES=50 PATTERN=gradient sh run-kms.sh
#   DMABUF=0 sh run-kms.sh               # dumb phase only
#
# PASS = kmsflip atomic dumb+dma-buf phases N/N flips with matching
#        checksums + modetest -s animation
#        + (unless skipped) kmscube runs 10 s + zero underrun/timeout/DSIM
#        deltas in dmesg. DRM/panfrost deltas are printed diagnostics.
#
# On the device only; hold the host lock: flock /tmp/gpsdev.lock sh run-kms.sh
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/data/kmslogs}
mkdir -p "$OUT" 2>/dev/null || OUT=/tmp/kmslogs
mkdir -p "$OUT"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$OUT/kms-$STAMP.log
KMSFLIP=${KMSFLIP:-$DIR/kmsflip}
DEV=${KMS_DEV:-/dev/dri/card0}
MODULE=${KMS_MODULE:-exynos}
FRAMES=${FRAMES:-10}
PATTERN=${PATTERN:-solid}
DMABUF=${DMABUF:-1}
TIMEOUT_MS=${TIMEOUT_MS:-250}

say() { printf '%s\n' "$*"; }

[ -x "$KMSFLIP" ] || { say "KMS: FAIL missing binary $KMSFLIP (run build.sh)"; exit 3; }
if [ ! -e "$DEV" ]; then
	say "KMS: SKIP no $DEV (DECON KMS subdriver not bound; only render node today)"
	exit 3
fi

MESA_MODE=${MESA_MODE:-kms}
if ! . "$DIR/env.sh" "$MESA_MODE"; then
	say "KMS: WARN Mesa 20.3.5 not available; modetest/kmscube may use stock Mesa"
fi

say "== KMS card0 test $STAMP =="
uname -a
"$DIR/kms-dmesg.sh" snapshot > "$OUT/before-$STAMP.txt"

# --- resources (kmsflip + modetest) ---
"$KMSFLIP" --device "$DEV" --list > "$OUT/list-$STAMP.log" 2>&1
lrc=$?
cat "$OUT/list-$STAMP.log"
CONN=$(sed -n 's/^CONNECTOR id=\([0-9]*\).*status=connected.*/\1/p' "$OUT/list-$STAMP.log" | head -1)
MODENAME=$(sed -n 's/^  MODE \([^ ]*\) .*/\1/p' "$OUT/list-$STAMP.log" | head -1)
say "KMS: connector=${CONN:-?} preferred_mode=${MODENAME:-?}"

mok=0
mnote=""
vok=0
if command -v modetest >/dev/null 2>&1; then
	# libdrm 2.4.134: -M <module> uses drmOpen(); -D <path> opens directly.
	# The DECON subdriver registers as "exynos", but keep -D as a fallback.
	for base in "-M $MODULE" "-D $DEV"; do
		set -- $base
		timeout 10 modetest "$@" > "$OUT/modetest-$STAMP.log" 2>&1
		rrc=$?
		[ "$rrc" -eq 0 ] || continue
		for marg in "#0" "$MODENAME"; do
			[ -n "$marg" ] || continue
			timeout -s KILL 8 modetest "$@" -s "$CONN:$marg" \
				> "$OUT/modetest-pattern-$STAMP.log" 2>&1
			mrc=$?
			case "$mrc" in
			0|124|137)
				mok=1
				MODETEST_BASE="$base"
				break 2
				;;
			esac
			mnote="$mnote (mode $marg rc=$mrc)"
		done
	done
	if [ "$mok" -eq 1 ]; then
		set -- $MODETEST_BASE
		timeout -s KILL 8 modetest "$@" -v \
			> "$OUT/modetest-vflip-$STAMP.log" 2>&1
		vrc=$?
		case "$vrc" in
		0|124|137) vok=1 ;;
		esac
		say "KMS: modetest -v vsync flip rc=$vrc"
	fi
	# modetest exits non-zero immediately if the mode set fails; the
	# timeout kill (124) or SIGKILL (137) means it survived the run.
else
	mnote="modetest missing (run install-deps.sh)"
fi
say "KMS: modetest ${MODETEST_BASE:-?} -s ${CONN:-?}: pattern=$mok vflip=$vok$mnote"

# --- deterministic dumb-buffer page flips ---
set -- --device "$DEV" --frames "$FRAMES" --pattern "$PATTERN" \
	--timeout-ms "$TIMEOUT_MS" --dump "$OUT/kms-$STAMP.ppm"
[ -n "$CONN" ] && set -- "$@" --connector "$CONN"
[ "$DMABUF" = 0 ] && set -- "$@" --no-dmabuf
"$KMSFLIP" "$@" > "$LOG" 2>&1
krc=$?
cat "$LOG"

# --- kmscube (M2: panfrost-rendered dma-buf scanout) ---
kc_ok=2
if [ "${SKIP_KMSCUBE:-0}" = 1 ]; then
	say "KMS: kmscube skipped (SKIP_KMSCUBE=1, M1 gate)"
elif command -v kmscube >/dev/null 2>&1; then
	timeout -s KILL 10 kmscube -D "$DEV" > "$OUT/kmscube-$STAMP.log" 2>&1
	krc2=$?
	last=$(tail -3 "$OUT/kmscube-$STAMP.log" 2>/dev/null)
	say "KMS: kmscube rc=$krc2 ${last:+| $last}"
	case "$krc2" in
	0|124|137) kc_ok=0 ;;
	*) kc_ok=1 ;;
	esac
else
	kc_ok=1
	say "KMS: kmscube missing (run install-deps.sh)"
fi

"$DIR/kms-dmesg.sh" snapshot > "$OUT/after-$STAMP.txt"
"$DIR/kms-dmesg.sh" delta "$OUT/before-$STAMP.txt" "$OUT/after-$STAMP.txt"
drc=$?

say "KMS: kmsflip_rc=$krc modetest_ok=$mok modetest_vflip=$vok kmscube_ok=$kc_ok dmesg_rc=$drc"
if [ "$lrc" -eq 0 ] && [ "$krc" -eq 0 ] && [ "$mok" -eq 1 ] &&
   [ "$kc_ok" -ne 1 ] && [ "$drc" -eq 0 ]; then
	say "RESULT: PASS"
	exit 0
fi
say "RESULT: FAIL"
exit 1
