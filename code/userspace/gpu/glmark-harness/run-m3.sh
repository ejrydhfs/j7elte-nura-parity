#!/bin/sh
# M3 script for j7elte: capture GL stack state + FPS, judge hardware GL.
#
# Captures: `eglinfo -B`, `glxinfo -B`, `es2_info`, `glxgears` FPS,
# `glmark2-es2 --results fps` full log (EGL/GLES2 through X, 320x240,
# `-b :duration=2.0`), then parses the renderer string (must NOT be
# llvmpipe for PASS) and the FPS table vs THRESHOLDS.md baselines.
#
# Run on the device (X must be up):
#     DISPLAY=:0 ./run-m3.sh [output-dir]
#
# Logs go to [output-dir]/m3-<date>/ (default ./m3-logs/m3-<date>).
# Prints PASS:/FAIL:/WARN: lines; exits nonzero on FAIL.
# WARN = per-scene regression below threshold (does not fail the run).
# POSIX sh only (no bashisms).

OUT=${1:-./m3-logs}/m3-$(date +%Y%m%d-%H%M%S)
FAIL=0
mkdir -p "$OUT" || { echo "FAIL: cannot create $OUT"; exit 2; }

pass() { printf 'PASS: %s\n' "$1"; }
warn() { printf 'WARN: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1"; FAIL=1; }

DISP=${DISPLAY:-:0}
export DISPLAY="$DISP"

# --- 1. eglinfo -B ---
if command -v eglinfo >/dev/null 2>&1; then
	EGL_PLATFORM=surfaceless eglinfo -B >"$OUT/eglinfo-B.log" 2>&1
	pass "eglinfo -B captured ($OUT/eglinfo-B.log)"
else
	fail "eglinfo not installed"
fi

# --- 2. glxinfo -B ---
if command -v glxinfo >/dev/null 2>&1; then
	glxinfo -B >"$OUT/glxinfo-B.log" 2>&1
	pass "glxinfo -B captured ($OUT/glxinfo-B.log)"
else
	fail "glxinfo not installed"
fi

# --- 3. es2_info ---
if command -v es2_info >/dev/null 2>&1; then
	es2_info >"$OUT/es2_info.log" 2>&1
	pass "es2_info captured ($OUT/es2_info.log)"
else
	fail "es2_info not installed (mesa-demos)"
fi

# --- 4. Renderer string: must NOT be llvmpipe for PASS ---
RENDERER=""
for f in "$OUT/glxinfo-B.log" "$OUT/es2_info.log" "$OUT/eglinfo-B.log"; do
	if [ -f "$f" ]; then
		line=$(grep -im1 -E 'renderer string|GL_RENDERER|profile renderer' "$f" || true)
		if [ -n "$line" ]; then
			RENDERER="$line"
			break
		fi
	fi
done
printf '%s\n' "$RENDERER" >"$OUT/renderer.txt"
case "$RENDERER" in
	*llvmpipe*|*swrast*|*Softpipe*|"")
		fail "software renderer ($RENDERER) -- still llvmpipe, no hardware GL"
		;;
	*)
		pass "hardware renderer suspected ($RENDERER)"
		;;
esac

# --- 5. glxgears FPS @320x240 ---
if command -v glxgears >/dev/null 2>&1; then
	glxgears -info -geometry 320x240 >"$OUT/glxgears.log" 2>&1 &
	GID=$!
	sleep 17
	kill "$GID" 2>/dev/null
	wait "$GID" 2>/dev/null
	GFPS=$(grep -oE '[0-9]+\.[0-9]+ FPS' "$OUT/glxgears.log" | tail -n 3)
	if [ -n "$GFPS" ]; then
		echo "$GFPS" >"$OUT/glxgears-fps.txt"
		pass "glxgears FPS captured: $(echo "$GFPS" | tr '\n' ' ')(baseline llvmpipe 173-221 FPS; hardware should exceed)"
	else
		fail "glxgears produced no FPS lines (see $OUT/glxgears.log)"
	fi
else
	fail "glxgears not installed"
fi

# --- 6. glmark2-es2 full log ---
if command -v glmark2-es2 >/dev/null 2>&1; then
	glmark2-es2 -b :duration=2.0 --size 320x240 --results fps \
		>"$OUT/glmark2-es2.log" 2>&1
	pass "glmark2-es2 log captured ($OUT/glmark2-es2.log)"
else
	fail "glmark2-es2 not installed"
fi

# --- 7. FPS table vs baseline ---
# Baseline (llvmpipe, from docs/reports/gpu-verify.md section 3.3):
# "key|baseline". HEAVY=1 marks the four ~1-FPS CPU-bound scenes which
# must improve (or be explained), never merely hold.
BASELINES='
build-novbo|67|0
build-vbo|87|0
texture-nearest|224|0
texture-linear|232|0
texture-mipmap|244|0
shading-gouraud|39|0
shading-blinn|33|0
shading-phong|20|0
shading-cel|23|0
bump-highpoly|15|0
bump-normals|110|0
bump-height|46|0
effect2d-kernel0|43|0
effect2d-box|48|0
pulsar|149|0
desktop-blur|15|0
desktop-shadow|30|0
buffer-map|21|0
buffer-subdata|32|0
buffer-interleave|33|0
ideas|1|1
jellyfish|1|1
terrain|1|1
shadow|21|0
refract|1|1
conditionals-step|85|0
function-low|70|0
'

# Extract the FPS number for scene line $1 from the glmark log on stdin.
scene_fps() {
	grep -i -m1 -F "$1" | grep -oE 'FPS: [0-9]+' | grep -oE '[0-9]+' | tail -n 1
}

if [ -f "$OUT/glmark2-es2.log" ]; then
	# Map baseline keys to glmark2-es2 scene name fragments.
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
		fps=$(scene_fps "$pat" <"$OUT/glmark2-es2.log")
		if [ -z "$fps" ]; then
			warn "scene $key: no FPS line found (run may have been cut short)"
			continue
		fi
		# Floor = max(baseline*0.5, 5), integer math; heavy scenes exempt
		# from the floor but must improve over baseline.
		floor=$((base / 2))
		if [ "$floor" -lt 5 ]; then floor=5; fi
		if [ "$heavy" = "1" ]; then
			if [ "$fps" -gt "$base" ]; then
				pass "scene $key: $fps FPS > baseline $base (heavy scene improved)"
			else
				warn "scene $key: $fps FPS not above 1-FPS baseline -- needs explanation"
			fi
		elif [ "$fps" -ge "$floor" ]; then
			pass "scene $key: $fps FPS >= floor $floor (baseline $base)"
		else
			warn "scene $key: $fps FPS below floor $floor (baseline $base)"
		fi
	done <<EOF
$BASELINES
EOF
fi

echo "logs in $OUT"
if [ "$FAIL" -ne 0 ]; then
	echo "M3 RESULT: FAIL (software GL or missing tool)"
	exit 1
fi
echo "M3 RESULT: PASS (renderer non-llvmpipe; check WARN lines for regressions)"
exit 0
