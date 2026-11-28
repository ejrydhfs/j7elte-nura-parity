#!/bin/sh
# /data/mesa203-run-smoke.sh - Mesa 20.3.5 (panfrost/kmsro) smoke test, j7elte.
#
#   sh /data/mesa203-run-smoke.sh                 # surfaceless + GBM, renderD128
#   sh /data/mesa203-run-smoke.sh surfaceless     # EGL_MESA_platform_surfaceless
#   sh /data/mesa203-run-smoke.sh gbm             # gbm_surface on renderD128
#   rootshell /bin/sh /data/mesa203-run-smoke.sh kms
#                                                 # card0 -> exynos_dri (kmsro)
#                                                 # -> renderD128, needs root
#
# Runs a full-screen vertex+fragment shader job into an FBO (plus a
# gbm_surface + eglSwapBuffers in gbm mode) and prints EGL/GL strings.
# Touches no CRTC: nothing is displayed and the running session is untouched.
set -u

case "${1:-both}" in
kms)
	shift
	. /data/mesa203-env.sh kms
	exec nice -n 5 /data/mesa203-build/mesa203-smoke gbm /dev/dri/card0
	;;
*)
	. /data/mesa203-env.sh render
	EGL_PLATFORM=surfaceless
	export EGL_PLATFORM
	exec nice -n 5 /data/mesa203-build/mesa203-smoke "${1:-both}" \
		"${2:-/dev/dri/renderD128}"
	;;
esac
