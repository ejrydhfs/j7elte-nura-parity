# gpu31-watchdog — silent hangs: cost 5.11 s → <1 s (750 ms dispatch budget)

Date: 2026-11-22. Owner: G31_Watchdog. Device: int41
(`3.10.108 #37 SMP PREEMPT Sun Nov 22 15:58:21 EST 2026`) before the
change; int42 (same tree + commit `39cce8c`) for the after measurement.
Mesa 20.3.5 at `/data/mesabuild/mesa20-b`, clients `gfdemo`
(`GFDEMO_BENCH=1 GFDEMO_PX=256 GFDEMO_FRAMES=300 GFDEMO_NOREAD=1`) and
`rdtest`. All device work as root over ssh under
`flock /tmp/gpsdev.lock`; plain runs (never `PAN_MESA_DEBUG=sync|trace`);
**no reboot, no unbind**. Status: driver change folded into 0001, host
checks green, silicon verified on int42 (§4): a provoked silent hang is
detected and recovered in 849-856 ms (was 5.11 s), three consecutive
300-frame runs are 0 ERR at 155.7-158.5 fps, and the 30-frame 256²
readback is 0.64 s.

## 0. Verdict

A **silent hang** — job dispatched, the slot never raises DONE or ERR,
`JS_STATUS` reads `NO_EXCEPTION`/a latched stop code, `JOB_INT`
STAT/RAWSTAT 0, `head == tail == jc`, `COMMAND_NEXT` drained — has
exactly one way out: the per-job `drm_sched` delayed work, which calls
`timedout_job -> p7580_job_recover`. That budget was **5000 ms**, so one
silent job froze its frame for 5 s.

