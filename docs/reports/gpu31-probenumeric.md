# gpu31 probenumeric — numeric ST-OPS/ST-RUN/ST-RETIRE + stamped dispatch context

Date: 2026-11-20. Owner: G31_ProbeNumeric. Phone up on int14; no flash by this ticket — Main flashes.

## 1. Why (int14 evidence)

- `/proc/sys/kernel/kptr_restrict = 4` on the device: every `%p` in the
  ST-OPS probe (`job/opsoff/ops/run=0`) was **censored, not NULL** — the
  zeros proved nothing.
- Runtime lift of kptr_restrict gave the decisive line:
  `ops job=ffffffc04ebb9000 opsoff=...18 ops=ffffffc000b4bff0
  run=ffffffc0005780c0 slot=1`, with kallsyms confirming
  `run == p7580_drv_run_job` and `ops == &p7580_drv_sched_ops`.
  The pump calls **our** run_job, yet ST-RUN never prints and free_job
  retires immediately.
- ST-RUN sat *after* the `slot`/`df`/`drv` derivation, so the function
  was returning before it — most likely `drv == NULL` because
  `container_of(ent, struct p7580_drv_file, entity[0])` is wrong for
  this entity. This ticket moves the proof to entry **and** removes the
  derivation as the primary path.

## 2. Change

Shim (`code/gpu31/sched/src/drm_sched.{c,h}` → regen 0004 + 2 mirrors):

- ST-OPS prints `job/opsoff/ops/run/runexp/owner/slot` as
  `(unsigned long)` + `%lu` (works censored or not; `%px` does not
  exist on 3.10). `runexp=` is the new `p7580_drv_run_job_addr()`
  accessor: weak default (`NULL`, keeps shim-only/emu builds linking)
  in drm_sched.c, strong definition in submit.c — no symbol lookup
  needed, `run == runexp` is the in-dmesg check.
- ST-FINISH gains `owner=%lu` (links retire back to the ST-RUN line).

Driver (`code/gpu31/driver/0001-...-sources.patch`, submit.c + job.h):

- `struct p7580_submit_job` gains `file_priv`/`drv_priv` (stamped at
  SUBMIT next to `slot`) + `run_seq` (monotonic run_job entry count).
- `p7580_drv_run_job` rewritten: ST-RUN-entry print is the **first
  statement** (raw `sched_job/ent/owner/ops/runexp`, numeric), then a
  `run_seq` stamp, then stamped `df`/`drv` — entity `container_of` math
  survives only as a range-checked fallback, with a `run_fallback`
  print when it is used. Every early return has its own bail print
  (`run_bail NULLJOB/BADSLOT/BADENT/NODRV`) carrying the errno cause.
- `timedout_job`/`free_job` read the stamped `df`/`drv` (no entity
  math). ST-RETIRE prints `ran=N owner=N` (`ran=` is the entry stamp:
  call order provable even if a print is lost).

No other driver files touched (regen keeps every other hunk byte-identical;
`+1,897 → +1,976` submit.c, `+1,155 → +1,168` job.h).

## 3. Verification (exercised)

- Host: `sched-host-test` native **263 checks, 0 failures**;
  aarch64/qemu **263 checks, 0 failures** (numeric ST-OPS confirmed in
  stdout: `ops job=… opsoff=… ops=… run=… runexp=0 …` — `runexp=0` is
  the weak default, expected without the backend TU).
- `make gates` PASS.
- Applied-tree `-fsyntax-only` + `-c` clean vs real 3.10.108 headers:
  `drm_sched.c`, `panfrost7580_submit.c`, `panfrost7580_job.c`
  (`gem.c` only pre-existing pagemap warning).
- Scratch pristine tree: `git apply --check` clean for all 5 backport
  patches + both driver patches; applied tree contains the new strings
  (`run_bail`, `runexp`, `file_priv`).
- Patch hygiene: mirrors byte-identical
  (`sha256 1e1c7a58…`); `code/gpu31/series/SHA256SUMS` 0004 line updated.
- NOT verified (Main/integrator own): Image build, flash, `/dev/dri`
  submit-test, egltest readback. No reboot/flash attempted.

## 4. Expected dmesg after flash (one NULL-job, slot 1)

```
p7580_sched: pump job_count=1
p7580_sched: ops job=<N> opsoff=<N+24> ops=<ops> run=<run> runexp=<run> owner=<job> slot=1
p7580_sched: run job=<N> ent=<N> owner=<job> ops=<ops> runexp=<run>
p7580_sched: run_job seq=0 slot=1 sslot=1 jc=0x… owner=<N>
p7580_sched: pm_get HELD seq=0 slot=1
p7580_job: dispatch slot=1 jc=0x… …
p7580_job: irq … / fence signal …
p7580_sched: fence job_count=0
p7580_sched: finish ran=1 slot=1 owner=<N>
p7580_sched: retire seq=0 slot=1 sslot=1 ran=1 owner=<N>
```

Diagnosis rows:

| Seen | Meaning |
|---|---|
| `run != runexp` | pump dispatched through the wrong ops table; stale `.o`/duplicate shim |
| `run` entry present, then `run_bail BADSLOT/BADENT/NODRV` | the named guard rejected the job; `df`/`drv` stamp vs entity mismatch |
| `run` entry, no `run_job`, `retire … ran=1` | PM re-get failed (`pm_get FAIL`) or `p7580_job_run` returned ERR_PTR |
| `finish ran=0` + `retire ran=0` | retired without ever running (NULL-ops F4 or error-path free) |
| `opsoff - job != 24` | TU reading a different struct than the alloc wrote |

## 5. Integrator notes

- `patches/drm-backport/0004` authoritative; the two
  `code/gpu31/{series,sched/patches}` copies are mirrors.
- Apply order unchanged (backport 0001–0005 → KMS Mode-B → dmabuf 0001 →
  driver 0001 + 0002 → fragments d0/d4).
- `build-int14.sh` gate strings referencing the old
  `ops job=%p opsoff=%p` format need a refresh to the numeric format
  (and the `p7580_sched:` count 11 grows with the new bail prints) —
  left to the integrator bump; flagged, not silently patched.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against §4.
