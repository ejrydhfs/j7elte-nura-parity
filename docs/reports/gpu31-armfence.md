# gpu31-armfence — arm-time completion fence (int24 follow-up)

Date: 2026-11-21. Owner: G31_ArmFence. Phone: up on int24; this ticket
builds/tests host-side only — Main flashes, integrator owns int25.

## 1. Fault being fixed

Deterministic `INSTR_TYPE_MISMATCH`, one per frame, on the last submit:
Mesa recycles the batch BO while the GPU still executes it (torn
instruction stream; `jc` inside a valid mapping, descriptor legal).
Root cause: the 0004 shim created the completion fence in `run_job`
(async worker), so at SUBMIT time `drm_sched_job_arm()` left
`sched_job->fence == NULL` and the out_sync install was a no-op (the
old F-JT1 NULL leg). Mesa therefore received an empty syncobj and
reused the BO immediately after the ioctl returned.

Fix (mainline `drm_sched_job_arm` semantics): allocate the completion
fence at arm time, install it on out_sync during the ioctl, and have
run_job/completion signal that same fence — never a second one.

## 2. Call-order contract (new, enforced)

```
SUBMIT ioctl:
  alloc sched_job (fence == NULL)
  arm ......................... ops->alloc_fence -> job->fence (live, unsignaled)
  deps ........................ add_dependency (in-fences, implicit excl)
  queue ....................... drm_sched_job_submit
  out_sync install ............ replace_fence(handle, job->fence)  [DURING ioctl]
async worker:
  pump ........................ run_job must return job->fence (same pointer)
  IRQ / timeout ............... dma_fence_signal(job->fence)
  finish ...................... cancel timeout, put, free_job
```

Enforcement points:

- `drm_sched_job_arm` (shim): calls `ops->alloc_fence` exactly once,
  assigns `job->fence`. Idempotent (set fence is never replaced).
  Fails closed: NULL `alloc_fence` or NULL/ERR return leaves
  `fence == NULL`; the out_sync install then skips (job still runs).
- Pump (shim): after `run_job` returns, requires
  `job->fence && run_fence == job->fence`. A second fence (or a NULL
  arm) takes the F4 free path: the extra reference is dropped, the
  armed fence is dropped, the job is freed — out_sync never points at
  a fence the completion path will not signal. (Also fixed a latent
  NULL-ops deref on this path: `job->ops->free_job` is now NULL-
  guarded like the first dispatch-failure leg.)
- `arm` INITs both cb nodes, so it must precede `add_dependency`
  (mainline order alloc → arm → deps → submit): re-INIT after a queue
  detaches the dep waiter and wedges the signal drain in an infinite
  loop. Found by the host test (infinite `DRAIN` on the queued
  dep_cb); the dep-gating and close-queued tests now arm first, and
  the driver SUBMIT arms before both dep loops.
- Backend split: `p7580_job_alloc_fence` (jobs TU: owns the fence
  container + ops + per-slot contexts, `dma_fence_init`-based) is the
  ONLY allocator; `p7580_job_run` binds the armed fence (one shared
  ref, no second allocation). The driver `run_job` stamps
  `done_fence = armed + get`, calls `p7580_job_hw_submit` directly,
  publishes the armed fence as the BO excl fence (instrtype
  serialisation), returns it. Fence contexts are allocated in
  `p7580_job_dev_init` (`dma_fence_context_alloc` per slot).

Error/ref discipline: queued-but-never-dispatched frees drop both
refs via `drm_sched_job_cleanup`; ERR-path pump drops dep + armed
refs before `free_job`; `free_job` drops the single shared ref via
`p7580_job_free` (alias never put twice, F-JOB3 intact). Dark
timeout still always signals (close-fix guarantee preserved).

## 3. Files changed

- `code/gpu31/sched/src/drm_sched.{c,h}` + regenerated 0004 mirrors
  (`sched/patches`, `series`, `patches/drm-backport`, byte-identical,
  round-trip verified): `alloc_fence` hook on `drm_sched_backend_ops`,
  arm-time allocation, same-fence pump enforcement, fence-field docs.
- `code/gpu31/jobs/panfrost7580_job.{c,h}` + driver-0001-embedded
  copies: `p7580_job_alloc_fence`, bind-only `p7580_job_run`,
  `dma_fence_init` modernisation, context init, header docs.
- Driver 0001 `submit.c` hunk only: `p7580_drv_alloc_fence`
  (drv_priv-stamped `jd` + `emit_seqno`), ops table entry, opsfix
  guard extension, arm-before-deps, SUBMIT-time out_sync install of
  the armed fence, run_job rebind (no run_job-time out_sync work).
- `code/gpu31/series/SHA256SUMS`: new 0004 hash
  `8e79d77c…e55b5`.

Untouched: 0001/0002/0003/0005, gem, client, mmu, syncobj, Makefile
gates (all still PASS unmodified).

## 4. Host test output (acceptance: fence armed at submit, same pointer signalled)

`code/gpu31/sched/tests/sched-host-test.c`: every submit site now
arms (alloc → arm → submit); new tests:

- `test_armfence_submit_armed_same_signalled`: alloc leaves
  `fence == NULL`; arm makes it live + unsignaled; re-arm is a no-op
  (same pointer); submit keeps it live; dispatch returns the same
  pointer (`run_count == 1`); signal + finish retires (`free == 1`).
- `test_armfence_noalloc_fails_closed`: no `alloc_fence` → arm
  leaves NULL → dispatch frees the job via F4, FIFO advances.

```
native:        280 checks, 0 failures
qemu-aarch64:  280 checks, 0 failures
gates:         all PASS
sched kcheck:  4 files syntax-only + object compile PASS
jobs kcheck:   panfrost7580_job.c syntax-only + object compile PASS
               (-I../iommu for panfrost7580_mmu.h)
job_test:      all checks passed
submit_fix:    all checks passed
client:        MOCK SUITE: PASS
apply-check:   backport 0001–0005 all apply clean to a scratch tree
```

## 5. Hand to integrator (int25)

Suggested build: copy of `build-int24.sh` with int25 names + a gate
for the new contract, e.g. `alloc_fence` present once in `submit.o`,
`drm_sched_job_arm` referenced in `submit.o`, `p7580_job_alloc_fence`
`T` in `job.o`, and `F-JT1`-era NULL-install comment gone from the
applied `submit.c`. No flash per ticket (Main flashes).
