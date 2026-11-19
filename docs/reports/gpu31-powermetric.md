# GPU31 powermetric — idle-vs-load power proxy (DVFS validation, stuck-rail check)

Date: 2026-11-19. Phone: SM-J700M (j7elte), Nura Alpine aarch64,
kernel `3.10.108 #35 SMP PREEMPT` (phone UP, SSH/RNDIS working).
Driver: kbase `t7xx/r15p0` (UK 10.6, DDK `K:r15p0-00rel0(GPL)`),
Interactive governor, `polling_speed` 30 ms.
Only Integrator builds and only M1Verify flashes (after explicit go);
this slice did neither, never rebooted, never wrote the shared tree.

**Verdict: DVFS + rail + power-domain all track the GPU correctly.**
Idle parks at the 266 MHz OPP (rail 812 500 uV, `disabled`, GPU
`suspended`); sustained kbase load powers the block (`active`,
`power_state=1`), holds 266 MHz at ~57–58% utilization, keeps the rail
at 812 500 uV `enabled`, and suspends ~1 s after the jobs stop.
Forced-668 proves the rail follows an OPP change (812 500 → 1 100 000 uV
`enabled`, back on restore). `time_in_state` accumulates only on the
step the GPU actually ran. No stuck rail, no stuck clock, no stuck
power-domain. Battery/fuel-gauge current is too noisy at this charge
state to resolve the ~150 mW G3D delta — the rail + `runtime_active_time`
+ `time_in_state` trio is the proxy to watch, not the battery nodes.

## 1. Method (reproducible)

Sampler + load generator live in `code/gpu31/powermetric/`:

- `gpu-powermetric.sh` — read-only sampler (mali clock/vol/util/perf/
  power_state/runtime_status/active/suspended times/time_in_state/dvfs,
  vdd_g3d node+uV+state+users+limits, battery current/voltage/status/
  capacity/temp, fg current, gpufreq cooling, thermal zones).
- `gpu-job-load.c` — sustained `SOFT_JOB` submitter (dump-time atoms,
  same pattern as the `kbase-selftest` bring-up path). Host-built with
  `aarch64-linux-gnu-gcc -static`, uploaded over ssh-stdin to
  `/home/user/gpu-job-stress` (user-writable; `/data` is root-only),
  run as root (`/dev/mali0` is `crw------- root`).
- All device commands under short `flock /tmp/gpsdev.lock` holds;
  procedure per sample: read mali nodes → regulator node → battery/fg →
  thermal. `time_in_state` unit = jiffies @ HZ=100 → 10 ms/tick
  (`gpu_dvfs_api.c:476-498`, defconfig `CONFIG_HZ=100`).

DVFS ground truth (shared tree, read-only): nominal OPPs
`gpu_exynos7580.c:46-55` (800/734/668 @ 1.10 V; 534/440/350 @ 1.00 V;
266/160 @ 0.90 V); live `asv_table` on #35 is ASV-trimmed to
668→1 100 000, 534→1 000 000, 440→937 500, 350→881 250, 266→812 500 uV
(160 MHz step compiled out → 5 live steps `668 534 440 350 266`).
Thresholds min 78 / max 85 (top 99/100, bottom 10/20), highspeed
420 MHz @ load 95 delay 0. Soft-job load is far below the 78% up-ramp,
so the governor is *expected* to hold the floor (see §3).

## 2. Numbers

### 2.1 Idle baseline (X + fbdev running, no GL client)

3 samples, 3 s apart (06:28:09–06:28:15Z), all identical:

