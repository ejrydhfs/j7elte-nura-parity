# integration35 rebuild (6d187b2 gpu31-cachedir: direction-correct DMA
# cache maintenance on every BO path -- clean before any invalidate).
# gem gains p7580_gem_clean_for_device (sync_for_device DMA_TO_DEVICE,
# writeback) and p7580_gem_invalidate_for_cpu (clean THEN
# sync_for_cpu DMA_FROM_DEVICE); SUBMIT uses clean only; completion /
# debug read / snapshot / JDBODY dump run clean-then-invalidate;
# dma_unbind cleans before dma_unmap_page; driver 0001 regenerated
# with the snapshot (no bare job_done_sync) and PTEWALK owner-BO
# invalidate call sites. Host model test gem/cachedir_model.c, 27
# checks, negative controls included.
# Consumes 6d187b2 on top of int34 (0072a74 serialfence).
# All prior gates (6b/6c/6e/6f/6g/6h/6i/6j/6k/6l/6m/6n/6o/6p/6q/6r/6s/6t)
# unchanged + new 6u cachedir gates (helper defs + clean-then-invalidate
# order in tree, submit = clean only, unbind clean-before-unmap,
# no bare sync_for_cpu(BIDIRECTIONAL) anywhere, and object evidence:
# helper T symbols, for_device/w3=1 clean + for_cpu/w3=2 invalidate
# relocs in invalidate_for_cpu, submit disasm clean-only, call sites in
# submit.o; prior gates intact).
# submit.c hunk integrity (late function
# panfrost_ioctl_submit_override in tree/object/System.map).
# gem_vm_ops mmap 5d97c65, vm_pgoff fix 5f8ecd2, shmem-backed CREATE_BO
# dcd5171, job-lifetime PM model c81e8a7, teardown PM guards 5c52f1f + close fix 476b75d
# + jobtrace 1244082: bounded dispatch/IRQ/fence trace, out_sync
# NULL-install fix, completion dma_sync_single_for_cpu (gpu31-coherency:
# bind-time dma_map + SUBMIT sync_for_device + free_job sync_for_cpu;
# flush_dcache_page was a no-op on arm64/no-HIGHMEM) + jobtrace2 1450d16:
# run_job out_sync install retired by armfence 49f7df2 (F-JT1 stash gone:
# alloc at arm, SUBMIT-time replace_fence of the armed fence, run_job
# binds the same fence, pump enforces identity),
# job->bos refcount transfer on SUBMIT (F-JT2 active) + schedtrace 8fd555e:
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
# Copy of build-int19.sh with int35 names + steps 6b/6c gates (unchanged
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
SCR=${SCR:-$EXT/scratch-int35}
LOG=${LOG:-$EXT/logs/gpu31-int35-build.log}
EVID=${EVID:-$EXT/logs/gpu31-int35-evidence.log}
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
echo "int35: PM-ordering folded in driver 0001 (c81e8a7 + 5c52f1f); no overlay applied"

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
gate_fail() { note "int35 GATE FAIL: $1"; exit 1; }
SCHED_O=drivers/gpu/drm/drm_sched.o
SUBMIT_O=drivers/gpu/drm/panfrost/panfrost7580_submit.o
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ -f "$SCHED_O" ] || gate_fail "$SCHED_O missing"
[ -f "$SUBMIT_O" ] || gate_fail "$SUBMIT_O missing"
note "== int35 gate: objects present =="
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
GSTAGE=$EXT/offstage-int35
rm -rf "$GSTAGE"
mkdir -p "$GSTAGE/include/linux" "$GSTAGE/drivers/gpu/drm"
cp drivers/gpu/drm/drm_sched.h "$GSTAGE/drivers/gpu/drm/"
cp drivers/gpu/drm/drm_syncobj.h "$GSTAGE/drivers/gpu/drm/" 2>/dev/null || true
cp include/linux/dma-fence.h include/linux/dma-resv.h "$GSTAGE/include/linux/" 2>/dev/null || true
cat > "$EXT/offcheck33.c" <<'EOF'
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
	-D__KERNEL__ -include linux/kconfig.h -DKBUILD_MODNAME='"offcheck33"' \
	-S -O0 -o "$EXT/offcheck33.s" "$EXT/offcheck33.c" \
	|| gate_fail "offset getter compile failed"
