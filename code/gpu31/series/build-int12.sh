#!/bin/sh
# integration12 reproducible rebuild: M1 + backport shims 0001-0005 +
# Mode-B v2 render-ONLY + panfrost7580 (submit/open/render-only fixes,
# gem_vm_ops mmap 5d97c65, vm_pgoff fix 5f8ecd2, shmem-backed CREATE_BO
# dcd5171, job-lifetime PM model c81e8a7, teardown PM guards 5c52f1f + close fix 476b75d
# + jobtrace 1244082: bounded dispatch/IRQ/fence trace, out_sync
# NULL-install fix, completion dcache invalidate + jobtrace2 1450d16:
# run_job out_sync install via drm_syncobj_replace_fence (F-JT1 active),
# free_job job->bos walk calling p7580_gem_job_done_sync per BO (F-JT2
# active, job->bos refcount transfer on SUBMIT) + schedtrace 8fd555e:
# bounded ST-IOCTL/ST-QUEUE/ST-RUN/ST-PMGET/ST-RETIRE probes in submit.c
# and ST-PUSH/ST-PUMP/ST-FENCE in the 0004 shim (budget 8 each)) + d0+d4.
# Copy of build-int11.sh with int12 names. Step 4b (PM-ordering overlay) retired: the
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
SCR=${SCR:-$EXT/scratch-int12}
LOG=${LOG:-$EXT/logs/gpu31-int12-build.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR"

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
#    into driver 0001).
for p in "$REPO"/code/gpu31/driver/000[12]-*.patch; do
	git apply --check "$p" && git apply "$p"
done

# 4b. retired (was PM-ordering overlay in int6): folded into driver 0001
# at c81e8a7 (job-lifetime PM) + 5c52f1f (teardown holds). Kept as a
# log line only.
echo "int12: PM-ordering folded in driver 0001 (c81e8a7 + 5c52f1f); no overlay applied"

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

# 7. pack boot.img (stock dt.img rule: dt.img is NEVER rebuilt).
# All pack I/O on the external disk: seed inputs are staged from /tmp/bimg
# (read-only) into $EXT/stage-int12; outputs land in $EXT.
STAGE=${STAGE:-$EXT/stage-int12}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int12.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int12.img"
sha256sum "$EXT/kernel-boot-int12.img"
# Keep a copy of the built Image next to the boot.img (scratch is deleted
# after the build, so the tree Image would otherwise be lost).
cp arch/arm64/boot/Image "$EXT/Image-int12"
sha256sum "$EXT/Image-int12"
