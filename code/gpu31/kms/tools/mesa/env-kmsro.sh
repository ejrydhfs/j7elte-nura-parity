#!/bin/sh
# env-kmsro.sh - select the kmsro-capable Mesa 20.3.5 (panfrost + kmsro) stack.
#
#   . env-kmsro.sh                      # in the current shell (on the phone)
#   sh -c '. env-kmsro.sh && kmscube -D /dev/dri/card0'
#   MESA_KMSRO_DEBUG=1 . env-kmsro.sh   # add MESA-LOADER/EGL verbose logging
#
# Layout - side by side with the panfrost-only build, nothing is replaced:
#   /data/mesabuild/mesa20-b                panfrost only   (unchanged)
#   /data/mesabuild/mesa20-kmsro            panfrost+kmsro  (build tree)
#   /data/mesabuild/mesa20-kmsro-install    same, installed with DESTDIR
#                                           (preferred at run time: one lib
#                                           dir, Mesa's own megadriver symlinks)
#
# Why this build exists: Mesa 20.3.5 compiles kmsro only when it is named in
# -Dgallium-drivers (meson.build:219, with_gallium_kmsro =
# gallium_drivers.contains('kmsro')).  mesa20-b was configured with
# -Dgallium-drivers=panfrost, so src/gallium/targets/dri/ has panfrost_dri.so
# but no exynos_dri.so, and card0 can never be paired: the loader opens
# <drmGetVersion(card0)->name>_dri.so = exynos_dri.so, which is exactly the
# kmsro driver (src/gallium/targets/dri/target.c, GALLIUM_KMSRO block).
#
# MESA_LOADER_DRIVER_OVERRIDE must stay UNSET here: card0 has to resolve to
# exynos_dri.so through its kernel driver name, and kmsro then picks the
# render node itself (drmOpenWithType("panfrost", DRM_NODE_RENDER)).  An
# override would either force exynos onto a render node or force panfrost
# onto card0 - both are the failure modes measured in
# docs/reports/kms-mesa-pairing.md rows 4/10/13.
#
# Note for the render node *today*: the running kernel (#37) still reports
# its name as "panfrost7580", so a render-node-only probe needs
#   MESA_LOADER_DRIVER_OVERRIDE=panfrost
# as a temporary workaround.  After the driver rename lands
# (code/gpu31/driver/0001-panfrost7580-driver-sources.patch) that override is
# no longer needed for either node and must not be set - see
# docs/reports/mesa-kmsro-build.md section 5.
set -u

B=${MESA20_KMSRO:-/data/mesabuild/mesa20-kmsro}
I=${MESA20_KMSRO_INSTALL:-/data/mesabuild/mesa20-kmsro-install}

# Nothing that silently points the loader at another stack may survive.
unset MESA_LOADER_DRIVER_OVERRIDE
unset EGL_PLATFORM GBM_BACKEND
unset LIBGL_ALWAYS_SOFTWARE GBM_ALWAYS_SOFTWARE
unset DRI_PRIME PAN_MESA_DEBUG LD_PRELOAD
unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH LIBGL_DEBUG EGL_LOG_LEVEL

if [ -d "$I/usr/local/lib/dri" ]; then
	LD_LIBRARY_PATH=$I/usr/local/lib
	LIBGL_DRIVERS_PATH=$I/usr/local/lib/dri
elif [ -d "$B/src/gallium/targets/dri" ]; then
	LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm
	LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$B/src/gallium/targets/dri
	if [ -d "$B/src/mapi/shared-glapi" ]; then
		LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$B/src/mapi/shared-glapi
	fi
	LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$B/src/glx
	LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri
else
	printf 'env-kmsro.sh: no Mesa 20.3.5 kmsro build\n' >&2
	printf '  build tree:   %s/src/gallium/targets/dri\n' "$B" >&2
	printf '  install tree: %s/usr/local/lib/dri\n' "$I" >&2
	return 1 2>/dev/null || exit 1
fi

if [ -n "${MESA_KMSRO_DEBUG:-}" ] && [ "$MESA_KMSRO_DEBUG" != 0 ]; then
	LIBGL_DEBUG=verbose
	EGL_LOG_LEVEL=debug
	export LIBGL_DEBUG EGL_LOG_LEVEL
fi

export LD_LIBRARY_PATH LIBGL_DRIVERS_PATH
