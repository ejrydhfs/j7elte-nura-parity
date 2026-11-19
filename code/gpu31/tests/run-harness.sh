#!/bin/sh
# run-harness.sh - unified GL pass/fail harness for j7elte (Exynos 7580 / Mali-T720).
#
# Gates (see docs/reports/gpu31-tests.md):
#   G1 renderer string (glxinfo -B / es2_info / eglinfo -B)
#   G2 glmark2-es2 score floor (scenes.tbl via tools/check-floors.sh, WARN-only)
#   G3 2D surface (xwd -root + tools/parse-xwd.py)
#   G4 GPU utilisation sanity (tools/get-gpu-state.sh before/after load)
#   G5 single-command verdict artifact (verdict.json + verdict.txt + RESULT)
#
# Runs green on llvmpipe today (--expect software, the default for
# --expect auto on a software renderer) and is ready for hardware
# (--expect hardware flips G1 to require a non-software renderer).
#
# Usage:
#   DISPLAY=:0 ./run-harness.sh [--expect software|hardware|auto]
#       [--out DIR] [--geometry WxH] [--min-distinct N]
#       [--duration S] [--gears-secs N] [--glmark-timeout S]
#       [--skip-gears] [--skip-glmark] [--skip-2d] [--skip-gpu]
#
# Device rules: run the whole script inside one host
#   flock /tmp/gpsdev.lock -c '...' hold (<120 s); never reboot;
#   big logs default to /data on the device (root fs is ~94% full).
# POSIX sh only.
set -u

HERE=$(dirname "$0")
TOOLS=${HARNESS_TOOLS:-"$HERE/tools"}
SCENES=${HARNESS_SCENES:-"$HERE/scenes.tbl"}
GETSTATE="$TOOLS/get-gpu-state.sh"
CHECKFLOORS="$TOOLS/check-floors.sh"
PARSEXWD="$TOOLS/parse-xwd.py"

EXPECT=auto
OUT=""
GEOMETRY=""
MIN_DISTINCT=100
DURATION=2.0
GEARS_SECS=15
GLMARK_TIMEOUT=110
SKIP_GEARS=0 SKIP_GLMARK=0 SKIP_2D=0 SKIP_GPU=0 SKIP_PIGLIT=0

while [ $# -gt 0 ]; do
	case "$1" in
		--expect) EXPECT=${2:?missing value}; shift 2 ;;
		--expect=*) EXPECT=${1#--expect=}; shift ;;
		--out) OUT=${2:?missing value}; shift 2 ;;
		--out=*) OUT=${1#--out=}; shift ;;
		--geometry) GEOMETRY=${2:?missing value}; shift 2 ;;
		--geometry=*) GEOMETRY=${1#--geometry=}; shift ;;
		--min-distinct) MIN_DISTINCT=${2:?missing value}; shift 2 ;;
		--min-distinct=*) MIN_DISTINCT=${1#--min-distinct=}; shift ;;
		--duration) DURATION=${2:?missing value}; shift 2 ;;
		--duration=*) DURATION=${1#--duration=}; shift ;;
		--gears-secs) GEARS_SECS=${2:?missing value}; shift 2 ;;
		--gears-secs=*) GEARS_SECS=${1#--gears-secs=}; shift ;;
		--glmark-timeout) GLMARK_TIMEOUT=${2:?missing value}; shift 2 ;;
		--glmark-timeout=*) GLMARK_TIMEOUT=${1#--glmark-timeout=}; shift ;;
		--skip-piglit) SKIP_PIGLIT=1; shift ;;
		--piglit-subset) PIGSUBSET=${2:?missing value}; shift 2 ;;
		--piglit-subset=*) PIGSUBSET=${1#--piglit-subset=}; shift ;;
		--skip-load) SKIP_GEARS=1; SKIP_GLMARK=1; shift ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		*) echo "run-harness: unknown argument: $1" >&2; exit 2 ;;
	esac
done
case "$EXPECT" in software|hardware|auto) ;; *)
	echo "run-harness: --expect must be software|hardware|auto" >&2; exit 2 ;; esac

DISP=${DISPLAY:-:0}
export DISPLAY="$DISP"

if [ -z "$OUT" ]; then
	if [ "$(uname -m)" = "aarch64" ] && [ -d /data ]; then
		OUT=/data/glmark-harness/h-$(date +%Y%m%d-%H%M%S)
	else
		OUT=./glmark-harness-logs/h-$(date +%Y%m%d-%H%M%S)
	fi
fi
if ! mkdir -p "$OUT"; then echo "FAIL: cannot create $OUT"; exit 2; fi
LOG="$OUT/harness.log"
: >"$LOG"

