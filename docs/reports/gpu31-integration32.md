# gpu31-integration32 — jddecode (bo_count stamp + live bo_list snapshot + JDBODY), all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator32. Phone: on int31;
this ticket builds only — Main flashes. No flash per ticket.

Consumes fed4e0c (jddecode: `job->bo_count = nr` stamped after the
bos[] transfer in `p7580_drv_submit`; snapshot owner lookup now uses
live bo_list; 512 B JDBODY dump) on top of int31 (9e98589; 2a2f481,
8892278). No source changes beyond the int32 script (copy of int31 +
renamed names + new 6r gates) and this report.

## 1. Build

Script: `code/gpu31/series/build-int32.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int32`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0, first run green (no gate-script rerun).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int32-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image`, no submit/DT errors — only the
ambient `vmlinux has a LOAD segment with RWX permissions` ld
warning and tree-internal notes, same as int31).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=0x? loglevel=7`-family cmdline seed, 27316224 B =
kernel 18177208 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int32-evidence.log` (272 lines, 0 FAIL,
15 PASS).
Scratch + stage deleted after the build; kept:
`Image-int32`, `kernel-boot-int32.img`, both logs, `offcheck32.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int31 gates, all PASS):

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
- Pmget (0d42bcd, int29 6n form — PASS): same 5-site/1-inline form
  as int31.
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
- Submitsnap 6p (c95f7b9 — PASS): `uint32_t desc[8]` +
  `uint32_t sw[8]`, `job->desc[wi]` SUBMIT loop, `jd->last.sw`
  dispatch copy, `sw=%08x ×8` on ERR + dispatch + TIMEOUT +
  PTEWALK (MATCH + NO-OWNER), `job->desc[4] &= ~0x1u` chain_mode
  clear, no stale `desc_w4`; `sw=` in job.o + submit.o +
  Image (`sw=` count = 5).
- Syncobj-wait 6q (b79c502 — PASS): `if
  (!dma_fence_is_signaled(fences[i]))` WAIT_ALL re-test with the
  `Only a signaled fence counts` comment, writable
  `static struct dma_fence_ops p7580_sync_signaled_ops` (no
  `static const`), `.release = p7580_sync_signaled_release` at
  definition; `panfrost_syncobj_ioctl` `T` in syncobj.o;
  `WAIT-RECHECK(0-timeout, post-wait)` device line in the client
  `pf7580-selftest.c`.

This ticket's changes (new 6r gates, PASS):

- Jddecode 6r (fed4e0c — PASS): applied tree carries
  `job->bo_count = nr;` (:1360) with the `stamp bo_count here`
  comment; `list_for_each_entry(le, &drv->bo_list, node)` live
  snapshot lookup in the submit path (same scan as the PTEWALK
  pin); `p7580_drv_submit` disasm carries a store (stamp path);
  three `p7580_job: JDBODY` lines (words/+64/+128) in submit.o +
  Image (Image `JDBODY` count = 3).

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` (last def in the
1594-line hunk) present in the applied tree, `T` in submit.o, one
`T` in System.map (`ffffffc00057a040`). A silent truncation (the
int27 first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int32`:
  `156b52161615f07c589113946f630da8ffd289c8290847542ca37df8d330e334`
- `kernel-boot-int32.img`:
  `3005107ba1667dc6309fc168e143b3d0898831962508b4e63a4ed7bc70f17352`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
