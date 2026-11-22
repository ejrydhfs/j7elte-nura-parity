#!/bin/sh
# gpu31 latency matrix: 256x256 gfdemo, per-frame glFinish.
# Reproduces docs/reports/gpu31-latency.md §4. Run ON the phone as root
# (needs the p7580 debugfs trace_arm knob). Push iotrace.so first.
#
#   sh run.sh            # full matrix
#   B=... sh run.sh      # alternate Mesa build dir
set -x
B=${B:-/data/mesabuild/mesa20-b}
K=${K:-/sys/kernel/debug/p7580}
APP=${APP:-/home/user/gfdemo/gfdemo}
SO=${SO:-/tmp/iotrace.so}

runf() { # frames noread tag
	dmesg -c >/dev/null
	echo "-- $3"
	t0=$(date +%s%N)
	env GFDEMO_BENCH=1 GFDEMO_PX=256 GFDEMO_FRAMES=$1 ${2:+GFDEMO_NOREAD=1} \
		LD_PRELOAD="$SO" IOTRACE=/tmp/iotrace-$3.log \
		LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri \
		LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri \
		MESA_LOADER_DRIVER_OVERRIDE=panfrost EGL_PLATFORM=surfaceless \
		timeout 90 "$APP"
	t1=$(date +%s%N)
	echo "wall_ms=$(( (t1 - t0) / 1000000 ))"
	echo -n "faults(timeout+err lines): "
	dmesg | grep -cE "p7580_job: (TIMEOUT|ERR)"
	dmesg | grep -E "p7580_job: (TIMEOUT|dispatch|irq-enter|ERR )" | head -12
}

echo 0 > "$K/trace_arm" 2>/dev/null
echo 0 > "$K/iova_arm" 2>/dev/null
runf 300 1 clean-1
runf 300 1 clean-2
# Provoke the fault-cascade signature: bounded trace on, 4 frames.
echo 400 > "$K/trace_arm"
runf 4 1 provoke-traced
echo 0 > "$K/trace_arm"
runf 300 1 postfault
runf 30 "" readback