OFF_OPS=$(grep -A 6 '^off_ops:' "$EXT/offcheck33.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_SLOT=$(grep -A 6 '^off_slot:' "$EXT/offcheck33.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_RAN=$(grep -A 6 '^off_ran:' "$EXT/offcheck33.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
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
note "== int35 gate: PASS (ops=24/slot=232/ran=236, single defs, numeric ST-OPS + runexp, ST-RUN/bail/retire live) =="

# 6c. IRQ/completion gates (irqfix d852459): JS1 zero-req selection (2936796),
# STAT|RAWSTAT demux, re-arm + AS0 program on recover/resume, job probes.
JOB_O=drivers/gpu/drm/panfrost/panfrost7580_job.o
[ -f "$JOB_O" ] || gate_fail "$JOB_O missing"
note "== int35 gate: job object present =="
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
note "== int35 irq gate: PASS (JS1 zero-req per 2936796, STAT|RAWSTAT demux, rearm+AS0 recover/resume, job probes live) =="

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
note "== int35 slot gate: PASS (FS->JS0, req0->JS1, ONLY_COMPUTE->JS2) =="


# 6e. ABI-align gates (de142b3): BUILD_BUG_ON ABI gates present in both
# TUs, SYNC_FILE bit-0 tolerance on HTOF/FTOH (other bits rejected),
# dispatcher copy-in truncate + copy-back cap. FAIL (exit 1, scratch
# kept) on any disagreement.
SYNC_O=drivers/gpu/drm/panfrost/panfrost7580_syncobj.o
IOCTL_O=drivers/gpu/drm/panfrost/panfrost7580_ioctl.o
[ -f "$SYNC_O" ] || gate_fail "$SYNC_O missing"
[ -f "$IOCTL_O" ] || gate_fail "$IOCTL_O missing"
note "== int35 gate: syncobj/ioctl objects present =="
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
note "== int35 abi gate: PASS (BUILD_BUG_ON pins, SYNC_FILE tolerance, copy-size caps live) =="
# 6f. gpuwrite gates (this ticket): Zero-fill descriptor contract +
# DEBUG_READ_BO 0xCF fops route; six-ioctl driver table untouched;
# debug readback reuses job_done_sync (no second maintenance impl).
# FAIL (exit 1, scratch kept) on any disagreement.
GEM_O=drivers/gpu/drm/panfrost/panfrost7580_gem.o
[ -f "$GEM_O" ] || gate_fail "$GEM_O missing"
note "== int35 gate: gem object present =="
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
note "== int35 gpuwrite gate: PASS (Zero contract, 0xCF fops route, 6-row table, debug symbols live) =="

# 6h. coherency gates (c207bdd): pre-signal invalidate BEFORE every
# fence signal (IRQ DONE, timedout own-fence, recover -EIO); free_job
# keeps its post-signal job_done_sync + unref walk. FAIL (exit 1,
# scratch kept) on any disagreement.
note "== int35 gate: coherency objects present =="
ls -l --time-style=full-iso "$JOB_O" "$GEM_O" "$SUBMIT_O" | tee -a "$EVID"
# applied-tree call sites: 3x pre_signal_sync calls (IRQ job->bos,
# timedout job->bos, recover fault_job->bos) plus decl + comment mentions.
[ "$(grep -c 'p7580_gem_job_bos_pre_signal_sync(.*->bos,$' drivers/gpu/drm/panfrost/panfrost7580_job.c)" = 3 ] \
	|| gate_fail "job.c must carry 3 pre_signal call sites, got $(grep -c 'p7580_gem_job_bos_pre_signal_sync(.*->bos,$' drivers/gpu/drm/panfrost/panfrost7580_job.c)"
grep -n 'p7580_gem_job_bos_pre_signal_sync' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# applied-tree free_job: post-signal job_done_sync + unref walk.
grep -q 'p7580_gem_job_done_sync' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "free_job job_done_sync walk missing in applied submit.c"
grep -n 'p7580_gem_job_done_sync\|drm_gem_object_unreference_unlocked' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# gem TU: helper def + per-page for_cpu invalidate inside job_done_sync.
grep -q 'void p7580_gem_job_bos_pre_signal_sync' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "pre_signal_sync def missing in applied gem.c"
grep -q 'dma_sync_single_for_cpu' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "for_cpu invalidate missing in applied gem.c"
# symbols live in objects: helper def in gem.o, 3 undef refs in job.o.
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_job_bos_pre_signal_sync$' \
	|| gate_fail "pre_signal_sync not T in gem.o"
[ "$(aarch64-linux-gnu-nm "$JOB_O" | grep -c ' U p7580_gem_job_bos_pre_signal_sync$')" = 1 ] \
	|| gate_fail "pre_signal_sync undef ref missing in job.o"
aarch64-linux-gnu-nm "$GEM_O" "$JOB_O" | grep 'p7580_gem_job_bos_pre_signal_sync' | tee -a "$EVID"
# objdump: call-site evidence -- bl to the helper in the IRQ handler,
# timedout, and recover paths of job.o.
for fn in p7580_job_irq p7580_job_timedout p7580_job_recover; do
	aarch64-linux-gnu-objdump -d --disassemble=$fn "$JOB_O" \
		| grep -q 'bl.*<p7580_gem_job_bos_pre_signal_sync' \
		|| gate_fail "no bl to pre_signal_sync in $fn disasm"
	aarch64-linux-gnu-objdump -d --disassemble=$fn "$JOB_O" \
		| grep 'bl.*<p7580_gem_job_bos_pre_signal_sync' | tee -a "$EVID"
done
# linked Image: exactly one definition wins the link.
[ "$(grep -c ' T p7580_gem_job_bos_pre_signal_sync$' System.map)" = 1 ] \
	|| gate_fail "System.map pre_signal_sync count != 1"
grep ' p7580_gem_job_bos_pre_signal_sync$' System.map | tee -a "$EVID"
note "== int35 coherency gate: PASS (pre-signal IRQ/timeout/recover, post-signal free_job intact) =="

# 6i. errtrace gates (7a5bf9b): unconditional ERR/TIMEOUT diagnostics +
# debugfs trace_arm/dump knobs. FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int35 gate: errtrace objects present =="
ls -l --time-style=full-iso "$JOB_O" | tee -a "$EVID"
# applied-tree: ERR format string with full pin set (slot/js_status/stat/
# head/tail/jc) present in job.c; err helper def present.
grep -q 'p7580_job: ERR slot=' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR format string missing in applied job.c"
grep -q 'js_status=0x%08x' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR js_status pin missing in applied job.c"
grep -q 'head=0x%llx tail=0x%llx' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR head/tail pins missing in applied job.c"
grep -q 'p7580_job_err_pin' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "err_pin helper missing in applied job.c"
grep -q 'TIMEOUT slot=' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "TIMEOUT line missing in applied job.c"
grep -n 'p7580_job: ERR slot=\|TIMEOUT slot=' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# applied-tree: debugfs knobs created (p7580 dir + trace_arm + dump files).
grep -q 'debugfs_create_dir' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "debugfs dir create missing in applied job.c"
grep -q '"trace_arm"' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "trace_arm knob missing in applied job.c"
grep -q '"dump"' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "dump knob missing in applied job.c"
# symbols/strings live in job.o: ERR string + knob names + TIMEOUT.
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'p7580_job: ERR slot=' \
	|| gate_fail "ERR string missing in job.o"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'trace_arm' \
	|| gate_fail "trace_arm string missing in job.o"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'TIMEOUT slot=' \
	|| gate_fail "TIMEOUT string missing in job.o"
aarch64-linux-gnu-strings "$JOB_O" | grep -E 'p7580_job: (ERR|TIMEOUT)|trace_arm|^dump$' | tee -a "$EVID"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' t p7580_job_err_pin$' \
	|| gate_fail "p7580_job_err_pin static missing in job.o nm"
# linked Image: ERR + TIMEOUT + knob strings visible; helper inlined/static
# (nm-gated in job.o above, strings-gated here).
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: ERR slot=' \
	|| gate_fail "ERR string missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'TIMEOUT slot=' \
	|| gate_fail "TIMEOUT string missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'trace_arm' \
	|| gate_fail "trace_arm string missing in Image"
note "Image p7580_job: ERR = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: ERR')"
# 6j. instrtype gates (71a9412): excl-fence publish at dispatch time in
# run_job for every submitted BO; resv->lock taken inside add_excl
# (never in run_job: SUBMIT-DEADLOCK rule). FAIL (exit 1, scratch
# kept) on any disagreement.
note "== int35 gate: instrtype objects present =="
ls -l --time-style=full-iso "$SUBMIT_O" | tee -a "$EVID"
# applied tree: run_job publishes excl fence per BO over stable job->bos refs.
grep -q 'dma_resv_add_excl_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "add_excl publish missing in applied submit.c"
grep -q 'rj->bo_count' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "bo_count walk missing in applied submit.c run_job"
grep -n 'dma_resv_add_excl_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# locking discipline: run_job must NOT take resv->lock itself (shim owns it).
grep -A30 'dma_resv_add_excl_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'mutex_lock' \
	&& gate_fail "run_job must not lock resv itself (shim locks inside add_excl)"
# shim owns the lock: applied-tree dma-resv.c add_excl locks resv->lock.
grep -q 'void dma_resv_add_excl_fence' drivers/base/dma-resv.c \
	|| gate_fail "add_excl def missing in applied dma-resv.c"
grep -A12 'void dma_resv_add_excl_fence' drivers/base/dma-resv.c | grep -q 'mutex_lock(&obj->lock)' \
	|| gate_fail "add_excl must lock resv->lock inside the shim"
grep -A12 'void dma_resv_add_excl_fence' drivers/base/dma-resv.c | tee -a "$EVID"
# object evidence: submit.o carries an UNDEF ref (call site), gem/shim TU
# (drivers/base/built-in.o or dma-resv.o) carries the T definition.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U dma_resv_add_excl_fence$' \
	|| gate_fail "U dma_resv_add_excl_fence missing in submit.o (no call site)"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep 'dma_resv_add_excl_fence' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q 'bl.*<dma_resv_add_excl_fence>' \
	|| gate_fail "no bl to dma_resv_add_excl_fence in p7580_drv_run_job disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep 'dma_resv_add_excl_fence' | tee -a "$EVID"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -B2 -A1 'dma_resv_add_excl_fence' | tee -a "$EVID"
# linked Image: exactly one T definition wins the link.
[ "$(grep -c ' T dma_resv_add_excl_fence$' System.map)" = 1 ] \
	|| gate_fail "System.map dma_resv_add_excl_fence count != 1"
grep ' dma_resv_add_excl_fence$' System.map | tee -a "$EVID"
note "== int35 instrtype gate: PASS (excl publish in run_job, shim-owned lock, call-site live) =="

# 6k. iova gates (2dd207c): free-time gpu_unmap before iova_free,
# invalidate-before/after at map (reuse fence), OVERLAP detector,
# ERR-MAP dump + iova_arm knob. FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int35 gate: iova objects present =="
ls -l --time-style=full-iso "$GEM_O" "$SUBMIT_O" "$JOB_O" | tee -a "$EVID"
# applied tree: free calls gpu_unmap BEFORE iova_free (order matters:
# the IOVA must be unmapped everywhere before it is reusable).
grep -q 'p7580_gem_gpu_unmap(obj);' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "gpu_unmap call missing in applied gem.c free path"
grep -q 'p7580_iova_free(gd, bo);' drivers/gpu/drm/panfrost/panfrost7580_gem.c \
	|| gate_fail "iova_free call missing in applied gem.c free path"
[ "$(grep -n 'p7580_gem_gpu_unmap(obj);' drivers/gpu/drm/panfrost/panfrost7580_gem.c | head -1 | cut -d: -f1)" -lt \
  "$(grep -n 'p7580_iova_free(gd, bo);' drivers/gpu/drm/panfrost/panfrost7580_gem.c | head -1 | cut -d: -f1)" ] \
	|| gate_fail "free path must call gpu_unmap before iova_free"
grep -n 'p7580_gem_gpu_unmap(obj);\|p7580_iova_free(gd, bo);' drivers/gpu/drm/panfrost/panfrost7580_gem.c | tee -a "$EVID"
# applied tree: submit TU carries the unmap def + map_bo reuse fence.
grep -q '^void p7580_gem_gpu_unmap' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "gpu_unmap def missing in applied submit.c"
grep -q 'p7580_drv_register_iova_trace' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "register_iova_trace missing in applied submit.c"
grep -q 'p7580_iova: OVERLAP' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "OVERLAP detector missing in applied submit.c"
grep -q 'p7580_iova: map obj=' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "map trace missing in applied submit.c"
grep -q 'p7580_iova: unmap obj=' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "unmap trace missing in applied submit.c"
# invalidate-map-invalidate: two p7580_mmu_invalidate calls in the
# map_bo function body (purge-before + publish-after).
[ "$(sed -n '/^static int p7580_drv_map_bo/,/^}/p' drivers/gpu/drm/panfrost/panfrost7580_submit.c | grep -c 'p7580_mmu_invalidate(drv->regs')" = 2 ] \
	|| gate_fail "map_bo must carry exactly 2 mmu_invalidate calls (before+after)"
sed -n '/^static int p7580_drv_map_bo/,/^}/p' drivers/gpu/drm/panfrost/panfrost7580_submit.c | grep -n 'p7580_mmu_invalidate' | tee -a "$EVID"
# applied tree: jobs TU carries ERR-MAP dump + iova_arm knob + resolver.
grep -q 'p7580_job: ERR-MAP' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "ERR-MAP missing in applied job.c"
grep -q '"iova_arm"' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "iova_arm knob missing in applied job.c"
grep -q 'p7580_job_set_iova_resolver\|p7580_job_register_iova_budget' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "iova resolver/budget hooks missing in applied job.c"
grep -n 'p7580_job: ERR-MAP\|"iova_arm"\|p7580_job_set_iova_resolver\|p7580_job_register_iova_budget' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# object evidence: gpu_unmap T in submit.o, U in gem.o; free path
# calls it; map_bo calls invalidate twice.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T p7580_gem_gpu_unmap$' \
	|| gate_fail "p7580_gem_gpu_unmap not T in submit.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' U p7580_gem_gpu_unmap$' \
	|| gate_fail "U p7580_gem_gpu_unmap missing in gem.o (free path must call it)"
aarch64-linux-gnu-nm "$GEM_O" "$SUBMIT_O" | grep 'p7580_gem_gpu_unmap' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_gem_free_object "$GEM_O" \
	| grep -q 'bl.*<p7580_gem_gpu_unmap>' \
	|| gate_fail "no bl to p7580_gem_gpu_unmap in p7580_gem_free_object disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_gem_free_object "$GEM_O" \
	| grep 'p7580_gem_gpu_unmap' | tee -a "$EVID"
# map_bo is static and inlined into p7580_drv_submit at -O2: the
# two bl sites (purge-before + publish-after) land in the caller's
# disassembly (reloc-gated, robust to branch-range stubs).
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_invalidate')" = 2 ] \
	|| gate_fail "p7580_drv_submit disasm must carry 2 inlined map_bo mmu_invalidate relocs (before+after)"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep 'p7580_mmu_invalidate' | tee -a "$EVID"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_gpu_unmap "$SUBMIT_O" | grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_mmu_invalidate')" = 1 ] \
	|| gate_fail "p7580_gem_gpu_unmap disasm must bl p7580_mmu_invalidate once"
# trace/ERR-MAP strings live in objects + linked Image.
for s in 'p7580_iova: map obj=' 'p7580_iova: unmap obj=' 'p7580_iova: OVERLAP' 'p7580_job: ERR-MAP' 'iova_arm'; do \
	aarch64-linux-gnu-strings "$SUBMIT_O" "$JOB_O" | grep -q "$s" \
	|| gate_fail "$s missing in submit.o/job.o"; \
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "$s" \
	|| gate_fail "$s missing in Image"; \
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'p7580_iova: (map|unmap|OVERLAP)|p7580_job: ERR-MAP' | tee -a "$EVID"
# linked Image: exactly one gpu_unmap definition wins the link.
[ "$(grep -c ' T p7580_gem_gpu_unmap$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_gpu_unmap count != 1"
grep ' p7580_gem_gpu_unmap$' System.map | tee -a "$EVID"
note "== int35 iova gate: PASS (free-time unmap before iova_free, invalidate-map-invalidate, OVERLAP + ERR-MAP live) =="
# 6l. armfence gates (49f7df2: arm-time completion fence — the
# INSTR_TYPE_MISMATCH root cause: the 0004 shim used to create the fence
# in run_job, so SUBMIT-time sched_job->fence was NULL and the out_sync
# install was a no-op; Mesa recycled the batch BO while the GPU read it.
# New contract: alloc at arm (ops->alloc_fence), install armed fence on
# out_sync during the ioctl, run_job binds the same fence, pump enforces
# identity (F4 path on mismatch). FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int35 gate: armfence objects present =="
ls -l --time-style=full-iso "$SCHED_O" "$SUBMIT_O" "$JOB_O" | tee -a "$EVID"
# applied tree: arm hook declared + defined in the shim (arm before deps).
grep -q 'void drm_sched_job_arm' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "drm_sched_job_arm def missing in applied drm_sched.c"
grep -q 'alloc_fence(job)' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "alloc_fence call missing in applied drm_sched.c (arm)"
grep -q 'job->fence = fence' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "fence assignment missing in applied drm_sched.c (arm)"
grep -n 'drm_sched_job_arm\|alloc_fence' drivers/gpu/drm/drm_sched.c | tee -a "$EVID"
grep -q 'alloc_fence' drivers/gpu/drm/drm_sched.h \
	|| gate_fail "alloc_fence hook missing in applied drm_sched.h"
# applied tree: pump identity enforcement (mismatch frees the job, never publishes).
grep -q 'run_fence != job->fence' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "pump identity check missing in applied drm_sched.c"
grep -q 'struct dma_fence \*run_fence' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "run_fence decl missing in applied drm_sched.c pump"
# applied tree: SUBMIT arms before the dep loops and installs the armed
# fence on out_sync during the ioctl.
grep -q 'drm_sched_job_arm(sched_job)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "SUBMIT arm call missing in applied submit.c"
grep -q 'struct dma_fence \*fence = sched_job->fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "SUBMIT out_sync install of armed fence missing in applied submit.c"
grep -q 'drm_syncobj_replace_fence(s, fence)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "SUBMIT replace_fence of armed fence missing in applied submit.c"
# applied tree: run_job binds sched_job->fence (one shared ref, no second
# allocation); the F-JT1 stash (run_job-time install) is gone.
grep -q 'struct dma_fence \*armed = sched_job->fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job armed-fence bind missing in applied submit.c"
grep -q 'dma_fence_get(armed)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job shared-ref get missing in applied submit.c"
grep -q 'No out_sync work here' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job no-out_sync-work marker missing in applied submit.c"
awk '/^static struct dma_fence \*p7580_drv_run_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'drm_syncobj_replace_fence' \
	&& gate_fail "run_job must not call replace_fence (install is SUBMIT-time now)"
# applied tree: jobs TU owns contexts + the ONLY allocator; run binds.
grep -q 'dma_fence_context_alloc(1)' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "fence context init missing in applied job.c"
grep -q 'struct dma_fence \*p7580_job_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "p7580_job_alloc_fence def missing in applied job.c"
grep -q 'p7580_job_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "p7580_job_alloc_fence decl missing in applied job.h"
grep -n 'p7580_job_alloc_fence\|dma_fence_context_alloc' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
# driver 0001: alloc hook + ops table entry + call-site guard extension.
grep -q 'p7580_drv_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "p7580_drv_alloc_fence missing in applied submit.c"
grep -q '\.alloc_fence = p7580_drv_alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "alloc_fence ops entry missing in applied submit.c"
grep -q 'sched_job->ops->alloc_fence' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ops call-site guard must cover alloc_fence in applied submit.c"
# object evidence: arm T in sched.o, U in submit.o; alloc T in job.o + drv hook T in submit.o.
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' T drm_sched_job_arm$' \
	|| gate_fail "drm_sched_job_arm not T in drm_sched.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U drm_sched_job_arm$' \
	|| gate_fail "U drm_sched_job_arm missing in submit.o (SUBMIT must arm)"
aarch64-linux-gnu-nm "$JOB_O" | grep -q ' T p7580_job_alloc_fence$' \
	|| gate_fail "p7580_job_alloc_fence not T in job.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [tT] p7580_drv_alloc_fence$' \
	|| gate_fail "p7580_drv_alloc_fence missing in submit.o (static t expected)"
aarch64-linux-gnu-nm "$SCHED_O" "$SUBMIT_O" "$JOB_O" | grep -E 'drm_sched_job_arm|alloc_fence' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -q 'bl.*<drm_sched_job_arm' \
	|| gate_fail "no bl to drm_sched_job_arm in p7580_drv_submit disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep 'drm_sched_job_arm' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q 'bl.*<drm_syncobj_replace_fence' \
	&& gate_fail "run_job must not install out_sync (install is SUBMIT-time now)"
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_pump "$SCHED_O" \
	| grep -c 'bl.*<dma_fence_put' | tee -a "$EVID"
# Image sanity: ST-OPS/ST-RUN family still live after the 0004 regen.
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d' \
	|| gate_fail "ST-OPS family missing in Image after armfence regen"
# linked Image: exactly one arm + one allocator win the link.
[ "$(grep -c ' T drm_sched_job_arm$' System.map)" = 1 ] \
	|| gate_fail "System.map drm_sched_job_arm count != 1"
[ "$(grep -c ' T p7580_job_alloc_fence$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_job_alloc_fence count != 1"
grep -E ' (drm_sched_job_arm|p7580_job_alloc_fence)$' System.map | tee -a "$EVID"
note "== int35 armfence gate: PASS (alloc at arm, SUBMIT out_sync install, identity pump, contexts live) =="
 # 6m. ptewalk gates (06943c1: walker scans live bo_list for owner
 # resolution (snapshot + live), always re-reads the 8 descriptor words
 # from leaf_phys + page_off (even NO-OWNER), every line prints snap= and
 # live= ranges; jc_page walk from 9231f17 + CROSS-PAGE bound unchanged.
 # Fault-time AS0 PT walk in the PM-held timedout path BEFORE recover
 # rebinds AS0. FAIL (exit 1, scratch kept) on any disagreement.
 note "== int35 gate: ptewalk walker present =="
 grep -q 'jc_page = jc & PAGE_MASK' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk guard jc_page mask missing in applied submit.c"
 grep -q '((jc_page >> shift) & 0x1FFu)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk guard jc_page mid-level index missing in applied submit.c"
 grep -q '((jc_page >> 12) & 0x1FFu)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk guard jc_page leaf index missing in applied submit.c"
 grep -q 'if (jc & ~PAGE_MASK)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	&& gate_fail "stale silent non-aligned early return still present in applied submit.c"
 grep -q 'page_off + 32 > PAGE_SIZE' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk page_off CROSS-PAGE bound missing in applied submit.c"
 grep -q 'p7580_drv_ptewalk_pin' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "p7580_drv_ptewalk_pin missing in applied submit.c"
 grep -q 'p7580_job: PTEWALK' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "PTEWALK pin line missing in applied submit.c"
 grep -q 'leaf_phys=0x%llx bo_phys=0x%llx' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "PTEWALK leaf-vs-BO comparison missing in applied submit.c"
 grep -q 'p7580_drv_ptewalk_pin(drv, job);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "PTEWALK call missing before recover in applied submit.c"
 awk '/p7580_drv_timedout_job\(struct drm_sched_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	| grep -q 'p7580_drv_ptewalk_pin(drv, job);' \
 	|| gate_fail "PTEWALK call not inside timedout path in applied submit.c"
 # 06943c1 new: snap/live owner resolution + unconditional leaf dump.
 grep -q 'snap_iova' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk snap_iova missing in applied submit.c"
 grep -q 'live_iova' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live_iova missing in applied submit.c"
 grep -q 'mutex_lock(&drv->bo_lock)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live bo_list scan (bo_lock) missing in applied submit.c"
 grep -q 'list_for_each_entry(le, &drv->bo_list, node)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk bo_list walk missing in applied submit.c"
 grep -q 'drm_gem_object_reference(le->obj)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live GEM ref missing in applied submit.c"
 grep -q 'if (!owner && live_bo)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live-owner fallback missing in applied submit.c"
 grep -q 'snap=0x%llx/0x%zx live=0x%llx/0x%zx' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk snap=/live= print missing in applied submit.c"
 grep -q 'NO-OWNER snap=' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk NO-OWNER snap= line missing in applied submit.c"
 grep -q 'w=%08x %08x %08x %08x %08x %08x %08x %08x' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk 8-word dump format missing in applied submit.c"
 awk '/NO-OWNER snap=/,/\);/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	| grep -q 'pw\[0\]' \
 	|| gate_fail "ptewalk NO-OWNER line must still dump the 8 words (unconditional dump)"
 grep -q 'drm_gem_object_unreference_unlocked(live_obj)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
 	|| gate_fail "ptewalk live GEM unref missing in applied submit.c"
 aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' [tT] p7580_drv_ptewalk_pin$' \
 	|| aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_timedout_job "$SUBMIT_O" \
 		| grep -q 'PTEWALK\|ptewalk' \
 	|| gate_fail "p7580_drv_ptewalk_pin missing in submit.o (nm + disasm)"
 aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'snap=' \
 	|| gate_fail "snap= string missing in submit.o"
 aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'live=' \
 	|| gate_fail "live= string missing in submit.o"
 aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'NO-OWNER' \
 	|| gate_fail "NO-OWNER string missing in submit.o"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: PTEWALK' \
 	|| gate_fail "PTEWALK string missing in Image"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'snap=' \
 	|| gate_fail "snap= string missing in Image"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'live=' \
 	|| gate_fail "live= string missing in Image"
 aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'NO-OWNER' \
 	|| gate_fail "NO-OWNER string missing in Image"
 grep -n 'p7580_drv_ptewalk_pin\|PTEWALK\|snap_iova\|live_iova\|bo_lock' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
 note "== int35 ptewalk gate: PASS (jc_page walk intact, snap/live owner + unconditional leaf dump live) =="
# 6n. pmget gates (0d42bcd: normalise pm_get 1-is-powered to 0; every
# caller tests `< 0` — raw 1 "already active" is success, only negative
# errno is dark. run_job ST-PMGET fail + timedout dark early-return +
# ioctl override must all use `< 0`; helper must normalise 1 to 0.
# FAIL (exit 1, scratch kept) on any disagreement.
note "== int35 gate: pmget call sites present =="
grep -q 'if (p7580_drv_pm_get(drv) < 0)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "run_job pm_get < 0 missing in applied submit.c"
grep -q 'pmraw = p7580_drv_pm_get(drv);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "timedout pmraw capture missing in applied submit.c"
grep -q 'if (pmraw < 0)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "timedout pmraw < 0 missing in applied submit.c"
[ "$(grep -c 'p7580_drv_pm_get(drv)' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 5 ] \
	|| gate_fail "submit.c must carry exactly 5 pm_get call sites (run_job + timedout capture + pm_arm + teardown_hold + ioctl override), got $(grep -c 'p7580_drv_pm_get(drv)' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
[ "$(grep -c 'p7580_drv_pm_get(drv) < 0' drivers/gpu/drm/panfrost/panfrost7580_submit.c)" = 1 ] \
	|| gate_fail "only run_job keeps the inline pm_get < 0 form (timedout uses pmraw now), got $(grep -c 'p7580_drv_pm_get(drv) < 0' drivers/gpu/drm/panfrost/panfrost7580_submit.c)"
grep -q 'if (ret < 0)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "ioctl override ret < 0 missing in applied submit.c"
grep -q 'Normalise pm_runtime_get_sync' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "pm_get 1-to-0 normalisation missing in applied submit.c"
grep -n 'p7580_drv_pm_get(drv)\|if (pmraw < 0)\|if (ret < 0)\|Normalise pm_runtime_get_sync' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_pm_get "$SUBMIT_O" \
	| grep -q 'cmp.*#0x1\|mov.*#0x1\|cmp.*#1' \
	|| note "pm_get normalisation compare not pattern-visible (source-gated above)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: pm_get FAIL' \
	|| gate_fail "ST-PMGET fail string missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_job: PTEWALK' \
	|| gate_fail "PTEWALK string missing in Image (pmget must not drop ptewalk)"
note "== int35 pmget gate: PASS (run_job + timedout + ioctl < 0, 1 normalised, PTEWALK live) =="
# 6o. timedout-probe gates (b023f4b: three unconditional pr_err pins in
# the PM-held timedout path -- TMD-ENTER at wrapper entry (sched_job/job/
# drv/pmraw captured at declaration), TMD-PREWALK before the PT walk,
# TMD-WALKER at walker entry. `< 0` pm_get semantics unchanged.
# FAIL (exit 1, scratch kept) on any disagreement.
note "== int35 gate: timedout-probe pins present =="
grep -q 'p7580_job: TMD-ENTER sched_job=%lu job=%lu drv=%lu pmraw=%d' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-ENTER pin missing in applied submit.c"
grep -q 'p7580_job: TMD-PREWALK job=%lu drv=%lu' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-PREWALK pin missing in applied submit.c"
grep -q 'p7580_job: TMD-WALKER drv=%lu job=%lu' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-WALKER pin missing in applied submit.c"
grep -q 'pmraw = p7580_drv_pm_get(drv);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "TMD-ENTER pmraw capture missing in applied submit.c"
awk '/p7580_drv_timedout_job\(struct drm_sched_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'p7580_job: TMD-ENTER' \
	|| gate_fail "TMD-ENTER not inside timedout path in applied submit.c"
awk '/p7580_drv_timedout_job\(struct drm_sched_job/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'p7580_job: TMD-PREWALK' \
	|| gate_fail "TMD-PREWALK not inside timedout path in applied submit.c"
awk '/p7580_drv_ptewalk_pin\(struct p7580_drv/,/^}/' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	| grep -q 'p7580_job: TMD-WALKER' \
	|| gate_fail "TMD-WALKER not inside walker in applied submit.c"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_job: TMD-ENTER' \
	|| gate_fail "TMD-ENTER missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_job: TMD-PREWALK' \
	|| gate_fail "TMD-PREWALK missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_job: TMD-WALKER' \
	|| gate_fail "TMD-WALKER missing in submit.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep 'p7580_job: TMD-' | tee -a "$EVID"
for s in 'p7580_job: TMD-ENTER' 'p7580_job: TMD-PREWALK' 'p7580_job: TMD-WALKER'; do \
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q "$s" \
		|| gate_fail "probe '$s' missing in Image"; \
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep 'p7580_job: TMD-' | tee -a "$EVID"
note "Image p7580_job: TMD- count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_job: TMD-')"
# hunk integrity: driver 0001 submit.c fully applied -- late function
# panfrost_ioctl_submit_override (last def in the 1594-line hunk) must
# exist in the applied tree and the object, so a silent truncation
# (the int27 first-run failure mode) fails loudly here.
grep -q 'int panfrost_ioctl_submit_override' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "late function panfrost_ioctl_submit_override missing in applied submit.c (hunk truncated?)"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' T panfrost_ioctl_submit_override$' \
	|| gate_fail "panfrost_ioctl_submit_override missing in submit.o nm (hunk truncated?)"
grep -q ' T panfrost_ioctl_submit_override$' System.map \
	|| gate_fail "panfrost_ioctl_submit_override missing in System.map"
grep ' panfrost_ioctl_submit_override$' System.map | tee -a "$EVID"
note "== int35 timedout-probe gate: PASS (TMD-ENTER/PREWALK/WALKER in tree + submit.o + Image, submit.c hunk intact) =="
# 6p. submitsnap gates (c95f7b9: SUBMIT-time w0..w7 snapshot in
# job->desc[8], sw= on ERR/TIMEOUT/dispatch/dump, PTEWALK gains sw=,
# JS_HEAD=jc verdict (offset 0 correct, no dispatch change)).
# FAIL (exit 1, scratch kept) on any disagreement.
note "== int35 gate: submitsnap snapshot present =="
grep -q 'uint32_t desc\[8\]' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "desc[8] snapshot missing in applied job.h"
grep -q 'uint32_t sw\[8\]' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	|| gate_fail "last.sw[8] missing in applied job.h"
grep -q 'job->desc\[wi\]' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "SUBMIT 8-word snapshot loop missing in applied submit.c"
grep -q 'jd->last.sw' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "dispatch last.sw copy missing in applied job.c"
grep -q 'sw=%08x %08x %08x %08x %08x %08x %08x %08x' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	|| gate_fail "sw= 8-word print missing in applied job.c (ERR/dispatch/TIMEOUT)"
grep -q 'sw=%08x %08x %08x %08x %08x %08x %08x %08x' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "PTEWALK sw= compare missing in applied submit.c"
grep -q 'job->desc\[4\] &= ~0x1u' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "chain_mode desc[4] clear missing in applied submit.c"
grep -q 'desc_w4' drivers/gpu/drm/panfrost/panfrost7580_job.c \
	&& gate_fail "stale desc_w4 token still present in applied job.c"
grep -q 'desc_w4' drivers/gpu/drm/panfrost/panfrost7580_job.h \
	&& gate_fail "stale desc_w4 token still present in applied job.h"
aarch64-linux-gnu-strings "$JOB_O" | grep -q 'sw=' \
	|| gate_fail "sw= missing in job.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'sw=' \
	|| gate_fail "sw= missing in submit.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'sw=' \
	|| gate_fail "sw= missing in Image"
grep -n 'sw=' drivers/gpu/drm/panfrost/panfrost7580_job.c | tee -a "$EVID"
grep -n 'sw=' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
note "Image sw= count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'sw=')"
note "== int35 submitsnap gate: PASS (desc[8] snapshot, sw= on ERR/TIMEOUT/dispatch/PTEWALK live) =="
# 6q. syncobj-wait gates (b79c502: WAIT_ALL zero-budget false-success
# fixed via is_signaled re-test, writable signaled-ops, WAIT-RECHECK
# device lines in the client). FAIL (exit 1, scratch kept) on any
# disagreement.
note "== int35 gate: syncobj-wait fix present =="
grep -q 'if (!dma_fence_is_signaled(fences\[i\]))' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "WAIT_ALL is_signaled re-test missing in applied syncobj.c"
grep -q 'Only a signaled fence counts' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "WAIT_ALL re-test comment missing in applied syncobj.c"
grep -q 'static struct dma_fence_ops p7580_sync_signaled_ops' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "writable signaled-ops table missing in applied syncobj.c"
grep -q 'static const struct dma_fence_ops p7580_sync_signaled_ops' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	&& gate_fail "stale const signaled-ops still present in applied syncobj.c"
grep -q '\.release = p7580_sync_signaled_release' drivers/gpu/drm/panfrost/panfrost7580_syncobj.c \
	|| gate_fail "signaled release-at-definition missing in applied syncobj.c"
aarch64-linux-gnu-nm "$SYNC_O" | grep -q ' T panfrost_syncobj_ioctl$' \
	|| gate_fail "panfrost_syncobj_ioctl missing in syncobj.o (wait path)"
grep -q 'WAIT-RECHECK' "$REPO/code/gpu31/client/pf7580-selftest.c" \
	|| gate_fail "WAIT-RECHECK device line missing in client pf7580-selftest.c"
grep -n 'WAIT-RECHECK' "$REPO/code/gpu31/client/pf7580-selftest.c" | tee -a "$EVID"
note "== int35 syncobj-wait gate: PASS (WAIT_ALL re-test, writable signaled-ops, WAIT-RECHECK lines) =="
# 6r. jddecode gates (fed4e0c: bo_count stamp re-arms excl/PTEWALK/
# sync/unref; snapshot owner lookup scans live bo_list; 512 B JDBODY
# dump). FAIL (exit 1, scratch kept) on any disagreement.
note "== int35 gate: jddecode bo_count stamp present =="
grep -q 'job->bo_count = nr;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "bo_count stamp missing in applied submit.c"
grep -q 'stamp bo_count here' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "bo_count stamp comment missing in applied submit.c"
grep -n 'job->bo_count = nr;' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# disasm evidence: the stamp lands inside p7580_drv_submit.
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -q 'str.*\[x[0-9]*, #' \
	|| gate_fail "no store in p7580_drv_submit disasm (bo_count stamp path)"
# snapshot owner lookup now scans the live bo_list (same scan as PTEWALK pin).
grep -q 'list_for_each_entry(le, &drv->bo_list, node)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "live bo_list snapshot lookup missing in applied submit.c"
grep -q 'p7580_job: JDBODY' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "JDBODY dump missing in applied submit.c"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'JDBODY' \
	|| gate_fail "JDBODY missing in submit.o"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'JDBODY' \
	|| gate_fail "JDBODY missing in Image"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep 'JDBODY' | tee -a "$EVID"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'JDBODY' | tee -a "$EVID"
note "== int35 jddecode gate: PASS (bo_count stamp, live bo_list snapshot lookup, JDBODY live) =="
# 6s. multidep gates (7e71c82: wait-all AND node for N in_syncs
# (no more single-dep -EOPNOTSUPP), bounded stage+errno on every early
# error path in the shim (drm_sched_job_err) and the driver 0001
# (p7580_submit_err)). FAIL (exit 1, scratch kept) on any disagreement.
note "== int35 gate: multidep wait-all dep path present =="
# applied tree: AND-node struct + ops + merge helper + err helpers.
grep -q 'struct drm_sched_dep_and' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "AND-node struct missing in applied drm_sched.c"
grep -q 'drm_sched_dep_and_ops' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "AND-node ops missing in applied drm_sched.c"
grep -q 'drm_sched_dep_merge' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "AND merge helper missing in applied drm_sched.c"
grep -q 'drm_sched_job_add_dependency' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "add_dependency missing in applied drm_sched.c"
grep -q 'drm_sched_job_err' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "shim err helper missing in applied drm_sched.c"
grep -q 'p7580_submit_err' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "driver submit_err helper missing in applied submit.c"
# old single-dep reject must be gone from the dependency path: no
# -EOPNOTSUPP return in the applied drm_sched.c.
grep -q 'EOPNOTSUPP' drivers/gpu/drm/drm_sched.c \
	&& gate_fail "stale -EOPNOTSUPP reject still present in applied drm_sched.c"
# stage errno strings in the applied tree.
grep -q 'p7580_sched: err stage=%s rc=%ld' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "shim err stage string missing in applied drm_sched.c"
grep -q 'p7580_sched: submit_err stage=%s rc=%ld' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "driver submit_err stage string missing in applied submit.c"
# staged call sites present in the driver dep loops.
grep -q 'p7580_submit_err("dep"' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "driver dep staged error missing in applied submit.c"
# symbols/relocs live in the objects (merge + err helpers are static and
# inlined into the single exported add_dependency at -O2, so the nm
# evidence is the static AND-node symbols + the exported entry point;
# the reloc evidence is the inlined body's dma_fence_init /
# printk / leaf-callback references inside add_dependency's disasm).
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' r drm_sched_dep_and_ops$' \
	|| gate_fail "drm_sched_dep_and_ops missing in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' t drm_sched_dep_and_leaf_fired$' \
	|| gate_fail "drm_sched_dep_and_leaf_fired missing in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -q ' T drm_sched_job_add_dependency$' \
	|| gate_fail "drm_sched_job_add_dependency missing in drm_sched.o"
aarch64-linux-gnu-nm "$SCHED_O" | grep -E 'drm_sched_dep_and_(ops|leaf_fired)|drm_sched_job_add_dependency' | tee -a "$EVID"
# the inlined merge body inside add_dependency references the AND node
# (dma_fence_init for the node alloc, leaf callbacks for the trampolines,
# printk for the inlined err-helper stage prints).
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*dma_fence_init' \
	|| gate_fail "no dma_fence_init reloc in add_dependency disasm (AND-node alloc path)"
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -q 'drm_sched_dep_and_leaf_cb_' \
	|| gate_fail "no leaf-callback reloc in add_dependency disasm (AND-node trampolines)"
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*printk' \
	|| gate_fail "no printk reloc in add_dependency disasm (inlined err-helper stage prints)"
aarch64-linux-gnu-objdump -dr --disassemble=drm_sched_job_add_dependency "$SCHED_O" \
	| grep -E 'dma_fence_init|dep_and_leaf_cb_|printk' | tee -a "$EVID"
# AND-node strings + stage errno strings live in the objects.
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: err stage=' \
	|| gate_fail "err stage= missing in drm_sched.o"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'drm_sched_dep_and' \
	|| gate_fail "dep_and driver name missing in drm_sched.o"
aarch64-linux-gnu-strings "$SUBMIT_O" | grep -q 'p7580_sched: submit_err stage=' \
	|| gate_fail "submit_err stage= missing in submit.o"
# linked Image: stage errno strings live (AND-node ops is static data,
# checked in the object above; the linked evidence is the strings).
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: err stage=' \
	|| gate_fail "err stage= missing in Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: submit_err stage=' \
	|| gate_fail "submit_err stage= missing in Image"
grep -q ' T drm_sched_job_add_dependency$' System.map \
	|| gate_fail "drm_sched_job_add_dependency missing in System.map"
grep 'drm_sched_job_add_dependency$' System.map | tee -a "$EVID"
note "Image err stage= count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_sched: err stage=')"
note "Image submit_err stage= count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_sched: submit_err stage=')"
note "== int35 multidep gate: PASS (wait-all AND node live, stage errno strings live, no EOPNOTSUPP) =="
# 6t. serialfence gates (0072a74: per-file implicit serialization --
# each SUBMIT after arm borrows the previous submit's armed fence
# (serial_prev under serial_lock) and attaches it as a dependency
# when still unsignaled, then publishes its own armed fence; rollback
# clears the slot on error paths, fini drops the last slot ref; slot
# assignment untouched). FAIL (exit 1, scratch kept) on disagreement.
note "== int35 gate: serial wiring present =="
grep -q 'struct mutex serial_lock;' drivers/gpu/drm/panfrost/panfrost7580_drv.h \
	|| gate_fail "serial_lock missing in applied drv.h"
grep -q 'struct dma_fence \*serial_prev;' drivers/gpu/drm/panfrost/panfrost7580_drv.h \
	|| gate_fail "serial_prev missing in applied drv.h"
grep -q 'mutex_init(&df->serial_lock);' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_lock init missing in applied submit.c"
grep -q 'df->serial_prev = NULL;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_prev NULL init missing in applied submit.c"
grep -q 'struct dma_fence \*published = NULL;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "published tracker missing in applied submit.c"
grep -q 'prev = df->serial_prev;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_prev borrow missing in applied submit.c"
grep -q '!dma_fence_is_signaled(prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "signaled-skip guard missing in applied submit.c"
grep -q 'drm_sched_job_add_dependency(sched_job, prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial implicit dep attach missing in applied submit.c"
grep -q 'df->serial_prev = published;' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial_prev publish missing in applied submit.c"
grep -q 'if (df->serial_prev == published)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial rollback missing in applied submit.c"
grep -n 'serial_lock\|serial_prev\|published\|add_dependency(sched_job, prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c | tee -a "$EVID"
# disasm evidence: the serial attach path inside p7580_drv_submit calls
# add_dependency 3x (in_sync loop + excl + serial prev). The
# dma_fence_is_signaled skip-guard is a static inline (test_bit on
# fence->flags in dma-fence.h): no out-of-line call at -O2, so the
# compiled evidence is the tree guard (gated above) plus the
# add_dependency relocs + the U ref; source-gate the exact inline use.
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_add_dependency' \
	|| gate_fail "no add_dependency reloc in p7580_drv_submit disasm (serial attach path)"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*drm_sched_job_add_dependency')" -ge 3 ] \
	|| gate_fail "expected >=3 add_dependency relocs in p7580_drv_submit (in_sync/excl/serial), got fewer"
grep -q '!dma_fence_is_signaled(prev)' drivers/gpu/drm/panfrost/panfrost7580_submit.c \
	|| gate_fail "serial is_signaled inline guard missing in applied submit.c"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep 'drm_sched_job_add_dependency' | tee -a "$EVID"
# fini drop + object linkage: serial fields referenced from submit.o,
# dep target resolved in the linked kernel.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U drm_sched_job_add_dependency$' \
	|| gate_fail "add_dependency U ref missing in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -E 'drm_sched_job_add_dependency' | tee -a "$EVID"
grep -q ' T drm_sched_job_add_dependency$' System.map \
	|| gate_fail "add_dependency missing in System.map (serial dep target)"
note "== int35 serialfence gate: PASS (serial_prev chain wired, dep attach + publish + rollback + fini live) =="
# 6u. cachedir gates (6d187b2: direction-correct DMA cache maintenance;
# CLEAN before ANY invalidate). Tree: helper defs/decls, clean-then-
# invalidate order, SUBMIT clean-only, unbind clean-before-unmap, no
# bare sync_for_cpu(BIDIRECTIONAL) left; objects: direction immediates
# (TO_DEVICE clean / FROM_DEVICE invalidate) through the dma_map_ops
# slots COMPUTED from the header, call order inside each helper,
# submit.o call sites (snapshot + JDBODY invalidate, no bare
# job_done_sync in the snapshot), System.map exports. FAIL (exit 1,
# scratch kept) on any disagreement.
note "== int35 gate: cachedir helpers present =="
GEMC=drivers/gpu/drm/panfrost/panfrost7580_gem.c
GEMH=drivers/gpu/drm/panfrost/panfrost7580_gem.h
SUBC=drivers/gpu/drm/panfrost/panfrost7580_submit.c
grep -q '^void p7580_gem_clean_for_device(struct drm_gem_object \*obj)$' "$GEMC" \
	|| gate_fail "p7580_gem_clean_for_device def missing in applied gem.c"
grep -q '^void p7580_gem_invalidate_for_cpu(struct drm_gem_object \*obj)$' "$GEMC" \
	|| gate_fail "p7580_gem_invalidate_for_cpu def missing in applied gem.c"
grep -q '^void p7580_gem_clean_for_device(struct drm_gem_object \*obj);$' "$GEMH" \
	|| gate_fail "clean_for_device decl missing in applied gem.h"
grep -q '^void p7580_gem_invalidate_for_cpu(struct drm_gem_object \*obj);$' "$GEMH" \
	|| gate_fail "invalidate_for_cpu decl missing in applied gem.h"
# the one shared page walk carries both directions and nothing else:
# exactly 2 dma_sync calls, DMA_TO_DEVICE on the for_device leg,
# DMA_FROM_DEVICE on the for_cpu leg, NEVER DMA_BIDIRECTIONAL.
SYNC_SRC=$(awk '/^static void p7580_bo_sync_pages/,/^}/' "$GEMC")
[ "$(echo "$SYNC_SRC" | grep -c 'dma_sync_single_for_')" = 2 ] \
	|| gate_fail "p7580_bo_sync_pages must carry exactly 2 dma_sync calls, got $(echo "$SYNC_SRC" | grep -c 'dma_sync_single_for_')"
echo "$SYNC_SRC" | grep -q 'DMA_TO_DEVICE' \
	|| gate_fail "DMA_TO_DEVICE missing on the for_device leg in applied gem.c"
echo "$SYNC_SRC" | grep -q 'DMA_FROM_DEVICE' \
	|| gate_fail "DMA_FROM_DEVICE missing on the for_cpu leg in applied gem.c"
echo "$SYNC_SRC" | grep -q 'DMA_BIDIRECTIONAL' \
	&& gate_fail "stale DMA_BIDIRECTIONAL sync still present in p7580_bo_sync_pages (direction-correct only)"
# invalidate = clean THEN invalidate (the order is the contract).
INV_SRC=$(awk '/^void p7580_gem_invalidate_for_cpu/,/^}/' "$GEMC")
INV_CLEAN=$(echo "$INV_SRC" | grep -n 'p7580_bo_sync_pages(bo, 1);' | cut -d: -f1)
INV_DROP=$(echo "$INV_SRC" | grep -n 'p7580_bo_sync_pages(bo, 0);' | cut -d: -f1)
[ -n "$INV_CLEAN" ] && [ -n "$INV_DROP" ] && [ "$INV_CLEAN" -lt "$INV_DROP" ] \
	|| gate_fail "invalidate_for_cpu is not clean-then-invalidate in applied gem.c (clean=$INV_CLEAN drop=$INV_DROP)"
# SUBMIT = clean only (never an invalidate: the BO body is the CPU's output).
SUB_SRC=$(awk '/^void p7580_gem_submit_sync/,/^}/' "$GEMC")
echo "$SUB_SRC" | grep -q 'p7580_gem_clean_for_device(obj);' \
	|| gate_fail "submit_sync must call clean_for_device in applied gem.c"
echo "$SUB_SRC" | grep -q 'invalidate_for_cpu\|DMA_FROM_DEVICE\|p7580_bo_sync_pages(bo, 0)' \
	&& gate_fail "submit_sync still invalidates (must be clean only) in applied gem.c"
# completion = clean-then-invalidate via the helper.
DONE_SRC=$(awk '/^void p7580_gem_job_done_sync/,/^}/' "$GEMC")
echo "$DONE_SRC" | grep -q 'p7580_gem_invalidate_for_cpu(obj);' \
	|| gate_fail "job_done_sync must call invalidate_for_cpu in applied gem.c"
echo "$DONE_SRC" | grep -q 'dma_sync_single_for_cpu(bo->sync_dev' \
	&& gate_fail "job_done_sync still runs a bare sync_for_cpu in applied gem.c"
# unbind: clean BEFORE dma_unmap_page (BIDIRECTIONAL unmap invalidates).
UB_SRC=$(awk '/^void p7580_gem_dma_unbind/,/^}/' "$GEMC")
UB_CLEAN=$(echo "$UB_SRC" | grep -n 'p7580_gem_clean_for_device(obj);' | cut -d: -f1)
UB_UNMAP=$(echo "$UB_SRC" | grep -n 'dma_unmap_page(' | cut -d: -f1)
[ -n "$UB_CLEAN" ] && [ -n "$UB_UNMAP" ] && [ "$UB_CLEAN" -lt "$UB_UNMAP" ] \
	|| gate_fail "dma_unbind must clean before dma_unmap_page (clean=$UB_CLEAN unmap=$UB_UNMAP)"
grep -n 'p7580_gem_invalidate_for_cpu\|p7580_gem_clean_for_device\|dma_sync_single_for_' "$GEMC" | tee -a "$EVID"
# driver 0001 call sites: snapshot + JDBODY owner BO invalidate, and the
# snapshot must no longer run the bare job_done_sync.
grep -q 'p7580_gem_invalidate_for_cpu(&jbo->base);' "$SUBC" \
	|| gate_fail "snapshot invalidate_for_cpu call missing in applied submit.c"
grep -q 'p7580_gem_invalidate_for_cpu(owner);' "$SUBC" \
	|| gate_fail "PTEWALK JDBODY invalidate_for_cpu call missing in applied submit.c"
[ "$(grep -c 'p7580_gem_job_done_sync(' "$SUBC")" = 1 ] \
	|| gate_fail "submit.c must carry exactly 1 job_done_sync call (free_job only), got $(grep -c 'p7580_gem_job_done_sync(' "$SUBC")"
SNAP_INV=$(grep -n 'p7580_gem_invalidate_for_cpu(&jbo->base);' "$SUBC" | cut -d: -f1)
SNAP_RD=$(grep -n 'job->desc\[wi\] =' "$SUBC" | cut -d: -f1)
[ -n "$SNAP_INV" ] && [ -n "$SNAP_RD" ] && [ "$SNAP_INV" -lt "$SNAP_RD" ] \
	|| gate_fail "snapshot must invalidate the owner BO before reading desc (inv=$SNAP_INV read=$SNAP_RD)"
OWN_INV=$(grep -n 'p7580_gem_invalidate_for_cpu(owner);' "$SUBC" | cut -d: -f1)
JDBODY_LN=$(grep -n 'p7580_job: JDBODY' "$SUBC" | head -1 | cut -d: -f1)
[ -n "$OWN_INV" ] && [ -n "$JDBODY_LN" ] && [ "$OWN_INV" -lt "$JDBODY_LN" ] \
	|| gate_fail "JDBODY dump must invalidate the owner BO before the kmap read (inv=$OWN_INV dump=$JDBODY_LN)"
grep -n 'p7580_gem_invalidate_for_cpu\|p7580_gem_job_done_sync\|p7580_gem_submit_sync\|job->desc\[wi\] =' "$SUBC" | tee -a "$EVID"
# tree-computed direction values + dma_map_ops slots (pahole-free): the
# immediates/slots the compiler emits must match the headers the tree
# was built with.
DIR_TO=$(awk '/enum dma_data_direction/,/};/' include/linux/dma-direction.h \
	| sed -n 's/.*DMA_TO_DEVICE = \([0-9][0-9]*\).*/\1/p')
DIR_FROM=$(awk '/enum dma_data_direction/,/};/' include/linux/dma-direction.h \
	| sed -n 's/.*DMA_FROM_DEVICE = \([0-9][0-9]*\).*/\1/p')
[ "$DIR_TO" = 1 ] || gate_fail "DMA_TO_DEVICE computed $DIR_TO != 1"
[ "$DIR_FROM" = 2 ] || gate_fail "DMA_FROM_DEVICE computed $DIR_FROM != 2"
DMAOPS_OFF=$(awk '/^struct dma_map_ops \{/,/^\};/' include/linux/dma-mapping.h \
	| awk 'match($0, /\(\*[a-z_0-9]+\)/) {
		n = substr($0, RSTART + 2, RLENGTH - 3);
		if (n == "sync_single_for_cpu") print "cpu=" off;
		if (n == "sync_single_for_device") print "dev=" off;
		if (n == "unmap_page") print "unmap=" off;
		off += 8 }')
OFF_CPU=$(echo "$DMAOPS_OFF" | sed -n 's/^cpu=//p')
OFF_DEV=$(echo "$DMAOPS_OFF" | sed -n 's/^dev=//p')
OFF_UNMAP=$(echo "$DMAOPS_OFF" | sed -n 's/^unmap=//p')
[ -n "$OFF_CPU" ] && [ -n "$OFF_DEV" ] && [ -n "$OFF_UNMAP" ] \
	|| gate_fail "could not compute dma_map_ops slots from include/linux/dma-mapping.h"
note "computed dma_map_ops slots: for_cpu=$OFF_CPU for_device=$OFF_DEV unmap=$OFF_UNMAP; dirs TO=$DIR_TO FROM=$DIR_FROM"
# object evidence: helper T symbols + the outlined page walk.
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_clean_for_device$' \
	|| gate_fail "p7580_gem_clean_for_device not T in gem.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' T p7580_gem_invalidate_for_cpu$' \
	|| gate_fail "p7580_gem_invalidate_for_cpu not T in gem.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -q ' t p7580_bo_sync_pages$' \
	|| gate_fail "p7580_bo_sync_pages not t in gem.o"
aarch64-linux-gnu-nm "$GEM_O" | grep -E ' p7580_(gem_(clean_for_device|invalidate_for_cpu)|bo_sync_pages)$' | tee -a "$EVID"
# the directions and slots the compiled walk really passes.
SYNC_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_bo_sync_pages "$GEM_O")
echo "$SYNC_DIS" | grep -Eq "mov[[:space:]]+w3, #0x$DIR_TO([^0-9a-f]|\$)" \
	|| gate_fail "no DMA_TO_DEVICE (w3=#0x$DIR_TO) immediate in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -Eq "mov[[:space:]]+w3, #0x$DIR_FROM([^0-9a-f]|\$)" \
	|| gate_fail "no DMA_FROM_DEVICE (w3=#0x$DIR_FROM) immediate in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -Eq "mov[[:space:]]+w3, #0x0([^0-9a-f]|\$)" \
	&& gate_fail "p7580_bo_sync_pages still passes DMA_BIDIRECTIONAL (w3=#0x0)"
echo "$SYNC_DIS" | grep -q "\[x[0-9]*, #$OFF_DEV\]" \
	|| gate_fail "no dma_ops sync_single_for_device slot (#$OFF_DEV) load in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -q "\[x[0-9]*, #$OFF_CPU\]" \
	|| gate_fail "no dma_ops sync_single_for_cpu slot (#$OFF_CPU) load in p7580_bo_sync_pages disasm"
# slot load -> direction immediate pairing (same basic block): the
# for_device leg must pass TO_DEVICE, the for_cpu leg FROM_DEVICE.
echo "$SYNC_DIS" | awk '
	/ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_DEV"'\]/ { want = 1; devline = NR }
	/ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_CPU"'\]/ { want = 2; cpuline = NR }
	/mov[[:space:]]+w3, #0x1([^0-9a-f]|$)/ { if (want == 1 && NR - devline <= 6) okdev = 1; want = 0 }
	/mov[[:space:]]+w3, #0x2([^0-9a-f]|$)/ { if (want == 2 && NR - cpuline <= 6) okcpu = 1; want = 0 }
	END { exit !(okdev && okcpu) }' \
	|| gate_fail "direction immediate not paired with its dma_ops slot in p7580_bo_sync_pages disasm"
echo "$SYNC_DIS" | grep -E 'ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_DEV"'\]|ldr[[:space:]]+x[0-9]*, \[x[0-9]*, #'"$OFF_CPU"'\]|mov[[:space:]]+w3, #0x[0-9]' | tee -a "$EVID"
# per-helper call counts + order (clean first, then the for_cpu leg).
CL_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_clean_for_device "$GEM_O")
INV_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_invalidate_for_cpu "$GEM_O")
SUB_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_submit_sync "$GEM_O")
DONE_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_job_done_sync "$GEM_O")
UB_DIS=$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_dma_unbind "$GEM_O")
[ "$(echo "$CL_DIS" | grep -c '<p7580_bo_sync_pages>')" = 1 ] \
	|| gate_fail "clean_for_device must make exactly 1 sync_pages call (clean only)"
