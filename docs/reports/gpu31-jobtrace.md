# GPU31 jobtrace — bounded dispatch/IRQ/fence tracing + readback-safe completion path

Date: 2026-11-20. Owner: G31_JobTrace. Ticket: driver run_job / IRQ /
completion; GEM dma_sync; Mesa cache-flush expectations.

## 0. Verdict (honest)

Phone unreachable at ticket time (RNDIS route down:
`ping 172.16.42.1` 100% loss; USB shows only a fastboot-mode device,
no RNDIS gadget), so **no flash and no post-flash egltest evidence was
possible in this pass**. What this ticket delivers instead:

1. A compile-verified patch (canon slices + regenerated driver 0001)
   that the integrator can build as-is.
2. Two static root-cause findings that fully explain the silicon
   symptom (see §1) — one of them (out_sync NULL-install) makes the
   previous "fence proof" vacuous and is fixed here.
3. Exact post-flash rerun commands + expected dmesg lines so Main (or
   the next online agent) can close the evidence gap in one boot
   (see §6).

Host + kernel-syntax verification exercised (§5). No flash, no device
writes, no tree edits outside `code/gpu31`.

## 1. Why readback is all-zero (static analysis, ground truth)

Two independent defects, both in the committed int9 sources:

### F-JT1 (fence-proof vacuous): out_sync installed from a NULL fence

`p7580_drv_submit` (driver `panfrost7580_submit.c`, §"4. out-sync
install") read `sched_job->fence` at SUBMIT time and installed it into
the caller's syncobj:

```c
struct dma_fence *fence = sched_job->fence;   /* ALWAYS NULL here */
...
if (fence)
    ret = drm_syncobj_replace_fence(s, fence);
```

But the 0004 shim only assigns `job->fence = run_fence` from
`run_job`'s return **on the async worker** (`drm_sched_pump`, long
after the ioctl returns; `patches/drm-backport/0004-*.patch`
`job->fence = run_fence` with no other assignment). At SUBMIT time the
field is still the `kzalloc` zero — so the `if (fence)` leg never ran
and the syncobj kept its empty (already-signaled) state. Every
`SYNCOBJ-WAIT` on the "job fence" then returned 0 immediately **without
waiting for the GPU at all** — including the egltest path insofar as it
relies on sync/fence retirement. The int9-era fence proof
(`gpu31-submit-hw.md` §9) was therefore vacuous by construction: an
instant-0 wait is indistinguishable from a fast retire, and dmesg had no
dispatch/IRQ lines to tell them apart.

Fixed here (§3.3): SUBMIT validates the handle (`-ENOENT` preserved)
and stashes it on the job (`job->out_sync`); the async `run_job`
installs the real fence via `drm_syncobj_replace_fence` (takes its own
ref, matching the F-JOB3 alias rule).

### F-JT2 (coherency): no completion-side invalidate for HIGHMEM SHMEM

BOCREATE path (`panfrost_ioctl_create_bo`) allocates `P7580_BO_SHMEM`
with `cache_attr = P7580_GEM_WC`; the CPU mmap applies
`pgprot_writecombine` (`MT_NORMAL_NC`, no CPU lines — correct). But the
**backing pages themselves** come from `shmem_read_mapping_page` and on
this 1 GiB-RAM board routinely sit in HIGHMEM, where the CPU touches
them through a `kmap` (dcacheable) alias. The GPU writes the same
physical page through its own port under COHERENCY_NONE. Nothing ever
invalidated the kmap alias after the fence signaled, so a CPU read after
retirement (`glReadPixels`, the next `mmap` fault, or the client's
`pf7580_cpu_map`) could serve stale dcache bytes — i.e. exactly the
observed all-zero readback with 60/60 draws "completing" in 21 ms
(2889 FPS retires the fence; the bytes the CPU sees are another
question).

Fixed here (§3.4): `p7580_gem_job_done_sync()` runs on the
fence-retirement path (`free_job`, strictly after signal) and issues
`flush_dcache_page` per HIGHMEM page of referenced BOs. CMA (true WC,
no alias) and dma-direct SHMEM (dcache-clean) skip — bounded by page
count, no MMIO, no sleep.

