# gpu31 SUBMIT deadlock — root cause, call graph, fix

Date: 2026-11-19. Author: G31_SubmitFix2.
Symptom (ground truth, not re-probed): on the panfrost7580 driver image
(int4), the SUBMIT ioctl selftest and Xorg wedge in uninterruptible sleep
(D state) until reboot. No device access, no flash, no Xorg-touching
repro was run for this ticket: every finding below is by static analysis
of the snapshotted driver (`code/gpu31/driver/0001-*`), the canonical
slices (`jobs/`, `gem/`, `iommu/`), and the 0001–0004 backport shims
(`code/gpu31/series/` + `patches/drm-backport/`). Xorg shares the render
node, so any wedged submitter looks like "card0 hangs X".

## Root causes (three independent D-state / infinite-wait defects)

### R1 (primary, D state) — scheduler never initialized on the open path
`p7580_drv_file_init()` (`submit.c`) calls `drm_sched_entity_init()` on
the static `p7580_drv_sched`, but nothing ever calls `drm_sched_init()`:
not probe (`main.c`), not the file init itself. The static is zeroed, so
`sched->lock` is a zeroed mutex (`count == 0`; `mutex_init` sets 1) and
`sched->wq == NULL`, `sched->timeout == 0`, `sched->ready == false`.
The first `drm_sched_entity_init()` does `mutex_lock(&sched->lock)` on
that zeroed mutex and sleeps uninterruptibly forever — the first DRM open
(Xorg at boot, the selftest) wedges in D. Even past that, `entity->wq`
would be NULL (`queue_work(NULL)` oops) and `entity->timeout == 0` would
disarm every per-job timeout, deleting the only recovery net below.
Fix owner: G31_OpenFix (claimed: call `p7580_drv_sched_ensure()` before
the entity loop + one probe line; explicitly left untouched here, no
overlap: this ticket does not edit `p7580_drv_file_init` or probe).
Status at commit time: pending peer landing; this report documents the
defect so the hang is explained with or without it.

### R2 (D state) — resv self-deadlock on every SUBMIT with a BO
`p7580_drv_submit()` (`submit.c`, implicit-BO-excl loop) did:

```c
mutex_lock(&bo->resv.lock);
f = dma_resv_get_excl(&bo->resv);
mutex_unlock(&bo->resv.lock);
```

but the 0002 shim's `dma_resv_get_excl()` itself does
`mutex_lock(&obj->lock)`. The resv lock is a plain non-recursive mutex,
so the first SUBMIT referencing any BO self-deadlocks the submitter in
uninterruptible `mutex_lock` — D state, same signature as R1, and it
fires as soon as R1 is fixed. Fix (this ticket, patch hunk S1): drop the
outer lock; `get_excl` locks internally (its contract). Rule going
forward: never hold `resv->lock` across a 0002 helper — they all lock
internally.

### R3 (infinite fence wait) — timed-out job's own fence never signaled
`p7580_job_run()` creates the fence, then `p7580_job_hw_submit()` bails
(`WARN_ON` + return) when `COMMAND_NEXT` is still busy — the new fence
stays unsignaled and the job never enters `in_flight[slot]`. The sched
timeout (`p7580_job_timedout`) ran `p7580_job_recover()`, which signals
only the job latched in `in_flight[slot]` (the *older* stuck job, F-JOB4)
— nobody ever signals the timed-out job's fence. No second timeout
re-arms it and its `finish_cb` never fires: any waiter on that fence
(out-sync, sched retirement) hangs forever. Fix (this ticket, S3): after
`recover()`, `timedout` signals `job->done_fence` if still pending —
bounded (one timeout + one recovery), idempotent (`is_signaled` guard,
same F-JOB4 precedent; the 0001 fence has no error state, so plain
signal + dmesg is the tree's established fault idiom).

### R4 (contributing, correctness) — submit proceeds on dead GPU
`panfrost_ioctl_submit_override()` ignored `pm_runtime_get_sync()`'s
return and programmed HEAD/START on a dark GPU: the slot wedges with no
IRQ ever coming, and every waiter depends on the R3/timeout net (which R1
had disarmed). Fix (this ticket, S2): on `ret < 0`, `pm_runtime_put_noidle`
(balance, same arm as the power-slice probe path) and return the error —
bounded, no wait on dead hardware.

### R5 (contributing, stale snapshot) — MMU invalidate predates F-MMU1..3
The snapshot's `mmu.c`/`mmu.h` predated the canonical F-MMU1 (LOCK-region
width encoding), F-MMU2 (`wmb` before LOCK), F-MMU3 (stepped `wait_ready`
poll) fixes: raw-address invalidates leave stale TLB entries, so valid
mappings fault → ERR IRQ → 5 s timeout + 3 s recovery per submit (looks
like a hang under load). Fix (this ticket, S4): re-snapshotted both hunks
verbatim from canonical `iommu/` (the 12bdaec `job.c` precedent), now
including `p7580_mmu_teardown()` (F-MMU5).

