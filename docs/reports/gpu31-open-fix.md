# gpu31 open-hang fix (R1) — scheduler never initialized on the open path

Date: 2026-11-19. Author: G31_OpenFix.
Ticket: `p7580_drv_file_init` / `p7580_drv_open` / `.open`/`.postclose` /
priv init-destroy (F-IO3).
Device state per Main (ground truth, not re-probed): panfrost7580 int4
image creates `/dev/dri/card0` + `renderD128`, but **opening the node
hangs**: `pf7580-selftest --probe-only` (no SUBMIT at all) hung with zero
output, and a kernel trace showed `p7580_drv_file_init+0x60/0x118` calling
into `p7580_drv_open+0x3c/0x88`. So the deadlock is in open/file_init,
before any ioctl. Phone is currently on the M1 image (#37, no `/dev/dri`),
so all proof below is static + host-side; no flash, no live boot (Main's gate).

Related: `docs/reports/gpu31-submit-fix.md` (G31_SubmitFix2, R2–R5 + full
lock/wait call graph; this report covers R1 only), `docs/reports/gpu31-driver-review.md`
(F-IO3: per-fd state never constructed/destroyed), `code/gpu31/series/0004-*`
(0004 sched shim), `code/gpu31/sched/` (emu + host test).

## 1. Call graph (open path, before fix)

```
open("/dev/dri/renderD128")
 └─ f_op->open = drm_open                              (p7580 fops)
     └─ drm_stub_open -> drm_open (drm_fops.c:183,118)
         └─ drm_open_helper (drm_fops.c:249)
             ├─ drm_gem_open(dev, priv)                (no locks/waits)
             ├─ drm_prime_init_file_private            (no locks/waits)
             ├─ dev->driver->open = p7580_drv_open    (main.c)
             │   ├─ kzalloc(df)                        (GFP_KERNEL, no lock)
             │   └─ p7580_drv_file_init(df, drv)       (submit.c:154) ← HANG
             │       ├─ drm_syncobj_manager_init       (0003: idr_init +
             │       │                                 mutex_init; no wait)
             │       └─ drm_sched_entity_init × 3      (0004, sched.c:102)
             │           └─ mutex_lock(&sched->lock)  ← WEDGED HERE
             │               sched == &p7580_drv_sched (static, ZEROED:
             │               lock never mutex_init'd, wq NULL,
             │               timeout 0, ready false)
             └─ (never reached: master create/set, filelist add)
```

3.10 `mutex_lock` on a zeroed mutex (`count == 0`; `mutex_init` sets 1)
sleeps for an owner that will never release → the opener sits in D state.
That is the `file_init → open` frame in the silicon trace. No other lock
or wait exists on this path: `p7580_drv_file_init` takes no other lock,
calls no `copy_from_user`, no `idr_alloc`, no `alloc_ordered_workqueue`.

Offending lines (pre-fix `code/gpu31/driver/0001-*`):
- `panfrost7580_submit.c` `p7580_drv_file_init`: entity-init loop ran
  `drm_sched_entity_init(&df->entity[i], &p7580_drv_sched)` with the
  global sched never `drm_sched_init`'d — no `p7580_drv_sched_ensure()`
  call anywhere on the probe/open path (`p7580_drv_submit` calls it,
  but SUBMIT is unreachable before open returns).
- `panfrost7580_main.c` `p7580_drv_probe`: `p7580_job_dev_init` but no
  sched warm-up, so the first open paid first-init under the opener.

Ruled out (checked, not the cause):
- Double-lock of the same mutex in file_init/open: no — the only lock
  taken is `sched->lock` inside entity_init; open holds none.
- `drm_gem`/stub helpers under a lock: no — `drm_gem_open` runs before
  `driver->open` without holding `struct_mutex` (drm_fops.c:287 runs
  outside the master/filelist critical sections at :300/346).
- Uninitialised priv: no — `df` is `kzalloc`'d; `entities_live = 0`
  before the loop, so the `postclose` unwind (`file_fini` checks the
  flag) is safe on the error path. Layout aliasing holds
  (`mgr/job_fctx/job_dev` are the first three members of both
  `p7580_drv_file` and GEM's `p7580_file_priv` — verified by
  `offsetof` in the host test).
- Ops-table recursion (`file_init` calling back into `open`): no —
  `.open = p7580_drv_open` → `file_init` is a leaf; no fops call inside.
- GEM's `p7580_file_priv_init` is dead code on this driver: nothing
  calls it (the real file state is `p7580_drv_file` via
  `p7580_drv_file_init`); its stale "jobs context freed by the jobs
  slice's own postclose leg" comment describes a postclose leg that does
  not exist. Left untouched (harmless), documented here for F-IO3.

## 2. Fix (minimal, lock discipline + init)

Two hunks in `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(+8 / +7 lines, no other files touched):

1. `panfrost7580_submit.c` — `p7580_drv_file_init`: call
   `p7580_drv_sched_ensure()` **before** the entity-init loop; on error
   return early (nothing constructed yet, nothing to unwind):
   ```c
   if (!df || !drv)
           return -EINVAL;
   /* R1: ... Ensure here so open stays O(1) ... */
   ret = p7580_drv_sched_ensure();
   if (ret)
           return ret;
   ```
   `ensure` inits once (workqueue + lock + `live` flag under
   `p7580_drv_sched_lock`); every later open hits the live flag — O(1),
   no waits on the fast path (mutex take/release only, bounded).
2. `panfrost7580_main.c` — `p7580_drv_probe` after `p7580_job_dev_init`:
   one `p7580_drv_sched_ensure()` warm-up so the first open never
   constructs kernel threads under the opener; failure `goto err_mmu`
   (non-fatal in practice — the first open retries via (1)).

Postclose is already correct and unchanged: `postclose` clears
`driver_priv`, calls `p7580_drv_file_fini` (finis live entities, then
`drm_syncobj_manager_fini`), `kfree`s — mirrors init in reverse, no
locks held across, no waits.

Lock discipline after fix: open takes `sched_lock` (inside ensure,
once) then per-entity `sched->lock` (inside entity_init, bounded,
never nested with `bo_lock`/`struct_mutex`/`resv.lock`); no lock is
taken before it is initialised; no wait, sleep, or allocation with
reclaim-under-lock anywhere on the path.

Land order with G31_SubmitFix2 (commit `c553b8e`, R2–R5 already in the
series): this R1 hunk lands first or together — their timeout
disarm (`timedout_job` → `recover` → fence signal) needs a live sched
to ever arm.

## 3. Host-side proof

- `patch -p1` fresh-apply (GNU patch, strict) + `git apply --check`:
  `0001` applies clean with both hunks (`17 files, 5718 insertions`).
  (Post-commit note: peer commit `e4edb3a` repaired the `@@ -0,0 +1,N`
  counts — submit.c 507→515, main.c 576→583 — after R1/R2–R5 added body
  lines; `git apply` tolerates that skew but GNU `patch` truncated the
  submit tail. Counts now byte-audited across all 17 sections.)
- Mock open-path test (new throwaway in `/tmp`, NOT committed — peer
  `code/gpu31/driver/submit_fix_test.c` owns the driver test slot):
  compiles the REAL 0004 `drm_sched` + `drm_syncobj` shim bodies
  against the sched single-threaded kernel emulation and a faithful
  copy of the fixed `file_init`/`file_fini`:
  - `cc` native **and** `aarch64-linux-gnu-gcc` + `qemu-aarch64`:
    `RESULT PASS: 13 checks, 0 failures` (zeroed-sched lock observed =
    hang cause; fixed init returns 0, `entities_live=1`, 3 entities on
    `sched->entities`; fini unregisters all; `offsetof` layout aliasing
    holds for `mgr/job_fctx/job_dev`).
- `-fsyntax-only` of all 8 extracted driver `.c` files (with both R1
  hunks) against the real 3.10.108 headers + staged 0001–0004 shims:
  **8/8 clean** (verified after peer `e4edb3a`, which also removed the
  local `P7580_GPU_INT_RAWSTAT/CLEAR` duplicates of `jm_hw.h`).
- Peer's `code/gpu31/driver/submit_fix_test` (T1–T6): still passes
  (`make check` → `all checks passed`), so R2–R5 are unaffected by the
  two R1 hunks.

## 4. Acceptance

- Updated driver patch series: `code/gpu31/driver/0001-*` (+15 lines:
  file_init ensure + probe warm-up). `0002` untouched.
- This report: `docs/reports/gpu31-open-fix.md` (call graph, offending
  lines, fix, host-side proof).
- Not done here (explicit non-goals): live-device boot/open test (no
  `/dev/dri` on current M1 image; Main's gate), render-only flag
  (G31_RenderOnlyFix).
- Suggested Main verification: rebuild int4 image, boot, run
  `pf7580-selftest --probe-only` (GET_PARAM/CREATE_BO-4K/MMAP_BO/
  GET_BO_OFFSET, no SUBMIT) — previously hung with zero output; with
  R1 it must print and exit 0. Then `strace -f -e trace=open,ioctl`
  one open+GET_PARAM to confirm O(1) open with no waits.
