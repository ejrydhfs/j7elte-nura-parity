# gpu31-multidep — N in-fence chain submit (sched F4 follow-up)

Date: 2026-11-21. Owner: G31_MultiDep. Phone: up on int32; this ticket
builds/tests host-side only — Main flashes, integrator owns the build.

## 1. Fault being fixed

With the int32 `bo_count` stamp, Mesa reaches the tiler-chain submit
and **asserts**: `panfrost_batch_submit_jobs: pan_job.c:1033
Assertion !ret`. Our `--submit-test` (single job, no in_sync) still
returns rc=0. The failing call is the tiler-chain submit with in_syncs
and multiple BOs.

Known shim limitation (gpu31-sched.md F4): a second distinct
dependency returns `-EOPNOTSUPP`. Mesa passes an array of in_sync
syncobj handles for a batch; the driver resolves each to a fence and
calls `drm_sched_job_add_dependency` per fence (plus one implicit BO
excl fence per BO). The second unsignaled distinct fence made the
ioctl fail → Mesa's assert fired.

## 2. Fix

### 2.1 Shim: wait-all AND node (no struct layout change)

`code/gpu31/sched/src/drm_sched.{c,h}` + regenerated 0004 mirrors
(`sched/patches`, `series`, `patches/drm-backport`, byte-identical,
`git apply --check` PASS, bodies `diff`-identical to `src/`):

- First unsignaled fence stores directly in `job->dep_fence`
  (fast path, zero allocation — unchanged).
- Second (and later) DISTINCT unsignaled fence merges into a
  `struct drm_sched_dep_and` node: `dep_fence` is re-pointed at the
  node, the job's single `dep_cb` waits on the node, and one
  per-leaf trampoline per slot (32) parks on each leaf. The node
  signals itself only after **every** leaf signals (AND drain), which
  fires `dep_cb` → pump re-queue → dispatch.
- Same fence twice (or an already-listed leaf) is a no-op 0;
  already-signaled fences are skipped; beyond 32 leaves fails
  `-ENOSPC` (ioctl caps at 16 in_syncs + 64 BOs, so unreachable).
- The old `-EOPNOTSUPP` single-dep reject is gone; ordering is
  unchanged (the pump still gates only the FIFO head on
  `dep_fence`). No `struct drm_sched_job` layout change: all
  `offsetof` asserts (ops=24/slot=232/ran=236) and the integrator's
  layout gates hold verbatim.
- Ref discipline: the node holds one ref per leaf; the merge steals
  the job's old-leaf ref (no churn). Release drops leaf refs, then
  frees the node. Both release sites (`entity_fini` queued-cancel,
  pump error legs) go through `drm_sched_job_cleanup` after
  detaching `dep_cb`, so the node `cb_list` is empty at free.

### 2.2 Bounded stage+errno attribution (shim + driver ioctl)

Every early error path now prints `stage + rc` on the shared bounded
budget (first 8 SUBMITs, same discipline as ST-PUSH/ST-PUMP), so the
next silicon assert is immediately attributable instead of a bare rc:

- Shim `drm_sched_job_err(stage, err)`: stages `dep` (merge/callback
  failures: `-ENOMEM`/`-ENOSPC`/callback rc), `submit` (NULL job),
  `pump` (NULL ops/run_job), `run` (ERR_PTR/NULL/mismatch returns).
  Success paths print nothing new.
- Driver `p7580_submit_err(stage, err)` in `panfrost7580_submit.c`
  (0001 hunk only): stages `validate` (all envelope/slot/present
  rejects), `in_sync` (handle resolve), `bo` (lookup/map), `job`
  (alloc/transfer), `ops` (opsfix guard), `dep` (both dep loops),
  `queue` (sched submit), `out_sync` (handle/find/replace).
  Existing ST-IOCTL/ST-QUEUE success prints unchanged.
- No logic change in the driver dep loops: they already iterate all
  N in_syncs + all BO excl fences and propagate the return — which
  is exactly what failed before (second `add_dependency` ≠ 0) and
  now succeeds.

## 3. Host tests (acceptance: 0/1/2/N deps + ordering)

`code/gpu31/sched/tests/sched-host-test.c`: old `test_dep_gating`
updated to multidep semantics (second dep merges; AND needs both
leaves — one signal still gates, second dispatches), plus:

- `test_multidep_zero`: dep-less job dispatches immediately.
- `test_multidep_one`: single leaf stored directly (no node);
  pre-signaled dep skipped (NULL dep, direct dispatch).
- `test_multidep_two`: both signal orders (a-then-b, b-then-a):
  first leaf still gates, second dispatches.
- `test_multidep_many`: 5 leaves; 4-of-5 still gates (exercises the
  3rd+ append-a-leaf path); 5th dispatches.
- `test_multidep_ordering`: two jobs, j2 chained on dep + j1's fence:
  head gate holds j2 while j1 runs; j1 retire releases j2; retire
  order 61 → 62 (run log + free log pinned).
- `test_multidep_errors`: NULL submit → `-EINVAL` (stage=submit);
  NULL/signaled deps skipped; duplicate adds are no-ops.

```
native:        367 checks, 0 failures
qemu-aarch64:  367 checks, 0 failures
gates:         all PASS (new: AND-node present, err-present, no EOPNOTSUPP return)
sched kcheck:  4 files syntax-only + object compile PASS (real 3.10.108 headers)
apply-check:   backport 0001-0005 apply clean (0005 needs Makefiles, as before)
```

Note: ASan reports a pre-existing harness UAF in
`test_timeout_push_ready` (uses `j` after `finish_work` freed it) —
present on the unmodified baseline too (verified via `git stash`);
normal (non-ASan) runs are clean. Not touched by this ticket.

## 4. Files changed

- `code/gpu31/sched/src/drm_sched.{c,h}`: AND node + merge +
  `drm_sched_job_err` + doc updates.
- `code/gpu31/sched/emu/include/linux/kernel-emu.h`: `ENOSPC`/`EEXIST`/
  `EFAULT` defines (errno parity for the new paths).
- `code/gpu31/sched/tests/sched-host-test.c`: updated gate test + 6
  new multidep tests + header docs.
- `code/gpu31/sched/Makefile`: gates (AND present, err present,
  EOPNOTSUPP-return gone).
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`:
  `p7580_submit_err` helper + 22 staged call sites (submit.c only;
  hunk counts re-fixed, `git apply --check` PASS).
- `code/gpu31/series/SHA256SUMS`: new 0004 hash
  `f6d3a81b…e474`.
- Regenerated mirrors (byte-identical, `diff`-identical to `src/`):
  `sched/patches/0004-*`, `series/0004-*`,
  `patches/drm-backport/0004-*`.

Untouched: 0001/0002/0003/0005, gem, jobs, client, mmu, Makefile
gates for other slices.

## 5. Hand to integrator

Suggested build: copy of `build-int32.sh` with int33 names + gates
for the new contract, e.g. `drm_sched_dep_and` present once in
`sched.o`, `p7580_submit_err` present in `submit.o`, ST-QUEUE +
`submit_err`/`err stage=` strings live in the Image, 0004 sha256
`f6d3a81b…`. No flash per ticket (Main flashes).

On silicon expect: the tiler-chain submit returns 0 (no Mesa
assert at `pan_job.c:1033`) and ideally red centre; if a submit
still fails, `dmesg` now names `submit_err stage=… rc=…` /
`sched: err stage=… rc=…` directly.