`P7580_JOB_TIMEOUT_MS` is now **750 ms** (it is the value already passed
to `drm_sched_init`, so the whole change is the constant plus a runtime
seed/knob). Measured on int41, a natural silent hang cost
**5.000 s + 0.106 s recovery = 5.11 s** (client-observed frame
5166 ms); with the same recovery cost the new bound is **≈0.86 s**
(≈0.92 s including the frame's own ~59 ms), i.e. under the 1 s target,
while
750 ms sits 2× above the worst measured frame (512² readback 373 ms, all
of it Mesa's CPU detile; GPU jobs are tens of µs) and ~3000× above the
largest measured GPU job (244 µs).

Two debugfs knobs (0600, inert by default) make this verifiable and
tunable without a rebuild:

* `p7580/hang_inject` — deterministic silent-hang provocation: write N
  to mask the slot's DONE+ERR bits at START for the next N dispatches.
  The job still runs and retires in hardware, no IRQ is delivered, and
  the fence waits the whole budget; `recover` re-arms the mask.
* `p7580/job_timeout_ms` — the budget in ms (100..60000). It seeds the
  entities of later opens (`entity->timeout` is copied at
  `entity_init`) and updates the live `sched->timeout`, so one boot can
  A/B 5000 vs 750 ms, and a legitimately long frame can be given more
  headroom without a kernel rebuild.

## 1. Mechanism and code map

Why the fence waits the timeout at all: the driver signals
`job->done_fence` from the DONE leg of `p7580_job_irq()`. A silent hang
produces no IRQ, so the only signal comes from the timeout work in the
0004 shim (`drm_sched_job_timeout()` → backend `timedout_job`, i.e.
`p7580_drv_timedout_job()` → `p7580_job_timedout()` → `recover()`). The
shim arms that work **at dispatch** with `entity->timeout` jiffies
(`drm_sched_pump()`), and `entity->timeout` is copied from
`sched->timeout` in `drm_sched_entity_init()` (per open).

int41 state: `p7580_drv_sched_ensure()` already passed
`msecs_to_jiffies(P7580_JOB_TIMEOUT_MS)` to `drm_sched_init()`, and the
constant was 5000. The fix is therefore a value change, not wiring:

| file | change |
|---|---|
| `panfrost7580_jm_hw.h` (driver 0001 + `code/gpu31/jobs` mirror) | `P7580_JOB_TIMEOUT_MS` 5000u → 750u; comment records the tradeoff and the knob |
| `panfrost7580_submit.c` | `static unsigned int p7580_drv_job_timeout_ms = P7580_JOB_TIMEOUT_MS;` passed to `drm_sched_init`; `p7580_drv_get/set_job_timeout_ms()` (clamp 100..60000, live `sched->timeout`, logs the new value) |
| `panfrost7580_job.c` / `.h` | `hang_arm` + timeout-hook fields; `p7580_job_set_timeout_hooks()`; `hang_inject` and `job_timeout_ms` debugfs files; the mask write in `p7580_job_hw_submit()` |
| `panfrost7580_main.c` | wires the timeout hooks at probe (beside `desc_dump`/`iova_resolve`) |
| host tests | `job_test` pins 750u; `submit_fix_test` parity macro 750u |

No shim change: the 0004 dispatch-timeout path, its delay-0 kick from the
ERR leg (`drm_sched_job_kick_timeout`) and the close budget
(`entity->timeout + 8 s`) are untouched. The fault-with-IRQ path still
recovers in ~milliseconds (the delay-0 kick returns 0 from
`mod_delayed_work`), so only the no-IRQ case pays the budget.

The `hang_inject` mask is written **before** `COMMAND_NEXT=START` under
`hw_lock`, so the completion IRQ is never generated. The one difference
from a natural silent hang is diagnostic only: `JOB_INT_RAWSTAT` keeps
the latched DONE bit (nothing clears it because no IRQ runs), while a
natural silent hang reads 0/0. The fence/timing/recovery path is
identical.

## 2. Timing model and tradeoff

| term | measured | source |
|---|---|---|
| GPU job, dispatch → DONE IRQ (256²) | 36 µs / 56 µs / 244 µs (chain node 1/2, fragment) | gpu31-latency §1.2 |
| worst frame, 512² readback | 373 ms, alternating with 56 ms | gpu31-readback §2 |
| recovery after timeout (power cycle + AS0 + re-arm) | 106 ms (int41, this ticket); 107 ms/fault in the storm | int41 dmesg |
| silent-hang cost, int41 | **5.000 + 0.106 = 5.11 s** (client frame 5.166 s) | §3 |
| silent-hang cost, int42 | **≈0.750 + 0.106 = 0.86 s** (client frame ≈0.92 s; measured in §4) | |

The 373 ms frame is CPU work in Mesa's scalar `panfrost_load_tiled_image`
(the GPU has already signalled), so it is not a candidate for the job
timeout. 750 ms still gives 2× headroom over the slowest observed frame
and ~3000× over the slowest observed GPU job. The chosen cost of being
wrong is explicit: a legitimate GPU job that really runs >750 ms would
be reset and its fence signalled early (possible render corruption, one
`ERR`/`TIMEOUT` line in dmesg); raise `p7580/job_timeout_ms` (or the
constant) for such a workload. No such job exists in this stack: the
fragment job is the longest and completes in 244 µs.

Side effects: `drm_sched_entity_fini()`'s close budget is
`entity->timeout + 8 s`, i.e. 8.75 s instead of 13 s; a close behind a
silent hang now blocks <1 s, not 5 s.

## 3. Silicon before (int41, 5 s budget): a natural silent hang costs 5.11 s

Three clean 300-frame runs on int41 (159.7 / 154.7 / 154.2 fps, walls
1.96 / 2.02 / 2.03 s) were followed by a faulting one (`dmesg -c`
before every run, `ev_arm=0`). The surviving tail of that run's dmesg
(the sm5703 fuelgauge driver floods printk, so only the last ~10 s of
the ring is kept) starts with the timeout block of the first fault:

```
[1099.612566] p7580_job: TIMEOUT slot=0 js_status=0x00000058
             (DATA_INVALID_FAULT) stat=0x00000000 rawstat=0x00000000
             head=0xaeb15c0 tail=0xaeb15c0 seq=1055 jc=0xaeb15c0
             cfg=0x00083700 sw=... 00010013 ...
[1099.718741] panfrost7580 11400000.mali: pf7580: power cycle with busy cores
[1099.718910] p7580_job: recover-done slot=0 latched=0x00000058 cycle=1
             js0=0x00000000 js1=0x00000000 intmask=0x00070007 drain_rc=0
[1099.724362] p7580_job: ERR slot=0 js_status=0x00000058 ... stat=0x00010000
             rawstat=0x00010000 ... seq=1059
```

* `stat/rawstat == 0` at the timeout: **no IRQ ever arrived** — the
  silent signature. The job's dispatch was 5000 ms earlier (5.000 s is
  the only arming source, §1); `seq=1055` at 4 jobs/frame ≈ **frame
  264**, i.e. the fault appeared late in the run.
* `recover-done` 106 ms later with `cycle=1` (full power-on);
  `latched=0x58` is the status the slot had latched before recovery.
* Run totals: **ERR=36 TIMEOUT=37 recover-done=37, "did not idle on
  power-off" = 37** (one aborted cores-off per recovery, fail-closed),
  bench 26.003 s / 300 frames = **11.5 fps**, versus 1.88-1.95 s
  (154-160 fps) for the clean runs. The 36 follow-on faults arrive
  *with* the ERR IRQ (`stat=0x00010000`) and use the delay-0 kick
  (~112 ms cadence: `TIMEOUT` every ~0.1128 s in the log), so the 5 s
  is paid exactly once, by the silent first fault.

Natural silent-hang reproducer (int41, 512² glFinish readback), the same
signature without any instrumentation — `rdtest --size 512 --frames 30
--read 1 --pre-finish 1`, from a fresh process:

```
FRAME 0 5180ms ... MISMATCH      FRAME 8 5166ms ... MISMATCH
FRAME 1 5165ms ... MISMATCH      FRAME 9 5166ms ... MISMATCH
...                              FRAME 10 58ms ... OK
TOTAL frames=30 ok=20 zero=0 mismatch=10

[1311.971466] p7580_job: TIMEOUT slot=0 js_status=0x00000008
             (NO_EXCEPTION) stat=0x00000000 rawstat=0x00000000
             head=0xbc1b5c0 tail=0xbc1b5c0 seq=35 jc=0xbc1b5c0
[1312.078350] p7580_job: recover-done slot=0 latched=0x00000008
             cycle=1 js0=0x00000000 js1=0x00000000 ...
[1317.141434] p7580_job: TIMEOUT ... seq=39   (same signature)
```

Ten consecutive frames each paid **5166 ms** = 5000 ms budget + 107 ms
recovery + ~59 ms of frame work; the kernel ring only retains the last
two TIMEOUT blocks because the sm5703 fuelgauge driver floods printk,
but the per-frame cost is unambiguous. `stat/rawstat == 0` on every
TIMEOUT line: no IRQ ever arrived. Pixels were black until the state
cleared at frame 10 (the recovery power cycle), then correct again.
The same run at 256² `--read 0` showed one silent TIMEOUT (`TIMEOUT=1`,
`silent=1`, `ERR=0`).

Corroboration from earlier slices: gpu31-firstfault run E frame 34 took
**5031 ms** with the same 0x08 silent line; gpu31-latency closed
5.01 s per fault from the frame arithmetic. Both are the 5000 ms budget.

## 4. Silicon after (int42, 750 ms budget)

Device `3.10.108 #37 SMP PREEMPT Sun Nov 22 16:27:00 EST 2026`
(`Image-int42` sha256 `ee50ff52…`); `p7580/job_timeout_ms` reads 750
and `hang_inject` is back to 0 after the runs. Same clients, same lock,
no reboot during the acceptance.

### 4.1 Deterministic A/B, one boot (`hang_inject` masks the next START)

| budget | `hang_inject` | TIMEOUT | `recover-done` | hang + recovery |
|---|---|---|---|---|
| 5000 ms | 84.813275 | 89.816456 (**+5003 ms**) | 89.930258 (+114 ms) | 5.117 s |
| 750 ms | 90.264915 | 91.013573 (**+748.7 ms**) | 91.120343 (+106.8 ms) | **0.855 s** |
| 750 ms (×3) | 196.537014 / 197.797384 / 199.056996 | +741.9 ms each | +106.8 ms | 0.849 s |

dmesg at 750 ms:

```
p7580_job: hang_inject slot=1 seq=0 irq-masked
p7580_job: TIMEOUT slot=1 js_status=0x00000000 (NO_EXCEPTION)
           stat=0x00000000 rawstat=0x00000002 head=0x6219000
           tail=0x6219000 seq=0 jc=0x6219000 ...
p7580_job: recover-done slot=1 latched=0x00000000 cycle=1
           js0=0x00000000 js1=0x00000000 intmask=0x00070007 drain_rc=0
```

`rawstat=0x2` is the injected DONE bit that no IRQ cleared (a natural
silent hang reads 0/0, §1); dispatch -> designation -> recovery is the
same path. The masked job is the first chain node (slot 1), so the
client's frame does not wait on its fence; the wait lands on the entity
FIFO / close. gfdemo with 3 frames does wait (its glFinish covers the
chain):

| budget | control | injected | delta |
|---|---|---|---|
| 5000 ms | 0.035 s | 5.421 s | +5.386 s |
| 750 ms | 0.039 s | 1.153 / 1.155 / 1.157 s | +1.115 s |

The user-visible delta is ~0.27-0.30 s above the kernel hang + recovery
at *both* budgets: that constant is the cold first frame after the
recovery power-on (the same 228-379 ms first-frame cost gpu31-readback
measured for a fresh process), not timeout time. The silent hang
itself is detected and recovered in **849-856 ms, under the 1 s
target**; the client frame window is 1.15 s.

### 4.2 3 × 300 frames, 256² no-read

| run | wall | fps | ERR | TIMEOUT | recover | no_idle |
|---|---|---|---|---|---|---|
| 8 | 2.01 s | 155.7 | 0 | 0 | 0 | 0 |
| 9 | 1.98 s | 158.5 | 0 | 0 | 0 | 0 |
| 10 | 2.00 s | 156.9 | 0 | 0 | 0 | 0 |

Three consecutive clean runs (>=150 fps, 0 ERR/TIMEOUT/RECOVER),
interleaved with 1.91-1.94 s / 156-159 fps singletons in the same boot.

The pre-existing 0x58 storm (#6) appeared in 4 of the 7 runs on this
boot (runs 1, 2, 5, 7): 10.09 / 4.15 / 16.10 / 19.39 s, 13-37 faults,
`recover-done cycle=1`, each with its first fault silent (`0x58` or
`0x08`, stat/rawstat 0). Because that first silent fault now pays
750 ms instead of 5000 ms, the *same* storms that took 21-29 s on int41
take 4-19 s here. That is the unrelated defect; the watchdog neither
causes nor hides it.

### 4.3 Readback + long frame

* 30 frames 256² bare read: **wall 0.64 s**, FRAME 0 20 ms, FRAME 29
  17 ms, 30/30 OK, 0 ERR/TIMEOUT (target <2 s).
* 10 frames 512² bare read: wall 1.22 s, FRAME 9 163 ms, 10/10 OK,
  0 ERR/TIMEOUT - the 373 ms detile frames do not trip the 750 ms
  budget, which is the long-frame regression check.

## 5. Reproduce / accept

Script: `local://gpu31-watchdog-acceptance.sh` (A: same-boot 5000 vs
750 ms with `hang_inject`; B: 3 × 300 frames; C: 30-frame 256² readback;
D: 512² long-frame regression).

```
echo 5000 > /sys/kernel/debug/p7580/job_timeout_ms   # A/B: old budget
echo 1    > /sys/kernel/debug/p7580/hang_inject      # one silent dispatch
# dmesg: TIMEOUT +5003 ms, recover-done +114 ms (hang+recovery 5.117 s)
echo 750  > /sys/kernel/debug/p7580/job_timeout_ms
echo 1    > /sys/kernel/debug/p7580/hang_inject
# dmesg: TIMEOUT +748.7 ms, recover-done +107 ms (hang+recovery 0.855 s)
```

`hang_inject` needs no Mesa change and no special userspace: any submit
(the bench's first `WRITE_VALUE` chain node) is enough. `job_timeout_ms`
affects **new** opens (each client run is a new process), which is what
makes the A/B possible in one boot. Measure the *kernel* pair
(mask -> TIMEOUT -> recover-done) for the hang cost: a client that only
renders one frame can exit before the masked chain node's fence is
needed (close then waits it out), while gfdemo with 3 frames makes the
stall visible in the bench window (1.15 s at 750 ms; §4.1).

## 6. Limits / notes

* The value is a policy choice, not a hardware limit: 750 ms is far
  above every GPU job measured on this part, but a future workload with
  a genuinely long single job must raise it.
* `hang_inject` masks DONE+ERR for the whole slot for one dispatch; the
  job's hardware completion is real, so pixels stay correct while the
  frame stalls. It is a test knob (0600, default 0), not a runtime
  feature.
* The intermittent `0x58 DATA_INVALID` fragment storm seen on int41
  (§3 run) is a **separate, pre-existing defect**; this ticket only
  changes how long its first (silent) job holds the fence. Per-run
  numbers at 300 frames / 256² no-read, `dmesg -c` before each run,
  `ev_arm=0` (12 full 300-frame runs this session: 7 clean, 5 stormy;
  the first clean one was 2.02 s / 148 fps and the first stormy one
  29.5 s / 10.2 fps / 73 ERR lines -- the ring was cleared before the
  detailed runs below):
  | run series | wall | fps | ERR | TIMEOUT | recover | no_idle | first fault |
  |---|---|---|---|---|---|---|---|
  | series A 1-3 | 1.91 / 1.92 / 1.94 | 159.7 / 154.7 / 154.2 | 0 | 0 | 0 | 0 | — |
  | series A 4 | 26.00 | 11.5 | 36 | 37 | 37 | 37 | silent 0x58, seq 1055 (frame ~264), stat/rawstat 0, `recover-done cycle=1` |
  | series B (ev_arm=1) 1 | 28.98 | 10.4 | 34 | 33 | 31 | — | 0x58 fragment, `recover-done cycle=1` |
  | series B 2 | 1.91 | 156.8 | 0 | 0 | 0 | — | — |
  | series B 3 | 28.76 | 10.4 | 34 | 33 | 31 | — | same |
  | series C (ev_arm=0) 1-2 | 1.91 / 1.92 | 157.3 / 156.1 | 0 | 0 | 0 | — | — |
  | series C 3 | 21.10 | 14.2 | 34 | 32 | 32 | — | same |

  The first fault is always the fragment job, and inside one run every
  later fault repeats the same descriptor address/`jc`; recovery always
  reports `cycle=1` (full power-on), `latched=0x58`, and
  each recovery aborts cores-off once with "did not idle on power-off"
  (fail-closed). `ev_arm` has no effect on the rate. The 0x58 storm
  itself (a per-job hardware data-invalid that survives recovery) is the
  next slice's target, not this ticket's.
* int41 also aborts cores-off with "did not idle" on clean runs (5-16
  per 30-frame 512² run); that is the fail-closed path from
  gpu31-readback and is unchanged here.