## SUBMIT call graph (locks + waits annotated)

```
ioctl (process ctx, interruptible unless noted)
└─ panfrost_ioctl_submit_override              [submit.c]
   ├─ copy_from_user (bounded, -EFAULT)
   ├─ pm_runtime_get_sync  ──UNBOUNDED, UNINTERRUPTIBLE──▶ resume: p->lock +
   │     S2: check ret, abort on <0              poll clocks/reset/cores (each
   │                                             poll bounded ~100 ms)
   └─ p7580_drv_submit                         [submit.c]
      ├─ drm_gem_object_lookup (idr spinlock + kref, no sleep)
      ├─ p7580_drv_map_bo: bo_lock (mutex, short LS scan only) + mmu_map
      │    (GFP_KERNEL alloc, may sleep; NO lock held) + invalidate
      ├─ in-sync resolve: syncobj mgr->lock + syncobj->lock (short)
      ├─ BO excl loop: dma_resv_get_excl ──locks resv->lock internally──
      │     S1: NO outer lock (old outer lock = self-deadlock, D)
      ├─ drm_sched_job_add_dependency × N: shim allows ONE dep; 2nd distinct
      │    unsignaled fence → -EOPNOTSUPP (submit fails, no hang; §4 note)
      ├─ drm_sched_job_submit → queue_work (no wait)
      └─ out_sync install: reads sched_job->fence pre-dispatch (NULL → skip;
           fence still signaled later via IRQ/timeout; §4 note)

dispatch (ordered workqueue, process ctx)
└─ drm_sched_pump                              [0004 shim]
   ├─ queue_lock (spinlock, dropped before run_job) — no ABBA (fini order:
   │   entity->lock → queue_lock / sched->lock; pump takes queue_lock,
   │   sched->lock alone)
   └─ p7580_drv_run_job → p7580_job_run       [job.c]
      ├─ p7580_fence_create (kzalloc, may sleep; no lock)
      └─ p7580_job_hw_submit: hw_lock (spinlock_irqsave, register pokes
          only) + WARN_ON(COMMAND_NEXT busy) → early return (S3 covers)

IRQ (no sleeps, no mutexes)
└─ p7580_job_irq                               [job.c]
   ├─ JOB_INT_CLEAR (deassert), hw_lock spinlock only
   ├─ ERR(j): COMMAND_NEXT=NOP; recovery deferred to timeout ctx
   └─ DONE(j): clear in_flight[slot] under hw_lock, dma_fence_signal
      (wakes sched finish_cb + syncobj/WAIT_BO waiters)

timeout (workqueue, may sleep)
└─ p7580_job_timedout(job)                     [job.c, S3]
   ├─ early-out if signaled (IRQ-won race)
   ├─ p7580_job_recover(slot, status): reset_lock (mutex; concurrent slot
   │   faults serialize; worst hold ~2× drain + reset ≈ 6 s, bounded) →
   │   hw_lock (register stop only; order reset_lock → hw_lock matches
   │   everywhere, no ABBA) → drain polls (msleep(1), ≤ RESET_TIMEOUT_MS
   │   each, -ETIMEDOUT) → SOFT reset poll (bounded) → HARD fallback →
   │   CLEAR/re-arm → unlock → signal latched in_flight fence (F-JOB4)
   └─ S3: signal job->done_fence if still pending (covers swallowed START)

wait (WAIT_BO ioctl, process ctx)
└─ p7580_gem_wait → dma_resv_wait_timeout(intr=true, absolute→relative
    jiffies, F-IO2) — interruptible, bounded by fence signal; resv shim
    pins fences under resv->lock then waits UNLOCKED (no mutex-across-wait)
```

