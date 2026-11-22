#!/bin/sh
# mfbd/run.sh - 512x512 / 1024x1024 FBD + readback matrix for the T720 port.
#
# Proves gpu31-mfbd.md's verdict: FBD type is not size-dependent (T720 is
# SFBD-only), and the 5 s stalls come from a submit that faults/hangs and is
# only recovered by the drm_sched timeout. Same client, same size, three
# readback modes:
#
#   noread      no readback at all, glFinish only        (clean control)
#   read+finish glFinish() then glReadPixels()           (SUBMIT-fence wait)
#   read        glReadPixels() directly (BO WAIT_BO)     (stall reproducer)
#
# Runs on the phone (rootshell: dmesg), with its own Mesa 20.3.5 build:
#   scp code/gpu31/mesa/mfbd/rdtest.c code/gpu31/mesa/mfbd/run.sh \
#       user@172.16.42.1:/data/mfbd/     # mkdir -p /data/mfbd first
#   rootshell sh /data/mfbd/run.sh
# Host side, under the device lock:
#   flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force \
#     timeout 900 setsid -w ssh user@172.16.42.1 "rootshell sh /data/mfbd/run.sh"'
#
# Env overrides: MESA_B, SIZES, FRAMES, OUTDIR.
# Never uses PAN_MESA_DEBUG=sync|trace (device rule); FBD prints are env-gated
# diagnostics already present in the /data Mesa (PAN_FBD_DEBUG).
set -u

MESA_B=${MESA_B:-/data/mesabuild/mesa20-b}
SIZES=${SIZES:-"256 512"}
FRAMES=${FRAMES:-10}
OUTDIR=${OUTDIR:-/tmp/mfbd}
SRC=${SRC:-/data/mfbd/rdtest.c}
BIN=${BIN:-/tmp/rdtest}

mkdir -p "$OUTDIR"
if [ ! -x "$BIN" ]; then
	gcc -O2 -Wall -Wextra -std=c99 -o "$BIN" "$SRC" -lEGL -lGLESv2 || {
		echo "mfbd: build failed"; exit 1; }
fi

export LD_LIBRARY_PATH="$MESA_B/src/egl:$MESA_B/src/mapi/es2api:$MESA_B/src/gbm:$MESA_B/src/gallium/targets/dri"
export LIBGL_DRIVERS_PATH="$MESA_B/src/gallium/targets/dri"
export MESA_LOADER_DRIVER_OVERRIDE=panfrost
export EGL_PLATFORM=surfaceless

echo "=== mfbd matrix $(date) mesa=$MESA_B frames=$FRAMES sizes=$SIZES"

for sz in $SIZES; do
	for mode in noread readfinish read; do
		case $mode in
		noread)     args="--read 0" ;;
		readfinish) args="--read 1 --pre-finish 1" ;;
		read)       args="--read 1 --pre-finish 0" ;;
		esac
		tag="$sz-$mode"
		dmesg -c >/dev/null 2>&1 || true

		# PAN_FBD_DEBUG: FBDDBG fragjob lines carry fbd ptr (bit0 = MFBD tag),
		# fb size and tile bounds. Keep them beside the frame log.
		PAN_FBD_DEBUG=1 "$BIN" --size "$sz" --frames "$FRAMES" $args \
			> "$OUTDIR/$tag.out" 2> "$OUTDIR/$tag.fbd"
		rc=$?

		faults=$(dmesg 2>/dev/null | grep -cE 'p7580_job: (ERR|TIMEOUT)')
		status=$(dmesg 2>/dev/null | grep -oE 'js_status=0x[0-9a-f]+ \([A-Z_]+\)' |
			 sort | uniq -c | tr '\n' ' ')
		fbd=$(grep -m1 'FBDDBG fragjob' "$OUTDIR/$tag.fbd" |
		      sed 's/.*fbd=\([^ ]*\).*/fbd=\1/' 2>/dev/null)
		tiles=$(grep -m1 'FBDDBG fragjob' "$OUTDIR/$tag.fbd" |
			sed 's/.*tiles=\([^ ]*\).*/tiles=\1/' 2>/dev/null)

		printf '%-16s rc=%d faults=%s %s %s %s\n' "$tag" "$rc" "$faults" \
		       "${fbd:-fbd=?}" "${tiles:-tiles=?}" "$status"
		grep -E 'FRAME|TOTAL' "$OUTDIR/$tag.out" | tail -3

		# A 5 s stall in the frame log is the recovery-latency signature.
		slow=$(grep -E 'FRAME' "$OUTDIR/$tag.out" |
		       sed -n 's/.*FRAME [0-9]* \([0-9]*\)ms.*/\1/p' |
		       awk '$1 >= 1000 { n++ } END { print n+0 }')
		echo "   slow_frames(>=1000ms)=$slow"
	done
done

echo "=== done $(date)"
