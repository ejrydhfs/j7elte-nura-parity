# gpu31-readback — per-frame glReadPixels: ordering is not the fault; a power event latches the JM and recovery never clears it

Date: 2026-11-22. Ticket: G31_ReadbackFault. Device: int39 (`3.10.108 #37
SMP PREEMPT Sun Nov 22 11:41:00 EST 2026`, `Mali-T72x MP2 r1p0 0x0720`),
Mesa 20.3.5 at `/data/mesabuild/mesa20-b` (tree `/data/mesabuild/mesa20`),
driver 0001 only (no new image flashed for this ticket). All device work
under `flock /tmp/gpsdev.lock` as root, plain runs (never
`PAN_MESA_DEBUG=sync|trace`), **no reboot, no driver unbind/rebind** (int39
returns `ENODEV` on rebind, so it is not a usable reset path).

## 0. Verdict

1. **The readback ordering does not fault.** On a healthy GPU, per-frame
   bare `glReadPixels` and `glFinish()+glReadPixels` submit the *same* four
   jobs in the same order and are both clean (0 ERR/TIMEOUT) and
   pixel-correct at 256² and 512². Measured twice at 256² bare (`30/30`
   frames OK), once at 512² bare and 512² pre-finish (`30/30` OK).
2. There is **no readback blit and no extra submit** on this path: Mesa
   20.3 falls back to a direct tiled CPU map (`b8g8r8a8_unorm`, 16×16
   U-interleaved, stride 1024/2048) after one `WAIT_BO`. The `FBDDBG
   readback` line is the render-target BO itself.
3. The real defect is **a GPU power event plus a recovery that cannot clear
   its latch**: `pf7580_cores_off_locked()` powered the block down while
   the cores were still executing (22 × `tiler/shaders/L2 did not idle on
   power-off` per bench run, measured in logs of both clean and faulting
   runs). The JM then reports `JS_STATUS 0x41 POWER_FAULT` for the victim
   job. Soft/hard GPU reset, AS0 reprogram and JOB_INT re-arm do **not**
   clear that latch, so every later job re-reports it (72/72, 144/144 fault
   lines) until the block is power-cycled by a suspend. That is what made
   the earlier measurement look like a readback bug.
4. Fix (driver 0001, host-tested, **needs int40**):
   cores-off fails closed (`-EBUSY`) instead of powering a busy block down;
   `p7580_job_is_idle()` consults the hardware ACTIVE bit; a new
   `pf7580_power_cycle()` clears the POWER_FAULT latch from recovery and
   from resume before the first dispatch; `recover-done` prints the
   post-recovery JS state on every fault.

## 1. The two orderings, measured

Client: `code/gpu31/mesa/mfbd/rdtest.c` (now prints per-phase ms:
`phases=clear,draw,finish,read`). Env: Mesa 20.3.5, surfaceless EGL
pbuffer, `b8g8r8a8_unorm` SFBD render target, `glReadPixels(GL_RGBA,
GL_UNSIGNED_BYTE)` of the whole surface.

Driver-side job stream (traced, `trace_arm`, 256², 3 frames): exactly
**four jobs per frame**, always in this order, each completing with a DONE
IRQ before the next dispatch:

```
31.557312 dispatch slot=1 jc=0x3d8a000 sw[...4]=0x00010005   WRITE_VALUE (tiler field <- 0)
31.558281 dispatch slot=1 jc=0x3d8a100 sw[...4]=0x0002000b   VERTEX
31.559849 dispatch slot=1 jc=0x3d8a200 sw[...4]=0x0003000f   TILER
31.561436 dispatch slot=0 jc=0x3c575c0 sw[...4]=0x00010013   FRAGMENT
```

Bare `glReadPixels`:

```
draw batch pending
-> panfrost_flush_batches_accessing_bo(RT)  == panfrost_batch_submit_jobs()
   -> SUBMIT chain (WRITE_VALUE/VERTEX/TILER, slot 1, out_sync=0)
   -> SUBMIT fragment (slot 0, req=FS, out_sync=ctx->syncobj)
-> panfrost_bo_wait(RT, before_write=false)  -> WAIT_BO ioctl
   -> dma_resv_wait_timeout(RT resv)  == the fragment job's fence
      (published on every submitted BO at SUBMIT time by int39)
-> panfrost_bo_mmap + panfrost_load_tiled_image (CPU detile)
```

`glFinish()` then `glReadPixels`:

```
glFinish -> panfrost_flush_all_batches()  (same two SUBMITs, same jobs)
         -> fence wait on ctx->syncobj    (same fragment fence)
glReadPixels -> flush is a no-op (batch already frozen/submitted)
             -> WAIT_BO returns immediately -> same CPU detile
```

