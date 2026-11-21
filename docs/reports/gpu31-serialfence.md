# gpu31-serialfence — per-file job serialization (chain→fragment ordering)

Date: 2026-11-21. Owner: G31_SerialFence. Phone: up on int33-track
(`3.10.108`); this ticket builds/tests host-side only — Main flashes,
integrator owns the build.

## 1. Fault being fixed

Tiler heap written, FBD linkage exact, zero ERRs, but no triangle
coverage. Mesa submits the fragment job with `in_sync = 0`
(`pan_job.c:1046`) — it expects the **kernel** to order the tiler
chain before the fragment. Our driver uses per-slot scheduler
entities (`df->entity[slot]`), so JS1 tiler and JS0 fragment execute
concurrently; the fragment reads an incomplete polygon list
(`docs/reports/gpu31-polylist.md` §4, cause (b)).

Related: `gpu31-sched.md` (0004 F2–F3: dep gate + one-in-fence),
`gpu31-multidep.md` (§2.1: N in-fence AND node), `gpu31-polylist.md`
(§5: chain-gated fragment dispatch proposal).

## 2. Fix

### 2.1 Mechanism: implicit dependency on the previous job's armed fence

Chosen over a single scheduler entity for all slots (the ticket's
alternative): one entity would keep queue order but NOT execution
order — the 0004 pump clears `in_flight` synchronously right after
`run_job` returns, so two entities (or one shared entity with the
same pump) still run JS1/JS0 concurrently. The dep edge is what
orders execution, and it works across entities/slots without touching
slot assignment.

`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(submit.c + drv.h hunks only; `git apply --check` PASS):

- `struct p7580_drv_file` gains `serial_lock` (mutex) + `serial_prev`
  (`struct dma_fence *`, the previously submitted job's armed fence).
  Leading layout (`mgr/job_fctx/job_dev/drv`) is unchanged, so GEM's
  `file_priv` overlay still holds.
- `p7580_drv_file_init`: `mutex_init` + `serial_prev = NULL`.
- After `drm_sched_job_arm` (fence live) and before the in_sync/BO-excl
  dep loops: borrow `serial_prev` under the lock (temp `get`), attach
  it via `drm_sched_job_add_dependency` when it is non-NULL, distinct
  from this job's fence, and still unsignaled (already-signaled =
  retired = skipped, so no AND-leaf accumulation and no extra gate).
  Then publish this job's armed fence as the new `serial_prev`
  (replace + `get` under the lock; NULL arm clears the chain).
  Slot assignment above is untouched (requirements → slot).
- Error rollback: `published` (function scope, NULL init) records the
  installed fence; `err_job_bos` retracts the slot ref only if it
  still points at the failed job's fence (dep/queue failure after
  publish would otherwise leave a dangling predecessor).
- `p7580_drv_file_fini`: drops the slot ref after the entity drains
  (no new attach can happen then; the arming job owns its own ref
  via out_sync/retire).

Ref discipline: slot holds exactly one ref on the latest armed fence;
`add_dependency` holds its own ref when it gates; the temp borrow
`get`/`put` balances itself; every failure path after publish rolls
the slot back; close drops the last slot ref. No sleeps under the
lock (`is_signaled`/`get`/`put`/`add_dependency` callback arm only;
the BO-excl `get_excl` legs that sleep on resv locks stay outside).

No shim change: the 0004 dep gate + multidep AND node already enforce
"head waits while `dep_fence` unsignaled". No slot-routing change:
chain still `req==0 → JS1`, fragment `FS → JS0`.

### 2.2 Host tests (acceptance: ordering)

`code/gpu31/sched/tests/sched-host-test.c`: 4 new serial tests
(+32 checks; 367 → 399, native + qemu-aarch64 PASS) pinning the
mechanism the driver relies on, with one entity per slot exactly as
the driver owns them:

- `test_serial_cross_entity`: frag (entity B) chained on tiler
  (entity A) fence — B's pump does not run while A pending; runs
  after A retires.
- `test_serial_signaled_prev_skipped`: retired predecessor attaches
  nothing (`dep_fence == NULL`), dispatch immediate. (Uses a live
  signaled fence, not a freed job fence — the first draft used
  `j1->fence` after `finish_work` freed it (UAF: garbage flags
  attached + segfault with all 4 tests); fixed before commit.)
- `test_serial_retire_order`: run log + free log both tiler-then-frag.
- `test_serial_same_fence_noop`: duplicate attach is a no-op (no
  AND-node growth, no error).

```
native:        399 checks, 0 failures
qemu-aarch64:  399 checks, 0 failures
gates:         all PASS (unchanged)
sched kcheck:  4 files syntax-only + object compile PASS
apply-check:   driver 0001 applies clean
driver check:  submit_fix_test all checks passed
```

## 3. Files changed

- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`:
  serial chain (+99 lines: drv.h slot, init/fini, attach+publish,
  rollback; hunk counts re-fixed, `git apply --check` PASS).
- `code/gpu31/sched/tests/sched-host-test.c`: 4 serial tests + docs
  (+231 lines).

Untouched: 0004 shim (no change needed), 0001/0002/0003/0005, gem,
jobs, client, mmu, Makefile gates, `code/gpu31/series/` mirrors
(driver patch has no series mirror — only `patches/drm-backport`
mirrors 0001-0005, and this change is driver-0001, not backport).

## 4. Hand to integrator

Suggested build: copy of `build-int33.sh` with int34 names + a gate
for the new contract, e.g. `serial_prev` present in `submit.o`
(`nm`: `p7580_drv_submit` references `serial_prev`/`serial_lock`),
`serial chain` strings live in the Image, plus all prior int33 gates
unchanged (6b–6s). No flash per ticket (Main flashes).

On silicon expect: submits from one file strictly ordered — the
fragment job's dispatch waits for the tiler chain's fence, so the
tiler dump (`gpu31-polylist.md` §3) should now show triangle coverage
and egltest centre red. If the triangle is still absent with the gate
live, the cause is (a) (vertex/varing path), not (b): extend the 0xCF
dump to the varying/position buffers per polylist §5.2.
