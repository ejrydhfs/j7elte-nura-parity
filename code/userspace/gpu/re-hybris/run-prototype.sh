#!/bin/sh
# run-prototype.sh -- build the freestanding ARM32 loader and run the ROM's
# libGLES_mali.so under qemu-arm, calling its EGL entry points and logging
# the kbase UK ioctls it attempts.  Host-side diagnostic; needs qemu-arm.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
IMG=${1:-$HERE/egl_extract/libGLES_mali.so}
LOG=${2:-$HERE/logs/miniloader.log}
mkdir -p "$(dirname "$LOG")" "$HERE/miniloader/.tmp"
command -v qemu-arm >/dev/null || { echo "qemu-arm required"; exit 1; }
[ -f "$IMG" ] || { echo "missing blob: $IMG (run ./extract-rom-gles.sh)"; exit 1; }
( cd "$HERE/miniloader" && TMPDIR=$PWD/.tmp make >/dev/null )
qemu-arm "$HERE/miniloader/miniloader" "$IMG" 2>&1 | tee "$LOG"
