# gpu31-integration33 — multidep (wait-all AND node for N in_syncs + stage errno strings), all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator33. Phone: on int32;
this ticket builds only — Main flashes. No flash per ticket.

Consumes 7e71c82 (multidep: wait-all AND node in the 0004 shim for N
distinct in-fences — the old single-dep `-EOPNOTSUPP` reject is gone;
bounded stage+errno on every early error path in the shim
(`drm_sched_job_err`, stages dep/submit/pump/run) and the driver 0001
(`p7580_submit_err`, stages validate/in_sync/bo/job/ops/dep/queue/
out_sync); host tests green native + qemu) on top of int32 (af97832 on
fed4e0c jddecode). No source changes beyond the int33 script (copy of
int32 + renamed names + new 6s gates, plus two script-bug fixes found
in-flight) and this report.

## 1. Build

Script: `code/gpu31/series/build-int33.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int33`, `--check` green (backport
0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), `-j4` Image
build, RC=0. Pack and evidence flow ran inside the kept scratch after
two script bugs were fixed in the repo copy (see §4); all gates PASS.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int33-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image`, no submit/DT errors — only the
ambient `vmlinux has a LOAD segment with RWX permissions` ld
warning and tree-internal notes, same as int32).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=0x? loglevel=7`-family cmdline seed, 27332608 B =
kernel 18193592 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, first-run gates to 6o PASS, fixed 6p/6q/
6r/6s re-verified in the kept scratch and appended);
evidence: `…/logs/gpu31-int33-evidence.log` (264 lines, 0 FAIL,
16 PASS).
Scratch + stage deleted after the build; kept:
`Image-int33`, `kernel-boot-int33.img`, both logs, `offcheck33.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int32 gates, all PASS):

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
  as int32.
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
  `job->bo_count = nr;` (:1362) with the `stamp bo_count here`
  comment; `list_for_each_entry(le, &drv->bo_list, node)` live
  snapshot lookup in the submit path (same scan as the PTEWALK
  pin); `p7580_drv_submit` disasm carries a store (stamp path);
  three `p7580_job: JDBODY` lines in submit.o +
  Image (Image `JDBODY` count = 3).

This ticket's changes (new 6s gates, PASS):

- Multidep 6s (7e71c82 — PASS): applied tree carries
  `struct drm_sched_dep_and` + `drm_sched_dep_and_ops` +
  `drm_sched_dep_merge` + `drm_sched_job_add_dependency` +
  `drm_sched_job_err` in `drm_sched.c`, `p7580_submit_err` in
  `submit.c`; no `EOPNOTSUPP` in applied `drm_sched.c` (old
  single-dep reject gone); `p7580_sched: err stage=%s rc=%ld` in
  the shim + `p7580_sched: submit_err stage=%s rc=%ld` in the
  driver, with `p7580_submit_err("dep" …)` staged call sites in
  the driver dep loops; `r drm_sched_dep_and_ops` +
  `t drm_sched_dep_and_leaf_fired` + `T drm_sched_job_add_dependency`
  in drm_sched.o (merge/err helpers are static-inline at -O2, so
  the compiled evidence is the inlined body: `dma_fence_init`
  `R_AARCH64_CALL26` reloc + `drm_sched_dep_and_leaf_cb_*`
  relocs + `printk` reloc inside `drm_sched_job_add_dependency`
  disasm); `err stage=` + `dep_and` strings in drm_sched.o,
  `submit_err stage=` in submit.o;
  `p7580_sched: err stage=` (count = 1) + `p7580_sched:
  submit_err stage=` (count = 1) in Image;
  `T drm_sched_job_add_dependency` once in System.map.

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` (last def in the
1594-line hunk) present in the applied tree, `T` in submit.o, one
`T` in System.map (`ffffffc00057bac8`). A silent truncation (the
int27 first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int33`:
  `941f109302d16817a0138a66397514190eabb70bd05340a560168039b32ee72c`
- `kernel-boot-int33.img`:
  `90d666e574e6dddb13ec6627313a6be60ae911f2b142b34377852c2cfe7d35d3`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).

## 4. Script notes (in-flight fixes, both in the committed script)

- `sed s/int32/int33/` rename corrupted two gate patterns to
  `uint33_t`; gates 6p failed loudly on the first run exactly as
  designed. Fixed back to `uint32_t` in the committed script;
  6p/6q/6r re-verified PASS in the kept scratch.
- New 6s gates initially assumed `t drm_sched_dep_merge` /
  `t drm_sched_job_err` nm symbols plus a `drm_sched_dep_merge`
  disasm call; both helpers are `static` and inline into the one
  exported `drm_sched_job_add_dependency` at -O2 (no nm entries,
  no out-of-line call). Gates rewritten to the real compiled
  evidence (static AND-node symbols + `dma_fence_init` /
  leaf-callback / `printk` relocs inside the `add_dependency`
  disasm) and re-verified PASS in the kept scratch.
