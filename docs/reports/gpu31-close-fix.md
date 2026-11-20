# gpu31 close-path deadlock — bounded entity/syncobj teardown (CLOSE watchdog fix)

Date: 2026-11-20. Owner: G31_CloseFix.
Ticket: `drm_sched_entity_fini` / `drm_sched_fini` / syncobj manager fini in
`code/gpu31/sched/` + their call in the close path.

Device ground truth (per Main, int7 node): `--submit-test` on the real
render node — open / GET_PARAM / CREATE_BO / MMAP / DESC / SUBMIT(rc=0) /
WAIT_BO(rc=0) / FENCE-PROBE(rc=0, already signalled) / POST GET_PARAM(0x720)
all pass — then **`CLOSE: calling pf7580_close now` hangs** and the
watchdog resets the SoC ~14 s later. The submit leg is proven; the
remaining deadlock is in the close path: `p7580_drv_postclose` /
`p7580_drv_file_fini` / `drm_sched_entity_fini` / `drm_syncobj_manager_fini`.

## 1. Deadlock chain (static analysis vs 3.10 + mainline semantics)

Close order on 3.10 (`drm_fops.c:drm_release`, verified in the pristine
tree): `preclose` → `drm_gem_release` (handle refs; `struct_mutex`-locked
per-object free) → `struct_mutex` critical section → **`postclose`**
(holds `drm_global_mutex`) → prime teardown → `lastclose`. Our
`postclose` (`main.c`) takes one teardown PM hold, calls
`p7580_drv_file_fini` (`submit.c`), `kfree`s, releases the hold.
`file_fini` takes its own inner hold, finis all three sched entities,
finis the syncobj manager.

Three defects combined into the hang:

**D1 — fini could wait forever on a dispatched-but-unretired job.**
The old `drm_sched_entity_fini` spliced only the *queued* FIFO and then
did an unbounded `flush_workqueue` on the *shared* ordered wq. A job that
has already dispatched (dequeued by the pump, fence pending, owned by its
fence callbacks — on **no** list) is invisible to the splice. Its only
retirement is the IRQ DONE signal or the per-job timeout work. If the
block is dark at close (close-after-idle: autosuspend already gated the
clocks), no IRQ ever comes, and the old driver `timedout_job` **returned
early without signalling** when `pm_get` failed — so `job_count` stayed
elevated forever, `entity_idle` never completed, and the shared-wq flush
sat behind a delayed timer that could never retire the job. Worse, after
the flush returned, `df` was `kfree`'d while the timeout work was still
armed: the 5 s timer then fired into freed entities (`container_of` →
wild `drv` → wild MMIO/PM put) — a bus hang, i.e. the watchdog signature
(USB gone, not a mere D-state).

Contributing evidence: both WAIT_BO legs in the failing run proved
nothing — the driver never attaches the job fence to the BO resv
(known gap, submit-fix §4), so WAIT_BO on a fresh BO returns instantly
from an *empty* resv whether or not the GPU retired the job. The job may
very plausibly still have been in flight at CLOSE.

**D2 — stale completion + missing re-arm.**
`entity_idle` was completed once at init and never re-armed on push, so
a close wait could observe a stale completion. (No waiter existed yet,
but any bounded wait built on it would have been broken.)

**D3 — NULL-dep crash on the cancel path.**
The retire sweep called `dma_fence_remove_callback(job->dep_fence, …)`
unconditionally; most queued jobs carry no dep (`dep_fence == NULL`) and
the shim dereferenced `fence->cb_lock` — a NULL deref on the first
dep-less close. (Oops, not hang — fixed in the same pass since close
walks this path every time.)

Non-causes ruled out (checked against the 3.10 tree): `drm_global_mutex`
is held across `postclose` but no worker takes it (all ioctls are
`DRM_UNLOCKED`); `struct_mutex` is not held across `postclose`;
`pm_runtime_get_sync`'s only unbounded phase is waiting out a concurrent
suspend/resume transition whose callbacks are themselves bounded
(100 µs-step READY polls, 100 ms caps); the -EBUSY idle guard covers the
queue leg; GEM free is MMIO-free.

## 2. Fix (this commit)

Shim (`code/gpu31/sched/src/`, regenerated into all three patch copies —
`patches/drm-backport/`, `code/gpu31/sched/patches/`,
`code/gpu31/series/`, bodies byte-identical):

- `drm_sched.h`: new `DRM_SCHED_CLOSE_MARGIN_JIFFIES` (8 s past one job
  timeout + one recovery), `drm_sched_close_budget()`,
  `drm_sched_entity_wait_idle()` (0 idle / -ETIME on expiry / -EINVAL on
  NULL), `<linux/jiffies.h>` + `<linux/completion.h>` includes.
- `drm_sched.c` `entity_fini`: idempotent (second fini is a no-op:
  stopped + empty lists + detached + zero count); stops dispatch first
  (stopped + unlink under the right locks, wq snapshotted under
  `sched->lock`); `cancel_work_sync` the pump; frees queued jobs with
  the dep callback detached *before* freeing and `job_count`
  decremented per free (NULL-dep safe); then **bounded** wait for
  dispatched jobs (`flush` + `wait_idle(budget)`) + final drain.
  No unbounded wait exists on this path anymore.
