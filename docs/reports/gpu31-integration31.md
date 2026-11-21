# gpu31 integration31 — SUBMIT snapshot sw= + syncobj WAIT fix, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator31. Phone: on int30;
this ticket builds only — Main flashes. No flash per ticket.

Consumes b79c502 (syncobj WAIT: WAIT_ALL zero-budget false-success
fixed via `is_signaled` re-test → `-ETIME`, ANY dead-clause cleanup,
writable signaled-ops with release at definition, mock sim-clock host
test `t_syncobj_wait_block` PASS, WAIT-RECHECK device lines) and
c95f7b9 (SUBMIT-time w0..w7 snapshot: `desc[8]` + `last.sw[8]`,
`sw=` on ERR/TIMEOUT/dispatch/dump, PTEWALK gains `sw=` submit-vs-fault
compare, JS_HEAD=jc verdict offset 0 correct, no dispatch change;
submit.c hunk 1547/1547) on top of int30. No source changes beyond the
int31 script (copy of int30 + renamed names + new 6p/6q gates) and
this report.

## 1. Build

Script: `code/gpu31/series/build-int31.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int31`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0, first run green (no gate-script rerun).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int31-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image`, no submit/DT errors — only the
ambient `vmlinux has a LOAD segment with RWX permissions` ld
warning and tree-internal notes, same as int30).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=0x? loglevel=7`-family cmdline seed, 27316224 B =
kernel 18177208 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int31-evidence.log` (258 lines, 0 FAIL,
14 PASS).
Scratch + stage deleted after the build; kept:
`Image-int31`, `kernel-boot-int31.img`, both logs, `offcheck31.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int30 gates, all PASS):

- Ops layout: **ops=24 slot=232 ran=236**, single defs, numeric
  ST-OPS/`runexp`/ST-RUN/bails/retire/finish, weak/strong
  run_job_addr linkage live.
- IRQ/completion: `(status | rawstat)` demux, rearm + AS0 program on
  recover/resume, all `p7580_job:` probes in tree + objects + Image.
- Slot routing (2936796): FS → JS0, req0 → JS1, ONLY_COMPUTE → JS2
  in jm_hw.h + `p7580_job: dispatch` in job.o + Image.
- ABI-align (de142b3): BUILD_BUG_ON pins, SYNC_FILE tolerance,
  copy caps, handler symbols in object + System.map.
- gpuwrite: Zero contract, `0xCF` fops route,
  `job_done_sync` reuse, 6-row table, debug symbols live.
- Coherency: 3 pre_signal call sites (IRQ `job->bos`,
  timedout `job->bos`, recover `fault_job->bos`), `T` in gem.o,
  single `U` in job.o, `bl` in all three fns, one `T` in System.map.
- Errtrace: `p7580_job: ERR slot=` + TIMEOUT + `trace_arm`
  + `dump` in tree + job.o + Image.
- Instrtype: `dma_resv_add_excl_fence(` in submit run_job,
  no `mutex_lock` within 30 lines after; shim owns the lock; `U` in
  submit.o, `bl` with reloc, one `T` in System.map.
- Iova: free path `p7580_gem_gpu_unmap(obj);` BEFORE
  `p7580_iova_free(gd, bo);`; map_bo 2× invalidate; OVERLAP +
  ERR-MAP + `iova_arm` strings; one `T p7580_gem_gpu_unmap`.
- Armfence (49f7df2): arm at `drm_sched_job_arm` (`T` once in
  System.map), SUBMIT out_sync install, identity pump, contexts live.
- Pmget (0d42bcd, int29 6n form — PASS): run_job keeps `if
  (p7580_drv_pm_get(drv) < 0)`; timedout keeps
  `pmraw = p7580_drv_pm_get(drv);` + TMD-ENTER then `if (pmraw
  < 0)`; ioctl override `if (ret < 0)`; `Normalise
  pm_runtime_get_sync's 1 (already active) to 0` helper comment.
  Image: `p7580_sched: pm_get FAIL` still live.
- Timedout-probe (b023f4b, 6o gate — PASS): `p7580_job: TMD-ENTER`
  + `pmraw` capture inside `p7580_drv_timedout_job`;
  `p7580_job: TMD-PREWALK` inside timedout before
  `p7580_drv_ptewalk_pin`; `p7580_job: TMD-WALKER` at walker entry.
  All three in submit.o + Image (`TMD-` count = 3).
- Ptewalk (06943c1, 6m — PASS): `jc_page` walk intact, snap/live
  owner resolution (`snap_iova`/`live_iova`, `bo_lock` scan, GEM
  ref/unref, live fallback), `snap=`/`live=` on MATCH +
  NO-OWNER + CROSS-PAGE, unconditional 8-word `w=` dump, PTEWALK
  count = 5 in Image.

This ticket's changes (new gates, PASS):

- Submitsnap 6p (c95f7b9 — PASS): applied tree carries
  `uint32_t desc[8]` (submit job) + `uint32_t sw[8]` (last-job),
  `job->desc[wi]` SUBMIT snapshot loop, `jd->last.sw` dispatch
  copy, `sw=%08x ×8` on ERR + dispatch + TIMEOUT + PTEWALK
  (MATCH + NO-OWNER lines), `job->desc[4] &= ~0x1u` chain_mode
  clear, no stale `desc_w4` tokens; `sw=` in job.o + submit.o +
  Image (`sw=` count = 5: dispatch/ERR/TIMEOUT/PTEWALK×2).
- Syncobj-wait 6q (b79c502 — PASS): applied syncobj.c carries
  `if (!dma_fence_is_signaled(fences[i]))` WAIT_ALL re-test with
  the `Only a signaled fence counts` comment, writable
  `static struct dma_fence_ops p7580_sync_signaled_ops` (no
  `static const`), `.release = p7580_sync_signaled_release` at
  definition; `panfrost_syncobj_ioctl` `T` in syncobj.o;
  `WAIT-RECHECK(0-timeout, post-wait)` device line in the client
  `pf7580-selftest.c` (submit-test path).

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` (last def in the
1547-line hunk) present in the applied tree, `T` in submit.o, one
`T` in System.map (`ffffffc000579fa0`). A silent truncation (the
int27 first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int31`:
  `1043a2577a8fc8990dcc8126e1dfc13ad8f3990dffd2879049135e201f36141b`
- `kernel-boot-int31.img`:
  `3130bdcef88541a5124ec86eea2ad905d1c473d72d313b0b3bdbf6f59c466aaf`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