So the **only** difference is which lock the CPU blocks on (the BO's
`dma_resv` exclusive fence via `WAIT_BO` vs the context syncobj fence via
`glFinish`) and *when*. The GPU job set, order, descriptors, tiling and
dependent fences are identical. `st_ReadPixels`' staging-blit path
(`blit_to_staging`) was **not** taken: `_mesa_format_matches_format_and_type`
sent it to `_mesa_readpixels` → `pipe_transfer_map(PIPE_MAP_READ)` on the
render target itself, so the ticket's "readback blit descriptor" does not
exist on this path. The map descriptor is:

```
FBDDBG readback rsrc=0x7fb6cef390 bo=0x247b000 tex_addr=0x247b000
      stride=1024 tiled=1 fmt=b8g8r8a8_unorm 256x256 box=0,0 256x256
FBDDBG fragjob … fbd=0x24bd480 fb=256x256 clear=0x4 min=0,0 max=256,256
      tiles=0,0..15,15                       (SFBD, FBD tag bit0 = 0)
```

## 2. Silicon: the clean matrix (int39, healthy block)

`rdtest`, 30 frames each, `dmesg -c` before / `dmesg` after every run:

| run | mode | faults | pixels | frame ms (mean / p95 / max) |
|---|---|---|---|---|
| acc256a | 256², bare read | **0** | 30/30 OK | 17 / 20 / 370¹ |
| acc256b | 256², bare read (repeat) | **0** | 30/30 OK | 17 / 20 / 300¹ |
| acc512 | 512², bare read | **0** | 30/30 OK | 56–374² |
| acc512pf | 512², glFinish then read | 4 silent hangs³ | 30/30 OK | 348–379² |
| acc256pf | 256², glFinish then read | 144 fault lines⁴ | 0/30 OK⁴ | — |
| acc256nr | 256², no readback (glFinish) | 142 fault lines⁴ | n/a | 6.5 |

Pixels for every OK frame: corner `13,13,31,255` (= `glClearColor(0.05,
0.05, 0.12, 1)`), centre `255,0,0,255` (the red triangle), checksum
identical across frames.

¹ first frame of a fresh process pays 228–379 ms (page-cache/BO setup), rest
17 ms.
² 512² alternates 56 ms and 373 ms; the cost is Mesa's scalar
`panfrost_load_tiled_image` CPU detile of a 1 MiB tiled surface, not GPU
work (GPU jobs complete in tens of µs).
³ `TIMEOUT slot=0 js_status=0x08 (NO_EXCEPTION) stat=0 head=tail=jc`: a
job that never latched, recovered only by the 5 s sched timeout; the frame's
pixels were still correct because no frame waits on it.
⁴ see §3: these runs were already in the faulting state; the *same
configurations* were clean earlier in the same boot (rows 1-2). This is the
proof that the ordering is not the variable.

## 3. Silicon: the fault, and why it persists

### 3.1 First fault: fragment job, `DATA_INVALID_FAULT 0x58`

Traced NOREAD run (53 frames, `trace_arm=700`, `dmesg -c` drained every
20 ms so the ring buffer could not drop the transition):

```
1101.151853 dispatch slot=0 jc=0x70615c0 cfg=0x00083700 as=0 aff=0x3 seq=147
            sw=00000001 00000000 00000000 00000000 00010013 …     (FRAGMENT)
1101.151859 post-start slot=0 rawstat=0x00000000 js_status=0x08 (NO_EXCEPTION)
1101.151869 irq-enter  rawstat=0x00010000 stat=0x00010000
1101.151884 ERR slot=0 js_status=0x00000058 (DATA_INVALID_FAULT) stat=0x00010000
            head=0x70615c0 tail=0x70615c0 seq=147 jc=0x70615c0 …
1101.151900 ERR-MAP slot=0 seq=147 nbos=10 h11=0x7084000/0x10000 4=0x701f000/0x41000
            12=0x7094000/0x100000 5=0x7060000/0x1000 …
1101.152758 JDBODY slot=0 seq=147 jc=0x70615c0 00010258 00000000 070615c0 00000000
            00010013 …  00000000 000f000f 07061480 …    (FBD=0x7061480)
1101.152789 PTEWALK … MATCH snap=0x7061000/0x10000 live=0x7061000/0x10000
```

Frame ~36 of that process. The fragment descriptor at `0x70615c0` is the
same address the previous 36 frames used (`sw[0]=0` in all 211 other
dispatches); at this frame the driver's **SUBMIT-time** snapshot (taken by
the chain walk before queueing) read `exception_status` (word 0) = **1**.
`ptewalk` says the page is mapped and matches. So the descriptor content,
not the page tables, differs on that one frame [INFERENCE: a stale/
recycled 4 KiB batch-pool BO carrying a previous `exception_status` is the
leading candidate; the JM writes `JOB_NOT_STARTED`/fault codes into word 0
and Mesa's pool allocator returns uninitialised memory. Needs the FBD+BO
dump below on int40 to confirm].

