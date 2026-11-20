# gpu31 schedtrace — bounded SUBMIT→dispatch→retire probes (ST-*)

Date: 2026-11-20. Owner: G31_SchedTrace. Phone up on int11 (#37,
renderD128, X healthy per Main); no flash by this ticket — Main flashes.
Ticket: driver `submit.c` (ioctl queue + `run_job` entry + `pm_get`
result) + 0004 sched shim (entity push, worker dequeue, job
submit/retire).

Problem (int11 NULL-job): SUBMIT returns rc=0, the out-sync wait
returns signalled immediately, but **no `p7580_job:` trace lines**
appear even though `p7580_job_trace` defaults to 8 and prints inside
`p7580_job_hw_submit` (dispatch/IRQ/fence). Mesa offscreen renders
"2700 FPS" with all-zero readback. Conclusion: `run_job` is never
reached, or `p7580_drv_pm_get()` fails closed before `hw_submit`.

## 1. Probe points (8 sites, 2 budgets, first 8 events each)

`schedtrace` adds one `pr_info` per site, each guarded by a plain
`static unsigned int` budget defaulting to 8 (0 = off), same pattern
as the jobtrace `p7580_job_trace`/`jd->trace_left` in
`jobs/panfrost7580_job.c` (plain static, NOT `module_param_named`:
the integrator's object rule defines `KBUILD_MODNAME` at Image link
time, but slice-level `-fsyntax-only` checks compile the files
standalone). Racy decrement under concurrent ioctls overshoots by at
most a line or two; harmless. Prefix `p7580_sched:` distinguishes
these from the `p7580_job:` hw_submit traces.

Driver `panfrost7580_submit.c` (`p7580_sched_trace` budget, 5 sites):

| ID | Site | Message | Reads as |
|---|---|---|---|
| ST-IOCTL | `p7580_drv_submit` entry, after envelope validation | `p7580_sched: ioctl jc=… req=… bo=%u in=%u out=%u` | ioctl reached the driver |
| ST-QUEUE | after `drm_sched_job_submit` returns | `p7580_sched: queued seq=%u slot=%d submit_rc=%d` | 0004 accepted (0) or refused |
| ST-RUN | `p7580_drv_run_job` entry, BEFORE `pm_get` | `p7580_sched: run_job seq=%u slot=%d jc=…` | async worker entered |
| ST-PMGET | `pm_get` return value (both arms) | `p7580_sched: pm_get HELD/FAIL seq=%u slot=%d` | re-get verdict |
| ST-RETIRE | `p7580_drv_free_job`, before `p7580_job_free` | `p7580_sched: retire seq=%u slot=%d` | full chain completed |

0004 `drm_sched.c` (`p7580_sched_trace` budget, 3 sites):

| ID | Site | Message | Reads as |
|---|---|---|---|
| ST-PUSH | `drm_sched_entity_push_job`, after queue + before kick | `p7580_sched: push job_count=%d` | FIFO queued, pump kicked |
| ST-PUMP | `drm_sched_pump`, after dequeue (dep met or dep-less) | `p7580_sched: pump job_count=%d` | worker dequeued, `run_job` next |
| ST-FENCE | `drm_sched_job_finish`, after signal, before `free_job` | `p7580_sched: fence job_count=%d` | fence signalled, retiring |

Plus one `#include <linux/printk.h>` in `drm_sched.c` (explicit;
in-tree it was previously only transitive) and one host-only
`emu/include/linux/printk.h` redirect (`pr_info` → `printf`) so the
sched host test compiles the same body.

## 2. Expected dmesg sequences

Healthy NULL-job (first SUBMIT, slot 0):

```
p7580_sched: ioctl jc=… req=0x… bo=0 in=0 out=N
p7580_sched: push job_count=1
p7580_sched: queued seq=0 slot=0 submit_rc=0
p7580_sched: pump job_count=1
p7580_sched: run_job seq=0 slot=0 jc=…
p7580_sched: pm_get HELD seq=0 slot=0
p7580_job: dispatch slot=0 jc=… cfg=… as=0 aff=… seq=0
p7580_job: irq slot=0 stat=… js_status=… (ACTIVE_IDLE) has_job=1
p7580_job: fence signal slot=0 seq=0 sig_rc=0
p7580_sched: fence job_count=0
p7580_sched: retire seq=0 slot=0
```

