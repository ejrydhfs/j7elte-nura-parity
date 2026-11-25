#!/bin/sh
# env.sh - select the Mesa/EGL stack for kms test tooling (j7elte, on device).
#
#   . env.sh [egl|kms|system]
#
#   egl     Mesa 20.3.5 from /data/mesabuild/mesa20-b (panfrost 7580) and
#           EGL_PLATFORM=surfaceless. Use for the headless GBM/EGL test while
#           no card0 exists.
#   kms     Same Mesa 20.3.5 libraries/override, but no EGL_PLATFORM, so
#           kmscube/modetest can drive a future /dev/dri/card0 via GBM.
#   system  Stock Mesa 26.3.0 (/usr/lib, /usr/lib/dri) - reference only.
#
# Why the override: MESA_LOADER_DRIVER_OVERRIDE=panfrost pins the DRI module
# to panfrost_dri.so regardless of the render node's DRM driver name.  It is
# redundant since the driver was renamed to "panfrost" (2d19f91), but kept so
# these tools still work against kernels/modules built before that rename,
# where the loader would look for panfrost7580_dri.so and fall back to
# llvmpipe (docs/reports/gpu31-mesa20.md section 3,
# docs/reports/kms-panfrost-rename.md).
set -u

_kms_mode=${1:-egl}
B=${MESA20_B:-/data/mesabuild/mesa20-b}

unset LIBGL_ALWAYS_SOFTWARE
unset DRI_PRIME
unset PAN_MESA_DEBUG
unset EGL_LOG_LEVEL LIBGL_DEBUG

case "$_kms_mode" in
system)
	unset MESA_LOADER_DRIVER_OVERRIDE
	unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH EGL_PLATFORM
	if [ -d /usr/lib/dri ]; then
		LIBGL_DRIVERS_PATH=/usr/lib/dri
		export LIBGL_DRIVERS_PATH
	fi
	;;
kms|egl)
	if [ ! -d "$B/src/gallium/targets/dri" ]; then
		printf 'env.sh: %s missing; install the Mesa 20.3.5 build or use "system"\n' "$B" >&2
		return 1 2>/dev/null || exit 1
	fi
	LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri
	[ -d "$B/src/mapi/shared-glapi" ] &&
		LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$B/src/mapi/shared-glapi
	LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$B/src/glx
	export LD_LIBRARY_PATH
	LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri
	export LIBGL_DRIVERS_PATH
	MESA_LOADER_DRIVER_OVERRIDE=panfrost
	export MESA_LOADER_DRIVER_OVERRIDE
	if [ "$_kms_mode" = egl ]; then
		EGL_PLATFORM=surfaceless
		export EGL_PLATFORM
	fi
	;;
*)
	printf 'usage: . %s [egl|kms|system]\n' "$0" >&2
	return 2 2>/dev/null || exit 2
	;;
esac

printf 'env.sh: mode=%s B=%s LD_LIBRARY_PATH=%s\n' "$_kms_mode" "$B" "${LD_LIBRARY_PATH:-<system>}"
