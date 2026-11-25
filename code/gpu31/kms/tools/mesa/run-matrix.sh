#!/bin/sh
# run-matrix.sh - Mesa/Gbm/EGL driver-pairing matrix for the DECON card0.
#
#   sh run-matrix.sh <cell|all>
#
# Requires the KMS modules to be loaded (card0 + renderD128) and should run
# under the shared device lock:
#
#   flock /tmp/gpsdev.lock sh run-matrix.sh all
#
# Every cell prints a "CELL <name> renderer=<...>" line; the loader lines of
# interest ("MESA-LOADER", "drmshim", "failed to open ... _dri.so") are kept
# so the decision point is visible in the log.  No cell commits a CRTC and no
# cell writes to the kernel: kmscube cells are bounded by `timeout` and run
# last (they hit the known atomic page-flip bug).
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
P=$DIR/mesaprobe
SHIM=$DIR/drmshim.so
M20=${MESA20_B:-/data/mesabuild/mesa20-b}
CARD=${CARD:-/dev/dri/card0}
REND=${REND:-/dev/dri/renderD128}
TMO=${TMO:-10}
KC=${KC:-/usr/sbin/kmscube}
KEEP="PROBE |MESA-LOADER|drmshim|panfrost|kmsro|swrast|egl:|failed"

log=/tmp/mesa-matrix.log
: >"$log"

sysenv() {
	unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH EGL_PLATFORM GBM_BACKEND
	unset MESA_LOADER_DRIVER_OVERRIDE PAN_MESA_DEBUG LIBGL_DEBUG EGL_LOG_LEVEL
	unset LD_PRELOAD DRM_NAME_SHIM GBM_ALWAYS_SOFTWARE LIBGL_ALWAYS_SOFTWARE
}

m20env() {
	sysenv
	LD_LIBRARY_PATH=$M20/src/egl:$M20/src/mapi/es2api:$M20/src/gbm:$M20/src/gallium/targets/dri
	LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$M20/src/mapi/shared-glapi:$M20/src/glx
	export LD_LIBRARY_PATH
	LIBGL_DRIVERS_PATH=$M20/src/gallium/targets/dri
	export LIBGL_DRIVERS_PATH
	MESA_LOADER_DRIVER_OVERRIDE=panfrost
	export MESA_LOADER_DRIVER_OVERRIDE
}

# cell_run <name> <cmd...>  -- run, log raw, print the renderer summary
cell_run() {
	name=$1; shift
	printf '\n========== CELL %s : %s\n' "$name" "$*" | tee -a "$log"
	"$@" 2>&1 | tee -a "$log" | grep -E "$KEEP" | sed 's/^/  /'
	rc=$?
	r=$(grep -a "PROBE renderer:" "$log" | tail -1 | sed 's/PROBE renderer: //')
	printf 'CELL %s renderer=%s\n' "$name" "${r:-<none>}"
}

probe() { "$P" "$@"; }

have_card() { [ -e "$CARD" ]; }

# Cell that needs a KMS node: print a machine-readable SKIP instead of noise.
skip_if_no_card() {
	have_card && return 0
	printf 'CELL %s renderer=<skipped: no %s>\n' "$1" "$CARD"
	return 1
}

case "${1:-all}" in
sys-card-baseline)
	skip_if_no_card "$1" || exit 0
	sysenv; cell_run "$1" probe --gbm "$CARD" --render --addfb ;;
sys-card-verbose)
	skip_if_no_card "$1" || exit 0
	sysenv; LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --gbm "$CARD" --render ;;
sys-card-gbmbackend)
	skip_if_no_card "$1" || exit 0
	sysenv; GBM_BACKEND=dri LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export GBM_BACKEND LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --gbm "$CARD" --render ;;