FAIL=0
G_RENDERER=FAIL G_GLMARK=FAIL G_2D=FAIL G_GPU=FAIL
RENDERER_RAW="" RENDERER_CLASS=unknown
NOTE_2D="" NOTE_GPU="" NOTE_GLMARK=""

log() { printf '%s\n' "$1" | tee -a "$LOG"; }
pass() { log "PASS: $1"; }
warn() { log "WARN: $1"; }
fail() { log "FAIL: $1"; FAIL=1; }

log "== glmark-harness $EXPECT out=$OUT disp=$DISP =="

# --- G4a: GPU state before load ---
if [ "$SKIP_GPU" -eq 0 ] && [ -x "$GETSTATE" ]; then
	sh "$GETSTATE" >"$OUT/gpu-state-before.txt" 2>&1 \
		&& pass "gpu state before captured" \
		|| { warn "get-gpu-state.sh before failed (continuing)"; : >"$OUT/gpu-state-before.txt"; }
else
	: >"$OUT/gpu-state-before.txt"
	[ "$SKIP_GPU" -eq 1 ] && NOTE_GPU="skipped by flag"
fi

# --- G1: renderer probes ---
if command -v glxinfo >/dev/null 2>&1; then
	glxinfo -B >"$OUT/glxinfo-B.log" 2>&1 \
		&& pass "glxinfo -B captured" \
		|| fail "glxinfo -B failed (see $OUT/glxinfo-B.log)"
else
	fail "glxinfo not installed"
fi
if command -v es2_info >/dev/null 2>&1; then
	es2_info >"$OUT/es2_info.log" 2>&1 \
		&& pass "es2_info captured" \
		|| fail "es2_info failed (see $OUT/es2_info.log)"
else
	fail "es2_info not installed (mesa-demos)"
fi
if command -v eglinfo >/dev/null 2>&1; then
	EGL_PLATFORM=surfaceless eglinfo -B >"$OUT/eglinfo-B.log" 2>&1 \
		&& pass "eglinfo -B captured" \
		|| warn "eglinfo -B failed (see $OUT/eglinfo-B.log)"
else
	warn "eglinfo not installed (renderer check uses glxinfo/es2_info)"
fi

for f in "$OUT/glxinfo-B.log" "$OUT/es2_info.log" "$OUT/eglinfo-B.log"; do
	[ -f "$f" ] || continue
	line=$(grep -im1 -E 'renderer string|GL_RENDERER|profile renderer' "$f" || true)
	if [ -n "$line" ]; then RENDERER_RAW="$line"; break; fi
done
printf '%s\n' "$RENDERER_RAW" >"$OUT/renderer.txt"

case "$RENDERER_RAW" in
	*llvmpipe*|*swrast*|*SWRast*|*Softpipe*|*softpipe*)
		RENDERER_CLASS=software ;;
	"")	RENDERER_CLASS=unknown ;;
	*Mali*|*mali*|*Panfrost*|*panfrost*|*ARM*|*Exynos*|*exynos*|*kbase*|*lima*)
		RENDERER_CLASS=hardware ;;
	*)	RENDERER_CLASS=hardware-unknown ;;
esac
log "renderer_class=$RENDERER_CLASS raw=[$RENDERER_RAW]"

case "$EXPECT" in
	software)
		case "$RENDERER_CLASS" in
			software) pass "renderer is software as expected ($RENDERER_RAW)"; G_RENDERER=PASS ;;
			*) fail "renderer is '$RENDERER_CLASS', expected software" ;;
		esac ;;
	hardware)
		case "$RENDERER_CLASS" in
			hardware) pass "hardware renderer ($RENDERER_RAW)"; G_RENDERER=PASS ;;
			hardware-unknown)
				pass "non-software renderer ($RENDERER_RAW)"
				warn "renderer string not a known HW id -- record driver name"; G_RENDERER=PASS ;;
			*) fail "software/unknown renderer ($RENDERER_RAW) -- no hardware GL" ;;
		esac ;;
	auto)
		case "$RENDERER_CLASS" in
			software|hardware) pass "renderer $RENDERER_CLASS ($RENDERER_RAW)"; G_RENDERER=PASS ;;
			hardware-unknown)
				pass "renderer ($RENDERER_RAW)"
				warn "unrecognised non-software renderer string"; G_RENDERER=PASS ;;
			*) fail "empty renderer string (no GL probe produced a renderer)" ;;
		esac ;;
esac

# --- G2a: glxgears reference (informational FPS, capture health is the gate) ---
if [ "$SKIP_GEARS" -eq 1 ]; then
	G_GEARS=SKIP
	log "glxgears: SKIP by flag"
