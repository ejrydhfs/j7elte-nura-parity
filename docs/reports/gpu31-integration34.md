# gpu31-integration34 — serialfence (per-file implicit serialization), all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator34. Phone: on int33;
this ticket builds only — Main flashes. No flash per ticket.

Consumes 0072a74 (serialfence: per-file job serialization via the
previous submit's armed fence attached as an implicit dependency —
`serial_lock` + `serial_prev` in `struct p7580_drv_file`, borrow +
`add_dependency` when still unsignaled after arm, publish of the new
armed fence, rollback on error paths, fini drop; slot assignment
untouched; serial host tests green native + qemu, 399 checks) on top
of int33 (5fe4615 on 7e71c82 multidep). No source changes beyond the
int34 script (copy of int33 + renamed names + new 6t gates, plus one
in-flight gate fix, see §4) and this report.

## 1. Build

Script: `code/gpu31/series/build-int34.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int34`, `--check` green (backport
0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), `-j4` Image
build, RC=0. Pack and evidence flow ran inside the kept scratch after
one gate fix was applied to the committed script and the full build
re-run green (§4); all gates PASS.

Command: `make ARCH/arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int34-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image` — only the ambient `vmlinux has a
LOAD segment with RWX permissions` ld warning, same as int33).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=0x? loglevel=7`-family cmdline seed, 27332608 B =
kernel 18193592 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS on the clean re-run);
evidence: `…/logs/gpu31-int34-evidence.log` (342 lines, 0 FAIL,
17 PASS).
Scratch + stage deleted after the build; kept:
`Image-int34`, `kernel-boot-int34.img`, both logs, `offcheck34.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int33 gates, all PASS):

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
  as int33.
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
- Jddecode 6r (fed4e0c — PASS): applied tree carries
  `job->bo_count = nr;` (:1387) with the `stamp bo_count here`
  comment; `list_for_each_entry(le, &drv->bo_list, node)` live
  bo_list snapshot lookup in the submit path (same scan as the
  PTEWALK pin); `p7580_drv_submit` disasm carries stores (stamp
  path); three `p7580_job: JDBODY` lines in submit.o +
  Image (Image `JDBODY` count = 3).
- Multidep 6s (7e71c82 — PASS): applied tree carries
  `struct drm_sched_dep_and` + `drm_sched_dep_and_ops` +
  `drm_sched_dep_merge` + `drm_sched_job_add_dependency` +
  `drm_sched_job_err` in `drm_sched.c`, `p7580_submit_err` in
  `submit.c`; no `EOPNOTSUPP` in applied `drm_sched.c`;
  `p7580_sched: err stage=%s rc=%ld` + `p7580_sched:
  submit_err stage=%s rc=%ld` strings with staged call sites;
  `r drm_sched_dep_and_ops` + `t drm_sched_dep_and_leaf_fired` +
  `T drm_sched_job_add_dependency` in drm_sched.o;
  `dma_fence_init` + leaf-callback + `printk` relocs inside
  `add_dependency` disasm;
  `err stage=` (count = 1) + `submit_err stage=` (count = 1) in
  Image; `T drm_sched_job_add_dependency` once in System.map.

This ticket's changes (new 6t gates, PASS):

- Serialfence 6t (0072a74 — PASS): applied tree carries
  `struct mutex serial_lock;` + `struct dma_fence *serial_prev;`
  in `panfrost7580_drv.h`, `mutex_init(&df->serial_lock);` +
  `df->serial_prev = NULL;` init, `struct dma_fence *published =
  NULL;` tracker, `prev = df->serial_prev;` borrow,
  `!dma_fence_is_signaled(prev)` skip-guard,
  `drm_sched_job_add_dependency(sched_job, prev)` implicit attach,
  `df->serial_prev = published;` publish,
  `if (df->serial_prev == published)` rollback in `submit.c`
  (line evidence in the log); `p7580_drv_submit` disasm carries
  3 `R_AARCH64_CALL26 drm_sched_job_add_dependency` relocs
  (in_sync loop + excl + serial prev); `U
  drm_sched_job_add_dependency` in submit.o;
  `T drm_sched_job_add_dependency` in System.map (serial dep target).
  The `is_signaled` guard is a `dma-fence.h` static inline
  (`test_bit` on flags) with no out-of-line call at -O2 — gated in
  the tree, not the relocs (see §4).

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` (last def in the
1594-line hunk) present in the applied tree, `T` in submit.o, one
`T` in System.map (`ffffffc00057be6c`). A silent truncation (the
int27 first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int34`:
  `9ddb00bba0ffdc0bda8b97871553a745923d87ff0c55193684216e3396ca416b`
- `kernel-boot-int34.img`:
  `b241e7be26b59c928c8bceeec43437ea3c4ab14412e8d0624fe5c2064b57407b`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).

## 4. Script notes (in-flight gate fix, in the committed script)

- New 6t gates initially assumed a `dma_fence_is_signaled` out-of-line
  reloc + `U` entry in submit.o. `dma_fence_is_signaled` is a static
  inline in `dma-fence.h` (flags `test_bit`), so `-O2` emits no call:
  the first full build failed loudly on exactly that gate as designed,
  with the tree-side serial wiring and 3× `add_dependency` relocs
  already proven in the same evidence log. Fixed the committed script
  to the real compiled evidence (tree guard + ≥3 `add_dependency`
  relocs + `U add_dependency`; no `is_signaled` reloc/`U` expectation)
  and re-ran the full build clean: all 17 gates PASS.
