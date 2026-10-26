#!/bin/sh
# Run the 32-bit bionic EGL probe under qemu-arm against the extracted ROM
# rootfs.  No Mali hardware and no /dev/mali0 exist here: this exercises the
# *loading and entry-point* half of the stack only, and says so.
#
#   ./run-qemu.sh              # normal run
#   TRACE=1 ./run-qemu.sh      # + qemu cpu trace of the faulting block
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
[ -x ./eglprobe ] || { echo "build first: make" >&2; exit 1; }
[ -d romroot/system/lib ] || { echo "extract first: ./extract-rom.sh" >&2; exit 1; }

# NOTE: the ROM's bionic runtime deadlocks under qemu-user before reaching the
# program's entry point (see parity/reports/gpu-libhybris.md §3) - the same
# happens to the ROM's own /system/bin/toybox.  The timeout keeps this honest
# instead of hanging a terminal; a timeout is the expected result here.
TRACE=${TRACE:-0}
if [ "$TRACE" = 1 ]; then
	timeout ${TMO:-30} qemu-arm -L "$here/romroot" -d cpu,nochain -D "$here/qemu-trace.log" \
		-E LD_LIBRARY_PATH=/vendor/lib/egl:/system/lib \
		-E ANDROID_ROOT=/system -E ANDROID_DATA=/data \
		./eglprobe || echo "[run-qemu] exit=$? (trace in qemu-trace.log)"
else
	timeout ${TMO:-30} qemu-arm -L "$here/romroot" \
		-E LD_LIBRARY_PATH=/vendor/lib/egl:/system/lib \
		-E ANDROID_ROOT=/system -E ANDROID_DATA=/data \
		./eglprobe || echo "[run-qemu] exit=$?"
fi
