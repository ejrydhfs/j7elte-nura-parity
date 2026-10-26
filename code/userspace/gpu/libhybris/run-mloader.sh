#!/bin/sh
# Run the hand-rolled ELF32 loader against the extracted ROM blob.
#
# No bionic is involved and no GPU exists here: this exercises load,
# relocation, constructors and the blob's EGL entry points only.
#
#   ./run-mloader.sh                 # normal
#   ./run-mloader.sh --nofault       # leave faults to a debugger
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

if [ ! -f romroot/vendor/lib/egl/libGLES_mali.so ]; then
	echo "blob not extracted: run ./extract-rom.sh first" >&2
	exit 1
fi
[ -x ./mloader ] || { echo "build first: make mloader" >&2; exit 1; }

ulimit -c 0 2>/dev/null || true
exec qemu-arm ./mloader romroot/vendor/lib/egl/libGLES_mali.so "$@"
