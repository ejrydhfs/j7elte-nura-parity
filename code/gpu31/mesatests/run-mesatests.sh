#!/bin/sh
# gpu31-mesatests runner — j7elte (Exynos 7580, Mali-T720), kernel 3.10.x, Nura Alpine aarch64.
# Runs on the DEVICE as root. No reboot, no flash, no kernel build.
# Big outputs go to /data/mesatests (root fs is ~94% full).
#
# Usage (on device):  sh /data/mesatests/run-mesatests.sh 2>&1 | tee /data/mesatests/run.log
# Re-run for hardware later: same script, results land in the same dir with
# a RENDERER tag; compare llvmpipe-vs-hardware with diff-fps.sh.
#
# What it does:
#  1. renderer probes (glxinfo -B, eglinfo -B, es2_info) + kernel/uname stamp
#  2. glxgears timed run (llvmpipe baseline reference)
#  3. glmark2-es2 + glmark2 full scene sweep, per-scene FPS, to CSV
#  4. piglit representative subset (shader, texture, depth) if piglit present
#     under /data/pigroot (extracted apk, NOT installed to / — root fs full).
#
# POSIX sh only. Each device command that calls this script MUST itself run
# under host `flock /tmp/gpsdev.lock`.

OUT=${OUT:-/data/mesatests}
PIGROOT=${PIGROOT:-/data/pigroot}
PIGLIT_BIN=$PIGROOT/usr/bin/piglit
PIGLIT_LIB=$PIGROOT/usr/lib/piglit
DISP=${DISPLAY:-:0}
DUR=${DUR:-2.0}

mkdir -p "$OUT"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
echo "== mesatests run $STAMP =="
uname -a | tee "$OUT/uname.log"
cat /sys/devices/11400000.mali/gpuinfo 2>/dev/null | tee "$OUT/gpuinfo.log"
ls /dev/dri 2>&1 | tee "$OUT/dri.log"
ls -l /dev/mali0 /dev/fb0 2>&1 | tee "$OUT/nodes.log"

# --- 1. renderer probes ---
echo "== renderer probes =="
DISPLAY=$DISP LIBGL_ALWAYS_SOFTWARE=1 glxinfo -B > "$OUT/glxinfo-B.log" 2>&1; echo "glxinfo rc=$?"
EGL_PLATFORM=surfaceless eglinfo -B > "$OUT/eglinfo-B.log" 2>&1; echo "eglinfo rc=$?"
DISPLAY=$DISP es2_info > "$OUT/es2info.log" 2>&1; echo "es2_info rc=$?"
grep -hE 'renderer|Renderer|Version|Accelerated|profile' "$OUT/glxinfo-B.log" "$OUT/es2info.log" 2>/dev/null

# --- 2. glxgears timed ---
echo "== glxgears (15 s) =="
DISPLAY=$DISP timeout 18 glxgears -info -geometry 320x240 > "$OUT/glxgears.log" 2>&1 &
wait $!
grep -E 'FPS|GL_RENDERER' "$OUT/glxgears.log" | tail -6

# --- 3. glmark2 sweeps ---
echo "== glmark2-es2 sweep =="
DISPLAY=$DISP timeout 110 glmark2-es2 -s 320x240 -b :duration=$DUR --results fps > "$OUT/glmark2-es2.log" 2>&1
echo "glmark2-es2 rc=$?"
grep -E 'FPS|Score|Error' "$OUT/glmark2-es2.log" | tail -40
echo "== glmark2 (GLX) sweep =="
DISPLAY=$DISP timeout 110 glmark2 -s 320x240 -b :duration=$DUR --results fps > "$OUT/glmark2.log" 2>&1
echo "glmark2 rc=$?"
grep -E 'FPS|Score|Error' "$OUT/glmark2.log" | tail -40

# per-scene CSV for later hardware diff
for f in "$OUT/glmark2-es2.log" "$OUT/glmark2.log"; do
    base=$(basename "$f" .log)
    awk '/^\[.*\]/{scene=$0} /FPS:/{n=split($0,a,":"); print scene " => " $0}' "$f" > "$OUT/$base-scenes.txt" 2>/dev/null
done

# --- 4. piglit subset ---
if [ -x "$PIGLIT_BIN" ] && [ -d "$PIGLIT_LIB/tests" ]; then
    echo "== piglit subset =="
    SUBSET_LIST=$(dirname "$0")/piglit-subset.txt
    [ -f "$SUBSET_LIST" ] || SUBSET_LIST="$OUT/piglit-subset.txt"
    if [ -f "$SUBSET_LIST" ]; then
        N=$(grep -vc '^#' "$SUBSET_LIST")
        echo "subset file: $SUBSET_LIST ($N tests)"
        PYTHONPATH=$PIGLIT_LIB:$PIGLIT_LIB/framework \
        LD_LIBRARY_PATH=$PIGROOT/usr/lib:$LD_LIBRARY_PATH \
        DISPLAY=$DISP timeout 100 \
          python3 "$PIGLIT_BIN" run gpu31-quick "$OUT/piglit-results" \
            --test-list "$SUBSET_LIST" -p gb -c 2>&1 | tail -25 \
          | tee "$OUT/piglit-run.log"
        python3 "$PIGLIT_BIN" summary console "$OUT/piglit-results" > "$OUT/piglit-summary.txt" 2>&1
        tail -15 "$OUT/piglit-summary.txt"
    else
        echo "no subset list ($SUBSET_LIST); skipping piglit run (see report for manual cmd)"
    fi
else
    echo "piglit not present under $PIGROOT; skipping (shader/texture/depth covered by glmark2 + deqp-notes in report)"
fi

echo "== done $STAMP; outputs in $OUT =="
ls -la "$OUT"