Remaining alternative (kept open, decidable by one boot): if the
post-flash trace shows dispatch + IRQ DONE on every frame AND readback
is still zero, the job executed but the fragment path wrote nowhere —
suspect the Mesa batch (tiler heap / framebuffer address), not the
kernel. The trace lines in §6 distinguish the three cases exactly.

## 2. What the patch contains

| File (canon) | Change |
|---|---|
| `code/gpu31/jobs/panfrost7580_job.h` | `trace_left` budget on `struct p7580_job_dev` (kernel branch); `out_sync` stash on `struct p7580_submit_job` |
| `code/gpu31/jobs/panfrost7580_job.c` | bounded `pr_info` tracing: dispatch (slot/jc/cfg/as/affinity/seq), IRQ (STAT + JS_STATUS + name + has_job), fence-signal (slot/seq/sig_rc), IRQ-ERR; budget seeded from `P7580_JOB_TRACE_DEFAULT` (8) at `p7580_job_dev_init` |
| `code/gpu31/gem/panfrost7580_gem.h` | `kmap_flushed` field; `p7580_gem_job_done_sync()` declaration |
| `code/gpu31/gem/panfrost7580_gem.c` | `p7580_gem_job_done_sync()` (`flush_dcache_page` per HIGHMEM SHMEM page; CMA/non-HIGHMEM skip) + `<asm/cacheflush.h>` |
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | regenerated from the four canon files above + `submit.c` three-part change below (10 refreshed new-file hunks, hunk counts updated; all 19 blocks still `--check` clean) |
| driver `panfrost7580_submit.c` (inside 0001) | (a) out_sync NULL-install fix (§3.3); (b) fence-retirement `p7580_gem_job_done_sync` walk over `drv->bo_list` in `free_job` (§3.4); (c) comments only in `run_job` (trace pointer) |

What was deliberately NOT added: no `module_param` (KBUILD_MODNAME is
only defined by the kbuild object rule; a param would break the
slice-level standalone `-fsyntax-only` gate — budget is seeded from the
`P7580_JOB_TRACE_DEFAULT` constant); no `dma_sync_sg_for_cpu` on the
GEM pages (they were never `dma_map_*`ed for the CPU — the GPU mapping
is a raw `page_to_phys` IOVA install, so there is no DMA address to
sync; the alias flush is the correct primitive); no L2-flush register
pokes (T720 `JS_CONFIG` START/END CLEAN+INVALIDATE already covers the
GPU L2 per the jobs slice §1).

## 3. Design notes

### 3.1 Bounded tracing

- One `unsigned int trace_left` per device, seeded with 8 at
  `p7580_job_dev_init`, consumed under `hw_lock` (dispatch 1 + IRQ 1 +
  fence-signal 1 = ≤3 per job → the default covers the first ~2–3
  jobs fully, then goes silent; no log spam at 3000 jobs/s).
- `pr_info` (not `pr_debug`): visible at the stock loglevel so the
  post-flash `dmesg | grep p7580_job` works with no cmdline change.
- Dispatch line prints the cfg value **as written** to CONFIG_NEXT, so
  START_MMU / FLUSH bits are auditable from silicon.
- IRQ line prints raw `JOB_INT_STAT`, the slot `JS_STATUS`, its decoded
  name (`p7580_js_status_name`), and whether a job was in flight —
  DONE-with-no-job vs no-IRQ-at-all distinguishes "retired empty" from
  "never dispatched".

### 3.2 IRQ-safety

All trace prints run with `hw_lock` held (`spin_lock_irqsave` context
in IRQ; same lock in dispatch). `pr_info` in IRQ context with a
spinlock held matches existing kernel practice for bounded bring-up
tracing (no sleep, no mutex; `pr_info` is lock-safe printk). The
`p7580_js_status_name()` call in IRQ is a pure table lookup (no sleep).

### 3.3 out_sync fix detail

- Old: `sched_job->fence` read at SUBMIT (NULL) → never installed →
  empty-syncobj instant-signal.
- New: SUBMIT validates (`drm_syncobj_find`, `-ENOENT` preserved) and
  stores `job->out_sync`; async `run_job` installs the returned fence
  (`drm_syncobj_replace_fence`, own ref) after `pm_put`, before
  returning it to the shim (which assigns `sched_job->fence` and arms
  retirement). ERR_PTR run_job failures skip the install (no fence to
  install); a handle closed between SUBMIT and dispatch finds no
  syncobj and skips (job still runs — same policy as the old err_out
  leg).
