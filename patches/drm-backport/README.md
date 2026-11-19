# DRM backport patches for j7elte (Nura 3.10.108) — M3 scheduler path

Minimal standalone patch series adding the `dma-fence → dma-resv →
drm_syncobj → drm_sched` foundation that `gpu-panfrost.md` §2–4 identifies as
the M3 critical path (~5–6k LOC of new core code in mainline; this series is
a ~1300-line correctness subset plus build wiring, new files only).

No DT changes. Nothing here touches `patches/drm/` (owned by the integrator).

## Series order (apply in order)

| # | File | Purpose | Panfrost ioctl need |
|---|---|---|---|
| 01 | `0001-dma-fence-minimal-subset.patch` | `include/linux/dma-fence.h` + `drivers/base/dma-fence.c`: ordered-seqno fence with context allocator, signal/wait/add_callback/remove_callback, `DMA_FENCE_FLAG_SIGNALED_BIT`. Omits fence-array/chain, debugfs, seqno-fence timeline wrapper | `SUBMIT` job fences; `BO_WAIT` |
| 02 | `0002-dma-resv-shim.patch` | `include/linux/dma-resv.h` + `drivers/base/dma-resv.c`: one-writer + shared-reader reservation with `add_shared/add_excl/test_signaled/wait_timeout/copy_fences`. Plain-mutex locking (no `ww_mutex` in 3.10) | `CREATE_BO` resv init; `SUBMIT` reserve/attach; prime import/export |
| 03 | `0003-drm_syncobj-subset.patch` | `drivers/gpu/drm/drm_syncobj.{h,c}`: per-file idr handle table, binary fence replace/find/wait. Omits timeline payloads, eventfd, sync_file fd passing | `SUBMIT` in/out syncobj handles |
| 04 | `0004-drm_sched-panfrost-job-only-shim.patch` | `drivers/gpu/drm/drm_sched.{h,c}`: per-entity FIFO on a shared ordered wq, fence-callback dependency gating, per-job timeout work, entity flush. Omits priorities, load balancing, GPU-reset recovery | `SUBMIT` JM dispatch (`run_job` programs the job slot; `timedout_job` resets it) |
| 05 | `0005-build-wiring-for-shims.patch` | `drivers/base/Makefile` + `drivers/gpu/drm/Makefile`: compile fence/resv under `CONFIG_DMA_SHARED_BUFFER`, syncobj/sched into `drm-y` | build only |

## Check status (honest)

- `git apply --check`: **PASS for all 5 patches, individually and as a full
  series**, verified 2026-11-18 in a private `/tmp` clone seeded with pristine
  copies of `drivers/base/Makefile` + `drivers/gpu/drm/Makefile` from the
  read-only tree (never in the shared tree).
- Syntax/compile: **each of the 8 new files passes BOTH**
  `aarch64-linux-gnu-gcc -fsyntax-only` AND a real `-c` object compile
  (`-std=gnu89 -nostdinc` against the actual 3.10.108
  `arch/arm64/include + include` headers, `__KERNEL__`, kconfig include)
  with **zero warnings/errors**, verified post-`git apply` from the same
  private clone. Objects: `dma_fence.o` 7184 B, `dma_resv.o` 8280 B,
  `drm_syncobj.o` 8608 B, `drm_sched.o` 10856 B.
- **NOT verified**: full kernel `Image`/module build (integrator serializes
  the one build; explicitly out of scope), link into `built-in.o`, boot on
  device, any panfrost GEM/job-slot code (patches here are foundation only —
  no `panfrost_drv.c`/`panfrost_job.c` yet), `drm_dev_alloc`/`drm_gem_*`
  adaptation (M2 work, separate slice).

## Integrator: test-apply + compile

```sh
# from the repo root, against a PRIVATE copy of the kernel tree:
K=/path/to/android_kernel_samsung_universal7580
cp -r "$K" /tmp/kcheck   # or: git worktree / git clone of your baseline
cd /tmp/kcheck
for p in patches/drm-backport/000[1-5]-*.patch; do
  git apply --check -v "$p" || break
done
git apply patches/drm-backport/000[1-5]-*.patch
# per-file syntax check against the real 3.10 headers:
KINC="-nostdinc -isystem $(dirname $(aarch64-linux-gnu-gcc -print-file-name=include/stdarg.h))"
for f in drivers/base/dma-fence.c drivers/base/dma-resv.c \
         drivers/gpu/drm/drm_syncobj.c drivers/gpu/drm/drm_sched.c; do
  aarch64-linux-gnu-gcc -fsyntax-only -std=gnu89 -Wall $KINC \
    -I$K/arch/arm64/include -I$K/arch/arm64/include/generated \
    -I$K/include -I$K/include/uapi -I$K/arch/arm64/include/uapi \
    -D__KERNEL__ -include $K/include/linux/kconfig.h "$f"
done
```

To build for real, enable `CONFIG_DRM` + `CONFIG_DMA_SHARED_BUFFER` per the
M1 config fragment (owned by `GpuDrm2.KernelConfigM1`), then run the normal
kernel build (`code/kernel/build.sh` is owned by the integrator — do not run
it from a slice).

## Design notes / known limits (for the M3 driver author)

1. `dma_fence.event`/`cb_list` use `kmalloc`/`wait_queue_head_t` allocated on
   first waiter; `release` frees the queue. `dma_fence_release_base()` must be
   called from the backend `release()` exactly once.
2. `dma_resv` locking is a plain mutex. Mainline uses `ww_mutex` for
   multi-resv atomic reservation; panfrost `SUBMIT` reserves one BO set at a
   time under `drm_sched_job` serialization, so a single global order (submit
   order) is deadlock-free. If a future multi-resv path appears, add explicit
   address-ordered locking.
3. `drm_syncobj_wait` on a NULL fence returns 0 (mainline: already signaled).
4. `drm_sched` timeout is hardcoded at queue time via `msecs_to_jiffies(5000)`
   in `drm_sched_pump`; wire it to `sched->timeout` if the backend wants a
   tunable (one-line change, marked in source).
5. Field name `in_flight` (not `current`): `current` is a macro in 3.10
   (`asm-generic/current.h`) and cannot be a struct member.
6. `patches/drm/0001-*` already touches `drivers/gpu/drm/Makefile` (adds the
   `exynos7580/` subdir line). Patch 05 appends after `drm_trace_points.o ...
   drm_prime.o`; if the display series lands first, expect a 2-line context
   fuzz — both edits are additive and commute; rebase by re-running the
   series generation, do not hand-edit.