| Proxy | Idle value |
|---|---|
| `mali/clock` | 0 MHz (`power_state=0` multiplies the readout to 0) |
| `mali/vol` | 812 500 uV (last-programmed OPP voltage, still readable) |
| `mali/utilization` / `perf` | 0 / 0 |
| `mali/time_in_state` | `266:6 350:0 440:0 534:0 668:0` (6 ticks = 60 ms since boot window) |
| `mali/power/runtime_status` | `suspended` (`runtime_active_time` 80 ms vs `runtime_suspended_time` ~140–146 s and growing) |
| `vdd_g3d` (regulator.4) | 812 500 uV, `disabled`, `num_users=0`, limits 600 000–1 300 000 |
| `battery/current_now` / `current_avg` | −195 / −194 mA (discharging; mA by `sm5703_get_curr`) |
| `battery/voltage_now` | 4 292 000 uV, status `Full`, capacity 100, temp 33.0 °C |
| `sm5703-fuelgauge CURRENT_NOW/AVG` | −164…−214 mA (tracks `battery/`, ±50 mA jitter) |
| `thermal-gpufreq-0` | 0/5; tz0 48 000 mC, tz1 46 000 mC |

### 2.2 Sustained load: 30 s SOFT_JOB burst (362 100 jobs, 0 fails)

Sampler every 5 s during the burst, plus 15 s settle:

| t | clock | vol | util | power/runtime | time_in_state (ticks) | vdd_g3d | battery now/avg/volt | fg now |
|---|---|---|---|---|---|---|---|---|
| T0 idle | 0 | 812 500 | 0 | 0/suspended | 266:6 | 812 500 disabled | −195/−194/4 292 000 | −164 |
| +5 s | 266 | 812 500 | 57 | 1/active | 266:1052 | 812 500 enabled | +50/+50/4 375 000 | +31 |
| +10 s | 266 | 812 500 | 57 | 1/active | 266:1558 | 812 500 enabled | +50/+50/4 375 000 | −23 |
| +15 s | 266 | 812 500 | 57 | 1/active | 266:2063 | 812 500 enabled | +23/+22/4 351 000 | +31 |
| +20 s | 266 | 812 500 | 57 | 1/active | 266:2569 | 812 500 enabled | +23/+22/4 351 000 | 0 |
| +25 s | 266 | 812 500 | 57 | 1/active | 266:3072 | 812 500 enabled | +46/+46/4 359 000 | −62 |
| +30 s (jobs done) | 0 | 812 500 | 0 | 0/suspended | 266:3472 | 812 500 disabled | +46/+46/4 359 000 | +66 |
| +45 s settle | 0 | 812 500 | 0 | 0/suspended | 266:3472 (frozen) | 812 500 disabled | −27/−26/4 359 000 | — |

Throughput: 362 100 soft jobs / 30 s ≈ 12 070 jobs/s. `runtime_active_time`
34 730 ms vs `runtime_suspended_time` 160 920 ms at settle. TIS delta over
the 25 s active window: 3072−6+~400 ≈ 3 466 ticks ≈ 34.7 s wall
(jiffy granularity + the final flush read) — all on step 266.

Shorter probes agree: a 6 s burst (64 120 jobs) left `util=21`,
`clock=266`, `power_state=1/active`, TIS `266:533`; a 15 s burst with
`dvfs_max_lock=266` held 266 @ util 58; unlocked re-burst held 266 @ 57.
Lock/unlock wrote only the lock node (released afterwards, `-1`/clear).

### 2.3 Forced OPP: rail follows the clock (stuck-rail negative test)

Vendor `clock` node forces `always_on` + DVFS off (`set_clock`,
`gpu_custom_interface.c`); restored with `echo 0`:

| Phase | clock | vol | power/runtime | vdd_g3d | TIS |
|---|---|---|---|---|---|
| pre (suspended) | 0 | 812 500 | 0/suspended | 812 500 disabled | 266:6 |
| `echo 668` + 3 s | 668 | 1 100 000 | 1/active | 1 100 000 enabled | — |
| 8 s load at 668 (183 900 jobs OK) | 668 | 1 100 000 | 1/active | 1 100 000 enabled | 668:1107 |
| `echo 0` restore + 12 s | 0 | 1 000 000* | 0/suspended | 1 000 000* disabled | 266:6; 534:1; 668:1111 |

\* `vol` reads back the last-programmed setpoint after suspend
(`show_vol` → `gpu_get_cur_voltage`, no power-gating of the readout);
the rail `state=disabled` is the operative parked fact. The transient
`534:1` tick is the governor stepping down through 534 during the
restore suspend — expected, not a stuck step.