- Client-visible effect: the `SYNCOBJ-WAIT(out_sync)` in
  `pf7580-selftest --submit-test` now blocks until the real job fence
  signals (or the 5 s timeout recovery signals it) instead of
  returning instantly.

### 3.4 Readback-safe path detail

- Hook point: `p7580_drv_free_job` (runs on `finish_work`, strictly
  after `cancel_delayed_work_sync(&timeout)` + fence signal per the
  0004 shim — every referenced BO is therefore post-fence).
- Walk: `drv->bo_list` under `bo_lock` (same discipline as
  `remove`/`map_bo`; every entry holds a GEM ref, so no use-after-free),
  scoped to `job->bo_count > 0`.
- Per-BO: `p7580_gem_job_done_sync` flushes only `PageHighMem` SHMEM
  pages (`flush_dcache_page`); CMA + dma-direct pages are no-ops.
- Not done: touching `vma`/`vm_page_prot`, changing the WC mmap path,
  or syncing the page tables (MMU slice already `dma_sync`s PT writes
  at map time; see `gpu31-dmabuf.md` §5).

## 4. Expected dmesg lines (post-flash)

With the default budget (first jobs), one NULL-job submit-test should
print:

```
p7580_job: dispatch slot=1 jc=0x<gpu_addr> cfg=0x00081301 as=0 aff=0x3 seq=0
p7580_job: irq slot=1 stat=0x00020002 js_status=0x00000000 (IDLE) has_job=1
p7580_job: fence signal slot=1 seq=0 sig_rc=0
```

- `slot=1` = vertex/tiler routing for `requirements=0` (matches
  `p7580_job_slot`).
- `cfg=0x00081301`: as_nr(0) | START_CLEAN_INV(0x300) | START_MMU(0x400)
  | END_CLEAN_INV(0x3000) | PRI8(0x80000) — the exact T720 value from
  `p7580_js_config(0)`, now auditable from silicon.
- `js_status=0x0 (IDLE)`: the slot went idle after DONE (benign; the
  fault table maps < 0x40 to benign names).
- `sig_rc=0`: first signal (normal retire). `-EINVAL` would mean the
  timeout path won the race (idempotent, by design).

Decision table:

| dmesg | readback | meaning |
|---|---|---|
| dispatch + irq + fence-signal | non-zero | job ran on HW; coherency fixed — close ticket |
| dispatch, NO irq, then 5 s timeout recovery | n/a | GPU never retired: clock/power/IRQ routing or MMU fault — check `panfrost7580: MMU fault` + `GPU MULTIPLE_FAULT` lines |
| NO dispatch line, fence waits full timeout | n/a | job never reached the slot: sched dep-blocked (in-fence/BO excl never signaled) or dark run_job (`ERR_PTR -EIO`, see pm lines) |
| dispatch + irq DONE, readback still zero | zero | kernel retire proven; bytes never landed — Mesa batch/tiler-heap addressing suspect (hand to G31_Mesa with this trace) |

Per-job cost: ≤3 lines for the first ~3 jobs, then silence (budget
exhausted). `trace_left` seed is a constant (`P7580_JOB_TRACE_DEFAULT`
= 8); raising it is a one-line canon edit + 0001 regen.

## 5. Verification (exercised)

- Host: `jobs/job_test` PASS (74 CHECKs), `gem/gem_validate` PASS,
  `driver/submit_fix_test` PASS (unchanged scope: resv/PM/timeout models).
- Kernel syntax (`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Werror`
  vs real 3.10.108 headers + staged 0001–0004 shims, same flags as the
  slice Makefiles): `panfrost7580_job.c` RC=0 (jobs Makefile kcheck
  shape), `panfrost7580_gem.c` RC=0 (`gem` Makefile `kcheck` — all 3
  files PASS, zero warnings), `panfrost7580_submit.c` RC=0 (same
  KINC + `-I panfrost -I staged-drm`, `KBUILD_MODNAME` defined).
