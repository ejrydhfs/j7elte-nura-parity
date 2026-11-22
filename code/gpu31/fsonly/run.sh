#!/bin/sh
# run.sh - device-side fragment-only probe matrix (gpu31-fsonly).
#
# Run on the phone (as root, so dmesg/DEBUG_READ_BO work), under the repo
# device lock, e.g.:
#
#   ssh user@172.16.42.1 'rootshell sh /home/user/fsonly/run.sh all'
#
# One case per invocation is also accepted: `run.sh ctrl|prefill|nofrag|
# presleep|recheck|defectb|small`.  Results (stdout, hook census logs) are
# appended under $OUT; dmesg is cleared before each case and the matching
# p7580 lines are printed after it.
#
# Expected on int38+ (Defect B dcache fix in the kernel, FS ISA page intact):
#   ctrl     no CPU prefill -> tiler bins nothing -> all clear, FS_WROTE=0
#   prefill  CPU big triangle -> magenta inside it -> FS_WROTE=1 (the result)
#   nofrag   prefill + fragment submit refused -> all clear (attribution)
#   small    CPU tiny triangle -> magenta only in tile 0 (tile locality)
#   defectb  prefill without the Mesa ISA workaround -> also FS_WROTE=1 on
#            int38; on a pre-fix kernel it shows the 0x52 + no pixels
#   presleep/recheck  pre-int38 Mesa workarounds for the FS ISA page; kept so
#            the same matrix separates Defect B on older kernels.
#
# Inputs on the device:
#   /home/user/fsonly/fsonly            the probe (fsonly.c, built on device)
#   /data/polylist/polycensus.so        LD_PRELOAD hook (PC2_* knobs)
#   /data/mesabuild/mesa20-b            diagnostic Mesa (PAN_FBD_* knobs)
#   /data/wdarm                         watchdog wrapper (max ~19 s)
set -u
B=/data/mesabuild/mesa20-b
APP=/home/user/fsonly/fsonly
HOOK=/data/polylist/polycensus.so
WD=/data/wdarm
OUT=${OUT:-/home/user/fsonly/out}
mkdir -p "$OUT"

export LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri
export LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri
export MESA_LOADER_DRIVER_OVERRIDE=panfrost
export EGL_PLATFORM=surfaceless

dmesg_after() {
	echo "--- dmesg ($1) ---"
	dmesg | grep -E 'p7580_job: (ERR|TIMEOUT)|MMU fault|p7580_sched: chain' |
		head -n 8
	echo "--- end dmesg ---"
}

case_ctrl() {
	env /data/wdarm 18 "$APP" --tag ctrl --frames 1
}

case_prefill() {
	env LD_PRELOAD="$HOOK" PC2_PREFILL_BIG=1 PC2_POLLSCAN=1 \
	    PC2_SCANLOG="$OUT/scan_prefill.txt" PC2_MAX=1 \
	    /data/wdarm 18 "$APP" --tag prefill --frames 1
}

case_nofrag() {
	env LD_PRELOAD="$HOOK" PC2_PREFILL_BIG=1 PC2_POLLSCAN=1 \
	    PC2_SCANLOG="$OUT/scan_nofrag.txt" PC2_MAX=1 PC2_REFUSE_FRAG=1 \
	    /data/wdarm 18 "$APP" --tag nofrag --frames 1
}

case_presleep() {
	env LD_PRELOAD="$HOOK" PC2_PREFILL_BIG=1 PC2_POLLSCAN=1 \
	    PC2_SCANLOG="$OUT/scan_presleep.txt" PC2_MAX=1 \
	    PAN_FBD_PRESLEEP_US=2000000 PAN_FBD_DEBUG=1 \
	    /data/wdarm 18 "$APP" --tag presleep --frames 1
}

case_recheck() {
	env LD_PRELOAD="$HOOK" PC2_PREFILL_BIG=1 PC2_POLLSCAN=1 \
	    PC2_SCANLOG="$OUT/scan_recheck.txt" PC2_MAX=1 \
	    PAN_FBD_RECHECK=1 PAN_FBD_DEBUG=1 \
	    /data/wdarm 18 "$APP" --tag recheck --frames 1
}

case_defectb() {
	env LD_PRELOAD="$HOOK" PC2_PREFILL_BIG=1 PC2_POLLSCAN=1 \
	    PC2_SCANLOG="$OUT/scan_defectb.txt" PC2_MAX=1 \
	    PAN_FBD_DEBUG=1 \
	    /data/wdarm 18 "$APP" --tag defectb --frames 1
}

case_small() {
	env LD_PRELOAD="$HOOK" PC2_PREFILL=1 PC2_POLLSCAN=1 \
	    PC2_SCANLOG="$OUT/scan_small.txt" PC2_MAX=1 \
	    PAN_FBD_PRESLEEP_US=2000000 \
	    /data/wdarm 18 "$APP" --tag small --frames 1
}

run_one() {
	tag=$1
	echo "================ $tag ================"
	dmesg -c >/dev/null 2>&1
	"case_$tag" 2>&1 | tee -a "$OUT/$tag.log"
	echo "CASE_RC=$?"
	dmesg_after "$tag" >>"$OUT/$tag.log"
	sync
}

what=${1:-all}
case $what in
all)
	for t in ctrl prefill nofrag presleep small defectb recheck; do
		run_one "$t"
	done
	;;
ctrl|prefill|nofrag|presleep|recheck|defectb|small)
	run_one "$what"
	;;
*)
	echo "usage: $0 [all|ctrl|prefill|nofrag|presleep|recheck|defectb|small]" >&2
	exit 2
	;;
esac
