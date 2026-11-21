# integration20 reproducible rebuild (mesa-render 2936796 slot fix + gpuwrite):
# M1 + backport shims 0001-0005 +
# Mode-B v2 render-ONLY + panfrost7580 (submit/open/render-only fixes,
# gem_vm_ops mmap 5d97c65, vm_pgoff fix 5f8ecd2, shmem-backed CREATE_BO
# dcd5171, job-lifetime PM model c81e8a7, teardown PM guards 5c52f1f + close fix 476b75d
# + jobtrace 1244082: bounded dispatch/IRQ/fence trace, out_sync
# NULL-install fix, completion dma_sync_single_for_cpu (gpu31-coherency:
# bind-time dma_map + SUBMIT sync_for_device + free_job sync_for_cpu;
# flush_dcache_page was a no-op on arm64/no-HIGHMEM) + jobtrace2 1450d16:
# run_job out_sync install via drm_syncobj_replace_fence (F-JT1 active),
# free_job job->bos walk calling p7580_gem_job_done_sync per BO (F-JT2
# active, job->bos refcount transfer on SUBMIT) + schedtrace 8fd555e:
# bounded ST-IOCTL/ST-QUEUE/ST-RUN/ST-PMGET/ST-RETIRE probes in submit.c
# and ST-PUSH/ST-PUMP/ST-FENCE in the 0004 shim (budget 8 each) + pumpfix de07c37:
# 0004 pump preserves SUBMIT slot, ST-OPS (ops/run_job/slot) + ST-FINISH (ran/slot)
# probes, NULL-guarded run_job call, IS_ERR_OR_NULL/ran stamp, retire-after-fence;
# driver stamps sched_job->slot at SUBMIT, run_job trusts it + opsfix df7c4b9:
# ops/slot/ran BUILD_BUG_ON pins in both TUs (ops=24/slot=232/ran=236),
# alloc round-trip BUG_ON, ST-OPS opsoff probe, submit call-site guard
# + probenumeric 3bcffae: numeric %lu ST-OPS
# (job/opsoff/ops/run/runexp/owner/slot) with runexp= from
# p7580_drv_run_job_addr() (weak NULL default in shim, strong in submit.c),
# ST-FINISH owner=%lu, ST-RUN-entry first-statement print, run_seq stamp,
# file_priv/drv_priv stamped at SUBMIT (run_job/timedout/free read stamps;
# entity math range-checked fallback only), per-guard run_bail
# NULLJOB/BADSLOT/BADENT/NODRV prints, ST-RETIRE ran=/owner=)
# + irqfix d852459 (SUPERSEDED routing: req==0 -> JS1 per 2936796):
# STAT|RAWSTAT IRQ demux ((status|rawstat) & m), JOB_INT_CLEAR re-arm,
# AS0 program_as + p7580_job_dev_rearm() on recover/resume, irq-arm
# (rawstat/stat/mask/js_present) + irq-enter (rawstat/stat) + post-start
# (rawstat/js_status) + irq/irq-err/fence-signal numeric probes + d0+d4.
# Copy of build-int19.sh with int20 names + steps 6b/6c gates (unchanged
# numeric ST-OPS/runexp, ST-RUN entry, 4 run_bail kinds + run_fallback,
# ST-RETIRE ran/owner, ST-FINISH owner, file_priv/drv_priv stamps,
# weak/strong run_job_addr linkage; JS1 zero-req (2936796), STAT|RAWSTAT demux,
# re-arm/program_as in recover+resume, job probes) + new step 6e
# ABI-align gates (de142b3: BUILD_BUG_ON ABI gates in syncobj + ioctl
# TUs, SYNC_FILE bit-0 tolerance on HTOF/FTOH with other bits rejected,
# dispatcher copy-in truncate / copy-back cap) + 6d freshness, step 8
# + gpuwrite (this ticket): Zero-fill pattern-test fix (v4/v5 genxml
# has no Immediate-32; T720 is PAN_ARCH 4) + DEBUG_READ_BO 0xCF fops
# route with p7580_gem_debug_read_word (reuses job_done_sync).
# Step 4b (PM-ordering overlay) retired: the
# job-lifetime PM model (live-job ref armed before drm_sched_job_submit,
# released in free_job; re-get/put in run_job/timedout_job; probe
# power-first; suspend -EBUSY while non-idle) plus teardown holds
# (teardown_hold/teardown_release/unmap_bo for postclose/file_fini/remove)
# are folded into driver 0001 (c81e8a7 + 5c52f1f), so no
# code/gpu31/pm-order/*.patch overlay exists or is needed.
# Syncobj ioctl (G31_SyncobjIoctl) is NOT waited for: build without it if
# absent (probe fills num_ioctls from the driver ioctl table at probe).
# Fresh scratch copy of the pristine kernel tree; host-side only (-j4
# allowed). Scratch tree, TMPDIR and ALL outputs live on the external
# disk (host / and /tmp are full/tmpfs): nothing is written to /tmp or /.
# Packs boot.img with the STOCK dt.img + reference ramdisk (never rebuilt),
# staged onto the external disk (read-only seed copies from /tmp/bimg).
EXT=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int20}
LOG=${LOG:-$EXT/logs/gpu31-int20-build.log}
EVID=${EVID:-$EXT/logs/gpu31-int20-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR"
: > "$EVID"

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR"

# 1. backport shims 0001-0005 (patches/drm-backport, f698920 + format repair)
for p in "$REPO"/patches/drm-backport/000[1-5]-*.patch; do
	git apply --check "$p" && git apply "$p"
done
# 2. Mode-B v2 render-ONLY node (02bd790: DRIVER_RENDER_ONLY, no card0)
git apply --check "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch \
&& git apply "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch
# 3. dmabuf CMA writecombine port (REQUIRED: arm64 has no
#    dma_alloc_writecombine; int4 §4 item 1 — fatal without it)
git apply --check "$REPO"/code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch \
&& git apply "$REPO"/code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch
# 4. panfrost7580 render-only driver (02bd790 on top of 20ea233:
#    submit deadlock/timeout fixes + open sched-ensure + render-only
#    DRIVER_RENDER_ONLY companion; gem_vm_ops mmap 5d97c65; vm_pgoff
#    fix 5f8ecd2; shmem-backed CREATE_BO dcd5171; job-lifetime PM
#    model c81e8a7 + teardown PM guards 5c52f1f + close fix 476b75d +
#    jobtrace 1244082 + jobtrace2 1450d16 (run_job out_sync install,
#    free_job job_done_sync walk, job->bos refcount transfer) folded
#    into driver 0001; opsfix df7c4b9 regen: submit.c +1,897 with
#    layout pin + call-site guard).
for p in "$REPO"/code/gpu31/driver/000[12]-*.patch; do
	git apply --check "$p" && git apply "$p"
done

# 4b. retired (was PM-ordering overlay in int6): folded into driver 0001
# at c81e8a7 (job-lifetime PM) + 5c52f1f (teardown holds). Kept as a
# log line only.
echo "int20: PM-ordering folded in driver 0001 (c81e8a7 + 5c52f1f); no overlay applied"

# 5. config: pmaports base + d0-m1-minimal + d4-panfrost7580, then closure
cp "$REPO"/code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
for f in "$REPO"/code/gpu31/config/fragments/d0-m1-minimal.fragment \
	 "$REPO"/code/gpu31/config/fragments/d4-panfrost7580.fragment; do
	grep -v '^#' "$f" | grep -v '^$' | cut -d= -f1 | while read -r sym; do
		grep -v "^${sym}=\|^# ${sym} is not set" .config > .config.tmp && mv .config.tmp .config
	done
	grep -v '^#' "$f" | grep -v '^$' >> .config
done
yes "" | make ARCH=arm64 olddefconfig >/dev/null

# 6. build
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
ls -l arch/arm64/boot/Image
sha256sum arch/arm64/boot/Image

# 6b. ops-layout + probenumeric gate (opsfix df7c4b9 + 3bcffae):
# compute the layout inside the build and FAIL (exit 1, scratch kept
# for diagnosis) on any disagreement. pahole-free: (a) offsetof getters
# compiled with the real kbuild include chain, immediates read back;
# (b) objdump of both objects for the compiler-emitted offset
# immediates; (c) nm single-definition + __ksymtab checks in objects
# and System.map; (d) applied-tree pin/round-trip/numeric ST-OPS +
# ST-RUN/bail/retire presence + Image strings; (e) weak/strong
# run_job_addr linkage (p7580_drv_run_job_addr in both objects'
# symtab, absolute in the linked Image System.map).
note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int20 GATE FAIL: $1"; exit 1; }
SCHED_O=drivers/gpu/drm/drm_sched.o
SUBMIT_O=drivers/gpu/drm/panfrost/panfrost7580_submit.o
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ -f "$SCHED_O" ] || gate_fail "$SCHED_O missing"
[ -f "$SUBMIT_O" ] || gate_fail "$SUBMIT_O missing"
note "== int20 gate: objects present =="
ls -l --time-style=full-iso "$SCHED_O" "$SUBMIT_O" arch/arm64/boot/Image | tee -a "$EVID"
# (c1) single definitions: exactly one alloc, one run_job, one ops table
[ "$(aarch64-linux-gnu-nm "$SCHED_O" | grep -c ' T drm_sched_job_alloc$')" = 1 ] \
	|| gate_fail "drm_sched_job_alloc not exactly once in drm_sched.o"
