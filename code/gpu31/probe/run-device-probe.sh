#!/bin/sh
# Probe the j7elte render-node path on the DEVICE (kernel #35, root shell).
#
# Runs on the device as root. No reboot, no flash, no kernel build, no module
# load: opens nodes read-write (no ioctl writes state) plus read-only sysfs.
# Call it under host `flock /tmp/gpsdev.lock -c '...'` with a short hold.
#
# Usage (on device):  sh run-device-probe.sh [probe-binary] [output-dir]
#   probe-binary defaults to ./render-node-probe (same dir as this script);
#   output-dir defaults to /data/probe (root fs is ~94% full, keep logs off /).
#
# What it records:
#   1. node availability (/dev/mali0, /dev/dri/*, /dev/fb0)
#   2. kbase driver state (sysfs node, module, mali dmesg tail)
#   3. render-node-probe text + JSON for every candidate node
#   4. userspace stack (llvmpipe renderer strings) for the path verdict
#
# Prints PASS:/FAIL:/SKIP: lines; exits 0 only if all asserted gates hold.
# POSIX sh only (no bashisms).

PROBE=${1:-$(dirname "$0")/render-node-probe}
OUT=${2:-/data/probe}
FAIL=0
SKIP=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=1; }
skip() { printf 'SKIP: %s\n' "$1"; SKIP=$((SKIP+1)); }

mkdir -p "$OUT" || { echo "FAIL: cannot create $OUT"; exit 2; }
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%d-%H%M%S)
LOG="$OUT/device-probe-$STAMP.log"
exec >"$LOG" 2>&1
echo "== device probe $STAMP =="
echo "node-date: $(date 2>/dev/null)"

# --- 0. kernel stamp (expect 3.10.108, Integrator build #35) ---
UNAME=$(uname -r 2>/dev/null)
VERSION=$(cat /proc/version 2>/dev/null)
echo "uname: $UNAME"
echo "version: $VERSION"
case "$UNAME" in
	3.10.108) pass "kernel 3.10.108" ;;
	*) fail "kernel is $UNAME (expected 3.10.108)" ;;
esac

# --- 1. node availability ---
echo "== nodes =="
for n in /dev/mali0 /dev/fb0 /dev/dri/renderD128 /dev/dri/card0; do
	if [ -e "$n" ]; then
		echo "present: $n ($(ls -l "$n" 2>/dev/null))"
	else
		echo "absent: $n"
	fi
done
ls /dev/mali0 /dev/fb0 >"$OUT/nodes-$STAMP.txt" 2>&1
ls /dev/dri >"$OUT/dri-$STAMP.txt" 2>&1 || echo "no /dev/dri" >"$OUT/dri-$STAMP.txt"
if [ -c /dev/mali0 ]; then pass "/dev/mali0 chardev present"; else fail "/dev/mali0 missing (kbase regression)"; fi
if [ -c /dev/fb0 ]; then pass "/dev/fb0 chardev present (display path)"; else fail "/dev/fb0 missing (display regression)"; fi
# M1 is the /dev/dri gate; here it is a recorded SKIP, not a failure: the
# current #35 kernel is CONFIG_DRM=n, so absent /dev/dri is expected state.
if [ -e /dev/dri/renderD128 ] || [ -e /dev/dri/card0 ]; then
	pass "/dev/dri render node present"
else
	skip "/dev/dri absent (expected on CONFIG_DRM=n kernel #35; M1 gate)"
fi

# --- 2. kbase driver state (read-only) ---
echo "== kbase sysfs =="
ls -d /sys/devices/11400000.mali /sys/module/mali_kbase 2>&1
[ -d /sys/devices/11400000.mali ] && pass "11400000.mali bound" || fail "11400000.mali not bound"
[ -d /sys/module/mali_kbase ] && pass "mali_kbase module present" || fail "mali_kbase module missing"
dmesg 2>/dev/null | grep -iE 'mali|kbase|panfrost|drm' | tail -n 15 >"$OUT/dmesg-$STAMP.txt" 2>&1
cat "$OUT/dmesg-$STAMP.txt"

# --- 3. render-node classification ---
echo "== render-node-probe =="
if [ ! -x "$PROBE" ]; then
	fail "probe binary $PROBE missing/not executable (copy the aarch64 static build first)"
else
	"$PROBE" --help >/dev/null 2>&1 || true
	for ND in /dev/dri/renderD128 /dev/dri/card0 /dev/mali0; do
		BASE=$(echo "$ND" | tr '/' '_')
		if [ ! -e "$ND" ]; then
			echo "-- $ND: absent, skipped"
			continue
		fi
		echo "-- $ND (text) --"
		"$PROBE" "$ND" 2>&1 | tee "$OUT/probe$BASE-$STAMP.txt"
		echo "rc=$?"
		echo "-- $ND (json) --"
		"$PROBE" --json "$ND" 2>&1 | tee "$OUT/probe$BASE-$STAMP.json"
		echo "rc=$?"
	done
	# /dev/mali0 must answer the kbase handshake: H2 with exit 3.
	if [ -e /dev/mali0 ]; then
		"$PROBE" --json /dev/mali0 >"$OUT/mali0-expect-$STAMP.json" 2>&1
		RC=$?
		cat "$OUT/mali0-expect-$STAMP.json"
		echo "rc=$RC"
		if [ "$RC" -eq 3 ]; then
			pass "/dev/mali0 classifies H2 (kbase UK, exit 3)"
		else
			fail "/dev/mali0 probe exit $RC (expected 3 = H2 kbase UK)"
		fi
	fi
fi

# --- 4. userspace renderer (expect llvmpipe until M3) ---
echo "== renderer =="
if command -v glxinfo >/dev/null 2>&1 && [ -n "$DISPLAY" ]; then
	DISPLAY=${DISPLAY:-:0} glxinfo -B >"$OUT/glxinfo-B-$STAMP.log" 2>&1
	grep -iE 'renderer|accelerated' "$OUT/glxinfo-B-$STAMP.log" | head -n 5
else
	echo "(glxinfo skipped: no DISPLAY or not installed)"
fi
if command -v eglinfo >/dev/null 2>&1; then
	EGL_PLATFORM=surfaceless eglinfo -B >"$OUT/eglinfo-B-$STAMP.log" 2>&1
	grep -im1 'renderer' "$OUT/eglinfo-B-$STAMP.log" || true
fi
echo "log: $LOG"

if [ "$FAIL" -ne 0 ]; then
	echo "DEVICE PROBE: FAIL (skips: $SKIP)"
	exit 1
fi
echo "DEVICE PROBE: PASS (skips: $SKIP)"
exit 0