echo "$CL_DIS" | grep -Eq 'mov[[:space:]]+w1, #0x0' \
	&& gate_fail "clean_for_device passes toward_device=0 (wrong direction)"
[ "$(echo "$SUB_DIS" | grep -c '<p7580_bo_sync_pages>')" = 1 ] \
	|| gate_fail "submit_sync must make exactly 1 sync_pages call (clean only)"
echo "$SUB_DIS" | grep -Eq 'mov[[:space:]]+w1, #0x0' \
	&& gate_fail "submit_sync passes toward_device=0 (must be clean only)"
for fn_dis in "$INV_DIS" "$DONE_DIS"; do
	[ "$(echo "$fn_dis" | grep -c '<p7580_bo_sync_pages>')" = 2 ] \
		|| gate_fail "invalidate/completion helper must make exactly 2 sync_pages calls (clean + invalidate)"
	echo "$fn_dis" | awk '
		/<p7580_bo_sync_pages>/ { n++; if (n == 1) first = NR; if (n == 2) second = NR }
		/mov[[:space:]]+w1, #0x0/ { z = NR }
		END { exit !(first && second && z && z > first && second > z) }' \
		|| gate_fail "clean-then-invalidate call order broken in a gem helper (object)"
done
echo "$INV_DIS" | grep -E '<p7580_bo_sync_pages>|mov[[:space:]]+w1, #0x0' | tee -a "$EVID"
# unbind: helper call before the unmap_page dma_ops slot, BIDIRECTIONAL
# (w3=#0x0) on the unmap itself.
UB_CALL=$(echo "$UB_DIS" | grep -n '<p7580_gem_clean_for_device>' | cut -d: -f1)
UB_SLOT=$(echo "$UB_DIS" | grep -n "\[x[0-9]*, #$OFF_UNMAP\]" | head -1 | cut -d: -f1)
[ -n "$UB_CALL" ] && [ -n "$UB_SLOT" ] && [ "$UB_CALL" -lt "$UB_SLOT" ] \
	|| gate_fail "dma_unbind must call clean_for_device before the unmap_page slot (call=$UB_CALL slot=$UB_SLOT)"