### 2.4 Battery/fuel-gauge current: not a usable G3D proxy here

`battery/current_now` swung −195 (idle) → +23…+62 (load) → −27 (settle)
while `voltage_now` sat 4.29–4.38 V at capacity 100 (`Full`, USB online):
the sign/magnitude is dominated by the charger state machine
(top-off cycling at full charge), not by the ~100–200 mA G3D delta.
Fuel-gauge `CURRENT_NOW` disagrees sample-to-sample (+31 vs +50, −62 vs
+46) beyond any GPU signal. Both nodes are still worth logging (gross
stuck-rail smoking gun: rail `enabled` at 1.1 V with GPU suspended and a
+500 mA charge offset would show), but pass/fail must key off §2.2–2.3.

## 3. DVFS reading (why the governor correctly held 266 MHz)

- Soft-job utilization sat at 57–58%, below every up-threshold on the
  floor step (266: max 85) and below the interactive highspeed gate
  (420 MHz @ load 95). `governor_interactive` therefore never steps up
  (`gpu_dvfs_governor.c:115-157`) — holding 266 @ 812.5 mV under this
  load is the *correct* policy outcome, not a stuck governor.
- The governor is demonstrably alive: `utilization` 0→57 within one
  30 ms tick of jobs starting, 57→0 within a tick of them stopping;
  `power_state`/`runtime_status`/`vdd_g3d state` follow the same edges;
  TIS accrues ≈100 ticks/s only while `active`.
- Real 3D (fragment-bound GL) will push utilization past 78–85% and
  should then step 266→350→440…; that workload does not exist yet on
  this kernel (llvmpipe only, no render node). The metric to re-check
  then: TIS spreading upward + `vol` stepping 812 500 → 881 250 →
  937 500 → 1 000 000 → 1 100 000 in lockstep.

## 4. Stuck-rail / stuck-state checklist (all PASS)

| Check | Expected | Observed |
|---|---|---|
| idle parks rail | 812 500 uV `disabled`, GPU `suspended` | PASS (§2.1; re-verified after every test, incl. final `TIS 266:0` reset + 8 s quiet) |
| load powers block | `power_state=1`, `active`, rail `enabled` | PASS (5/5 burst samples) |
| TIS accrues only on running step | 266 grows, others 0 | PASS (30 s: 266 +3 466 ticks, rest 0) |
| rail follows forced OPP | 668 → 1 100 000 uV `enabled` | PASS (§2.3) |
| restore releases | `echo 0` → suspended, `disabled`, policy back | PASS (max_lock −1, TIS cleared to 0) |
| thermal clamp uninvolved | gpufreq 0/5, tz0 ≤ 62 °C | PASS (peak tz0 62 °C mid-burst, 48–59 °C otherwise) |
| no fault counters | `gpu_status` all zero | PASS (`reset/mmu/warmup` counts 0 throughout) |

## 5. Limits / next steps

1. This load is SOFT_JOB (job-manager + power path only) — it validates
   power/domain/rail/DVFS plumbing, not shader/Tiler throughput. A
   fragment-heavy render load (panfrost path, later slices) must repeat
   §2.2 and is expected to climb OPPs.
2. Battery current is logged for continuity but is not acceptance-grade
   at full charge; a constant-voltage-supply or coulomb-count run below
   ~80% SoC would be needed to resolve mA/W per OPP.
3. Device left clean: stress binary `/home/user/gpu-job-stress` (user
   home, survives for re-runs), `dvfs_max_lock=-1`, `clock` policy
   restored, `time_in_state` zeroed, GPU `suspended`, rail `disabled`.

## 6. Files

- `code/gpu31/powermetric/gpu-powermetric.sh` (sampler), `gpu-job-load.c`
  (load source), `README` (build/run + DVFS truth).
- This report: `docs/reports/gpu31-powermetric.md`.
- No kernel patch (read-only tree use); no shared-tree modification.
