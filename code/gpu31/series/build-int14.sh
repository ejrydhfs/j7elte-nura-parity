#!/bin/sh
# integration14 reproducible rebuild: M1 + backport shims 0001-0005 +
# Mode-B v2 render-ONLY + panfrost7580 (submit/open/render-only fixes,
# gem_vm_ops mmap 5d97c65, vm_pgoff fix 5f8ecd2, shmem-backed CREATE_BO
# dcd5171, job-lifetime PM model c81e8a7, teardown PM guards 5c52f1f + close fix 476b75d
# + jobtrace 1244082: bounded dispatch/IRQ/fence trace, out_sync
# NULL-install fix, completion dcache invalidate + jobtrace2 1450d16:
# run_job out_sync install via drm_syncobj_replace_fence (F-JT1 active),
# free_job job->bos walk calling p7580_gem_job_done_sync per BO (F-JT2
# active, job->bos refcount transfer on SUBMIT) + schedtrace 8fd555e:
# bounded ST-IOCTL/ST-QUEUE/ST-RUN/ST-PMGET/ST-RETIRE probes in submit.c
# and ST-PUSH/ST-PUMP/ST-FENCE in the 0004 shim (budget 8 each) + pumpfix de07c37:
# 0004 pump preserves SUBMIT slot, ST-OPS (ops/run_job/slot) + ST-FINISH (ran/slot)
# probes, NULL-guarded run_job call, IS_ERR_OR_NULL/ran stamp, retire-after-fence;
# driver stamps sched_job->slot at SUBMIT, run_job trusts it + opsfix df7c4b9:
# ops/slot/ran BUILD_BUG_ON pins in both TUs (ops=24/slot=232/ran=236),
# alloc round-trip BUG_ON, ST-OPS opsoff probe, submit call-site guard) + d0+d4.
# Copy of build-int13.sh with int14 names + step 6b (ops-layout gate:
# the layout is computed inside the build — offsetof getters compiled
# with the real kbuild include chain plus objdump immediates — and the
# build FAILS on disagreement, so a stale object can never slip through).
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
SCR=${SCR:-$EXT/scratch-int14}
LOG=${LOG:-$EXT/logs/gpu31-int14-build.log}
EVID=${EVID:-$EXT/logs/gpu31-int14-evidence.log}
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
echo "int14: PM-ordering folded in driver 0001 (c81e8a7 + 5c52f1f); no overlay applied"

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

# 6b. ops-layout gate (opsfix df7c4b9): compute the layout inside the
# build and FAIL (exit 1, scratch kept for diagnosis) on any
# disagreement. pahole-free: (a) offsetof getters compiled with the
# real kbuild include chain, immediates read back; (b) objdump of both
# objects for the compiler-emitted offset immediates; (c) nm
# single-definition + __ksymtab checks in objects and System.map;
# (d) applied-tree pin/round-trip/ST-OPS presence + Image strings.
note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int14 GATE FAIL: $1"; exit 1; }
SCHED_O=drivers/gpu/drm/drm_sched.o
SUBMIT_O=drivers/gpu/drm/panfrost/panfrost7580_submit.o
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ -f "$SCHED_O" ] || gate_fail "$SCHED_O missing"
[ -f "$SUBMIT_O" ] || gate_fail "$SUBMIT_O missing"
note "== int14 gate: objects present =="
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
GSTAGE=$EXT/offstage-int14
rm -rf "$GSTAGE"
mkdir -p "$GSTAGE/include/linux" "$GSTAGE/drivers/gpu/drm"
cp drivers/gpu/drm/drm_sched.h "$GSTAGE/drivers/gpu/drm/"
cp drivers/gpu/drm/drm_syncobj.h "$GSTAGE/drivers/gpu/drm/" 2>/dev/null || true
cp include/linux/dma-fence.h include/linux/dma-resv.h "$GSTAGE/include/linux/" 2>/dev/null || true
cat > "$EXT/offcheck14.c" <<'EOF'
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
	-D__KERNEL__ -include linux/kconfig.h -DKBUILD_MODNAME='"offcheck14"' \
	-S -O0 -o "$EXT/offcheck14.s" "$EXT/offcheck14.c" \
	|| gate_fail "offset getter compile failed"
