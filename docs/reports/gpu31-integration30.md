# gpu31 integration30 — ptewalk snap/live owner + unconditional leaf dump, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator30. Phone: up on int29;
this ticket builds only — Main flashes. No flash per ticket.

Consumes 06943c1 (ptewalk: walker scans live `drv->bo_list` under
`bo_lock` with GEM ref while linked for owner resolution; falls back
to the live BO when the SUBMIT-time `bos[]` snapshot misses; prints
both `snap=` and `live=` ranges on every line; always re-reads the 8
descriptor words from `leaf_phys + page_off` — including NO-OWNER,
where `page_off` is the in-page jc offset; hunk 1542/1542) on top of
int29 (ptewalk-guard `jc_page` live). No source changes beyond the
int30 script (copy of int29 + renamed names + extended 6m:
snap/live + unconditional-dump gates) and this report.

## 1. Build

Script: `code/gpu31/series/build-int30.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int30`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0, first run green (no gate-script rerun this
time).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int30-build.log` (5393 lines, ends
`OBJCOPY arch/arm64/boot/Image`, no submit/DT errors — only the
ambient `vmlinux has a LOAD segment with RWX permissions` ld
warning and tree-internal `-Wstringop-truncation`-style notes, same
as int29).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int30-evidence.log` (247 lines, 0 FAIL,
12 PASS).
Scratch + stage deleted after the build; kept:
`Image-int30`, `kernel-boot-int30.img`, both logs, `offcheck30.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact (unchanged int29 gates, all PASS):

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

Ptewalk snap/live (06943c1, this ticket's change — extended 6m gate, PASS):

- Applied tree: `jc_page = jc & PAGE_MASK` walk intact; stale
  `if (jc & ~PAGE_MASK)` early return absent; `page_off + 32 >
  PAGE_SIZE` CROSS-PAGE bound intact; call inside timedout path
  before recover; new `snap_iova`/`live_iova` vars,
  `mutex_lock(&drv->bo_lock)` + `list_for_each_entry(le,
  &drv->bo_list, node)` scan + `drm_gem_object_reference(le->obj)`
  + `if (!owner && live_bo)` fallback +
  `drm_gem_object_unreference_unlocked(live_obj)` release;
  `snap=0x%llx/0x%zx live=0x%llx/0x%zx` on all three terminal lines;
  `NO-OWNER snap=` line still carries `w=%08x ×8` (unconditional
  dump verified: `pw[0]` inside the NO-OWNER print args).
- Objects: walker `t` in submit.o; `snap=`, `live=`, `NO-OWNER`
  strings in submit.o.
- Image: 25 total `p7580_job:` strings, 5× PTEWALK (UNMAPPED /
  NOT-LEAF / CROSS-PAGE snap/live / MATCH-MISMATCH snap/live / NO-OWNER
  snap/live + 8 words), `snap=` + `live=` + `NO-OWNER` all live,
  3× `TMD-`.

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` (last def in the
1542-line hunk) present in the applied tree, `T` in submit.o, one
`T` in System.map (`ffffffc000579fc0`). A silent truncation (the
int27 first-run failure mode) would fail loudly here.

## 3. Outputs (sha256)

- `Image-int30`:
  `a314bd0e6ba07f37c166a0c29851388afa701d2a8888e729e5804204ed4caec5`
- `kernel-boot-int30.img`:
  `29c38b9248e6a35593c011280b72d36a34edd18221e9eb8b49ee85b8266ba331`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