elif command -v glxgears >/dev/null 2>&1; then
	if command -v timeout >/dev/null 2>&1; then
		timeout $((GEARS_SECS + 5)) glxgears -info -geometry 320x240 \
			>"$OUT/glxgears.log" 2>&1 &
	else
		glxgears -info -geometry 320x240 >"$OUT/glxgears.log" 2>&1 &
	fi
	GID=$!
	sleep "$GEARS_SECS"
	kill "$GID" 2>/dev/null
	wait "$GID" 2>/dev/null
	GFPS=$(grep -oE '[0-9]+\.[0-9]+ FPS' "$OUT/glxgears.log" | tail -n 3)
	if [ -z "$GFPS" ]; then
		# vblank-synced runs print "N frames in 5.0 seconds = M FPS" lines.
		GFPS=$(grep -oE '[0-9]+ frames in [0-9.]+ seconds = [0-9.]+ FPS' "$OUT/glxgears.log" | tail -n 3)
	fi
	if [ -n "$GFPS" ]; then
		printf '%s\n' "$GFPS" >"$OUT/glxgears-fps.txt"
		pass "glxgears FPS: $(printf '%s' "$GFPS" | tr '\n' ' ')"
	else
		fail "glxgears produced no FPS lines (see $OUT/glxgears.log)"
	fi
else
	fail "glxgears not installed"
fi

# --- G2b: glmark2-es2 scored sweep + floors ---
if [ "$SKIP_GLMARK" -eq 1 ]; then
	G_GLMARK=SKIP; NOTE_GLMARK="skipped by flag"
	log "glmark2-es2: SKIP by flag"
elif command -v glmark2-es2 >/dev/null 2>&1; then
	if command -v timeout >/dev/null 2>&1; then
		timeout "$GLMARK_TIMEOUT" glmark2-es2 -s 320x240 -b ":duration=$DURATION" \
			--results fps >"$OUT/glmark2-es2.log" 2>&1
		rc=$?
		[ "$rc" -eq 124 ] && warn "glmark2-es2 hit the ${GLMARK_TIMEOUT}s timeout (partial scene table; floors still checked)"
	else
		glmark2-es2 -s 320x240 -b ":duration=$DURATION" --results fps \
			>"$OUT/glmark2-es2.log" 2>&1
	fi
	if [ -x "$CHECKFLOORS" ] && [ -f "$SCENES" ]; then
		sh "$CHECKFLOORS" "$SCENES" "$OUT/glmark2-es2.log" >"$OUT/floors.txt" 2>&1
		while IFS= read -r line; do log "$line"; done <"$OUT/floors.txt"
	else
		fail "floor checker missing ($CHECKFLOORS / $SCENES)"
	fi
	NFPS=$(grep -cE 'FPS: [0-9]+' "$OUT/glmark2-es2.log" 2>/dev/null || true)
	if [ "${NFPS:-0}" -ge 5 ]; then
		pass "glmark2-es2 captured ($NFPS FPS lines; floors are WARN-only)"
		G_GLMARK=PASS
	else
		fail "glmark2-es2 produced no usable FPS lines (see $OUT/glmark2-es2.log)"
	fi
else
	fail "glmark2-es2 not installed"
fi

# --- G3: 2D surface via xwd ---
if [ "$SKIP_2D" -eq 1 ]; then
	G_2D=SKIP; NOTE_2D="skipped by flag"
	log "2D surface: SKIP by flag"
elif ! command -v xwd >/dev/null 2>&1; then
	fail "xwd not installed (no 2D surface evidence)"
elif [ ! -x "$PARSEXWD" ] || ! command -v python3 >/dev/null 2>&1; then
	fail "parse-xwd.py/python3 missing (no 2D surface evidence)"
