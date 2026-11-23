#!/bin/sh
# build.sh - native on-device build of kmsflip (j7elte, Alpine/musl aarch64).
#
#   sh build.sh              # EGL/GBM + KMS build
#   NO_EGL=1 sh build.sh     # KMS-only (no EGL/GLES headers needed)
#
# The cross sysroot on the build host has no DRM/EGL headers and the device
# links its own musl libs, so kmsflip is built natively like egltest/tri-x11
# (code/gpu31/egltest/Makefile). Headers come from libdrm-dev + mesa-dev.
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
NO_EGL=${NO_EGL:-0}

for h in /usr/include/xf86drm.h /usr/include/xf86drmMode.h; do
	[ -r "$h" ] || {
		printf 'build: missing %s (apk add libdrm-dev)\n' "$h" >&2
		exit 3
	}
done
if [ "$NO_EGL" = 0 ]; then
	for h in /usr/include/EGL/egl.h /usr/include/GLES2/gl2.h /usr/include/gbm.h; do
		[ -r "$h" ] || {
			printf 'build: missing %s (apk add mesa-dev), or NO_EGL=1\n' "$h" >&2
			exit 3
		}
	done
fi

make -C "$DIR" NO_EGL="$NO_EGL" || exit 1

printf 'build: %s\n' "$DIR/kmsflip"
file "$DIR/kmsflip" 2>/dev/null || true
ldd "$DIR/kmsflip" 2>/dev/null || true
printf 'BUILD: PASS\n'
