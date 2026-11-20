# gpu31 pumpfix — 0004 shim pump/worker + run_job invocation, slot assignment

Date: 2026-11-20. Owner: G31_PumpFix. Phone up on int12; no flash by this ticket — Main flashes.

## 1. Silicon symptom (int12, one NULL-job submit)

```
p7580_sched: ioctl jc=0x2000000 req=0x0 bo=1 in=0 out=1
p7580_sched: push job_count=1
p7580_sched: queued seq=0 slot=1 submit_rc=0
p7580_sched: pump job_count=1
p7580_sched: retire seq=0 slot=0
```

Missing: `ST-RUN` (`run_job seq=...`), `ST-PMGET` (either arm), `ST-FENCE`
(`fence job_count=...`), and every `p7580_job:` line. Present: `ST-RETIRE`
with **slot 0** for a **slot-1** submit. `Image-int12` contains all probe
strings (verified), so this is not a stale build.

## 2. Root cause

Two defects, one signature:

**R1 — 0004 pump could retire without ever executing the job (shim side).**
The pump dequeued the head job and called `job->ops->run_job(job)`, but
nothing guarded the call: a `NULL` `job->ops` (header/ABI skew between the
`drm_sched.c` translation unit and the driver's `submit.c` — different
include paths to `drm_sched.h`, hand-maintained `struct drm_sched_job`) or
a `NULL`-returning `run_job` would oops or wedge instead of retiring
cleanly. A `NULL` return is not an `ERR_PTR`, so the old `IS_ERR` check
passed it to `dma_fence_add_callback(NULL, ...)`, whose `WARN_ON(!fence)`
fires and returns `-EINVAL` — landing in the retire path with no dispatch.

**R2 — run_job re-derived the slot from the entity pointer (driver side)
and bailed before its first probe.** `p7580_drv_run_job` did:

```c
df = container_of(ent, struct p7580_drv_file, entity[0]);
slot = (int)(ent - &df->entity[0]);
if (slot < 0 || slot >= P7580_NUM_JOB_SLOTS)
        return ERR_PTR(-EINVAL);
```

The `container_of(..., entity[0])` + index arithmetic assumes `ent` points
inside `p7580_drv_file.entity[]`. It does — but the subtraction is taken
against the *derived* `df`, so it is tautologically in range and always
yields a shifted index when the real container layout differs (the GEM
`p7580_file_priv` alias, the jobs `p7580_file_ctx` view, and the driver
`p7580_drv_file` disagree about what precedes `entity[]`). On silicon it
recomputed **slot 0 for a slot-1 entity**, and — worse — any layout where
the check misfires returns `ERR_PTR(-EINVAL)` **before the ST-RUN probe**,
which sits after the derivation. The 0004 F4 path then frees the job and
re-queues the pump: dmesg shows pump → retire with no run/pmget/fence and
a mutated slot. Exactly the int12 trace.

## 3. Fix

0004 shim (`code/gpu31/sched/src/drm_sched.{c,h}` → regen 0004 + 2 mirrors):

- `struct drm_sched_job` gains `int slot` (submit-chosen dispatch slot)
  and `int ran` (dispatch stamp; `credits` removed — unused everywhere).
- `drm_sched_job_alloc` zero-inits both via `kzalloc` (slot defaults 0).
- Pump: new `ST-OPS` probe prints `job->ops`, `job->ops->run_job`, and the
  carried `job->slot` (answers Main's Q1 in dmesg: if `run` is not
  `p7580_drv_run_job`, the struct layout/include path is wrong).
- Pump: `NULL`-ops/`NULL`-run_job retires through the F4 free path
  instead of faulting.
- Pump: slot clamped (`< 0` → 0), preserved across dispatch.
- Pump: `run_job` return checked with `IS_ERR_OR_NULL`; a `NULL` return
  becomes `ERR_PTR(-ENODATA)` and retires via the error path.
