# /data/mesa203-env.sh - select the Mesa 20.3.5 (panfrost/kmsro) stack.
#
#   . /data/mesa203-env.sh          # render-node mode (renderD128, panfrost)
#   . /data/mesa203-env.sh kms      # card0/GBM mode (KWin: exynos -> kmsro -> panfrost)
#   . /data/mesa203-env.sh system   # back to the stock Mesa 26.3 (no-op reset)
#
# Source it in the shell that execs the program: LD_LIBRARY_PATH /
# LIBGL_DRIVERS_PATH are read by the loader inside the process.
#
# Mesa 20.3.5 is the userspace that speaks this port's v5.2 panfrost UAPI;
# the system Mesa 26 submits a fragment job that never retires on this driver.
MESA203_PREFIX=/data/mesa203-install
export MESA203_PREFIX

case "${1:-render}" in
system)
	unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH GBM_BACKENDS_PATH
	unset MESA_LOADER_DRIVER_OVERRIDE EGL_PLATFORM
	LIBGL_DRIVERS_PATH=/usr/lib/dri
	export LIBGL_DRIVERS_PATH
	printf 'mesa203-env: system Mesa (%s)\n' "$(ls /usr/lib/libEGL.so.1* 2>/dev/null | head -1)"
	return 0 2>/dev/null || exit 0
	;;
kms)
	# card0 is the exynos/DECON KMS node: without the override the loader
	# picks exynos_dri.so, which is the kmsro entry point, and kmsro opens
	# the render node named "panfrost" for the actual 3D work.
	unset MESA_LOADER_DRIVER_OVERRIDE
	;;
*)
	# Render node only: force panfrost_dri.so even on kernels whose DRM
	# driver is still named panfrost7580 (pre-rename images).
	MESA_LOADER_DRIVER_OVERRIDE=panfrost
	export MESA_LOADER_DRIVER_OVERRIDE
	;;
esac

# libEGL.so.1, libGLESv2.so.2, libGLESv1_CM.so.1, libgbm.so.1,
# libglapi.so.0, libGL.so.1 live here.
LD_LIBRARY_PATH="$MESA203_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

# panfrost_dri.so / exynos_dri.so (one gallium megadriver, 22 names).
LIBGL_DRIVERS_PATH="$MESA203_PREFIX/lib/dri"
export LIBGL_DRIVERS_PATH

# Not used by 20.3.5 (split gbm backends arrived in Mesa 22); exported so the
# same environment also works if a newer gbm is ever layered in.
GBM_BACKENDS_PATH="$MESA203_PREFIX/lib/dri"
export GBM_BACKENDS_PATH

# Neutralise a stale llvmpipe / PRIME / EGL_PLATFORM environment. This build
# has no llvmpipe at all (gallium-drivers=panfrost,kmsro).
unset LIBGL_ALWAYS_SOFTWARE DRI_PRIME PAN_MESA_DEBUG EGL_PLATFORM