[ "$(aarch64-linux-gnu-nm "$SUBMIT_O" | grep -c ' [tT] p7580_drv_run_job$')" = 1 ] \
	|| gate_fail "p7580_drv_run_job not exactly once in submit.o"
[ "$(aarch64-linux-gnu-nm "$SUBMIT_O" | grep -c ' [rRdD] p7580_drv_sched_ops$')" = 1 ] \
	|| gate_fail "p7580_drv_sched_ops not exactly once in submit.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep ' drm_sched_job_alloc$' | tee -a "$EVID"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -E ' p7580_drv_(run_job|sched_ops)$' | tee -a "$EVID"
[ "$(grep -c ' [Tt] drm_sched_job_alloc$' System.map)" = 1 ] \
	|| gate_fail "System.map drm_sched_job_alloc count != 1 (duplicate shim copy?)"
[ "$(grep -c ' [tT] p7580_drv_run_job$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_drv_run_job count != 1"
[ "$(grep -c ' [rR] p7580_drv_sched_ops$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_drv_sched_ops count != 1"
grep -E ' (drm_sched_job_alloc|p7580_drv_run_job|p7580_drv_sched_ops)$' System.map | tee -a "$EVID"
# (c2) __ksymtab export present for the alloc entry point
aarch64-linux-gnu-nm "$SCHED_O" | grep -q '__ksymtab_drm_sched_job_alloc' \
	|| gate_fail "no __ksymtab_drm_sched_job_alloc in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -E '__ksymtab_drm_sched_job_alloc$' | tee -a "$EVID"
# (a) compute the true in-tree layout: stage the shim headers (0004 is
# authoritative; drivers/gpu/drm/drm_sched.h shadows the tree's own
# 3.10 drm_sched.h-less drivers/gpu/drm) and compile offsetof getters
# against the real headers, exactly the kcheck include chain
# (code/gpu31/sched/Makefile). Immediates read back from the .s.
GSTAGE=$EXT/offstage-int20
rm -rf "$GSTAGE"
mkdir -p "$GSTAGE/include/linux" "$GSTAGE/drivers/gpu/drm"
cp drivers/gpu/drm/drm_sched.h "$GSTAGE/drivers/gpu/drm/"
cp drivers/gpu/drm/drm_syncobj.h "$GSTAGE/drivers/gpu/drm/" 2>/dev/null || true
cp include/linux/dma-fence.h include/linux/dma-resv.h "$GSTAGE/include/linux/" 2>/dev/null || true
cat > "$EXT/offcheck20.c" <<'EOF'
#include <drm_sched.h>
int off_ops(void) { return (int)offsetof(struct drm_sched_job, ops); }
int off_slot(void) { return (int)offsetof(struct drm_sched_job, slot); }
int off_ran(void) { return (int)offsetof(struct drm_sched_job, ran); }
EOF
GCC_SYSINC=$(aarch64-linux-gnu-gcc -print-file-name=include)
aarch64-linux-gnu-gcc -nostdinc -isystem "$GCC_SYSINC" \
	-std=gnu89 -Iarch/arm64/include -Iarch/arm64/include/generated \
	-Iinclude -Iinclude/uapi -Iarch/arm64/include/uapi \
	-I"$GSTAGE/include" -I"$GSTAGE/drivers/gpu/drm" \
	-D__KERNEL__ -include linux/kconfig.h -DKBUILD_MODNAME='"offcheck20"' \
	-S -O0 -o "$EXT/offcheck20.s" "$EXT/offcheck20.c" \
	|| gate_fail "offset getter compile failed"
OFF_OPS=$(grep -A 6 '^off_ops:' "$EXT/offcheck20.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_SLOT=$(grep -A 6 '^off_slot:' "$EXT/offcheck20.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_RAN=$(grep -A 6 '^off_ran:' "$EXT/offcheck20.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
note "computed in-tree layout: ops=$OFF_OPS slot=$OFF_SLOT ran=$OFF_RAN"
[ "$OFF_OPS" = 24 ] || gate_fail "computed ops offset $OFF_OPS != 24"
[ "$OFF_SLOT" = 232 ] || gate_fail "computed slot offset $OFF_SLOT != 232"
[ "$OFF_RAN" = 236 ] || gate_fail "computed ran offset $OFF_RAN != 236"
# (b) compiler-emitted offset immediates: the alloc TU stores entity+ops
# as ONE stp at #16 (ops word = base+24). run_job (3bcffae) reads the
# SUBMIT-stamped slot from the backend job (slot is word 2 of struct
# p7580_submit_job, seqno u32 + slot int -> ldr wN, [xN, #4]) with a
# range check against P7580_NUM_JOB_SLOTS (cmp #0x2); the sched slot at
# #232 survives only as sslot= in the run_fallback print (range-checked
# fallback path), so #232 is informational here, not required.
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_job_alloc "$SCHED_O" \
	| grep -q 'stp.*\[x[0-9]*, #16\]' || gate_fail "no entity+ops stp #16 in drm_sched_job_alloc disasm"
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_job_alloc "$SCHED_O" \
	| grep 'stp.*\[x[0-9]*, #16\]' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q '\[x[0-9]*, #4\]' || gate_fail "no #4 stamped-slot load in p7580_drv_run_job disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep '\[x[0-9]*, #4\]' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q 'cmp.*#0x2' || gate_fail "no slot range check (cmp #0x2) in p7580_drv_run_job disasm"
grep -q 'offsetof(struct drm_sched_job, slot) != 232' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "slot pin missing in applied drm_sched.h"
grep -q 'offsetof(struct drm_sched_job, ran) != 236' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "ran pin missing in applied drm_sched.h"
grep -q 'offsetof(struct drm_sched_job, slot) != 232' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "slot pin missing in applied submit.c"
grep -q 'offsetof(struct drm_sched_job, ran) != 236' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ran pin missing in applied submit.c"
grep -q 'BUG_ON(job->ops != ops)' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "alloc round-trip BUG_ON missing in applied drm_sched.c"
grep -q 'BUILD_BUG_ON(offsetof(struct drm_sched_job, ops) != 24)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ops pin missing in applied submit.c"
grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "ST-OPS numeric probe missing in applied drm_sched.c"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' \
	|| gate_fail "ST-OPS numeric string missing in drm_sched.o"
aarch64-linux-gnu-strings "$SCHED_O" | grep 'p7580_sched: ops' | tee -a "$EVID"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: finish ran=%d slot=%d owner=%lu' \
	|| gate_fail "ST-FINISH owner string missing in drm_sched.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' \
	|| gate_fail "ST-OPS numeric string missing in Image"
# ST-RUN entry + per-guard bail + retire ran/owner (submit.c TU)
grep -q 'p7580_sched: run job=%lu ent=%lu owner=%lu ops=%lu runexp=%lu' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ST-RUN entry probe missing in applied submit.c"
for b in NULLJOB BADSLOT BADENT NODRV; do
	aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q "p7580_sched: run_bail $b" \
		|| gate_fail "run_bail $b missing in submit.o"
done
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: run_fallback' \
	|| gate_fail "run_fallback missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: retire seq=%u slot=%d sslot=%d ran=%u owner=%lu' \
	|| gate_fail "ST-RETIRE ran/owner missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: run_job' \
	|| gate_fail "ST run_job missing in submit.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: run job=%lu ent=%lu owner=%lu ops=%lu runexp=%lu' \
	|| gate_fail "ST-RUN entry missing in Image"
for b in NULLJOB BADSLOT BADENT NODRV; do
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "p7580_sched: run_bail $b" \
		|| gate_fail "run_bail $b missing in Image"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: retire seq=%u slot=%d sslot=%d ran=%u owner=%lu' \
	|| gate_fail "ST-RETIRE ran/owner missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: finish ran=%d slot=%d owner=%lu' \
	|| gate_fail "ST-FINISH owner missing in Image"
# file_priv/drv_priv stamps in submit.c job struct; run_job_addr linkage
grep -q 'file_priv' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "file_priv stamp missing in applied job.h"
grep -q 'drv_priv' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "drv_priv stamp missing in applied job.h"
grep -q 'p7580_drv_run_job_addr' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "run_job_addr hook missing in applied drm_sched.c"
grep -q 'p7580_drv_run_job_addr' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job_addr strong def missing in applied submit.c"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' W p7580_drv_run_job_addr$' \
	|| gate_fail "p7580_drv_run_job_addr not weak (W) in drm_sched.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_drv_run_job_addr$' \
	|| gate_fail "p7580_drv_run_job_addr not strong text (T) in submit.o"
aarch64-linux-gnu-nm "$SCHED_O" "$SUBMIT_O" | grep ' p7580_drv_run_job_addr$' | tee -a "$EVID"
grep -q ' T p7580_drv_run_job_addr$' System.map \
	|| gate_fail "p7580_drv_run_job_addr not T in System.map (strong def must win link)"
grep ' p7580_drv_run_job_addr$' System.map | tee -a "$EVID"
note "Image p7580_job: count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job:')"
note "Image p7580_job: dispatch = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: dispatch')"
note "Image p7580_job: irq = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: irq')"
note "== int20 gate: PASS (ops=24/slot=232/ran=236, single defs, numeric ST-OPS + runexp, ST-RUN/bail/retire live) =="

# 6c. IRQ/completion gates (irqfix d852459): JS1 zero-req selection (2936796),
# STAT|RAWSTAT demux, re-arm + AS0 program on recover/resume, job probes.
JOB_O=drivers/gpu/drm/panfrost/panfrost7580_job.o
[ -f "$JOB_O" ] || gate_fail "$JOB_O missing"
note "== int20 gate: job object present =="
ls -l --time-style=full-iso "$JOB_O" | tee -a "$EVID"
# JS1 for requirement-less jobs (2936796: mainline v5.2 slot rule; SUPERSEDES
# irqfix d852459's req==0 -> JS0). Applied tree must route FS -> JS0,
# everything else (incl. req==0 chain) -> JS1, ONLY_COMPUTE -> JS2.
grep -q 'return P7580_SLOT_VERTEX_TILER' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "req==0 -> JS1 tail return missing in applied jm_hw.h"
grep -q 'return P7580_SLOT_FRAGMENT' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "FS -> JS0 route missing in applied jm_hw.h"
grep -q 'return P7580_SLOT_COMPUTE' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "ONLY_COMPUTE -> JS2 route missing in applied jm_hw.h"
grep -B2 -A8 'static inline int p7580_job_slot' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h | tee -a "$EVID"
# STAT|RAWSTAT demux: union read in the applied IRQ handler
grep -q '(status | rawstat)' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "STAT|RAWSTAT demux missing in applied job.c"
grep -c '(status | rawstat)' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# re-arm helper declared + defined + used in recover; program_as on both paths
grep -q 'p7580_job_dev_rearm' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "rearm decl missing in applied job.h"
grep -q 'void p7580_job_dev_rearm' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "rearm def missing in applied job.c"
grep -q 'p7580_job_dev_rearm' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "rearm use missing in applied main.c (resume)"
grep -q 'p7580_mmu_program_as' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "program_as missing in applied job.c (recover)"
grep -q 'p7580_mmu_program_as' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "program_as missing in applied main.c (resume)"
grep -q 'P7580_JS_STATUS' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "JS_STATUS read missing in applied job.c"
# job probe strings in applied tree
for s in 'p7580_job: irq-arm' 'p7580_job: irq-enter' 'p7580_job: post-start' 'p7580_job: irq slot=' 'p7580_job: fence signal' 'p7580_job: irq-err'; do
	grep -q "$s" drivers/gpu/drm/panfrost/panfrost7580_job.c \
		|| gate_fail "probe '$s' missing in applied job.c"
done
# job probe strings in job.o
for s in 'p7580_job: irq-arm' 'p7580_job: irq-enter' 'p7580_job: post-start' 'p7580_job: irq slot=' 'p7580_job: fence signal' 'p7580_job: irq-err' 'p7580_job: dispatch'; do
	aarch64-linux-gnu-strings "$JOB_O" | grep -q "$s" \
		|| gate_fail "probe '$s' missing in job.o"
done
aarch64-linux-gnu-strings "$JOB_O" | grep 'p7580_job:' | tee -a "$EVID"
# job probe strings in the linked Image
for s in 'p7580_job: irq-arm' 'p7580_job: irq-enter' 'p7580_job: post-start' 'p7580_job: irq slot=' 'p7580_job: fence signal' 'p7580_job: irq-err' 'p7580_job: dispatch'; do
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "$s" \
		|| gate_fail "probe '$s' missing in Image"
done
note "== int20 irq gate: PASS (JS1 zero-req per 2936796, STAT|RAWSTAT demux, rearm+AS0 recover/resume, job probes live) =="

# 6g. slot-routing gate (mesa-render 2936796): req==0 -> JS1 in the
# applied tree AND compiled into job.o AND visible in the linked Image.
# FAIL (exit 1, scratch kept) on any disagreement.
grep -q '(requirements == 0)' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	&& gate_fail "stale irqfix req==0 special-case still present in applied jm_hw.h"
grep -q 'SLOT_VERTEX_TILER' drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h \
	|| gate_fail "VERTEX_TILER define missing in applied jm_hw.h"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'p7580_job: dispatch' \
	|| gate_fail "dispatch probe missing in job.o (slot evidence path)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: dispatch' \
	|| gate_fail "dispatch probe missing in Image"
note "slot routing: req0->JS1 (2936796) live in jm_hw.h + job.o + Image"
note "== int20 slot gate: PASS (FS->JS0, req0->JS1, ONLY_COMPUTE->JS2) =="


# 6e. ABI-align gates (de142b3): BUILD_BUG_ON ABI gates present in both
# TUs, SYNC_FILE bit-0 tolerance on HTOF/FTOH (other bits rejected),
# dispatcher copy-in truncate + copy-back cap. FAIL (exit 1, scratch
# kept) on any disagreement.
SYNC_O=drivers/gpu/drm/panfrost/panfrost7580_syncobj.o
IOCTL_O=drivers/gpu/drm/panfrost/panfrost7580_ioctl.o
[ -f "$SYNC_O" ] || gate_fail "$SYNC_O missing"
[ -f "$IOCTL_O" ] || gate_fail "$IOCTL_O missing"
note "== int20 gate: syncobj/ioctl objects present =="
ls -l --time-style=full-iso "$SYNC_O" "$IOCTL_O" | tee -a "$EVID"
# ABI BUILD_BUG_ON gates in the syncobj TU (applied tree)
grep -q 'BUILD_BUG_ON(sizeof(struct p7580_syncobj_handle) != 16)' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "16 B syncobj_handle pin missing in applied syncobj.c"
grep -q 'BUILD_BUG_ON(sizeof(struct p7580_syncobj_wait) != 32)' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "32 B syncobj_wait pin missing in applied syncobj.c"
grep -q 'BUILD_BUG_ON(P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE != (1u << 0))' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "HTOF SYNC_FILE bit pin missing in applied syncobj.c"
grep -q 'BUILD_BUG_ON(P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE != (1u << 0))' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "FTOH SYNC_FILE bit pin missing in applied syncobj.c"
# ABI BUILD_BUG_ON gates in the ioctl TU (applied tree)
grep -q 'BUILD_BUG_ON(sizeof(struct drm_panfrost_get_param) != 16)' drivers/gpu/drm/panfrost/panfrost7580_ioctl.c \
	|| gate_fail "16 B get_param pin missing in applied ioctl.c"
grep -q 'BUILD_BUG_ON(sizeof(struct drm_panfrost_submit) != 40)' drivers/gpu/drm/panfrost/panfrost7580_ioctl.c \
	|| gate_fail "40 B submit pin missing in applied ioctl.c"
# SYNC_FILE tolerance path: bit-0 mask kept, other bits rejected (both handlers)
grep -q 'args->flags & ~P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "HTOF SYNC_FILE tolerance missing in applied syncobj.c"
grep -q 'args->flags & ~P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "FTOH SYNC_FILE tolerance missing in applied syncobj.c"
# copy-size capping in the dispatcher (truncate copy-in, cap copy-back)
grep -q 'unsigned int cpin = usize < asize ? usize : asize' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "copy-in truncate missing in applied syncobj.c"
grep -q 'unsigned int cpout = usize < asize ? usize : asize' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "copy-back cap missing in applied syncobj.c"
# handler entry points live in the object + linked Image (nm, not strings:
# this TU carries no printk probes)
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_handle_to_fd$' \
	|| gate_fail "panfrost_syncobj_handle_to_fd missing in syncobj.o"
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_fd_to_handle$' \
	|| gate_fail "panfrost_syncobj_fd_to_handle missing in syncobj.o"
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_ioctl$' \
	|| gate_fail "panfrost_syncobj_ioctl missing in syncobj.o"
grep -q ' T panfrost_syncobj_handle_to_fd$' System.map \
	|| gate_fail "panfrost_syncobj_handle_to_fd missing in System.map"
grep -q ' T panfrost_syncobj_fd_to_handle$' System.map \
	|| gate_fail "panfrost_syncobj_fd_to_handle missing in System.map"
aarch64-linux-gnu-nm "$SYNC_O" "$IOCTL_O" | grep -E ' panfrost_syncobj_(handle_to_fd|fd_to_handle|ioctl)$' | tee -a "$EVID"
note "== int20 abi gate: PASS (BUILD_BUG_ON pins, SYNC_FILE tolerance, copy-size caps live) =="
# 6f. gpuwrite gates (this ticket): Zero-fill descriptor contract +
# DEBUG_READ_BO 0xCF fops route; six-ioctl driver table untouched;
# debug readback reuses job_done_sync (no second maintenance impl).
# FAIL (exit 1, scratch kept) on any disagreement.
GEM_O=drivers/gpu/drm/panfrost/panfrost7580_gem.o
[ -f "$GEM_O" ] || gate_fail "$GEM_O missing"
note "== int20 gate: gem object present =="
ls -l --time-style=full-iso "$GEM_O" | tee -a "$EVID"
# Zero is the only T720-valid write type (v4/v5 genxml: 1/2/3; no 4..7):
# the pattern-test must encode w10=3 and zero the immediate words.
grep -q 'w\[10\] = 3u' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	&& gate_fail "pattern-test leaked into gem.c (must live in the client only)"
grep -q 'P7580_DEBUG_READ_BO_NR.*0xCFu' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "0xCF debug nr missing in applied syncobj.c"
grep -q 'panfrost_debug_read_bo' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "debug dispatch missing in applied syncobj.c"
grep -q 'p7580_gem_debug_read_word' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "debug read word missing in applied gem.c"
grep -q 'p7580_gem_job_done_sync(obj);' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "debug path must reuse job_done_sync in applied gem.c"
grep -q 'int panfrost_debug_read_bo' drivers/gpu/drm/panfrost/panfrost7580_ioctl.c \
	|| gate_fail "debug handler missing in applied ioctl.c"
grep -q 'panfrost_debug_read_bo' drivers/gpu/drm/panfrost/panfrost7580_ioctl.h \
	|| gate_fail "debug decl missing in applied ioctl.h"
# six-ioctl table untouched: exactly the 6 v5.2 rows, no 0xCF row.
[ "$(grep -c 'DRM_IOCTL_DEF_DRV(PANFROST_' drivers/gpu/drm/panfrost/panfrost7580_main.c)" = 6 ] \
	|| gate_fail "driver ioctl table must stay at 6 rows (debug is fops-only)"
grep -q 'DRM_IOCTL_DEF_DRV(PANFROST_SUBMIT' drivers/gpu/drm/panfrost/panfrost7580_main.c \
	|| gate_fail "SUBMIT row missing in applied main.c"
# debug symbols live in objects + Image.
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_debug_read_word$' \
	|| gate_fail "p7580_gem_debug_read_word missing in gem.o"
aarch64-linux-gnu-nm "$IOCTL_O" | grep -q ' T panfrost_debug_read_bo$' \
	|| gate_fail "panfrost_debug_read_bo missing in ioctl.o"
aarch64-linux-gnu-strings "$SYNC_O" | grep -q 'p7580_debug_read_bo\|DEBUG_READ_BO\|0xCF\|debug' \
	|| note "debug strings optional in syncobj.o (handler may be string-free)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_gem_debug_read_word\|panfrost_debug_read_bo' \
	&& note "debug symbols live in Image" \
	|| note "debug symbols not string-visible in Image (nm-gated below instead)"
grep -q ' T p7580_gem_debug_read_word$' System.map \
	|| gate_fail "p7580_gem_debug_read_word missing in System.map"
grep -q ' T panfrost_debug_read_bo$' System.map \
	|| gate_fail "panfrost_debug_read_bo missing in System.map"
note "== int20 gpuwrite gate: PASS (Zero contract, 0xCF fops route, 6-row table, debug symbols live) =="

# 6d. freshness evidence (for the report): scratch birth vs object mtimes,
# plus proof the pristine seed carries no shim/driver files.
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/drm_sched.c" 2>&1 | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/panfrost/" 2>&1 | tee -a "$EVID"

# 7. pack boot.img (stock dt.img rule: dt.img is NEVER rebuilt).
# All pack I/O on the external disk: seed inputs are staged from /tmp/bimg
# (read-only) into $EXT/stage-int20; outputs land in $EXT.
STAGE=${STAGE:-$EXT/stage-int20}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int20.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int20.img"
sha256sum "$EXT/kernel-boot-int20.img"
# Keep a copy of the built Image next to the boot.img (scratch is deleted
# after the build, so the tree Image would otherwise be lost).
cp arch/arm64/boot/Image "$EXT/Image-int20"
sha256sum "$EXT/Image-int20"

# 8. delete scratch + stage (fresh tree per ticket; evidence kept in
# $EVID + Image-int20 + logs on the external disk).
rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