echo "$UB_DIS" | grep -Eq 'mov[[:space:]]+w3, #0x0([^0-9a-f]|$)' \
	|| gate_fail "dma_unbind unmap is not DMA_BIDIRECTIONAL (w3=#0x0) in object"
echo "$UB_DIS" | grep -E '<p7580_gem_clean_for_device>|\[x[0-9]*, #'"$OFF_UNMAP"'\]|mov[[:space:]]+w3, #0x0' | tee -a "$EVID"
# debug read + pre-signal completion both route through the
# clean-then-invalidate helper (no bare invalidate anywhere).
aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_debug_read_word "$GEM_O" \
	| grep -q '<p7580_gem_invalidate_for_cpu>' \
	|| gate_fail "debug_read_word does not route through invalidate_for_cpu"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_gem_job_bos_pre_signal_sync "$GEM_O" \
	| grep -q '<p7580_gem_invalidate_for_cpu>' \
	|| gate_fail "pre_signal completion does not route through invalidate_for_cpu"
# submit.o: snapshot + JDBODY call sites; free_job keeps job_done_sync.
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U p7580_gem_invalidate_for_cpu$' \
	|| gate_fail "invalidate_for_cpu U ref missing in submit.o"
aarch64-linux-gnu-nm "$SUBMIT_O" | grep -q ' U p7580_gem_submit_sync$' \
	|| gate_fail "submit_sync U ref missing in submit.o"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_invalidate_for_cpu')" = 1 ] \
	|| gate_fail "snapshot must carry exactly 1 invalidate_for_cpu call in p7580_drv_submit"
