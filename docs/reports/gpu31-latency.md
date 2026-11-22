# gpu31-latency — per-frame submit latency: timeline, root cause, fix

Date: 2026-11-22. Ticket: G31_Latency. Target image: int39 (built from
`code/gpu31/series/build-int39.sh`). Device evidence: int38 kernel
`3.10.108 #37 SMP PREEMPT Sat Nov 21 23:19:22 EST 2026`, Mesa 20.3.5
at `/data/mesabuild/mesa20-b`, `gfdemo` bench (`GFDEMO_BENCH=1
GFDEMO_PX=256 GFDEMO_FRAMES=N GFDEMO_NOREAD=1`, 256x256 pbuffer,
glClear + 3-vertex draw + `glFinish()` each frame). All device work
under `flock /tmp/gpsdev.lock`; `PAN_MESA_DEBUG=sync/trace` never used.
No flash was performed by this ticket.

## 0. Verdict

**There is no ~40 ms steady-state submit cost.** With a clean GPU state
the same 300-frame bench runs at **286-305 fps (3.3-3.5 ms/frame)**.

The 24.4 fps (41 ms) figure is a **fault-recovery artifact**: on a job
fault the IRQ ERR-without-DONE leg pins the error but **never signals
the job's fence**; the fence is only signalled by the 5 s
`drm_sched` timeout (`timedout_job -> p7580_job_recover`). Every fault
therefore stalls the frame for **5.01 s**, and recovery can leave the
slot wedged so the next job faults too. Arithmetic closes exactly:

```
300 frames x 3.5 ms + 2 x 5.01 s = 11.1 s  ->  27.0 fps
300 frames x 3.5 ms + 3 x 5.01 s = 16.1 s  ->  19   fps
```

Measured runs: 28.7 fps (one 10.44 s run), 286.5 fps, 286.8 fps. A
readback run (30 frames) took 45.42 s (0.7 fps) = ~8 fault stalls plus
per-frame readback waits on the same fences.

The fix (int39) kicks the faulted job's timeout work at delay 0 from
the IRQ, on the same ordered workqueue, so recovery signals the fence
in milliseconds instead of after the full retry budget. The 5 s timer
stays as the backstop for a genuinely stuck slot. Steady-state work
remains unchanged (same fences, same sync ordering, same FIFO).

## 1. Where one frame goes (measured, steady state)

### 1.1 Userspace / ioctl timeline

Method: `LD_PRELOAD` interposer around `ioctl(2)` (`/tmp/iotrace.c`,
built on-device, static timestamps from `CLOCK_MONOTONIC`), 50 frames,
NOREAD. 1020 DRM ioctls = 20.4/frame; the bench printed
`BENCH 256x256 frames=50 time=0.188s fps=266.5`.

| ioctl | calls/50f | mean | p50 | max | share |
|---|---|---|---|---|---|
| `PANFROST_SUBMIT` (0x40) | 100 (2/frame) | 1.094 ms | 1.056 ms | 3.044 ms | 109.4 ms |
| `PANFROST_WAIT_BO` (0x41) | 197 (~4/frame) | 0.199 ms | 0.003 ms | 1.085 ms | 39.2 ms |
| `PANFROST_CREATE_BO` (0x42) | 12 (warm-up) | 0.195 ms | 0.057 ms | 0.732 ms | 2.3 ms |
| `PANFROST_MMAP_BO`/`GET_PARAM`/other | 60/frame? | 0.002-0.010 ms | | | < 1.2 ms total |
| process wall (first->last ioctl) | | | | | 0.226 s |
| bench wall | | | | | 0.188 s |

Two submits per frame, exactly as Mesa 20.3 emits: (1) the
WRITE_VALUE->VERTEX->TILER chain, (2) the fragment job. The kernel
splits (1) into 3 dispatches (gpu31-chainsplit) -> **4 GPU jobs/frame**.
Per-frame pattern (example frames):

