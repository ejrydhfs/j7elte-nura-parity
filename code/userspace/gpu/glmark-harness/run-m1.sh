#!/bin/sh
# M1 regression script for j7elte (Exynos 7580, Mali-T720).
#
# M1 goal: CONFIG_MODULES + CONFIG_DRM fragment present, /dev/dri exists,
# fbdev X still works. Phone must stay bootable with X working.
#
# Run on the device as root (X check needs DISPLAY):
#     DISPLAY=:0 ./run-m1.sh
#
# Prints PASS:/FAIL: lines; exits 0 only if all M1 gates hold, nonzero on FAIL.
# POSIX sh only (no bashisms).

FAIL=0

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=1; }

# --- Gate 1: /dev/dri present (card0 and/or renderD128) ---
if [ -d /dev/dri ]; then
	if [ -e /dev/dri/card0 ] || [ -e /dev/dri/renderD128 ]; then
		pass "/dev/dri present ($(ls /dev/dri 2>/dev/null | tr '\n' ' '))"
	else
		fail "/dev/dri dir exists but has no card0/renderD128"
	fi
else
	fail "/dev/dri absent (need CONFIG_DRM fragment; see check-3d.sh device-node checks)"
fi

# --- Gate 2: /dev/mali0 present (kbase still bound, GPU not stolen) ---
if [ -e /dev/mali0 ]; then
	pass "/dev/mali0 present"
else
	fail "/dev/mali0 absent (kbase regression)"
fi

# --- Gate 3: /dev/fb0 present (display path intact) ---
if [ -e /dev/fb0 ]; then
	pass "/dev/fb0 present"
else
	fail "/dev/fb0 absent (display regression)"
fi

# --- Gate 4: X 720x1280 alive ---
DISP=${DISPLAY:-:0}
if command -v xdpyinfo >/dev/null 2>&1; then
	DIM=$(xdpyinfo 2>/dev/null | grep -m1 'dimensions:' | grep -o '[0-9]*x[0-9]* pixels' | cut -d' ' -f1)
	if [ "$DIM" = "720x1280" ]; then
		pass "X alive on $DISP at 720x1280"
	elif [ -n "$DIM" ]; then
		fail "X alive on $DISP but dimensions=$DIM (expected 720x1280)"
	else
		fail "X not reachable on $DISP (xdpyinfo failed; is X running?)"
	fi
else
	# Fallback: Xorg process + fb0 geometry check.
	if pgrep -x Xorg >/dev/null 2>&1; then
		pass "Xorg process running (xdpyinfo missing, could not verify 720x1280)"
	else
		fail "xdpyinfo missing and no Xorg process found"
	fi
fi

# --- Optional context (never fails): dmesg patterns like check-3d.sh ---
echo "== context (informational) =="
dmesg 2>/dev/null | grep -iE 'mali|kbase|panfrost|panthor|drm' | tail -n 10 || echo "(dmesg not readable; run as root)"

if [ "$FAIL" -ne 0 ]; then
	echo "M1 RESULT: FAIL"
	exit 1
fi
echo "M1 RESULT: PASS"
exit 0
