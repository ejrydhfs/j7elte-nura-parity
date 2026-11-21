# gpu31 integration28 — timedout-probe TMD pins, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator28. Phone: up on int27;
this ticket builds only — Main flashes. No flash per ticket.

Consumes b023f4b (timedoutprobe: three unconditional `pr_err` pins
in the PM-held timedout path — TMD-ENTER at wrapper entry with
`sched_job`/`job`/`drv`/`pmraw`, TMD-PREWALK before the PT walk,
TMD-WALKER at walker entry; `< 0` pm_get semantics unchanged, raw 1
"already active" still normalised to 0 by the 0d42bcd helper) on top
of int27 (pmget `< 0` + ptewalk + armfence + all prior fixes). No
source changes beyond the int28 script (copy of int27 + renamed
names + 6n update for the new `pmraw` capture form + new 6o TMD
gates + submit.c hunk-integrity check) and this report.

## 1. Build

Script: `code/gpu31/series/build-int28.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int28`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. First run hit a gate-script bug (not a tree
bug): the new 6o gate's `awk '/^static void p7580_drv_ptewalk_pin/'`
missed the walker's real signature `static noinline void ...`, so
TMD-WALKER "not inside walker" failed while the kept scratch showed
the pin present at submit.c :342. Fixed both 6o `awk` patterns to
match on the function name + argument list (noinline-proof) and
re-ran green; the fix was verified against the kept scratch before
the rerun. No tree/patch defects.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int28-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image`, no submit/DT errors — only the
ambient `vmlinux has a LOAD segment with RWX permissions` ld
warning and tree-internal `-Wstringop-truncation`-style notes, same
as int27).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int28-evidence.log` (222 lines, 0 FAIL,
12 PASS).
Scratch + stage deleted after the build; kept:
`Image-int28`, `kernel-boot-int28.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int27 gates, all PASS):

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
  +NO-OWNER lines, call inside timedout path (:503) before recover,
  walker in submit.o, `PTEWALK` string in Image — 6m gate PASS.
- Pmget (0d42bcd, 6n gate updated for the b023f4b capture form —
  PASS): run_job keeps `if (p7580_drv_pm_get(drv) < 0)` (:230
  ST-PMGET fail); timedout now captures `pmraw =
  p7580_drv_pm_get(drv);` (:488) + TMD-ENTER then tests `if (pmraw
  < 0)` (:492); ioctl override `if (ret < 0)` (:642, :1480);
  `Normalise pm_runtime_get_sync's 1 (already active) to 0` helper
  comment (:650). Exactly 5 `p7580_drv_pm_get(drv)` call sites
  (run_job + timedout capture + pm_arm + teardown_hold + ioctl
  override) and exactly 1 inline `< 0` form (timedout uses `pmraw`
  now). Image: `p7580_sched: pm_get FAIL` still live (fail leg
  retained); `p7580_job: PTEWALK` still live.

Timedout-probe (b023f4b, this ticket's change — 6o gate, PASS):

- Applied tree: `p7580_job: TMD-ENTER sched_job=%lu job=%lu
  drv=%lu pmraw=%d` + `pmraw = p7580_drv_pm_get(drv);` inside
  `p7580_drv_timedout_job` (:488–492); `p7580_job: TMD-PREWALK
  job=%lu drv=%lu` inside timedout before `p7580_drv_ptewalk_pin`
  (:502–503); `p7580_job: TMD-WALKER drv=%lu job=%lu` at walker
  entry (:342 inside `p7580_drv_ptewalk_pin`).
- Objects: all three `p7580_job: TMD-*` strings in
  `panfrost7580_submit.o`.
- Image: all three `p7580_job: TMD-*` strings live
  (`TMD-` count = 3; 25 total `p7580_job:` strings, 5× PTEWALK).

Hunk integrity (new check, PASS): driver 0001 submit.c fully
applied — late function `int panfrost_ioctl_submit_override` (last
def in the 1487-line hunk, verified 1487/1487 in-repo pre-build)
present in the applied tree, `T` in submit.o, one `T` in
System.map (`ffffffc000579fc0`). A silent truncation (the int27
first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int28`:
  `daa346f1b57056255ae836df06fbee7baba9630cdbf0f88c8fad4d7f8237cd14`
- `kernel-boot-int28.img`:
  `7de91f000c44d9818d2e1c522e8f50703e63e7e9a131374c05720dc87a7c3bb2`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