- Patch hygiene: regenerated 0001 `git apply --check` clean (all 19
  hunks; 10 refreshed new-file blocks with corrected `+1,N` counts);
  full-file `git apply --check` (not just `--check` on fragments)
  guards the int9-class truncation (`+1,691` vs 718) by construction.
- NOT verified (blocked, phone down): Image build (`build-int9.sh`
  needs the external-disk pristine tree + pack), flash, `/dev/dri`
  submit-test, egltest readback. No reboot/flash attempted — Main owns
  the flash gate.

## 6. Post-flash evidence gap — exact rerun (for Main / next online agent)

Prerequisites: phone on RNDIS (`ping 172.16.42.1`), int10 image built
from this commit (integrator: `sh code/gpu31/series/build-int9.sh`
naming aside — reuses 0001/0002 + int9 script; expect Image + boot.img
sha256s in the build log), flashed, `renderD128` present.

```sh
# 0. stage payloads (host, phone down at ticket time — re-upload)
/tmp/ap.sh  # password helper: SSH_ASKPASS=/tmp/ap.sh setsid -w ssh ...
# upload: /tmp/pf7580-submit (build-aarch64/pf7580-selftest),
#   /data/wdarm, /home/user/egltest, /home/user/eglrun.sh
# 1. clear + submit-test (15 s watchdog cap per int9):
ssh user@172.16.42.1 'sudo -n sh -c "dmesg -c >/dev/null"'
ssh user@172.16.42.1 'sudo -n sh -c "flock /tmp/gpsdev.lock /data/wdarm 15 /tmp/pf7580-submit --submit-test /dev/dri/renderD128"'
# 2. collect trace + fence proof:
ssh user@172.16.42.1 'sudo -n dmesg | grep -E "p7580_job|panfrost7580|MMU fault|MULTIPLE_FAULT" | head -n 40'
# 3. egltest offscreen (Mesa 20 env per SESSION-STATE):
ssh user@172.16.42.1 'sudo -n sh -c "flock /tmp/gpsdev.lock /home/user/eglrun.sh --offscreen-only"'
# record: draws line (FPS), readback corner/center RGBA, RENDER_OK vs RENDER_MISMATCH
```

Close criteria: §4 decision table row 1 (dispatch + IRQ DONE +
`sig_rc=0`, readback non-zero `RENDER_OK`). If row 2/3, paste the
`p7580_job` lines + `MMU fault`/`MULTIPLE_FAULT` lines into this report
and hand to G31_Power/G31_Mmu. If row 4 (trace proves retire, readback
still zero), hand to G31_Mesa with the dispatch cfg + slot lines.

## 7. Integrator notes

- Apply order unchanged: backport 0001–0005 → KMS Mode-B → dmabuf 0001
  → driver 0001 (this regen) + 0002 → fragments d0/d4 (`build-int9.sh`).
- This regen touched ONLY the 10 driver-0001 new-file hunks listed in
  §2 (canon edits in `jobs/` + `gem/` + new `submit.c`); 0002 wiring,
  backports, KMS, dmabuf, fragments untouched.
- Struct-field note: `p7580_submit_job.out_sync` and
  `p7580_bo.kmap_flushed` are new fields — any out-of-tree code that
  `kzalloc`s these structs is unaffected (zero-init); no initializer
  updates needed.
- `p7580_job_trace` is intentionally NOT a `module_param` (see §2);
  expose one later from `panfrost7580_submit.c` (which builds only as a
  kbuild object with KBUILD_MODNAME) if runtime tuning is wanted.

## 8. Files changed (this ticket)

- `code/gpu31/jobs/panfrost7580_job.h` (+9: `trace_left`, `out_sync`)
- `code/gpu31/jobs/panfrost7580_job.c` (+87/−3: trace block, budget seed, dispatch/IRQ/fence prints)
- `code/gpu31/gem/panfrost7580_gem.h` (+17/−1: `kmap_flushed`, sync decl; restored `get_sg_table` decl)
- `code/gpu31/gem/panfrost7580_gem.c` (+37: cacheflush include, `p7580_gem_job_done_sync`)
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (regen, +158/−16 net: the above + submit.c out_sync/sync/hook)
- `docs/reports/gpu31-jobtrace.md` (this file)