[ "$(aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -c 'R_AARCH64_CALL26[[:space:]]*p7580_gem_submit_sync')" = 2 ] \
	|| gate_fail "p7580_drv_submit must carry 2 submit_sync calls (jd + chain_mode fixup)"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -q 'job_done_sync' \
	&& gate_fail "snapshot still runs the bare job_done_sync in p7580_drv_submit"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_ptewalk_pin "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*p7580_gem_invalidate_for_cpu' \
	|| gate_fail "JDBODY dump does not invalidate the owner BO in p7580_drv_ptewalk_pin"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_free_job "$SUBMIT_O" \
	| grep -q 'R_AARCH64_CALL26[[:space:]]*p7580_gem_job_done_sync' \
	|| gate_fail "free_job post-signal job_done_sync lost"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_submit "$SUBMIT_O" \
	| grep -E 'p7580_gem_(invalidate_for_cpu|submit_sync)' | tee -a "$EVID"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_ptewalk_pin "$SUBMIT_O" \
	| grep -E 'p7580_gem_invalidate_for_cpu' | tee -a "$EVID"
aarch64-linux-gnu-objdump -dr --disassemble=p7580_drv_free_job "$SUBMIT_O" \
	| grep -E 'p7580_gem_job_done_sync' | tee -a "$EVID"