- Pump: `job->ran = 1` stamped after `run_job` returns.
- Finish: new `ST-FINISH` probe prints `ran` + `slot` (answers Main's Q2:
  `ran=0` = "retired without ever running", `ran=1` = "ran but its probe
  was skipped").

Driver (`code/gpu31/driver/0001-...-sources.patch`, `panfrost7580_submit.c`):

- SUBMIT stamps `sched_job->slot = slot` right after `drm_sched_job_alloc`
  + `drm_sched_job_arm`.
- `p7580_drv_run_job` trusts `sched_job->slot`; the entity-pointer math
  survives only as a range-checked fallback, and the `df` recovery is
  range-checked too (out-of-range entity → `df = NULL` → `-ENODEV`, not
  an OOB `container_of`).
- `timedout_job`/`free_job` get the same `df` range check.
- `ST-RUN` now prints `sslot=` (carried) + `owner=`; `ST-RETIRE` prints
  `sslot=` alongside `slot=` so a future skew is visible in one line.
- No other driver files touched (job.c/gem/mmu/power/syncobj identical).

Emu (`code/gpu31/sched/emu/include/linux/kernel-emu.h`): `IS_ERR_OR_NULL`
+ `ENODATA` (matches 3.10 `err.h`/`errno.h`, verified in the pristine tree).

## 4. Host tests

`T-pumpfix-run1` (new, in `sched-host-test.c`): 3 jobs, owner ids 11/12/13,
slots 0/1/2, submitted in order; pump each dispatch by hand (emulation
marks work pending; `in_flight` cleared manually between pumps).
Asserts `run_count == 3`, `run_log == [11,12,13]`, `free_count == 0`
before any fence signal (no early retire), slots preserved, then signal +
finish each fence and assert `free_log == [11,12,13]` with every retire
index `>=` its run index (retire-after-run ordering).

`T-pumpfix-nullrun`: `run_job == NULL` submits, pumps, asserts
`run_count == 0`, `free_count == 1` (clean F4 retire, no oops), slot kept.

`T-pumpfix-nullfence`: `run_job` returns `NULL`, asserts `run_count == 1`,
`free_count == 1` (error-path retire, no hang).

## 5. Verification (exercised)

- Host: `sched-host-test` native **252 checks, 0 failures**; aarch64/qemu
  **252 checks, 0 failures** (probes print 8 bounded lines, no assertion
  impact; was 217 before, +35 from the 3 new tests).
- `make gates` PASS; `make kcheck` PASS (4 files syntax-only + `-c` object
  compiles vs real 3.10.108 headers).
- `submit_fix_test` PASS, `job_test` PASS, `gem_validate` PASS
  (untouched scopes, no regressions).
- `submit.c` + `job.c` bodies `-fsyntax-only` and `submit.c -c` PASS vs
  real 3.10.108 headers + staged 0001–0004 shims (zero warnings).
- `git apply --check` clean: all 5 backport patches on a pristine scratch
  tree + driver 0001/0002 on top; regen 0004 re-checked after regen.
- Patch hygiene: hunk `+N` counts byte-audited (0004 `+1,572`/`+1,175`
  = 747 body lines; submit.c `+1,866`); trailing bytes preserved.
- Mirrors byte-identical: `sha256 7fd9500d…34fb1` for all three 0004
  copies; `code/gpu31/series/SHA256SUMS` 0004 line updated.
- NOT verified (Main/integrator own): Image build, flash, `/dev/dri`
  submit-test, egltest readback. No reboot/flash attempted. All build
  artefacts on the external disk; nothing written to `/` or `/tmp`
  (scratch trees under the external disk, deleted after; `/tmp/pfchk`
  removed).

## 6. Expected dmesg after flash (one NULL-job, slot 1)

```
p7580_sched: ioctl jc=0x… req=0x… bo=1 in=0 out=N
p7580_sched: push job_count=1
p7580_sched: queued seq=0 slot=1 submit_rc=0
p7580_sched: pump job_count=1
p7580_sched: ops ops=… run=… slot=1
p7580_sched: run_job seq=0 slot=1 sslot=1 jc=0x… owner=…
p7580_sched: pm_get HELD seq=0 slot=1
p7580_job: dispatch slot=1 jc=0x… cfg=… as=0 aff=… seq=0
p7580_job: irq slot=1 stat=… js_status=… (ACTIVE_IDLE) has_job=1
p7580_job: fence signal slot=1 seq=0 sig_rc=0
p7580_sched: fence job_count=0
p7580_sched: finish ran=1 slot=1
p7580_sched: retire seq=0 slot=1 sslot=1
```

Diagnosis rows for the new probes:

| Seen | Missing | Meaning |
|---|---|---|
| ST-PUMP, no ST-OPS | — | trace budget exhausted (8) or pre-pumpfix Image |
| ST-OPS `run=0` / `run != p7580_drv_run_job` | ST-RUN | header/ABI skew: job ops not the driver ops; check `drm_sched.h` include path |
| ST-FINISH `ran=0` + ST-RETIRE | ST-RUN/ST-PMGET/ST-FENCE | retire without ever running (dep race, NULL-ops F4 path, or error-path free) |
| ST-FINISH `ran=1` | `p7580_job: dispatch` | run_job returned but HW never programmed; `hw_submit`/PM scope |
| `sslot != slot` anywhere | — | slot derivation skew again; SUBMIT stamp vs entity math disagree |

## 7. Integrator notes

- `patches/drm-backport/0004` is authoritative; the two
  `code/gpu31/{series,sched/patches}` copies are mirrors.
- Apply order unchanged (backport 0001–0005 → KMS Mode-B → dmabuf 0001 →
  driver 0001 (this regen) + 0002 → fragments d0/d4).
- `code/gpu31/driver/0001` regen: `submit.c +1,842 → +1,866`; all other
  driver files byte-identical (only trailing-newline normalisation on the
  patch container).
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against §6; report which diagnosis row
  the node hits.