### 3.2 What follows: POWER_FAULT latch on both slots

Every later dispatch in the same process (and in the next runs) reports:

```
irq-enter rawstat=0x00020000 stat=0x00020000
post-start slot=1 js_status=0x00000041 (POWER_FAULT)
ERR slot=1 js_status=0x00000041 stat=0x00020000 head=tail=jc seq=N cfg=0x00083700
TIMEOUT slot=1 … same seq                   (the int39 kick, ~1 ms later)
```

`head == tail == jc`, `js_status 0x41`, `stat` set only at the instant of
dispatch: the JM latched START and immediately reported a *power* fault. The
fault counts are exact multiples of the four jobs per frame (72 = 18
frames, 144 = 36 frames), i.e. once latched **every** job faults. The same
run configuration was clean before the first fault and clean again in a new
process after the block had been power-cycled, which is why the earlier
`36 ERR/36 TIMEOUT` measurement looked like a readback property.

### 3.3 The power event: suspend under a busy JM

Every run's dmesg (clean *and* faulting) contains the power slice's
best-effort warnings, three lines per suspend attempt:

```
panfrost7580 11400000.mali: pf7580: tiler did not idle on power-off
panfrost7580 11400000.mali: pf7580: shaders did not idle on power-off
panfrost7580 11400000.mali: pf7580: L2 did not idle on power-off
```

`22 ×` each in a single 30-frame run (18 in `rb512`, 12-21 in the 512²
runs, 3 in a 3-frame run). `pf7580_cores_off_locked()` wrote `PWRON=0`,
polled `READY==0` for 100 ms, **warned and continued** — then the clock was
gated and pd-g3d dropped. That is precisely a GPU power event with work in
the block, i.e. the documented cause of `JS_STATUS POWER_FAULT`. The
software idle predicate could not prevent it:

```c
/* before */
int p7580_job_is_idle(jd) {
    for each slot: if (JS_COMMAND_NEXT) return 0;
                   if (jd->in_flight[i]) return 0;
    return 1;
}
```

`in_flight` is cleared by recovery while the JM may still be executing, and
a latched `0x41` reads `COMMAND_NEXT == 0`. So recovery + autosuspend can
power the block off under the very work that is still draining, and the
resulting power event latches `POWER_FAULT`.

### 3.4 Why recovery did not help

`p7580_job_recover()` under `reset_lock`:

1. `COMMAND_NEXT = HARD_STOP` on the faulting slot (and any slot with a
   pending command), latch-out `in_flight`, drain poll (3 s bound);
2. `POWER_CMD = SOFT_RESET`, poll `RESET_COMPLETED`, `HARD_RESET` on
   timeout;
3. `p7580_mmu_program_as(AS0)` + `p7580_job_dev_rearm()` (CLEAR + unmask);
4. signal the faulted job's fence.

A `POWER_FAULT` latch survives all of that: step 2 resets the GPU block,
not the cores, and the JM keeps reporting 0x41 for every new START. kbase
handles a power fault by forcing a power-off/on through its PM state
machine; this driver did not, so a single power event turned into a
permanent wedge until an unrelated suspend cycled the block.

## 4. Fix (driver 0001; needs int40)

1. **Cores-off fails closed.** `pf7580_cores_off_locked()` now returns
   `int`; on a core that does not acknowledge `PWRON=0` it powers the cores
   back on and returns `-EBUSY`. `pf7580_runtime_suspend()` propagates the
   error *without* gating the clock, so the block is never powered off under
   a busy JM (the PM core retries the suspend later). The warning text now
   says `… did not idle on power-off, aborting suspend`.
2. **Hardware-aware idle predicate.** `p7580_job_is_idle()` additionally
   requires that no present slot reports ACTIVE in `JS_STATUS` (bit 0),
   excluding our own HARD_STOP results (0x02/0x03/0x04). Recovery clearing
   `in_flight` can no longer make a still-running JM look idle.
3. **`pf7580_power_cycle()`** (new power-slice API, power.h):
   `cores_off` (best effort) + clock gate + the full power-on sequence
   (clock, SOFT_RESET with `RESET_COMPLETED` poll and HARD_RESET fallback,
   L2→shader→tiler PWRON with READY polls), all under `p->lock`.
   `pf7580_runtime_resume()` is refactored to share the same
   `pf7580_power_on_locked()`.