- `drm_sched.c` `push_job`: `atomic_inc_return`, `INIT_COMPLETION` on
  the 0→1 transition (re-arm).
- `drm_sched.c` `sched_fini`: stop workers first (ready=false + evict
  under `sched->lock`), snapshot wq, drain-then-destroy only if non-NULL
  (double fini safe).
- `dma-fence.c` `remove_callback`: NULL fence/cb → false (no detach).
- `drm_syncobj.c` `manager_fini`: documented non-blocking contract
  (drops owner refs only; concurrent WAIT pins its own refs before
  sleeping; double fini safe) + NULL guard.
- Emu (`kernel-emu.h`): `INIT_COMPLETION`, `complete`,
  `completion_done`, `atomic_inc_return` (test-only).

Driver (`code/gpu31/driver/0001-*.patch`, `submit.c` only):

- `p7580_drv_timedout_job` dark leg now **always signals the job fence**
  (CPU-only: callbacks + completion, never MMIO) when `pm_get` fails
  instead of returning with the fence pending. This is what makes the
  shim's bounded wait terminate: every dispatched job retires through
  finish_work within one timeout + one recovery regardless of power
  state. Recovery-skipped-dark is fail-closed (no register touched).
- `p7580_drv_file_fini`: cancel-ordering comment (entities → bounded
  wait → syncobj last; PM hold bound documented). No logic change
  beyond the timeout guarantee it depends on.

Correct cancel ordering (close): pump cancelled → queued jobs detached
+ freed → dispatched jobs bounded-waited (IRQ/timeout retires them) →
final drain → syncobj handles dropped (never waited on) → PM holds
released → file state freed. The timeout/IRQ paths can only touch entity
memory while `fini` is still inside its bounded wait (entity memory is
borrowed from the file state, freed after fini returns).

## 3. Host test output (mock GPU, ordering repro)

`code/gpu31/sched`: 217 checks (was 182), native **and** qemu-aarch64
PASS; 16 structural gates PASS; kernel `-fsyntax-only` + real `-c`
object compiles PASS against the real 3.10.108 headers (4 files, zero
warnings); `git apply --check` clean on all touched patches.

New close tests (`tests/sched-host-test.c`, backends mirror the fixed
driver legs — timeout hook always signals, like the dark-skip leg):

- C1 idle wait returns 0 immediately; NULL entity → -EINVAL; budget
  default = timeout + margin.
- C5 push re-arms `entity_idle` on 0→1 (no stale completion).
- C3 queued job with unsignaled dep: pump leaves it queued; fini frees
  it synchronously (free_count 1, queue empty, count 0); late dep signal
  does **not** re-queue the dead pump; second fini is a no-op.
- C4 dispatched-but-unretired (mock GPU never IRQs): `wait_idle` →
  -ETIME while outstanding (bounded, nothing freed early); timeout work
  → backend signal → finish retires (free_count 1); fini + double fini
  + double `sched_fini` all safe.
- C6 syncobj fini with a *pending* fence drops the handle without
  waiting; double fini safe; job-side ref released independently.
- C7 `remove_callback(NULL, …)` is a no-op.

`code/gpu31/driver/submit_fix_test`: new T7 (dark timeout signals
exactly once, idempotent) — **all checks passed**.
`code/gpu31/jobs/job_test`, `code/gpu31/gem/gem_validate` — pass
(unchanged, re-run).

## 4. Files changed

- `code/gpu31/sched/src/drm_sched.{h,c}`: bounded idempotent fini +
  wait_idle/close_budget + re-arming push + hardened sched_fini.
- `code/gpu31/sched/src/dma-fence.c`: NULL-safe remove_callback.
- `code/gpu31/sched/src/drm_syncobj.c`: non-blocking fini contract.
- `code/gpu31/sched/emu/include/linux/kernel-emu.h`: test primitives.
- `code/gpu31/sched/tests/sched-host-test.c`: 35 new close checks.
- `code/gpu31/driver/submit_fix_test.c`: T7 dark-timeout check.
- Regenerated (byte-identical bodies): `patches/drm-backport/000{1,3,4}-*`,
  `code/gpu31/{sched/patches,series}/000{1,3,4}-*`.
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`: always-signal
  dark timeout + close-ordering documentation.

## 5. For Main / Integrator

- Build + flash normally (int8 script globs the driver + backport
  patches; no Kconfig/DT change). Expected on device: `--submit-test`
  prints `CLOSE: returned (fd closed); process exiting` and exits 0 —
  no watchdog. If the GPU leg ever wedges again, close now takes at
  most ~one timeout + ~8 s (not infinity) and `dmesg` keeps the
  `-ETIME`/timeout/recovery trail.
- Deliberately NOT changed: WAIT_BO-vs-job-fence attachment (known gap,
  makes WAIT_BO vacuous on fresh BOs — the fence-probe should attach or
  the gap stays documented); BO resv excl install; PM autosuspend tuning.
