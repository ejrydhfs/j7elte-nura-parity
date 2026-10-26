#!/bin/sh
# extract-rom-gles.sh -- dump the j7elte (SM-J700M, Android 6) GL/HAL stack
# from system.raw with debugfs and print an ELF manifest (class, ABI, NEEDED,
# sha256).  No root, no mount; read-only against the image.
#
# Usage: ./extract-rom-gles.sh [system.raw] [outdir]
set -eu
IMG=${1:-../../../../system.raw}
OUT=${2:-romroot}
BLOB=$OUT/egl_extract

say() { printf '%s\n' "$*"; }

command -v debugfs >/dev/null || { say "debugfs (e2fsprogs) required"; exit 1; }
[ -f "$IMG" ] || { say "no such image: $IMG"; exit 1; }

case "$OUT" in /*) OUTABS=$OUT ;; *) OUTABS=$PWD/$OUT ;; esac
mkdir -p "$OUTABS/system/bin" "$OUTABS/egl_extract"
# rdump insists on creating the target directory itself
rm -rf "$OUTABS/system/lib" "$OUTABS/vendor/lib"

# /system partition contents are rooted at / in this image:
#   image /bin/linker  -> device /system/bin/linker
#   image /lib/*       -> device /system/lib/*
#   image /vendor/lib  -> device /vendor/lib (vendor partition, /system/vendor)
debugfs -R "dump /bin/linker $OUTABS/system/bin/linker" "$IMG" >/dev/null 2>&1
debugfs -R "dump /vendor/lib/egl/libGLES_mali.so $OUTABS/egl_extract/libGLES_mali.so" "$IMG" >/dev/null 2>&1
debugfs -R "rdump /lib $OUTABS/system" "$IMG" >/dev/null 2>&1 || true
debugfs -R "rdump /vendor/lib $OUTABS/vendor" "$IMG" >/dev/null 2>&1 || true
chmod -R u+rwX "$OUTABS" 2>/dev/null || true

say "# ROM GL/HAL stack extracted from $IMG into $OUTABS"
say "# paths are the on-device ones; image root == /system"
say ""
for f in \
  "$OUT/system/bin/linker" \
  "$BLOB/libGLES_mali.so" \
  "$OUT/system/lib/hw/gralloc.exynos5.so" \
  "$OUT/system/lib/hw/hwcomposer.exynos5.so" \
  "$OUT/system/lib/libc.so" \
  "$OUT/system/lib/libdl.so" \
  "$OUT/system/lib/libhardware.so" \
  "$OUT/system/lib/libion.so" \
  "$OUT/system/lib/libui.so" \
  "$OUT/system/lib/libbinder.so" \
  "$OUT/system/lib/libc++.so" \
  "$OUT/system/lib/libEGL.so" \
  "$OUT/system/lib/libGLESv1_CM.so" \
  "$OUT/system/lib/libGLESv2.so" \
  "$OUT/system/lib/libexynosutils.so" \
  "$OUT/system/lib/libexynosv4l2.so" \
  "$OUT/system/lib/libdisplay.so" \
  "$OUT/system/lib/libmpp.so" \
  "$OUT/system/lib/libExynosHWCService.so"
do
  [ -f "$f" ] || { say "MISSING $f"; continue; }
  say "## ${f#$OUT/}"
  readelf -h "$f" 2>/dev/null | awk -F: '/Class:|Data:|Machine:|Flags:|Type:/ {gsub(/^ +/,"",$2); printf "%s=%s ", $1, $2} END {print ""}'
  say "sha256 $(sha256sum "$f" | cut -d' ' -f1)"
  n=$(readelf -d "$f" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')
  say "NEEDED ${n:-<none>}"
  say ""
done
