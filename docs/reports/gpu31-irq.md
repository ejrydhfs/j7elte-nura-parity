# gpu31 IRQ — slot routing, mask/clear completion path, resume re-arm

Date: 2026-11-20. Ticket: `p7580_job_hw_submit` (JS registers written),
IRQ handler (STAT/RAWSTAT clear + fence), IRQ registration/masking,
slot selection. Symptom (Main, int15 silicon): NULL job dispatches on
slot 1 (`run_job seq=0 slot=1 sslot=1 jc=0x2000000`,
`pm_get HELD`, `p7580_job: dispatch slot=1 jc=0x2000000
cfg=0x00083700 as=0 aff=0x3 seq=0`), then no `p7580_job: irq` line,
`panfrost7580-job` IRQ count stays 0 (`panfrost7580-gpu` fired once),
and the fence retires on the 5 s timeout instead of by interrupt.

## 1. Register table (gpu31-jobabi §4/§7 vs what the driver writes)

| Register | Offset | Driver write | Reference | Verdict |
|---|---|---|---|---|
| `JOB_INT_RAWSTAT` | `0x1000` | read in irq-enter + post-start + loop | `[KT]` regmap `:203` raw latch | **added reads** (were missing) |
| `JOB_INT_CLEAR` | `0x1004` | `mask` at init/rearm/recover; per-slot `m` in IRQ | `[KT]` `:204` WO clear; `[V5]` `job_write(JOB_INT_CLEAR, mask)` per slot | match (now also cleared via rearm helper) |
| `JOB_INT_MASK` | `0x1008` | `mask & (js_present \| js_present<<16)` at init/rearm | `[KT]` `:205`; kbase `pm_enable_interrupts` writes `0xFFFFFFFF`; `[V5]` `MK_JS_MASK` union | **kept present-gated** (deliberate: kbase unmasks all 16 slots, this port has 3; unmasked STAT ⊆ RAWSTAT either way) |
| `JOB_INT_STAT` | `0x100C` | read as `status` in IRQ demux | `[KT]` `:206`; `[V5]` handler reads STAT | match |
| `JS_HEAD_NEXT_LO/HI` | `slot+0x40/0x44` | `jc` lo/hi | jobabi §4 `0x1840/0x1844` slot 0 | match |
| `JS_AFFINITY_NEXT_LO/HI` | `slot+0x50/0x54` | full `shader_present` | jobabi §4 | match |
| `JS_CONFIG_NEXT` | `slot+0x58` | `p7580_js_config(as_nr)` = `0x3700\|as` → `0x83700` | jobabi §6: START/END CLEAN_INV + START_MMU(10649) + PRI(8), no FLUSH_REDUCTION/JOBCHAIN/PROTECTED | match (`cfg=0x00083700` trace = `as=0` + `0x83700`) |
| `JS_COMMAND_NEXT` | `slot+0x60` | `START 0x01` after `wmb()` | `[KT]` `JS_COMMAND_START`; `[KB]` GO after cfg | match (**added `wmb()`** — HEAD/AFF/CFG must be visible before START latches on this weakly-ordered SoC) |
| `JS_STATUS` | `slot+0x24` | read in IRQ per-slot + post-start dump | jobabi §7 fault table | match (**added post-start + irq-err naming**) |
| `JS_FLUSH_ID_NEXT` | `slot+0x70` | not programmed | no FLUSH_REDUCTION on T720 | match |
| `JOB_IRQ_THROTTLE` | `0x1014` | untouched (0) | kbase writes throttle cycles per IRQ; 0 = deliver immediately | match (no coalescing) |
| `JOB_IRQ_JS_STATE` | `0x1010` | not read | kbase race-check register (jm_hw.c race comment) | **not ported** (single job per slot here; RAWSTAT tail re-check covers the missed-failure race instead) |

Completion condition (jobabi §4+§7): the dispatched job completes when
the handler observes `JOB_INT_STAT & MK_JS_MASK(slot)` with the DONE
bit set, clears the slot bit(s) via `JOB_INT_CLEAR`, and
`dma_fence_signal()`s the in-flight fence. The 5 s `timedout_job`
exists only for the no-IRQ case. Polarity verified against `[KT]`
regmap `:199-208` + `[V5]` `/tmp/pf52/panfrost_regs.h:191-200`.

## 2. What was actually wrong (3 defects, all fixed)

1. **Wrong slot for requirement-less jobs (the int15 signature).**
   `p7580_job_slot(0)` returned JS1 (vertex/tiler) while the NULL-job
   selftest (`requirements == 0`, `jc=0x2000000`) and the schedtrace
   expectation (`slot=0`) assume JS0. Mainline
   `panfrost_job_get_slot` (`/tmp/pf52/panfrost_job.c:102-122`) routes
   FS→JS0/else→JS1, but the requirement-less flush/NULL path belongs
   on JS0 on this 3-slot part. Fix: `requirements == 0 → JS0`
   (`jm_hw.h` + mirror in `0001`); nonzero non-FS still → JS1,
   `ONLY_COMPUTE` → JS2. Host test vector updated
   (`p7580_job_slot(0) == 0`).
2. **MASK-gated STAT could hide a completed slot.** The handler demuxed
   `STAT` only; if MASK ever excluded a finished slot, RAWSTAT latched
   but no IRQ demux ran and the job timed out. Fix: demux on
   `STAT | RAWSTAT` (all three branches), mirroring kbase's RAWSTAT
   tail re-check (`mali_kbase_jm_hw.c:442-455`); `CLEAR` drains both
   views. Added unconditional `irq-enter rawstat=… stat=…` probe so
   mask-wrong (RAWSTAT∖STAT) vs never-executed (both zero) is decidable
   from dmesg.