Lock order (global): `reset_lock` → `hw_lock` (recover) vs `hw_lock` alone
(hw_submit, IRQ) — consistent, no ABBA. `bo_lock`, `resv->lock`,
syncobj/mgr locks are leaf-level, never held across waits or each other
(S1 removed the one violation).

## Changes (this commit, plus R4-tail repair)

- `code/gpu31/jobs/panfrost7580_job.c` — S3: `p7580_job_timedout`
  signals the timed-out job's own fence after recovery (bounded,
  idempotent).
- `code/gpu31/driver/0001-*.patch` — regenerated hunks: `job.c` (S3),
  `mmu.c`/`mmu.h` verbatim re-snapshot from canonical `iommu/` (R5/S4);
  `submit.c` S1 (drop outer `resv->lock`) + S2 (abort on resume error);
  `main.c` R1 hunk is G31_OpenFix's (left untouched here).
- Patch-format repairs (same files, no logic change):
  - `@@` counts corrected (`submit.c` 507→515, `main.c` 576→583 after
    R1, then 583→581): the S2/R1 line edits added body lines without
    updating the counts, so GNU `patch(1)` applied only the first N
    lines and silently dropped the tail (missing
    `p7580_drv_submit(&staged)` use → `-Wunused-but-set-variable` on
    `staged` + `expected declaration at end of input` at the
    `put_noidle` block). `git apply` tolerates the skew, `patch`
    does not — counts are now byte-audited for all 17 sections.
  - `main.c`: removed the two local `P7580_GPU_INT_RAWSTAT/CLEAR`
    defines (they collide with the identical `jm_hw.h` pair;
    `-Werror` failure under the slice `-fsyntax-only` recipe).
- Verification actually run here (real 3.10.108 headers + staged
  0001–0004 shims, `aarch64-linux-gnu-gcc -std=gnu89 -fsyntax-only
  -Wall -Werror -Wno-unused-but-set-variable`): all 8 driver `.c`
  files clean; `patch -p1` fresh-apply whole; `git apply --check`
  passes.
- `code/gpu31/driver/submit_fix_test.c` + `Makefile` (new) — host tests
  T1–T6 (resv discipline, resume abort, timeout-signal, bounded
  recovery, idempotence, spurious-timeout skip). `make check`: all pass;
  `jobs`/`gem` slice tests still pass.
- Untouched by design: `p7580_drv_file_init` + probe R1 logic
  (G31_OpenFix); 0001–0004 shim patches (pool with shim owners); client
  selftest (G31_NodeProbe2); kms/render-only (G31_RenderOnlyFix).

## Acceptance / follow-ups for Main

- Verify: `git apply --check` 0001/0002; host `make check` in
  `code/gpu31/driver`, `jobs`, `gem`; land order with G31_OpenFix's R1
  hunk (R1 gates the R3 timeout net: `entity->timeout` is 0 until the
  scheduler is initialized, so timeouts never arm without it).
- Known non-hang issues left open (documented, no silent scope): single-dep
  shim rejects 2+ distinct unsynced in-fences with `-EOPNOTSUPP`;
  out-sync install skips when the job hasn't dispatched yet (no retro
  install — ordering gap, not a hang); job fence never attached as BO
  excl (WAIT_BO orders against the pre-submit fence set); fence
  `default_wait(…, timeout=0)` reports success on a pending fence (shim
  0001 follow-up; makes zero-timeout WAIT_BO vacuously succeed).
- After both hunks land, the device-side proof is: SUBMIT selftest + Xorg
  no longer enter D; a faulted submit resolves within ~8 s (5 s timeout +
  3 s recovery) with waiters woken.