else
	if DISPLAY=$DISP xwd -root -silent -out "$OUT/root.xwd" 2>"$OUT/xwd.err"; then
		if python3 "$PARSEXWD" "$OUT/root.xwd" >"$OUT/xwd-parse.txt" 2>"$OUT/xwd-parse.err"; then
			while IFS= read -r line; do log "xwd: $line"; done <"$OUT/xwd-parse.txt"
			W=$(grep -m1 '^width=' "$OUT/xwd-parse.txt" | cut -d= -f2)
			H=$(grep -m1 '^height=' "$OUT/xwd-parse.txt" | cut -d= -f2)
			D=$(grep -m1 '^distinct=' "$OUT/xwd-parse.txt" | cut -d= -f2)
			S=$(grep -m1 '^sampled=' "$OUT/xwd-parse.txt" | cut -d= -f2)
			EXP="$GEOMETRY"
			if [ -z "$EXP" ] && [ -e /dev/fb0 ] && [ "$(uname -m)" = "aarch64" ]; then
				EXP=720x1280
			fi
			if [ -n "$EXP" ] && [ "${W}x${H}" != "$EXP" ]; then
				fail "2D surface geometry ${W}x${H} != expected $EXP"
			elif [ "${D:-0}" -lt "$MIN_DISTINCT" ] 2>/dev/null; then
				fail "2D surface degenerate: distinct=${D:-?} < $MIN_DISTINCT (sampled=${S:-?})"
			else
				pass "2D surface ${W}x${H} distinct=${D} sampled=${S}"
				G_2D=PASS
			fi
		else
			fail "xwd parse failed (see $OUT/xwd-parse.err)"
		fi
	else
		fail "xwd capture failed (see $OUT/xwd.err; is X up on $DISP?)"
	fi
fi

# --- G4b: GPU state after load + sanity ---
if [ "$SKIP_GPU" -eq 1 ]; then
	G_GPU=SKIP; [ -z "$NOTE_GPU" ] && NOTE_GPU="skipped by flag"
	log "gpu utilisation: SKIP by flag"
elif [ ! -x "$GETSTATE" ]; then
	fail "get-gpu-state.sh missing"
else
	sh "$GETSTATE" >"$OUT/gpu-state-after.txt" 2>&1 \
		|| warn "get-gpu-state.sh after failed (sanity best-effort)"
	getv() { grep -m1 "^$2=" "$1" 2>/dev/null | cut -d= -f2-; }
	UB=$(getv "$OUT/gpu-state-before.txt" utilization)
	UA=$(getv "$OUT/gpu-state-after.txt" utilization)
	TB=$(getv "$OUT/gpu-state-before.txt" tis_total)
	TA=$(getv "$OUT/gpu-state-after.txt" tis_total)
	log "gpu: utilization before=[$UB] after=[$UA] tis before=[$TB] after=[$TA]"
	if [ "$UB" = "MISSING" ] && [ "$UA" = "MISSING" ]; then
		G_GPU=SKIP; NOTE_GPU="no kbase sysfs (non-target host?)"
		log "gpu utilisation: SKIP ($NOTE_GPU)"
	else
		ok=1
		case "$UA" in ''|*[!0-9]*) warn "gpu utilization unparsable: [$UA]"; ok=0 ;; esac
		case "$TA" in ''|*[!0-9]*) warn "tis_total unparsable: [$TA]"; ok=0 ;; esac
		if [ "$ok" -eq 1 ] && [ "${TB:-0}" -gt "${TA:-0}" ] 2>/dev/null; then
			warn "tis_total decreased ($TB -> $TA; counter reset?)"
		fi
		if [ "$ok" -eq 1 ]; then
			case "$RENDERER_CLASS" in
				software)
					if [ "$UA" = "0" ] && [ "${TB:-0}" = "${TA:-0}" ]; then
						pass "gpu idle during software render (util=0, tis unchanged)"
					else
						warn "gpu shows activity during software render (util=$UA tis $TB->$TA)"
					fi
					G_GPU=PASS ;;
				hardware|hardware-unknown)
					if [ "$UA" != "0" ] || [ "${TA:-0}" -gt "${TB:-0}" ] 2>/dev/null; then
						pass "gpu active during hardware render (util=$UA tis $TB->$TA)"
					else
						warn "gpu idle during hardware render (util=0, tis unchanged) -- needs explanation"
					fi
					G_GPU=PASS ;;
				*)
					warn "renderer unknown; gpu sanity informational only"
					G_GPU=PASS ;;
			esac
		else
			fail "gpu state unreadable while sysfs present"
		fi
	fi
fi
# --- G4c: piglit smoke (WARN-only informational; full 60-test runs use mesatests/run-piglit-subset.sh) ---
# Runs only when piglit exists on this host (device: /data/pigroot). Else SKIP (hosts stay green).
# Replays 10 baseline-passing binaries directly (-auto -fbo), one process each, short timeout.
PIG_RUN=0 PIG_PASS=0 PIG_FAIL=0 PIG_SKIPN=0
if [ "$SKIP_PIGLIT" -eq 1 ]; then
	log "piglit smoke: SKIP by flag"
elif [ ! -d /data/pigroot/usr/lib/piglit ] && [ ! -d /usr/lib/piglit ]; then
	log "piglit smoke: SKIP (no piglit install on this host; device runs use mesatests/run-piglit-subset.sh)"