3. **Reset/resume lost AS0 + slot unmask.** `p7580_job_recover` reset
   the GPU then unmasked slots without rebinding AS0 (stale comment
   said the integrator wires it between reset and re-arm — nobody
   did), and `p7580_drv_runtime_resume` never reprogrammed AS0 nor
   re-armed slots after a power-off resume (mainline
   `panfrost_device_resume`: soft_reset → power_on → mmu_enable(0) →
   job_enable_interrupts; kbase `pm_clock_on`: MMU first, JOB unmask
   last). Any post-reset/post-resume SUBMIT fetched its descriptor
   through dead tables and faulted or hung silently. Fix: recover now
   calls `p7580_mmu_program_as(regs, 0, jd->pgd_dma, KBASE_LO/HI)`
   then a shared `p7580_job_dev_rearm()` (CLEAR + present-gated MASK);
   resume does the same `program_as + rearm` after `pf7580_runtime_
   resume`. `pgd_dma` is stashed on the job device at probe
   (`dev_init` gains a `pgd_dma` param; host header uses a
   `p7580_dma_addr_t` u64 alias so the pure-C test still builds).

Also added (bounded, numeric): `irq-arm rawstat/stat/mask/js_present`
read-back at init (proves CLEAR drained before the first START),
`post-start rawstat/js_status(name)` dump right after START (proves
latch vs synchronous fault even with no IRQ), and the `wmb()` before
START. Unconditional `irq-enter` is one extra line per IRQ; the
dispatch/IRQ/fence prints stay under the existing jobtrace budget.

## 3. Files changed (this ticket)

- `code/gpu31/jobs/panfrost7580_jm_hw.h` — zero-req → JS0 + comment.
- `code/gpu31/jobs/panfrost7580_job.h` — `pgd_dma` field, `dev_init`
  `pgd_dma` param (host alias), `p7580_job_dev_rearm` decl.
- `code/gpu31/jobs/panfrost7580_job.c` — `mmu.h` include, irq-arm
  read-back, `wmb()` + post-start dump, STAT|RAWSTAT demux +
  irq-enter probe, `rearm()` helper, recover AS-rebind + helper use.
- `code/gpu31/jobs/job_test.c` — slot-0 vector for `requirements==0`.
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` — regen
  (ordered splice of the 4 touched files; untouched 15 files
  byte-identical): `jm_hw.h` slot route, `job.c`/`job.h` as above,
  `main.c` resume `program_as + rearm` + probe `pgd_dma` stash.
- this report.

## 4. Verification (exercised)

- Host: `job_test` native **all checks passed** (incl. updated
  `p7580_job_slot(0)==0`); aarch64 cross-build OK.
- Fault matrix (`code/gpu31/fault`): **12/12 PASS**.
- Sched gates (`code/gpu31/sched`): **all PASS**.
- Kernel syntax vs real 3.10.108 headers + staged 0001–0004 shims:
  `panfrost7580_job/submit/mmu/gem/ioctl/syncobj/power/devfreq`
  **8/8 `-fsyntax-only` PASS** (`main.c` needs the Mode-B tree for
  `DRIVER_RENDER_ONLY`; checked with stub define); `job` + `submit`
  **` -c` objects PASS** (20 712 / 26 696 B).
- NOT verified (Main/integrator own): Image build, flash, on-device
  submit-test. No flash attempted; all build artefacts on the external
  disk (`.../j7k/gpu31/tmp-irq/`).

## 5. Expected dmesg after flash (one NULL job, now slot 0)

```
p7580_job: irq-arm rawstat=0x00000000 stat=0x00000000 mask=0x00070007 js_present=0x7
p7580_sched: ioctl jc=0x2000000 req=0x0 bo=1 in=0 out=1
p7580_sched: push job_count=1
p7580_sched: queued seq=0 slot=0 submit_rc=0
p7580_sched: pump job_count=1
p7580_sched: run_job seq=0 slot=0 sslot=0 jc=0x2000000 owner=…
p7580_sched: pm_get HELD seq=0 slot=0
p7580_job: dispatch slot=0 jc=0x2000000 cfg=0x00083700 as=0 aff=0x3 seq=0
p7580_job: post-start slot=0 rawstat=0x00000000 js_status=0x00000000 (NO_EXCEPTION)
p7580_job: irq-enter rawstat=0x00000001 stat=0x00000001
p7580_job: irq slot=0 stat=0x00000001 js_status=0x00000000 (NO_EXCEPTION) has_job=1
p7580_job: fence signal slot=0 seq=0 sig_rc=0
p7580_sched: fence job_count=0
p7580_sched: retire seq=0 slot=0 sslot=0 ran=1 owner=…
```

Diagnosis rows: `post-start js_status>=0x40` = synchronous fault (bad
`jc`/tables, check `jc_validate` + AS program); `irq-enter` with
`rawstat & MK != stat & MK` = mask excluded a finished slot (now
still demuxed via RAWSTAT); `irq-enter` absent after `post-start` =
slot never executed (power/START lost — check `pm_get` + resume
re-arm); `fence signal … sig_rc=0` within 1 s = IRQ-completed
(healthy — the old 5 s timeout line should be gone).