sys-card-ovr-exynos)
	skip_if_no_card "$1" || exit 0
	sysenv; MESA_LOADER_DRIVER_OVERRIDE=exynos
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export MESA_LOADER_DRIVER_OVERRIDE LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --gbm "$CARD" --render ;;
sys-card-ovr-panfrost)
	skip_if_no_card "$1" || exit 0
	sysenv; MESA_LOADER_DRIVER_OVERRIDE=panfrost
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export MESA_LOADER_DRIVER_OVERRIDE LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --gbm "$CARD" --render ;;
sys-card-shim)
	skip_if_no_card "$1" || exit 0
	sysenv; LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug PAN_MESA_DEBUG=debug
	export LD_PRELOAD DRM_NAME_SHIM LIBGL_DEBUG EGL_LOG_LEVEL PAN_MESA_DEBUG
	cell_run "$1" probe --gbm "$CARD" --render --addfb ;;
sys-render-ovr-panfrost)
	sysenv; MESA_LOADER_DRIVER_OVERRIDE=panfrost
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export MESA_LOADER_DRIVER_OVERRIDE LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --gbm "$REND" --render ;;
sys-card-surfaceless)
	skip_if_no_card "$1" || exit 0
	sysenv; EGL_PLATFORM=surfaceless LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export EGL_PLATFORM LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --default ;;
sys-card-surfaceless-shim)
	skip_if_no_card "$1" || exit 0
	sysenv; EGL_PLATFORM=surfaceless LD_PRELOAD=$SHIM
	DRM_NAME_SHIM=panfrost7580=panfrost
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export EGL_PLATFORM LD_PRELOAD DRM_NAME_SHIM LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --default ;;
m20-card-ovr-panfrost)
	skip_if_no_card "$1" || exit 0
	m20env; LIBGL_DEBUG=verbose
	export LIBGL_DEBUG
	cell_run "$1" probe --gbm "$CARD" --render ;;
m20-card-shim-kmsro)
	skip_if_no_card "$1" || exit 0
	m20env; MESA_LOADER_DRIVER_OVERRIDE=exynos
	LD_PRELOAD=$SHIM DRM_NAME_SHIM=exynos7580=exynos,panfrost7580=panfrost
	LIBGL_DEBUG=verbose
	export MESA_LOADER_DRIVER_OVERRIDE LD_PRELOAD DRM_NAME_SHIM LIBGL_DEBUG
	cell_run "$1" probe --gbm "$CARD" --render ;;
m20-render-ovr-panfrost)
	m20env
	cell_run "$1" probe --gbm "$REND" --render ;;
opens)
	sysenv; cell_run "$1" probe --info "${1:+$CARD}" --opens ;;
render-opens)
	sysenv; cell_run "$1" probe --info "$REND" --opens ;;
render-opens-shim)
	sysenv; LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost
	export LD_PRELOAD DRM_NAME_SHIM
	cell_run "$1" probe --info "$REND" --opens ;;
sys-render-gbm-shim)
	sysenv; LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug PAN_MESA_DEBUG=debug
	export LD_PRELOAD DRM_NAME_SHIM LIBGL_DEBUG EGL_LOG_LEVEL PAN_MESA_DEBUG
	cell_run "$1" probe --gbm "$REND" ;;
sys-render-gbm-ovr-exynos)
	sysenv; MESA_LOADER_DRIVER_OVERRIDE=exynos
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
	export MESA_LOADER_DRIVER_OVERRIDE LIBGL_DEBUG EGL_LOG_LEVEL
	cell_run "$1" probe --gbm "$REND" ;;
sys-render-gbm-ovr-exynos-shimver)
	sysenv; MESA_LOADER_DRIVER_OVERRIDE=exynos
	LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost DRM_VER_SHIM=1.1
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug PAN_MESA_DEBUG=debug
	export MESA_LOADER_DRIVER_OVERRIDE LD_PRELOAD DRM_NAME_SHIM DRM_VER_SHIM
	export LIBGL_DEBUG EGL_LOG_LEVEL PAN_MESA_DEBUG
	cell_run "$1" probe --gbm "$REND" ;;