# linked kernel: exactly one strong def each.
[ "$(grep -c ' T p7580_gem_clean_for_device$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_clean_for_device count != 1"
[ "$(grep -c ' T p7580_gem_invalidate_for_cpu$' System.map)" = 1 ] \
	|| gate_fail "System.map p7580_gem_invalidate_for_cpu count != 1"
grep -E ' (p7580_gem_clean_for_device|p7580_gem_invalidate_for_cpu)$' System.map | tee -a "$EVID"
note "== int35 cachedir gate: PASS (clean-for-device TO_DEVICE + clean-then-invalidate FROM_DEVICE, submit clean-only, unbind clean-before-unmap, snapshot/JDBODY/pre-signal routed, no bare BIDIRECTIONAL) =="



note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/drm_sched.c" 2>&1 | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/panfrost/" 2>&1 | tee -a "$EVID"

# 7. pack boot.img (stock dt.img rule: dt.img is NEVER rebuilt).
# All pack I/O on the external disk: seed inputs are staged from /tmp/bimg
# (read-only) into $EXT/stage-int35; outputs land in $EXT.
STAGE=${STAGE:-$EXT/stage-int35}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int35.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int35.img"
sha256sum "$EXT/kernel-boot-int35.img"
# Keep a copy of the built Image next to the boot.img (scratch is deleted
# after the build, so the tree Image would otherwise be lost).
cp arch/arm64/boot/Image "$EXT/Image-int35"
sha256sum "$EXT/Image-int35"

# 8. delete scratch + stage (fresh tree per ticket; evidence kept in
# $EVID + Image-int35 + logs on the external disk).
rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