```
frame 0: sub0 start+0.000 ms dur 3.044, sub1 start+3.095 dur 0.870
frame 1: sub0 start+5.149 ms dur 1.224, sub1 start+6.389 dur 0.832
frame 2: sub0 start+8.578 ms dur 1.215, sub1 start+9.810 dur 0.833
frame 3: sub0 start+11.727 dur 1.449, sub1 start+13.207 dur 0.868
```

The first submit is the largest (cold chain: descriptor walk + shadow
BO create/map/free). Steady-state submit cost is dominated by the
synchronous SUBMIT leg (BO lookup/map + per-BO submit clean + one
shadow-BO create/map + 3-4 sched-job allocations), not by waiting: the
`glFinish()` wait itself is the tail of the last submit.

### 1.2 Kernel dispatch/completion timeline

Method: re-arm the driver's bounded trace
(`echo N > /sys/kernel/debug/p7580/trace_arm`), 4 frames, NOREAD,
`dmesg` microsecond stamps. Per job lines: `dispatch` -> `post-start`
-> `irq-enter` -> `irq` -> `fence signal`; printk time is included and
is itself ~0.2-0.8 ms/line on this console, so gaps are upper bounds.

```
1657.749848 dispatch slot=1 jc=0xecea100 seq=1       (chain node 1)
1657.749854 post-start rawstat=0 js_status=NO_EXCEPTION
1657.749875 irq-enter rawstat=0x00000002 stat=0x00000002
1657.749884 irq slot=1 js_status=0
1657.750704 fence signal slot=1 seq=1
1657.751668 dispatch slot=1 jc=0xecea200 seq=2       (chain node 2)
...
1657.753484 dispatch slot=0 jc=0xebb75c0 seq=3       (fragment)
```

- dispatch -> job DONE IRQ: **36 us (node 1), 56 us (node 2), 244 us
  (fragment, slot 0)**; the GPU work itself is tens of microseconds.
- IRQ -> fence signal: inside the handler, ~0.8 ms *as traced*; the
  untraced value is tens of microseconds (the trace's own printk is
  most of the gap).
- fence signal -> next dispatch: one workqueue hop
  (`fence_cb -> finish_work -> free_job -> pump -> run_job`),
  ~0.7-0.9 ms as traced (printk-inflated); untraced this is the
  ~1 ms/frame residual over the two submit ioctls.
- Chain node i+1 waits on node i's fence (drm_sched dependency); the
  fragment (slot 0) waits on the tiler (per-file `serial_prev`). That
  ordering is **required** for correctness on this part (the split
  exists to put a flush between VS and tiler; the fragment reads the
  polygon list) and is not the latency source: each dependency is a
  completed fence, not a poll.

### 1.3 Waits / power / IRQ

- `PANFROST_WAIT_BO` p50 is 3 us: `dma_resv_wait_timeout` ->
  `wait_for_completion_*` (event wakeup). The 1.085 ms max is the
  render-target wait for the frame's own GPU work, not a coarse poll.
- `DRM_IOCTL_SYNCOBJ_WAIT` (glFinish path) uses the same completion
  wait; under strace a normal frame's wait is < 0.3 ms.
- No autosuspend: a live-job PM ref is taken before queueing and
  released in `free_job`; `run_job` re-gets around the dispatch.
- No IRQ coalescing: `JOB_IRQ_THROTTLE` is 0 (kbase semantics).
- Serialization for same-file ordering (`serial_prev`) attaches a
  dependency only when the predecessor fence is still unsignaled; a
  retired predecessor is skipped, and duplicate attaches are no-ops.

## 2. The fault path: where 5 s comes from

Fault evidence (int38, 4-frame traced run; same signature in untraced
runs):

```
1657.767639 dispatch slot=0 jc=0xebb75c0 seq=11      (fragment)
1657.767645 post-start slot=0 rawstat=0x00010000 js_status=0x41 (POWER_FAULT)
1657.767656 irq-enter rawstat=0x00010000 stat=0x00010000
   ... no DONE bit, no fence signal; the ERR-without-DONE leg only pins ...
1662.777939 dispatch slot=1 ... seq=12                 5.010 s later
1662.777962 post-start slot=1 rawstat=0x00020000 js_status=0x41 (POWER_FAULT)
```