sys-render-gbm-ovr-exynos-shim)
	sysenv; MESA_LOADER_DRIVER_OVERRIDE=exynos
	LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost
	LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug PAN_MESA_DEBUG=debug
	export MESA_LOADER_DRIVER_OVERRIDE LD_PRELOAD DRM_NAME_SHIM
	export LIBGL_DEBUG EGL_LOG_LEVEL PAN_MESA_DEBUG
	cell_run "$1" probe --gbm "$REND" ;;
sys-render-surfaceless)
	sysenv; EGL_PLATFORM=surfaceless
	export EGL_PLATFORM
	cell_run "$1" probe --default ;;
sys-render-surfaceless-ovr-panfrost)
	sysenv; EGL_PLATFORM=surfaceless MESA_LOADER_DRIVER_OVERRIDE=panfrost
	export EGL_PLATFORM MESA_LOADER_DRIVER_OVERRIDE
	cell_run "$1" probe --default ;;
sys-render-surfaceless-shim)
	sysenv; EGL_PLATFORM=surfaceless LD_PRELOAD=$SHIM
	DRM_NAME_SHIM=panfrost7580=panfrost
	export EGL_PLATFORM LD_PRELOAD DRM_NAME_SHIM
	cell_run "$1" probe --default ;;
m20-render-kmsro)
	m20env; MESA_LOADER_DRIVER_OVERRIDE=exynos
	LD_PRELOAD=$SHIM DRM_NAME_SHIM=exynos7580=exynos,panfrost7580=panfrost
	LIBGL_DEBUG=verbose
	export MESA_LOADER_DRIVER_OVERRIDE LD_PRELOAD DRM_NAME_SHIM LIBGL_DEBUG
	cell_run "$1" probe --gbm "$REND" ;;
m20-render-noovr)
	m20env; unset MESA_LOADER_DRIVER_OVERRIDE
	cell_run "$1" probe --gbm "$REND" ;;
m20-render-shim)
	m20env; unset MESA_LOADER_DRIVER_OVERRIDE
	LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost
	export LD_PRELOAD DRM_NAME_SHIM
	cell_run "$1" probe --gbm "$REND" --render ;;
sys-render-noovr)
	sysenv
	cell_run "$1" probe --gbm "$REND" ;;
m20-render-surfaceless-ovr-panfrost)
	m20env; EGL_PLATFORM=surfaceless
	export EGL_PLATFORM
	cell_run "$1" probe --default ;;
kmscube-baseline)
	skip_if_no_card "$1" || exit 0
	sysenv
	cell_run "$1" timeout "$TMO" "$KC" -D "$CARD" ;;
kmscube-shim)
	skip_if_no_card "$1" || exit 0
	sysenv; LD_PRELOAD=$SHIM DRM_NAME_SHIM=panfrost7580=panfrost
	LIBGL_DEBUG=verbose
	export LD_PRELOAD DRM_NAME_SHIM LIBGL_DEBUG
	cell_run "$1" timeout "$TMO" "$KC" -D "$CARD" ;;
all)
	for c in render-opens render-opens-shim opens \
		 sys-card-baseline sys-card-verbose sys-card-gbmbackend \
		 sys-card-ovr-exynos sys-card-ovr-panfrost sys-card-shim \
		 sys-render-ovr-panfrost sys-card-surfaceless \
		 sys-card-surfaceless-shim m20-card-ovr-panfrost \
		 m20-card-shim-kmsro m20-render-ovr-panfrost \
		 sys-render-gbm-ovr-exynos sys-render-gbm-ovr-exynos-shim \
		 sys-render-surfaceless sys-render-surfaceless-ovr-panfrost \
		 sys-render-surfaceless-shim \
		 sys-render-gbm-shim sys-render-gbm-ovr-exynos-shimver \
		 sys-render-noovr \
		 m20-render-kmsro m20-render-noovr m20-render-shim \
		 m20-render-surfaceless-ovr-panfrost \
		 kmscube-baseline kmscube-shim; do
		sh "$0" "$c"
	done ;;
*)
	echo "unknown cell: $1" >&2; exit 2 ;;
esac