OFF_OPS=$(grep -A 6 '^off_ops:' "$EXT/offcheck14.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_SLOT=$(grep -A 6 '^off_slot:' "$EXT/offcheck14.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
OFF_RAN=$(grep -A 6 '^off_ran:' "$EXT/offcheck14.s" | sed -n 's/.*mov.*w0, *#\?//p' | sed 's/[^0-9].*//' | head -1)
note "computed in-tree layout: ops=$OFF_OPS slot=$OFF_SLOT ran=$OFF_RAN"
[ "$OFF_OPS" = 24 ] || gate_fail "computed ops offset $OFF_OPS != 24"
[ "$OFF_SLOT" = 232 ] || gate_fail "computed slot offset $OFF_SLOT != 232"
[ "$OFF_RAN" = 236 ] || gate_fail "computed ran offset $OFF_RAN != 236"
# (b) compiler-emitted offset immediates: the alloc TU stores entity+ops
# as ONE stp at #16 (ops word = base+24); the driver run_job loads the
# slot word at #232. Checked as stp/ldr pairs so an stp-fused store is
# not mistaken for a missing offset.
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_job_alloc "$SCHED_O" \
	| grep -q 'stp.*\[x[0-9]*, #16\]' || gate_fail "no entity+ops stp #16 in drm_sched_job_alloc disasm"
aarch64-linux-gnu-objdump -d --disassemble=drm_sched_job_alloc "$SCHED_O" \
	| grep 'stp.*\[x[0-9]*, #16\]' | tee -a "$EVID"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep -q '\[x0, #232\]' || gate_fail "no #232 slot load in p7580_drv_run_job disasm"
aarch64-linux-gnu-objdump -d --disassemble=p7580_drv_run_job "$SUBMIT_O" \
	| grep '\[x0, #232\]' | tee -a "$EVID"
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
grep -q 'p7580_sched: ops job=%p opsoff=%p' drivers/gpu/drm/drm_sched.c \
	|| gate_fail "ST-OPS opsoff probe missing in applied drm_sched.c"
aarch64-linux-gnu-strings "$SCHED_O" | grep -q 'p7580_sched: ops job=%p opsoff=%p' \
	|| gate_fail "ST-OPS opsoff string missing in drm_sched.o"
aarch64-linux-gnu-strings "$SCHED_O" | grep 'p7580_sched: ops' | tee -a "$EVID"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -q 'p7580_sched: ops job=%p opsoff=%p' \
	|| gate_fail "ST-OPS opsoff string missing in Image"
[ "$(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_sched:')" = 11 ] \
	|| gate_fail "p7580_sched: count != 11 in Image"
note "Image p7580_sched: count = $(aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -c 'p7580_sched:')"
note "== int14 gate: PASS (ops=24 in both TUs, single defs, round-trip + opsoff live) =="

# 6c. freshness evidence (for the report): scratch birth vs object mtimes,
# plus proof the pristine seed carries no shim/driver files.
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/drm_sched.c" 2>&1 | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/panfrost/" 2>&1 | tee -a "$EVID"

# 7. pack boot.img (stock dt.img rule: dt.img is NEVER rebuilt).
# All pack I/O on the external disk: seed inputs are staged from /tmp/bimg
# (read-only) into $EXT/stage-int14; outputs land in $EXT.
STAGE=${STAGE:-$EXT/stage-int14}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int14.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int14.img"
sha256sum "$EXT/kernel-boot-int14.img"
# Keep a copy of the built Image next to the boot.img (scratch is deleted
# after the build, so the tree Image would otherwise be lost).
cp arch/arm64/boot/Image "$EXT/Image-int14"
sha256sum "$EXT/Image-int14"

# 8. delete scratch + stage (fresh tree per ticket; evidence kept in
# $EVID + Image-int14 + logs on the external disk).
rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
