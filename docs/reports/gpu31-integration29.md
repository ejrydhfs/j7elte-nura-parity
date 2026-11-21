# gpu31 integration29 — ptewalk guard, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator29. Phone: up on int28;
this ticket builds only — Main flashes. No flash per ticket.

Consumes 9231f17 (ptewalk guard: walk `jc_page` (`jc & PAGE_MASK`),
drop the silent non-aligned early return; prints keep the full `jc`,
page_off re-read + bound unchanged; hunk 1487/1487) on top of int28
(timedoutprobe TMD-ENTER/PREWALK/WALKER pins). No source changes
beyond the int29 script (copy of int28 + renamed names + extended 6m
ptewalk-guard gates) and this report.

## 1. Build

Script: `code/gpu31/series/build-int29.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int29`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0, first run green (no gate-script rerun this
time).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int29-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image`, no submit/DT errors — only the
ambient `vmlinux has a LOAD segment with RWX permissions` ld
warning and tree-internal `-Wstringop-truncation`-style notes, same
as int28).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int29-evidence.log` (222 lines, 0 FAIL,
12 PASS).
Scratch + stage deleted after the build; kept:
`Image-int29`, `kernel-boot-int29.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int28 gates, all PASS):

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
- Pmget (0d42bcd, int28 6n form — PASS): run_job keeps `if
  (p7580_drv_pm_get(drv) < 0)` (:230 ST-PMGET fail); timedout keeps
  `pmraw = p7580_drv_pm_get(drv);` (:488) + TMD-ENTER then `if (pmraw
  < 0)` (:492); ioctl override `if (ret < 0)` (:642, :1480);
  `Normalise pm_runtime_get_sync's 1 (already active) to 0` helper
  comment (:650). Exactly 5 `p7580_drv_pm_get(drv)` call sites and
  exactly 1 inline `< 0` form. Image: `p7580_sched: pm_get FAIL`
  still live.
- Timedout-probe (b023f4b, 6o gate — PASS): applied tree carries
  `p7580_job: TMD-ENTER sched_job=%lu job=%lu drv=%lu pmraw=%d` +
  `pmraw = p7580_drv_pm_get(drv);` inside `p7580_drv_timedout_job`
  (:488–492); `p7580_job: TMD-PREWALK job=%lu drv=%lu` inside
  timedout before `p7580_drv_ptewalk_pin` (:502–503);
  `p7580_job: TMD-WALKER drv=%lu job=%lu` at walker entry (:342).
  Objects: all three `p7580_job: TMD-*` strings in
  `panfrost7580_submit.o`. Image: all three live (`TMD-` count = 3;
  25 total `p7580_job:` strings, 5× PTEWALK).

Ptewalk guard (9231f17, this ticket's change — extended 6m gate, PASS):

- Applied tree: `jc_page = jc & PAGE_MASK` (:~506); mid-level walk
  `((jc_page >> shift) & 0x1FFu)`; leaf walk `((jc_page >> 12) &
  0x1FFu)`; stale `if (jc & ~PAGE_MASK)` early return absent; prints
  keep full `jc` (UNMAPPED/NOT-LEAF/CROSS-PAGE/leaf lines, :365–451);
  `page_off + 32 > PAGE_SIZE` CROSS-PAGE bound intact; call inside
  timedout path (:503) before recover; walker `p7580_drv_ptewalk_pin`
  at submit.c :319 (static noinline).
- Objects: walker in submit.o, `PTEWALK` strings live; `TMD-WALKER`
  at walker entry (342).
- Image: `p7580_job: PTEWALK` still live.

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` (last def in the
1487-line hunk) present in the applied tree, `T` in submit.o, one
`T` in System.map (`ffffffc000579fc0`). A silent truncation (the
int27 first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int29`:
  `c4ed54c81af8b066b32ca7b53a4e9a0b5951eb275b8a00f156faab4c433f6d7e`
- `kernel-boot-int29.img`:
  `65c4ae0bf1b8c2efad8d9b6a9eb43059f9104eafa77dee4ad8dd001c40256182`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