`p7580_job_irq()` handles a slot's ERR bit by freezing the slot
(`COMMAND_NEXT=NOP`) and printing `irq-err`, but only the DONE leg
calls `dma_fence_signal()`. The fence therefore waits for
`entity->timeout` (5 s), whose work calls
`p7580_job_timedout()` -> `p7580_job_recover()` (HARD_STOP slots, wait
for drain, soft/hard GPU reset, reprogram AS0, re-arm IRQs) and only
then signals the fence. Every fault costs the full 5 s, and a fault
that upsets the slot can cascade into the next job (POWER_FAULT
immediately after post-start), which costs another 5 s.

`POWER_FAULT` (0x41) is what the JM reports when the job was killed by
a GPU power/reset event; the cascade is consistent with recovery
running late (5 s) and with resubmission racing the reset. The fix
does not change when recovery runs relative to the FIFO (still the
same ordered workqueue), only how soon it is triggered.

## 3. Fix (int39)

1. `panfrost7580_job.h`: `struct p7580_submit_job` gains an opaque
   `void *sched_job` backpointer (the 0004 job that owns it).
2. `panfrost7580_submit.c` (`p7580_drv_split_queue`): stamps
   `job->sched_job = sched_job` after the ops/entity validation, before
   arming.
3. `panfrost7580_job.c` (`p7580_job_irq`, ERR-without-DONE leg):
   after the err pin and unlock, if the faulting in-flight job has a
   `sched_job`, call `drm_sched_job_kick_timeout()`. The job pointer is
   captured under `hw_lock` (it cannot be freed while it is still
   `in_flight[]`), so the kick can never touch a retired job.
4. `drivers/gpu/drm/drm_sched.c/.h` (0004): new
   `drm_sched_job_kick_timeout()` = `mod_delayed_work(job->entity->wq,
   &job->timeout_work, 0)`. Runs on the **same ordered workqueue** as
   the pump and the finish work, so recovery still happens before the
   next job is dispatched and before `free_job`; `drm_sched_job_timeout`
   skips a job whose fence already signalled. The 5 s arm at dispatch
   is untouched (backstop).
5. **BO excl publish at SUBMIT (MFBD hand-off, the readback fault
   source).** `p7580_drv_submit()` now stamps the split's `last_fence`
   (the same fence installed on out_sync and published as `serial_prev`)
   as the exclusive fence on every submitted BO before the ioctl
   returns. Previously the first publish happened in `run_job`, i.e.
   after the async pump dispatched; Mesa's readback path
   (`flush_batches` then `panfrost_bo_wait(render_target)` right after
   SUBMIT) could therefore see the *previous* submit's fence and map a
   BO the GPU was about to write. That is the `DATA_INVALID_FAULT`
   (0x58) / `POWER_FAULT` (0x41) fragment fault measured in readback
   runs, and each instance cost the 5 s recovery.
6. **Intermediate nodes no longer overwrite BO exclusivity.**
   `p7580_submit_job.publish_excl` is set only for the node whose fence
   the split publishes (the last queued node; a one-node submit is its
   own last). `run_job` stamps BO excl fences only for that node, so an
   intermediate node cannot replace the published fence with an earlier
   one (which would let a `WAIT_BO` return after the vertex stage and
   read a tiler-written BO early). This also removes 10 resv lock/unlock
   pairs for each of the 2 intermediate chain nodes per frame.

No fence lifetime, BO sync ordering, dependency, slot routing or UAPI
change. The recovery path still does its pre-signal BO clean/invalidate
before signalling, so waiters keep the "fence implies visible data"
contract.

## 4. Measurements AFTER (int39)

Device slot in the external-disk log area:

