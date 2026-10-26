#!/bin/sh
# On-device 3D status check for the Samsung j7elte (Exynos 7580, Mali-T720).
#
# Run as root on the device:
#     ./check-3d.sh [path-to-kbase-probe]
#
# It only reads device state; it does not load modules or write anything.
# Expected result on Nura: kbase present, no DRM node, no hardware GL;
# Mesa software rendering (llvmpipe/swrast) is the only 3D path.

PROBE=${1:-./kbase-probe}

say() { printf '%-34s %s\n' "$1" "$2"; }

echo "== device nodes =="
for n in /dev/mali0 /dev/dri/card0 /dev/dri/renderD128 /dev/fb0 /dev/ion; do
	if [ -e "$n" ]; then
		say "$n" "present"
	else
		say "$n" "absent"
	fi
done

echo
echo "== kernel driver =="
if [ -r /proc/devices ]; then
	grep -q '^ *10 misc' /proc/devices && echo "misc major 10 registered"
fi
say "platform device" "$(test -d /sys/devices/platform/11400000.mali && echo yes || echo no)"
say "debugfs mali0" "$(test -d /sys/kernel/debug/mali0 && echo yes || echo no)"
say "gpu sysfs" "$(test -d /sys/kernel/gpu && echo yes || echo no)"
say "loaded gpu modules" "$(lsmod 2>/dev/null | awk 'NR>1 && $1 ~ /mali|panfrost|panthor/ {print $1}' | tr '\n' ' ')"
say "drm driver bound" "$(cat /sys/class/drm/*/device/driver/module 2>/dev/null || echo none)"

echo
echo "== dmesg (mali/kbase/panfrost/drm) =="
dmesg 2>/dev/null | grep -iE 'mali|kbase|panfrost|panthor|drm' | tail -n 20 || \
	echo "(dmesg not readable; run as root)"

echo
echo "== userspace stack =="
if command -v apk >/dev/null 2>&1; then
	for p in mesa-dri-gallium mesa-egl mesa-gbm xf86-video-fbdev xorg-server; do
		say "$p" "$(apk info -e "$p" >/dev/null 2>&1 && echo installed || echo missing)"
	done
else
	echo "(apk not found; not a postmarketOS/Alpine userland?)"
fi
for f in /usr/lib/dri/panfrost_dri.so /usr/lib/dri/swrast_dri.so /usr/lib/dri/kms_swrast_dri.so; do
	say "$f" "$(test -e "$f" && echo present || echo absent)"
done

echo
echo "== EGL/GL info =="
if command -v eglinfo >/dev/null 2>&1; then
	eglinfo -B 2>&1 | sed -n '1,40p'
else
	echo "(eglinfo not installed)"
fi

echo
echo "== kbase UAPI probe =="
if [ -x "$PROBE" ]; then
	"$PROBE"
else
	echo "(probe binary not found at $PROBE; build with: make CROSS_COMPILE=aarch64-linux-gnu-)"
fi

echo
echo "== interpretation =="
if [ ! -e /dev/dri/renderD128 ] && [ ! -e /dev/dri/card0 ]; then
	echo "No DRM render node: Mesa Panfrost (and any GBM/KMS client) cannot"
	echo "create a screen. There is no hardware 3D on this kernel; use Mesa"
	echo "llvmpipe/swrast via Xorg + xf86-video-fbdev (software rendering)."
else
	echo "A DRM node exists; check that its driver is 'panfrost' before expecting"
	echo "hardware 3D (this 3.10 kbase kernel has no panfrost driver)."
fi