4. **Jobs slice wires it** as `jd->power_cycle` (`p7580_job_dev_init` gained
   one hook argument; driver TU supplies `p7580_job_power_cycle_void`).
   `p7580_job_recover()` calls it when the latched status is
   `P7580_JS_STATUS_POWER_FAULT` (0x41) or when a present slot still reads a
   non-stop HW fault code after the GPU reset, then programs AS0 and re-arms
   JOB_INT as before.
5. **`recover-done` line** (always on, faults are rare) so the next silicon
   run carries the post-recovery state instead of a cascade:
   `recover-done slot=… latched=0x… cycle=0|1 js0=0x… js1=0x… intmask=0x… drain_rc=…`.
6. **Resume clears a latch** before any job can be dispatched
   (`resume-cycle rc=… (latched JS fault)`), so a block that comes back from
   autosuspend with a stale 0x41 cannot poison the first submit.
7. Host test `T9` in `code/gpu31/driver/submit_fix_test.c` pins the decision
   table (cycle on 0x41 / on a surviving non-stop fault code; never on
   `NO_EXCEPTION` or 0x02/0x03/0x04; AS0+JOB_INT re-arm on every exit).
   `make -C code/gpu31/driver check`: both tests pass.

Not changed: fence publication (int39), chain split, slot routing, Mesa.
No UAPI change.

## 5. Verification status

* **int39 silicon (this ticket):** ordering equivalence and clean matrix
  in §2; fault mechanics in §3. Acceptance "256² and 512² readback with 0
  ERR/TIMEOUT and correct pixels" is met *on a healthy block* (acc256a/b,
  acc512); the same binaries fault once the block has been power-cycled
  under load, which §4 fixes.
* **Not yet measured:** the int40 image. Required run after flash:
  1. `rdtest --size 256 --frames 100 --read 0` then `--read 1` ×3,
     `--size 512` likewise; expect 0 ERR/TIMEOUT, 100 % OK pixels.
  2. Force a fault (`echo on > …/power/control; sleep 0.2; echo auto >`),
     then the same runs: a fault must now print
     `recover-done … cycle=1 js0=0x00000000 js1=0x00000000 intmask=0x00030003`
     and the *same* process must keep rendering (no cascade, no 5 s stall).
  3. `dmesg | grep -c 'did not idle'` must be 0 (suspend aborts instead of
     powering a busy block down).

## 6. Residual (not closed here)

* **Origin of the first fault.** One fragment descriptor had
  `exception_status = 1` at SUBMIT (§3.1) out of 212 dispatches; the
  fault is `DATA_INVALID_FAULT 0x58` with `fault_ptr = jc`. Whether Mesa's
  batch-pool BO really carried a stale word 0, or a WRITE_VALUE/scoreboard
  job wrote that location, needs a fault-time dump of the FBD and the
  descriptor page (a small addition to the ERR pin, cheap on int40).
  Until then this is [INFERENCE].
* **Silent hangs** (`js_status 0x08`, `stat=0`, `head==tail==jc`, no IRQ)
  still cost the full 5 s sched timeout because the int39 kick needs an
  ERR IRQ. A dispatch watchdog (poll `JS_STATUS` ACTIVE after N ms, recover
  early) would bound them; separate from this ticket.
* **CPU detile cost**: 512² readback spends 56–373 ms/frame in
  `panfrost_load_tiled_image`; a tiled→linear blit (or the GL fast path)
  would make it steady. Perf item, not a fault.
* Correlation only: the single 44 MB-free run faulted; five runs at 98 MB
  free did not (one of them the identical config). Stated as correlation
  with unknown cause, not as a proven mechanism.

## 7. Files changed

* `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` — power.c/h
  (`pf7580_power_on_locked`, `pf7580_power_cycle`, fail-closed cores-off),
  job.c (`p7580_job_is_idle`, `p7580_job_slots_latched_fault`, recover
  cycle + `recover-done`, dev_init hook), job.h (hook, helper, init
  signature), main.c (resume latch clear, hook wiring), jm_hw.h
  (`P7580_JS_STATUS_POWER_FAULT`, stop-code predicate).
  sha256 `e9cb91a7086e526f106b07c2abe3cbf551bef57024e03341dd05aeb95d6dcfe8`.
* `code/gpu31/driver/submit_fix_test.c` — T9 latch-cycle model.
* `code/gpu31/mesa/mfbd/rdtest.c` — per-phase timings
  (`phases=clear,draw,finish,read`); frame-line prefix unchanged.

Device artifacts from this ticket: `/tmp/rb256.log`, `/tmp/rb512*.log`,
`/tmp/acc25*.log`, `/tmp/acc512*.log` (per-run dmesg), `/tmp/tr3.log`
(drained trace of the transition), `/tmp/rdtest`, `/data/mfbd/rdtest.c`.