| run | int38 (before) | int39 (after) |
|---|---|---|
| 300 frames NOREAD, clean | 286.5 / 286.8 / 305.0 fps | _to be filled after Main flashes int39_ |
| 300 frames, after a fault | 28.7 fps (10.44 s) | _to be filled_ |
| 30 frames with readback | 0.7 fps (45.42 s) | _to be filled_ |
| 2 submits + 4 dispatches per frame | 3.3-3.5 ms | expected unchanged steady state; fault frames bounded by recovery (ms) instead of 5 s |

Acceptance target: < 5 ms/frame at 256x256. Already met in steady state
on int38 (3.3-3.5 ms); int39 removes the fault-driven outliers that
produced the reported 41 ms average.

## 5. Residual work (not in this ticket)

- The chain split gives **every** node its own copy of all BO refs, so
  completion cache maintenance (`pre_signal_sync` in IRQ + `done_sync`
  in `free_job`) walks the same ~2.7 MB set 4x per frame (per node,
  clean+invalidate each pass). With excl publish now scoped to the
  published node, the intermediate nodes' fences are no longer
  externally observable (no out_sync, no BO excl), so their two sync
  passes can be skipped for `!publish_excl` in a follow-up. A partial
  queue failure makes the last *queued* node the published one even
  when it is not the planned last, so the flag must be fixed up after
  the queue loop before the syncs are elided.
- A shadow descriptor BO is created, mapped and freed on every SUBMIT
  (only for multi-node chains). Pooling it per file (reuse only after
  the previous split's fence) would remove a GEM create + MMU map +
  TLB invalidate per frame.
- `finish_work` -> `queue_work(pump)` is one workqueue hop per job.
  Folding retirement into the pump was rejected here (it races
  `entity_fini`'s `cancel_work_sync`); a safe variant needs a
  retirement pointer owned by the pump.

## 6. Tests / gates

- `code/gpu31/sched`: **407 checks, 0 failures** native and
  qemu-aarch64, all structural gates PASS, `kcheck` (4 files
  syntax-only + object compile) PASS. New
  `test_fault_kick_timeout` pins the shim contract: unarmed -> armed at
  delay 0, second kick stays delay 0, NULL tolerated.
- Host emu gained `mod_delayed_work()` with the kernel return contract.
- `build-int39.sh` gate 6z pins the applied tree, `job.o`
  (exactly one call reloc), `drm_sched.o`/`System.map` (definition),
  the 5 s backstop, and both patch mirrors.
- Patch series applies clean (0001-0004 + driver 0001) with the
  new-file hunk counts re-derived (job.c 1459, job.h 304, submit.c
  1974, drm_sched.c 1053, drm_sched.h 273).

## 7. Files changed

- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
- `patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch`
  (+ `code/gpu31/sched/patches/` and `code/gpu31/series/` mirrors,
  byte-identical)
- `code/gpu31/sched/src/drm_sched.{c,h}`
- `code/gpu31/sched/emu/include/linux/kernel-emu.h`
- `code/gpu31/sched/tests/sched-host-test.c`
- `code/gpu31/series/build-int39.sh`

## 8. Artifacts

Built by this ticket with `code/gpu31/series/build-int39.sh`
(`make rc 0`; 25 gate sections PASS, 0 GATE FAIL; driver-0001 sha256
`78998ec6201d8d8f77947d4c4a6a3edfb3fdc42574dce368a585f907e8962828`
at build time):

- `kernel-boot-int39.img` (external disk, `$EXT` below),
  sha256 `29f635029d52163e08ab612f9594b75e0e54bca915e1a187ab6e76be2aa3465d`
- `Image-int39`, sha256
  `3639377f36c0c995614b5f1b418c6f03ba6782ddf99a357dfbab609b690568cc`
- build log: `logs/gpu31-int39-build.log`
- gate evidence: `logs/gpu31-int39-evidence.log`
- sched host log: `logs/gpu31-int39-schedhost.log` (407 checks x2,
  0 failures; `gates: all PASS`; `kcheck` PASS)

`$EXT = /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31`

Flash and the "after" rows in §4 are Main's (device was off USB after a
GPU wedge before the build finished; no flash by this ticket).
