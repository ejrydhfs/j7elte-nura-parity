# gpu31 integration27 — pm_get `< 0` normalisation, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator27. Phone: up on int26;
this ticket builds only — Main flashes. No flash per ticket.

Consumes 0d42bcd (pmget: `p7580_drv_pm_get` normalises
`pm_runtime_get_sync`'s 1 "already active" to 0 = powered; every
caller — run_job ST-PMGET fail, timedout dark early-return, ioctl
override — tests `< 0`, so only negative errno takes the dark path)
on top of int26 (ptewalk 2f4d564 + armfence 49f7df2 + all prior
fixes). No source changes beyond the int27 script (copy of int26 +
renamed names + new 6n pmget gate) and this report.

## 1. Build

Script: `code/gpu31/series/build-int27.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int27`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. Second run green after the first run exposed
a corrupt committed patch: 0d42bcd's new-file hunk headers for
`panfrost7580_main.c` (`+1,715`, actual 716) and
`panfrost7580_submit.c` (`+1,1470`, actual 1478) were short, so
`git apply` silently truncated `submit.c` mid-function (file ended at
`int ret = p7580_drv_pm_get(drv);`, `panfrost7580_submit.o` missing).
Headers corrected in-tree to `+1,716` / `+1,1478`; resubmission of
this ticket's patch is Main's call (fix verified by this green
build). No other script defects.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int27-build.log` (5393 lines).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int27-evidence.log` (0 FAIL, 11 PASS).
Scratch + stage deleted after the build; kept:
`Image-int27`, `kernel-boot-int27.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (126616 B, 2026-11-21 02:50:52)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (249320 B, 02:51:00)
- `arch/arm64/boot/Image` (18177144 B, 02:52:51)

Prior fixes intact (unchanged int26 gates, all PASS):

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
  submit.o, `bl` with reloc, one `T` in System.map.
- Iova (2dd207c): free path `p7580_gem_gpu_unmap(obj);` BEFORE
  `p7580_iova_free(gd, bo);`; map_bo 2× invalidate; OVERLAP +
  ERR-MAP + `iova_arm` strings; one `T p7580_gem_gpu_unmap`.
- Armfence (49f7df2): arm at `drm_sched_job_arm` (`T` once in
  System.map), SUBMIT out_sync install, identity pump, contexts live.
- Ptewalk (2f4d564): `p7580_drv_ptewalk_pin` at submit.c :319,
  `p7580_job: PTEWALK` UNMAPPED/NOT-LEAF/CROSS-PAGE/leaf+`bo_phys`
  +NO-OWNER lines, call inside timedout path (:494) before recover,
  walker in submit.o, `PTEWALK` string in Image — 6m gate PASS.

Pmget (0d42bcd, this ticket's change — 6n gate, PASS):

- Applied tree: exactly 2 `p7580_drv_pm_get(drv) < 0` sites in
  submit.c (run_job :230 ST-PMGET fail, timedout :485 dark
  early-return); ioctl override `if (ret < 0)` (:633, :1471);
  `Normalise pm_runtime_get_sync's 1 (already active) to 0` helper
  comment (:641) — raw 1 can never take the dark early-return, so
  timeout recovery and the PTEWALK probe now run on the powered path.
- Image: `p7580_sched: pm_get FAIL` still live (fail leg retained);
  `p7580_job: PTEWALK` still live (pmget did not drop ptewalk).

## 3. Outputs (sha256)

- `Image-int27`:
  `b1e04cc36d627889257320b83612fba1a1fa1647fb8feb011bcbc7287746b9e5`
- `kernel-boot-int27.img`:
  `57c4a5dc6440829922a4ff2ad8eb80de6d9049f6bfff7f4a3ec60f7645b77b50`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