else
	PIGBIN=/data/pigroot/usr/lib/piglit/bin
	[ -d "$PIGBIN" ] || PIGBIN=/usr/lib/piglit/bin
	if [ "$RENDERER_CLASS" = software ]; then
		export LIBGL_ALWAYS_SOFTWARE=1
	fi
	if [ -d /data/pigroot/usr/lib/piglit ]; then
		export LD_LIBRARY_PATH="/data/pigroot/usr/lib/piglit/lib:/data/pigroot/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
		export PIGLIT_SOURCE_DIR=/data/pigroot/usr/lib/piglit
	fi
	: >"$OUT/piglit-smoke.txt"
	for t in glsl-vs-arrays glsl-fs-loop max-samplers tex3d-npot \
			'cubemap npot' fbo-generatemipmap-npot \
			gl-1.0-scissor-clear gl-1.0-scissor-bitmap \
			hiz-depth-read-fbo-d24-s0 object-namespace-pollution; do
		b=""
		for cand in "$PIGBIN/$t" "$PIGBIN/$(printf '%s' "$t" | tr ' ' '-')"; do
			[ -x "$cand" ] && b=$cand && break
		done
		[ -n "$b" ] || { PIG_SKIPN=$((PIG_SKIPN+1)); echo "skip | $t | no binary" >>"$OUT/piglit-smoke.txt"; continue; }
		PIG_RUN=$((PIG_RUN+1))
		if timeout 25 "$b" -auto -fbo >"$OUT/piglit-smoke-$PIG_RUN.log" 2>&1; then
			PIG_PASS=$((PIG_PASS+1)); echo "pass | $t" >>"$OUT/piglit-smoke.txt"
		else
			PIG_FAIL=$((PIG_FAIL+1)); echo "fail | $t | rc=$? (see piglit-smoke-$PIG_RUN.log)" >>"$OUT/piglit-smoke.txt"
		fi
	done
	log "piglit smoke: $PIG_PASS pass / $PIG_FAIL fail / $PIG_SKIPN skip of $PIG_RUN run"
	warn "piglit smoke is WARN-only (llvmpipe baseline 51/5/4; full 60-test diff via mesatests/run-piglit-subset.sh compare)"
fi



# --- G5: verdict artifact ---
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
if [ "$FAIL" -ne 0 ]; then RESULT=FAIL; else RESULT=PASS; fi
[ "$G_RENDERER" = "SKIP" ] && G_RENDERER=PASS
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
{
	printf '{\n'
	printf '  "schema": "glmark-harness/1",\n'
	printf '  "stamp": "%s",\n' "$STAMP"
	printf '  "expect": "%s",\n' "$EXPECT"
	printf '  "renderer": {"raw": "%s", "class": "%s"},\n' "$(esc "$RENDERER_RAW")" "$RENDERER_CLASS"
	printf '  "gates": {\n'
	printf '    "renderer": "%s",\n' "$G_RENDERER"
	printf '    "glmark": "%s",\n' "$G_GLMARK"
	printf '    "surface_2d": "%s",\n' "$G_2D"
	printf '    "gpu_util": "%s",\n' "$G_GPU"
	printf '    "piglit_smoke": "WARN"\n'
	printf '  },\n'
	printf '  "piglit": {"run": %d, "pass": %d, "fail": %d, "skip": %d},\n' "$PIG_RUN" "$PIG_PASS" "$PIG_FAIL" "$PIG_SKIPN"
	printf '  "notes": {"glmark": "%s", "surface_2d": "%s", "gpu": "%s"},\n' \
		"$(esc "$NOTE_GLMARK")" "$(esc "$NOTE_2D")" "$(esc "$NOTE_GPU")"
} >"$OUT/verdict.json"
{
	echo "glmark-harness verdict: $RESULT"
	echo "stamp: $STAMP  expect: $EXPECT"
	echo "renderer: [$RENDERER_RAW] class=$RENDERER_CLASS gate=$G_RENDERER"
	echo "glmark: gate=$G_GLMARK $NOTE_GLMARK"
	echo "surface_2d: gate=$G_2D $NOTE_2D"
	echo "gpu_util: gate=$G_GPU $NOTE_GPU"
	echo "piglit_smoke: run=$PIG_RUN pass=$PIG_PASS fail=$PIG_FAIL skip=$PIG_SKIPN (WARN-only)"
} >"$OUT/verdict.txt"
printf '%s\n' "$RESULT" >"$OUT/RESULT"
log "logs in $OUT"
log "$RESULT (verdict.json + verdict.txt + RESULT)"

if [ "$RESULT" = "FAIL" ]; then exit 1; fi
exit 0