(Note: `p7580_job:` dispatch/irq/fence lines come from the jobtrace
1244082 budget; `p7580_sched: fence` and `retire` both fire once per
job — fence first (finish path), retire second (free_job).)

Diagnosis table (the int11 question):

| Seen | Missing | Meaning |
|---|---|---|
| ST-IOCTL, ST-PUSH, ST-QUEUE | ST-PUMP and later | 0004 pump never dispatched: dep gate (`job->dep_fence` unsignalled), `entity->stopped`, `sched->ready == false`, or `in_flight` stuck |
| ST-PUMP | ST-RUN | `run_job` op not reached after dequeue (shim/backend wiring) |
| ST-RUN + ST-PMGET FAIL | ST-RETIRE still prints (F4 path frees) | dark block: `pm_get` fails closed before `hw_submit` — no `p7580_job:` dispatch, fence retires via ERR_PTR free path |
| ST-RUN + ST-PMGET HELD | `p7580_job: dispatch` | `p7580_job_run`/`hw_submit` not reached or silent (next ticket's scope) |
| all ST-* + all `p7580_job:` | non-zero readback still absent | dispatch+retire healthy; coherency/out-sync scope (jobtrace2 F-JT1/F-JT2 already active) |

## 3. Files changed (this ticket)

- `code/gpu31/sched/src/drm_sched.c` (+31: 3 probes + budget + printk include)
- `code/gpu31/sched/emu/include/linux/printk.h` (new, host-only)
- `patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch` (regen, `+1,499` → `+1,530`)
- `code/gpu31/sched/patches/0004-drm_sched-panfrost-job-only-shim.patch` (mirror)
- `code/gpu31/series/0004-drm_sched-panfrost-job-only-shim.patch` (mirror) + `SHA256SUMS` 0004 line
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (regen, submit.c `+1,788` → `+1,842`)
- this report

Mirrors are byte-identical (sha256 `eb9a8f5d…2825` for all three 0004 copies).

## 4. Verification (exercised)

- `git apply --check` clean: all 5 backport patches individually +
  full series + driver 0001 + driver 0002 (private scratch clone of
  the pristine tree; the KMS/dmabuf patches check against the fuller
  tree at integration as before — unrelated to this ticket).
- Kernel syntax (`aarch64-linux-gnu-gcc -fsyntax-only -std=gnu89
  -Wall` vs real 3.10.108 headers + staged 0001–0004 shims):
  `panfrost7580_submit.c`, `panfrost7580_job.c`,
  `panfrost7580_gem.c`, `dma-fence.c`, `dma-resv.c`,
  `drm_syncobj.c`, `drm_sched.c` — all RC=0, zero warnings.
- Real `-c` object compiles: `dma-fence.o`, `dma-resv.o`,
  `drm_syncobj.o`, `drm_sched.o`, `panfrost7580_submit.o` — all RC=0.
- Host: `sched-host-test` native 217 checks PASS + aarch64/qemu 217
  PASS (probes print 8 bounded lines to stdout, no assertion
  impact); `make gates` PASS; `make kcheck` PASS;
  `driver/submit_fix_test` PASS (unchanged scope).
- Patch hygiene: hunk counts byte-audited (submit.c `+1,842` = 842
  `+` body lines; `drm_sched.c` `+1,530`); trailing bytes
  (`-- \n2.56.0\n`) preserved on the 0004 regen.
- NOT verified (Main/integrator own): Image build, flash,
  `/dev/dri` submit-test, egltest readback. No reboot/flash
  attempted. All build artefacts on the external disk; nothing
  written to `/` or `/tmp` (scratch trees under `/tmp` are
  check-only clones, deleted after; the one 5.7 GB `/tmp` load is
  pre-existing tmpfs, not this ticket).

## 5. Integrator notes

- Apply order unchanged (backport 0001–0005 → KMS Mode-B → dmabuf
  0001 → driver 0001 (this regen) + 0002 → fragments d0/d4).
- `patches/drm-backport/0004` is authoritative; the two
  `code/gpu31/{series,sched/patches}` copies are mirrors.
- `code/gpu31/series/SHA256SUMS` 0004 line updated to the regen hash.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against §2; report which row of
  the diagnosis table the node hits.
