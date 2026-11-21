# gpu31 integration25 — arm-time fence, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator25. Phone: up on int24;
this ticket builds only — Main flashes.

Consumes 49f7df2 (armfence: `ops->alloc_fence` at arm via
`drm_sched_job_arm`, SUBMIT installs the armed fence on out_sync
during the ioctl, run_job binds the same fence, pump enforces
identity, fence contexts init at probe, F-JT1 run_job-time install
retired) on top of 2dd207c (int24) + all prior fixes. No source
changes beyond the int25 script (copy of int24 + renamed names +
new 6l armfence gate) and this report.

## 1. Build

Script: `code/gpu31/series/build-int25.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int25`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. Third run green after two gate corrections
(the F-JT1 early SUBMIT handle-validation stash legitimately remains —
the removed shape is the run_job-time install; `p7580_drv_alloc_fence`
is `static` so nm shows `t`, not `T`). No other script defects.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int25-build.log` (5393 lines).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int25-evidence.log` (0 FAIL).
Scratch + stage deleted after the build; kept:
`Image-int25`, `kernel-boot-int25.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (126616 B, 2026-11-21 01:59:02)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (242816 B, 01:59:09)
- `drivers/gpu/drm/panfrost/panfrost7580_gem.o` (253384 B, 01:59:09)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (212264 B, 01:59:10)
- `drivers/gpu/drm/panfrost/panfrost7580_ioctl.o` (in syncobj/ioctl gate)
- `arch/arm64/boot/Image` (18177144 B, 02:00)

Prior fixes intact (unchanged int24 gates, all PASS):

- Ops layout: **ops=24 slot=232 ran=236**, single defs, numeric
  ST-OPS/`runexp`/ST-RUN/bails/retire/finish, weak/strong
  run_job_addr linkage live.
- IRQ/completion: `(status | rawstat)` demux, rearm + AS0 program on
  recover/resume, all `p7580_job:` probes in tree + objects + Image.
- Slot routing (2936796): FS → JS0, req0 → JS1, ONLY_COMPUTE → JS2
  in jm_hw.h + `p7580_job: dispatch` in job.o + Image.
- ABI-align (de142b3): BUILD_BUG_ON pins, SYNC_FILE tolerance,
  copy caps, handler symbols in object + System.map.
- gpuwrite (67eaef1): Zero contract, `0xCF` fops route,
  `job_done_sync` reuse, 6-row table, debug symbols live.
- Coherency (c207bdd): 3 pre_signal call sites (IRQ `job->bos`,
  timedout `job->bos`, recover `fault_job->bos`), `T` in gem.o,
  single `U` in job.o, `bl` in all three fns, one `T` in System.map.
- Errtrace (7a5bf9b): `p7580_job: ERR slot=` + TIMEOUT + `trace_arm`
  + `dump` in tree + job.o + Image.
- Instrtype (71a9412): `dma_resv_add_excl_fence(` in submit run_job,
  no `mutex_lock` within 30 lines after; shim owns the lock; `U` in
  submit.o, `bl` with `R_AARCH64_CALL26` reloc, one `T` in System.map.
- Iova (2dd207c): free path `p7580_gem_gpu_unmap(obj);` BEFORE
  `p7580_iova_free(gd, bo);`; map_bo 2× invalidate; OVERLAP +
  ERR-MAP + `iova_arm` strings; `T p7580_gem_gpu_unmap` once in
  System.map (`ffffffc000578ac8`).

Armfence (49f7df2, this ticket's change — 6l gate, PASS):

- Arm (shim TU): `void drm_sched_job_arm` def at drm_sched.c :328,
  `alloc_fence(job)` call (:343), `job->fence = fence` (:346);
  `alloc_fence` hook documented in drm_sched.h; `T drm_sched_job_arm`
  (`@648`) + `__ksymtab` in drm_sched.o; armed-fence comment lines
  present in the applied tree.
- SUBMIT (ioctl path): `drm_sched_job_arm(sched_job)` AFTER the
  call-site guard and BEFORE the dep loops; `struct dma_fence *fence =
  sched_job->fence` (:~7356) + `drm_syncobj_replace_fence(s, fence)`
  installs the ARMED fence during the ioctl — so Mesa holds a live
  fence at SUBMIT return. `U drm_sched_job_arm` in submit.o;
  `bl <drm_sched_job_arm>` at :19f0 in `p7580_drv_submit` disasm.
- Pump (identity): `struct dma_fence *run_fence` decl; `run_fence !=
  job->fence` mismatch check (two legs: ERR/NULL first, then
  pointer-identity) — mismatch drops the stray ref, drops the armed
  fence, frees the job on the F4 path (out_sync never points at a
  fence the completion path will not signal).
- run_job (bind, no install): `struct dma_fence *armed =
  sched_job->fence` + `dma_fence_get(armed)` + `No out_sync work here`
  marker; `p7580_job_hw_submit` still programs the slot; no
  `bl <drm_syncobj_replace_fence>` inside `p7580_drv_run_job` disasm
  (install is SUBMIT-time now). `job->out_sync = args->out_sync`
  stash remains for compat; the F-JT1 *run_job-install* shape
  (`assigns run_job's return` rationale) is gone.
- Backend split: `struct dma_fence *p7580_job_alloc_fence` def at
  job.c :739 (`T @1060` in job.o, `T ffffffc00057d380` in System.map,
  once), declared in job.h; `dma_fence_context_alloc(1)` per-slot
  init in `p7580_job_dev_init` (job.c :327); `static struct dma_fence
  *p7580_drv_alloc_fence` at submit.c + `.alloc_fence =
  p7580_drv_alloc_fence` ops entry + `sched_job->ops->alloc_fence`
  call-site guard extension (`t` in submit.o, expected — static).
- Image sanity: ST-OPS family still live in Image after the 0004
  regen; exactly one `T drm_sched_job_arm` (`ffffffc00056cc48`) and
  one `T p7580_job_alloc_fence` win the link.

## 3. Outputs (sha256)

- `Image-int25`:
  `5484deb6af75f32d47144e10a0e491c12c1f6eaa2a04856f9ef111daf4446b87`
- `kernel-boot-int25.img`:
  `4cabdeabe35ba97b53653f1efe7149598ef29bc6b073d5570ab24fcff75871f4`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
