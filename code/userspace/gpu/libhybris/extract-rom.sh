#!/bin/sh
# Extract the minimum Android-6 32-bit ARM (bionic) runtime needed to load the
# ROM's Mali EGL blob, straight out of system.raw, into ./romroot.
#
# Layout mirrors the device so that the blob's absolute DT_NEEDED names
# (lib*.so, resolved from /system/lib) and its hard-coded "/vendor/lib/egl/..."
# paths match the device.  No file is modified in any way.
#
# Note: system.raw is the *contents* of /system (its ext4 root already is
# "system"), while vendor/ sits at that same root: on the device it is
# /system/vendor, exported as /vendor via a symlink.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
img=${SYSTEM_RAW:-$here/../../../../system.raw}
[ -f "$img" ] || { echo "system.raw not found (set SYSTEM_RAW=)" >&2; exit 1; }
out=$here/romroot

# <image path> <destination path in the emulated device tree>
FILES="
/bin/linker                             /system/bin/linker
/lib/libc.so                            /system/lib/libc.so
/lib/libdl.so                           /system/lib/libdl.so
/lib/libm.so                            /system/lib/libm.so
/lib/libz.so                            /system/lib/libz.so
/lib/liblog.so                          /system/lib/liblog.so
/lib/libcutils.so                       /system/lib/libcutils.so
/lib/libhardware.so                     /system/lib/libhardware.so
/lib/libsync.so                         /system/lib/libsync.so
/lib/libion.so                          /system/lib/libion.so
/lib/libutils.so                        /system/lib/libutils.so
/lib/libbacktrace.so                    /system/lib/libbacktrace.so
/lib/libc++.so                          /system/lib/libc++.so
/lib/libbinder.so                       /system/lib/libbinder.so
/lib/libui.so                           /system/lib/libui.so
/lib/libEGL.so                          /system/lib/libEGL.so
/lib/egl/egl.cfg                        /system/lib/egl/egl.cfg
/lib/hw/gralloc.exynos5.so              /system/lib/hw/gralloc.exynos5.so
/lib/hw/hwcomposer.exynos5.so           /system/lib/hw/hwcomposer.exynos5.so
/vendor/lib/egl/libGLES_mali.so         /vendor/lib/egl/libGLES_mali.so
"

mkdir -p "$out"
: > "$out/.debugfs.cmds"
set -- $FILES
while [ $# -gt 0 ]; do
	src=$1; dst=$2; shift 2
	mkdir -p "$out$(dirname "$dst")"
	printf 'dump %s %s\n' "$src" "$out$dst" >> "$out/.debugfs.cmds"
done
debugfs -f "$out/.debugfs.cmds" "$img" >/dev/null 2>&1 || true
rm -f "$out/.debugfs.cmds"

missing=0
set -- $FILES
while [ $# -gt 0 ]; do
	src=$1; dst=$2; shift 2
	[ -s "$out$dst" ] || { echo "MISSING: $src" >&2; missing=1; }
done
[ "$missing" -eq 0 ] || exit 1

echo "extracted $(( $(printf '%s\n' $FILES | wc -l) / 2 )) files into $out"
