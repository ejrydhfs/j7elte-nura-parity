# GPU31 perfcnt — OPTIONAL for first GL bring-up; DEFERRED with stub path

Date: 2026-11-19. Scope: v5.2 perfcnt paths, `code/gpu31/powermetric/`,
thermal max-lock. No device access, no flash, no shared-tree edits
(host-side decision only, like `gpu31-target.md`).

**Verdict: perfcnt is NOT needed for the first hardware GL bring-up.
Defer it. No new code in this slice — the probe stub already exists
(`pf7580_perfcnt_probe()` in the firmware slice) and every consumer that
matters (devfreq, thermal clamp, power proxy) runs on software accounting,
not on PRFCNT hardware counters.** This report is the deliverable, plus the
explicit deferral rationale below. No patch.

## 1. Why optional

1. **v5.2 has no perfcnt UAPI.** The locked target (`gpu31-target.md` §1–2)
   is 6 ioctls (`0x00–0x05`), `GET_PARAM` = `GPU_PROD_ID` only,
   `CREATE_BO.flags` reserved-zero. `PERFCNT_ENABLE`/`DUMP` appear only in
   v5.3+, gated on `unstable_ioctls`, and were explicitly rejected
   ("cost, no function"; adds `panfrost_perfcnt.c/.h` plus
   perfcnt-session/reset fault-recovery coupling). First GL therefore
   cannot — and need not — touch a perfcnt ioctl: there is none.
2. **Devfreq does not read counters.** `pf7580_dvfs_get_status()`
   (`code/gpu31/devfreq/pf7580_devfreq.c:133-149`) reports
   `busy_ns`/`idle_ns` accumulators fed by `pf7580_dvfs_mark_busy()`
   (`:161-170`), which the jobs slice calls on job retire / idle gaps
   (`pf7580_devfreq.h:185-190`). No PRFCNT register is read on the DVFS
   path — the "minimal counter read used by devfreq" is the software
   busy/idle accumulator, already implemented. Nothing further required.
3. **Thermal max-lock does not read counters.** The thermal slices close
   the loop as cooling-device `cur_state` → graded TMU event → existing
   `gpu_tmu_hot_check_and_work()` (`0001-gpu-cdev-state-to-tmu-maxlock.patch`)
   → `pf7580_dvfs_set_max()` OPP-ceiling clamp
   (`pf7580_devfreq.c:274-331`). It is an OPP max-frequency clamp, not a
   counter sample. The TMU-table fix (`0002-...patch`) is likewise about
   clamp values vs the live ASV table, not counters.
4. **Load tracking is proven without HW counters.** The powermetric slice
   (`gpu31-powermetric.md` §2–3) shows the governor, rail, `power_state`,
   and `time_in_state` all track a 362 k-job SOFT_JOB burst correctly
   (util 0→57, TIS +3 466 ticks on step 266, rail 812 500 `enabled`) using
   only sysfs/governor accounting. `time_in_state` + `runtime_active_time`
   are the proxy to watch — PRFCNT adds nothing to bring-up pass/fail.

## 2. Stub path (exists; sampling explicitly deferred)

`pf7580_perfcnt_probe()` (`code/gpu31/firmware/panfrost7580_fw.{c,h}`)
reproduces the kbase enable preamble
(`mali_kbase_instr_backend.c:145-181`) without submitting work:

```
PRFCNT_CONFIG <- MODE_OFF, BASE_LO/HI <- 0,
JM/SHADER/TILER/MMU_L2_EN <- per-AS masks,
PRFCNT_CONFIG <- MODE_MANUAL; -EBUSY while GPU_STATUS bit 2 set
```

Register evidence (M0 model + firmware header, all host-verifiable):

| Item | Value | Source |
|---|---|---|
| `GPU_STATUS` | `0x034`, bit 2 = `PRFCNT_ACTIVE` | `mali_hw.h:46`; `mali_midg_regmap.h` GPU_STATUS |
| `PRFCNT_BASE_LO/HI/CONFIG` | `0x060`/`0x064`/`0x068` | `panfrost7580_fw.h:57-59` |
| `PRFCNT_JM/SHADER/TILER/MMU_L2_EN` | `0x06C`/`0x070`/`0x074`/`0x07C` | `panfrost7580_fw.h:60-63` |
| `MODE_OFF` / `MODE_MANUAL` | `0` / `1`, AS in `CONFIG[7:4]` | `panfrost7580_fw.h:65-66`; `fw.c:91,110` |
| `PERFCNT_DONE` IRQ | bit 16 | `mali_hw.h:60` |
| Sampling commands + IRQ-16 handler | **not in M0 model** — deferred | see §3 |

## 3. What deferral means (revisit criteria, not a TODO)

Deferred to a post-bring-up debug/profiling slice, next to the sampling
path, per the in-code note (`panfrost7580_fw.c:102-107,113`):

- `PRFCNT_CLEAR`/`SAMPLE` via `GPU_CMD` + IRQ-16 handler wiring
  (`mali_hw.h` today carries only `SOFT/HARD_RESET` + cache-clean commands).
- The `BASE_HW_ISSUE_8186` tiler-enable quirk (kbase writes `TILER_EN = 0`
  before `MODE_MANUAL`, re-enables after) — probe passes the mask through
  verbatim; the quirk belongs to the sampling owner.
- Any ioctl surface for the above — which by definition postdates v5.2
  (v5.3+ `PERFCNT_*`, rejected by the target decision).

Revisit only when a consumer needs hardware block counters (Mesa HUD,
perf-driven governor tuning, shader/tiler bottleneck analysis) — never as
a bring-up gate. First-`SUBMIT` acceptance stays: render-node probe,
GEM-on-CMA, MMU fault test, fence/resv/syncobj, scheduler + `SUBMIT`,
DVFS + `panfrost_dri.so` headless render (`gpu31-target.md` §5 B0–B6).

## 4. Files

- This report: `docs/reports/gpu31-perfcnt.md` (new, this slice only).
- No code changes: probe stub `code/gpu31/firmware/panfrost7580_fw.{c,h}`
  (firmware slice), consumers `code/gpu31/devfreq/`, `code/gpu31/thermal/`,
  `code/gpu31/powermetric/` (own slices, untouched).
