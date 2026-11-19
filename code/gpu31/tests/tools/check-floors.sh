#!/bin/sh
# check-floors.sh - compare a glmark2 scene log against scenes.tbl floors.
# Usage: check-floors.sh SCENES_TBL GLMARK_LOG
# Prints PASS:/WARN: lines; the four heavy scenes must improve over 1 FPS,
# all others must reach max(baseline*0.5, 5 FPS). Exit 0 always (WARN-only).
# POSIX sh only.

TBL=${1:?usage: check-floors.sh SCENES_TBL GLMARK_LOG}
LOG=${2:?usage: check-floors.sh SCENES_TBL GLMARK_LOG}
FAIL_PAT_SEEN=0

scene_fps() {
	grep -i -m1 -F "$1" "$LOG" | grep -oE 'FPS: [0-9]+' | grep -oE '[0-9]+' | tail -n 1
}

while IFS='|' read -r key base heavy; do
	case "$key" in ''|\#*) continue ;; esac
	case "$key" in
		build-novbo) pat='use-vbo=false' ;;
		build-vbo) pat='use-vbo=true' ;;
		texture-nearest) pat='nearest' ;;
		texture-linear) pat='TextureLinear' ;;
		texture-mipmap) pat='TextureMipmap' ;;
		shading-gouraud) pat='ShadingGouraud' ;;
		shading-blinn) pat='ShadingBlinn' ;;
		shading-phong) pat='ShadingPhong' ;;
		shading-cel) pat='ShadingCel' ;;
		bump-highpoly) pat='BumpHigh' ;;
		bump-normals) pat='BumpNormals' ;;
		bump-height) pat='BumpHeight' ;;
		effect2d-kernel0) pat='Effect2DKernel' ;;
		effect2d-box) pat='Effect2DBox' ;;
		pulsar) pat='Pulsar' ;;
		desktop-blur) pat='DesktopBlur' ;;
		desktop-shadow) pat='DesktopShadow' ;;
		buffer-map) pat='BufferMap' ;;
		buffer-subdata) pat='BufferSubData' ;;
		buffer-interleave) pat='BufferInterleave' ;;
		ideas) pat='Ideas' ;;
		jellyfish) pat='Jellyfish' ;;
		terrain) pat='Terrain' ;;
		shadow) pat='Shadow' ;;
		refract) pat='Refract' ;;
		conditionals-step) pat='Conditionals' ;;
		function-low) pat='FunctionLow' ;;
	esac
	fps=$(scene_fps "$pat")
	if [ -z "$fps" ]; then
		echo "WARN: scene $key: no FPS line found (run may have been cut short)"
		continue
	fi
	floor=$((base / 2))
	if [ "$floor" -lt 5 ]; then floor=5; fi
	if [ "$heavy" = "1" ]; then
		if [ "$fps" -gt "$base" ]; then
			echo "PASS: scene $key: $fps FPS > baseline $base (heavy scene improved)"
		else
			echo "WARN: scene $key: $fps FPS not above $base-FPS baseline -- needs explanation"
		fi
	elif [ "$fps" -ge "$floor" ]; then
		echo "PASS: scene $key: $fps FPS >= floor $floor (baseline $base)"
	else
		echo "WARN: scene $key: $fps FPS below floor $floor (baseline $base)"
		FAIL_PAT_SEEN=1
	fi
done <"$TBL"
exit 0
