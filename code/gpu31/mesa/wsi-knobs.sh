#!/bin/sh
# wsi-knobs.sh - exact Mesa knobs for the panfrost-on-renderD128 HW path.
#
# Rationale (decided with G31_Mesa scope split — this file asserts the
# CLIENT/WINDOW-SYSTEM side only):
#   * MESA_LOADER_DRIVER_OVERRIDE=panfrost pins the DRI loader to the T720
#     Gallium driver for the render fd (G31_Mesa 0005 compat patch absorbs
#     modern-Mesa extra-param/MADVISE probes: the v5.2 kernel answers
#     -EINVAL and Mesa falls back — never widen the kernel UAPI).
#   * LIBGL_ALWAYS_SOFTWARE must be UNSET for HW GL (j7elte-gpu-soft ships
#     it =1 for the llvmpipe era; keep the package installed until M3 PASS).
#   * No DRI_PRIME (single GPU), no PAN_MESA_DEBUG by default.
#
# Usage:
#   ./wsi-knobs.sh [--print]        # print the knob exports to stdout
#   . ./wsi-knobs.sh               # apply them in the current shell
#   ./wsi-knobs.sh --check         # assert environment matches HW expectation
#
# POSIX sh only. Exit: 0 ready-or-printed, 3 not-HW-ready (check mode only),
# 1 usage error.
set -u

print_knobs() {
	printf 'unset LIBGL_ALWAYS_SOFTWARE\n'
	printf 'export MESA_LOADER_DRIVER_OVERRIDE=panfrost\n'
	printf 'unset DRI_PRIME\n'
	printf 'unset PAN_MESA_DEBUG\n'
	printf 'unset EGL_LOG_LEVEL LIBGL_DEBUG\n'
}

case "${1:-}" in
	--print)
		print_knobs
		exit 0
		;;
	--check)
		fail=0
		if [ "${LIBGL_ALWAYS_SOFTWARE:-}" = "1" ]; then
			printf 'NOT-READY: LIBGL_ALWAYS_SOFTWARE=1 forces llvmpipe\n' >&2
			fail=1
		fi
		if [ ! -e "${WSI_NODE:-/dev/dri/renderD128}" ]; then
			printf 'NOT-READY: no render node (%s)\n' "${WSI_NODE:-/dev/dri/renderD128}" >&2
			fail=1
		fi
		if [ "$fail" -ne 0 ]; then
			exit 3
		fi
		printf 'READY: HW WSI knobs set, render node present\n'
		exit 0
		;;
	''|--)
		# sourced or bare run: apply.
		unset LIBGL_ALWAYS_SOFTWARE
		MESA_LOADER_DRIVER_OVERRIDE=panfrost
		export MESA_LOADER_DRIVER_OVERRIDE
		unset DRI_PRIME
		unset PAN_MESA_DEBUG
		unset EGL_LOG_LEVEL LIBGL_DEBUG
		if [ "${1:-}" = "--" ]; then
			:
		elif [ $# -eq 0 ]; then
			:
		fi
		;;
	-h|--help)
		sed -n '2,20p' "$0"
		exit 0
		;;
	*)
		printf 'usage: %s [--print|--check]\n' "$0" >&2
		exit 1
		;;
esac
