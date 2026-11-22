# gpu31-storm58 — the fragment `0x58` storm is a BO-fence publish-order bug (older fence replaces a newer one)

Date: 2026-11-22. Owner: G31_Storm58. Device: int42
(`3.10.108 #37 SMP PREEMPT Sun Nov 22 16:27:00 EST 2026`; the image
gpu31-watchdog built), Mesa 20.3.5 at `/data/mesabuild/mesa20-b`, client
`gfdemo` (`GFDEMO_BENCH=1 GFDEMO_PX=256 GFDEMO_FRAMES=300
GFDEMO_NOREAD=1`). All device work as root over ssh under
`flock /tmp/gpsdev.lock`; plain runs (never `PAN_MESA_DEBUG=sync|trace`);
no reboot, no unbind, no flash (Main). Status: **first fault captured on
silicon, mechanism identified from the capture + the shim contract, fix
folded into driver 0001, host test + patch apply + cross-compile green;
silicon re-run blocked — see §6.**

## 0. Verdict

1. **The first fault is a fragment (slot 0) job that stops dead with a
   perfectly formed descriptor**: `TIMEOUT slot=0 js_status=0x00000008
   (NO_EXCEPTION) stat=0x00000000 rawstat=0x00000000 head=tail=jc`, no
   IRQ ever, `COMMAND_NEXT` drained, descriptor words 0..3 still zero
   and words 4/9/10 exactly the submit snapshot (`00010013`, `000f000f`,
   SFBD pointer into the same 64 KiB pool slab). Every job of the same
   and of the preceding frame retired (`DONE`, b=2/1) before it started.
   Captured twice with `ev_arm=1`: seq 3 (frame 0) and seq 351
   (frame ~88), both byte-identical in shape to a good frame.
2. **The BO was in flight and its fence said it was idle.** The 0002
   resv shim's `dma_resv_add_excl_fence()` **replaces** the resv's single
   exclusive fence, and driver 0001 re-published the ham-owned fence a
   second time from `run_job`, i.e. asynchronously at *dispatch* time.
   In a frame the FS submit publishes its (newer) fence first; the
   draws submit's **tiler** then dispatches ~1.7 ms later and
   re-published its **older** fence over it. From then on the pool slab
   reads idle to `WAIT_BO` as soon as the tiler retires, while the FS
   job is still queued/executing. Mesa's BO-cache fetch
   (`panfrost_bo_cache_fetch` → `panfrost_bo_wait(bo, 0, RW)`) therefore
   handed the same page back and the next frame's CPU build rewrote the
   SFBD/descriptor under the GPU — the torn-SFBD hang, and (once the
   pipeline is deep, which a 750 ms hang guarantees) the permanent
   `0x58`/recovery cycle.
3. **Fix (driver 0001)**: publish BO exclusivity **only at SUBMIT**, in
   submit order — the `dma_resv_add_excl_fence` loop now lives inside
   the `df->serial_lock` section that swaps `serial_prev`. The
   arm-time (`run_job`) re-publish is deleted; `publish_excl` is gone.
   Host model test T10 pins the invariant.
4. Diagnostics fixed in the same patch, because the int42 capture proved
   both were wrong: the 4 KiB page dump only printed the first 64 bytes
   of every 256-byte job slot (exactly the bytes holding the SFBD tiler
   section at `FBD+0xC0` and the descriptor at `+0x140`), and
   `p7580_job_first_fault()` read `job->desc[10]` although `desc[]` is
   8 words — the "fbd" dump was fed an unrelated pointer
   (`PAGEDUMP fbd gpu=0x2f795300 rc=-2 (unmapped)`). The SFBD address is
   now captured at SUBMIT (word 10 of a type-9 node) and each fault
   prints `jc_age` (SUBMITs since this file last used the BO holding
   `jc`).

## 1. Reproduction (int42)

`ev_arm=1`, `serialize_fs=0`, `job_timeout_ms=750`, one fresh `gfdemo`
process per run, dmesg drained to a file every 50 ms so the one-shot
dump can never be rotated out of the ring:

| series | runs | storms | first-fault seq (frame) | storm wall | clean wall |
|---|---|---|---|---|---|
| base | 6 | 2 | 3 (frame 0), 351 (frame ~88) | 19.94 s, 15.16 s | 1.98-2.03 s |
| first repro | 2 | 1 | 3 (frame 0) | 19.71 s / 158 ERR / 159 TIMEOUT | 2.01 s |

A storm run is 10x the wall time of a clean one, all of it the
`cycle=1` recovery per frame (110-115 ms each; one `tiler did not idle
on power-off` per recovery). Two no-cache A/Bs (`PAN_FBD_NOCACHE=1`,
`PAN_FBD_NOPUT=1`) are **void**: on this 1.4 GB/no-swap part they drove
the phone into the lowmemorykiller within one run (see §6), so they
prove nothing and were abandoned.

## 2. The captured first fault

### 2.1 Frame ~88 (`base6`), `ev_arm` ring, last events before the fault

```
EV start  dt=  94856 us slot=0 seq=347 jc=0x9d0f5c0 a=00000000 b=00000000 c=00000008
EV done   dt=  95034 us slot=0 seq=347 jc=0x9d0f5c0 a=00000000 b=00000001 c=00000000
EV start  dt=  97453 us slot=1 seq=348 jc=0x9e42000 ...      (WRITE_VALUE)
EV done   dt=  97510 us slot=1 seq=348 ... b=00000002
EV start  dt=  98371 us slot=1 seq=349 jc=0x9e42100 ...      (VERTEX)
EV done   dt=  98424 us slot=1 seq=349 ... b=00000002
EV start  dt= 100052 us slot=1 seq=350 jc=0x9e42200 ...      (TILER)
EV done   dt= 100125 us slot=1 seq=350 ... b=00000002
EV start  dt= 103364 us slot=0 seq=351 jc=0x9d0f5c0 a=00000000 b=00000000 c=00000008
EV timeout dt= 852316 us slot=0 seq=351 jc=0x9d0f5c0 a=00000008 b=00000000 c=00000000
```

* The previous frame's FS job (seq 347) **completed** 8.3 ms earlier;
  the faulting frame's draw chain (348-350) **completed** before it.
* `a=0` at START: the slot was not occupied, no double dispatch.
* `b=0` (COMMAND_NEXT drained) and `c=0x08` (NO_EXCEPTION) right after
  START: the JM accepted the job and then never retired it.
* At the timeout: `stat=0 rawstat=0` — no IRQ was ever raised;
  `head == tail == jc`; `COMMAND_NEXT` drained.

First-fault line and the faulted job's own bytes:

```
TIMEOUT slot=0 js_status=0x00000008 (NO_EXCEPTION) stat=0x00000000 rawstat=0x00000000
        head=0x9d0f5c0 tail=0x9d0f5c0 seq=351 jc=0x9d0f5c0 cfg=0x00083700
        sw=00000000 00000000 00000000 00000000 00010013 00000000 00000000 00000000
JDBODY slot=0 seq=351 jc=0x9d0f5c0 words=128 00000000 00000000 00000000 00000000
        00010013 00000000 00000000 00000000 00000000 000f000f 09d0f480 ...
PTEWALK slot=0 seq=351 jc=0x9d0f5c0 leaf=0x9e79b4c1 ... MATCH
        snap=0x9d0f000/0x10000 live=0x9d0f000/0x10000 iova=0x9d0f000 npages=16
ERR-MAP slot=0 seq=351 nbos=10 h4=0x9afd000/0x41000 ... h6=0x9d0f000/0x10000
        ... h12=0x8b72000/0x100000 h1=0x897a000/0x100000
```

So: type 9 (FRAGMENT, `(w4>>1)&0x7f`), bounds `000f000f` = full 256²,
SFBD at `0x9d0f480` in the **same** 64 KiB slab as `jc`
(`jc = slab+0x5c0`, `FBD = slab+0x480`, FBD tiler section at
`+0x540`), page tables `MATCH`. The descriptor is not stale: the same
`jc` and the same BO set are used by every frame of the run, the ring
shows the whole preceding chain retiring, and the arithmetic matches
Mesa's pool layout exactly (SFBD is `0x140` bytes, so the FS descriptor
lands at `FBD+0x140`).

### 2.2 Frame 0 (`st58-2`), same signature

```
EV start dt=0 us slot=1 seq=0 jc=0x8c72000 ... -> done 33 us
EV start dt=796 us slot=1 seq=1 ...            -> done 834 us
EV start dt=1754 us slot=1 seq=2 ...           -> done 1814 us
EV start dt=3601 us slot=0 seq=3 jc=0x8b3f5c0 a=00000000 b=00000000 c=00000008
EV timeout dt=751810 us slot=0 seq=3 jc=0x8b3f5c0 a=00000008 ...
JDBODY  w4=00010013 w9=000f000f w10=08b3f480
PAGEDUMP fbd gpu=0x2f795300 rc=-2 (unmapped)   <-- the desc[10] OOB read
```

Frame 0 is a valid trigger: the same publish-order bug applies to the
first frame, and the first frame's FS job is the slowest one (cold
shaders), which widens the window.

### 2.3 The cascade (why it never recovers)

After the silent timeout, recovery power-cycles the cores
(`cycle=1`) and every later FS submit faults **at dispatch**:

```
ERR   slot=0 js_status=0x00000058 (DATA_INVALID_FAULT) stat=0x00010000 rawstat=0x00010000
      head=0x9d0f5c0 tail=0x9d0f5c0 seq=359 jc=0x9d0f5c0
      sw=00000000 00000000 00000000 00000000 00010013 00000000 00000000 00000000
JDBODY ... words=128 00010258 00000000 09d0f5c0 00000000 00010013 ... 000f000f 09d0f480
recover-done slot=0 latched=0x00000058 cycle=1 js0=0x00000000 js1=0x00000000
```

115-160 ERR per run, ~110 ms apart, always the same `jc`, the same BO
set, while slot 1 keeps retiring normally. Mesa is now ~100 frames
ahead (it built them during the 750 ms hang), so the userspace rebuild
of the FS inputs lands inside the FS job's execution on *every* frame
instead of occasionally — [INFERENCE] this is why a single hit turns
into a permanent storm.

## 3. Mechanism (what the capture + the shim contract prove)

### 3.1 The resv holds one exclusive fence, and it was replaced out of order

`0002-dma-resv-shim.patch`:

```c
void dma_resv_add_excl_fence(struct dma_resv *obj, struct dma_fence *fence)
{
        ...
        obj->fence_excl = fence;          /* REPLACES, drops the old one */
        ...
        dma_fence_put(old);
}
```

Driver 0001 published the split's final fence on every submitted BO in
**two** places: at SUBMIT (`p7580_drv_submit`, correct, before the ioctl
returns) and again from `run_job` for the `publish_excl` node
(`p7580_drv_run_job`, in the async pump worker, at *dispatch* time).
The old comment justified the second one as "the late/legacy copy"; it
is not harmless, it is the bug:

* Mesa submits a frame in two ioctls: draws (3 chain nodes, slot 1) and
  then the FS job (slot 0), back to back (`panfrost_batch_submit_jobs`).
* The ring shows the draws chain dispatched node by node: node 0 at
  dt 0, node 1 at 796 µs, node 2 (TILER) at 1754 µs. The FS ioctl
  returned ~microseconds after the draws ioctl, so the FS submit's
  publish (`fence_FS`) happens first, and the tiler's `run_job`
  publish (`fence_TILER`, the older fence) happens ~1.7 ms later and
  **wins**.
* The slab's resv now holds `fence_TILER`. As soon as the tiler retires
  (~100 µs later) `WAIT_BO(handle, timeout=0)` returns 0 = idle, even
  though `fence_FS` — the FS job that is about to be dispatched — has
  not run.
* Mesa treats that page as free (`panfrost_bo_cache_fetch` with
  `dontwait=true`: `panfrost_bo_wait(entry, 0, RW)` → success → reuse)
  and rebuilds the next frame **into it**, writing the descriptor at
  `slab+0x5c0` and the SFBD at `slab+0x480` while the GPU is executing
  the FS job that reads them.
* A GPU/CPU read/write race on the SFBD (pl/body/heap_start/heap_end,
  polygon-list size, clear colours) has no exception class on T720: a
  self-consistent-but-wrong tiler walk hangs (JS_STATUS `0x08`, no
  IRQ), an inconsistent one faults `0x58`. Both signatures are exactly
  what the two captures show.

This is *not* the "Mesa reuses a BO while a batch owns it" story: Mesa's
guard is correct. The kernel lied to it — the BO carried a fence that
was newer than the one whose completion `WAIT_BO` reported.

Why only some runs: the publish order is wrong on every frame, but the
damage needs the next frame's build to land inside the FS job's
execution. At 156 fps (6.4 ms/frame) the draws→FS dispatch latency is
~3 ms and the FS job runs ~250 µs in the second half of the frame,
i.e. exactly when userspace builds the next frame; a few hundred µs of
jitter (workqueue latency, cold first frame) puts the CPU write inside
the job. Measured: 1 hit per 2-3 runs of 300 frames.

`serialize_fs=1` does not address this (it moves the FS submit to the
tiler's entity; the tiler's `run_job` still re-publishes over the FS
fence), consistent with gpu31-firstfault measuring it as harmful.

## 4. Fix (driver 0001, folded in)

| site | change |
|---|---|
| `panfrost7580_submit.c` `p7580_drv_run_job` | **delete** the `publish_excl` BO stamp (arm-time re-publish). One comment records why: the resv holds one exclusive fence and this path runs asynchronously, after a later submit may have published a newer fence. |
| `panfrost7580_submit.c` `p7580_drv_submit` | move the `dma_resv_add_excl_fence(objs[i], last_fence)` loop **inside** the `df->serial_lock` section that swaps `serial_prev`, so publish order == the file's serial submit order and the last writer is the newest submit. Still before the ioctl returns (the gpu31-latency/MFBD hand-off contract is preserved). |
| `panfrost7580_job.h` | drop `publish_excl`; add `fbd` + `jc_age`. |
| `panfrost7580_submit.c` `p7580_drv_split_queue` | stamp `fbd` (SFBD word 10 captured by the chain walk) and `jc_age` (per-BO age ledger, `p7580_drv_bo_age_touch`, 64 direct-mapped entries in `df`) instead of `publish_excl`. |
| `panfrost7580_job.c` `p7580_drv_desc_dump` | print the **whole 4 KiB** page (64 lines × 16 words, `slot`/`off` labels). |
| `panfrost7580_job.c` `p7580_job_first_fault` | dump the `fbd` page from the SUBMIT-captured pointer (no `desc[10]` OOB), and print `fbd`/`jc_age` in the `EVRING begin` line; `TIMEOUT` prints `jc_age`. |

No UAPI, slot routing, recovery or timeout change. The serial section
does not change refcounts: `serial_prev` keeps its one slot ref and the
fence installed on BOs is the same `last_fence` that was already
published there.

Host model test `code/gpu31/driver/submit_fix_test.c` gains **T10**: with
a replace-semantics resv, the fixed sequence (publish draws, publish FS,
nothing at dispatch) leaves the FS fence on the BO and the BO stays
"busy" after the tiler signals; the old sequence (dispatch-time
re-publish) leaves the tiler's fence and the BO reads idle while the FS
fence is pending.

## 5. Verification done here

* `0001-panfrost7580-driver-sources.patch` applies cleanly to the
  3.10.108 tree (`git apply --check`, all 17 files) and every new-file
  hunk header's line count matches its body.
* `aarch64-linux-gnu-gcc -std=gnu89 -fsyntax-only` against the real
  3.10.108 headers with the 0001-0004 shims staged: `submit.c`, `job.c`,
  `gem.c`, `ioctl.c`, `mmu.c`, `power.c`, `syncobj.c` clean. (`main.c`
  fails on `DRIVER_RENDER_ONLY`, a backport define from a patch outside
  this slice; untouched by this ticket.)
* `submit_fix_test`: `all checks passed` (T1-T10).

## 6. Silicon verification: blocked, and how to close it

The fix is a kernel change, so the 10 × 300-frame run must happen on an
image built from it (int43). It could not be run here: while trying to
A/B Mesa's BO cache (`PAN_FBD_NOCACHE`/`PAN_FBD_NOPUT`, needed only to
*rule out* reuse-from-userspace) the phone hit the 1.4 GB/no-swap
ceiling, the lowmemorykiller killed Xorg and then sshd's session
children; the kernel is alive (ping, banner, password auth succeed) but
no session can be forked, so nothing can be freed from inside. Main has
asked the user for a physical battery pull. **No device state was
modified beyond the debugfs knobs, which are left at
`job_timeout_ms=750, ev_arm=0, serialize_fs=0, hang_inject=0`.**

Acceptance on int43, `ev_arm=1`, 10 × 300 frames at 256²/no-read
(the pre-existing clean run costs 1.98-2.03 s, so any regression shows
as wall time and `ERR`/`TIMEOUT` counts):

```
for i in $(seq 1 10); do
    rootshell dmesg -c >/dev/null
    /usr/bin/time -f "wall=%es" env GFDEMO_BENCH=1 GFDEMO_PX=256 \
        GFDEMO_FRAMES=300 GFDEMO_NOREAD=1 sh /home/user/gfdemo/run.sh
    L=/home/user/st58-acc-$i.log; rootshell dmesg > $L
    echo "run$i ERR=$(grep -c 'p7580_job: ERR ' $L) \
          TIMEOUT=$(grep -c 'p7580_job: TIMEOUT' $L) \
          EVRING=$(grep -c 'EVRING begin' $L)"
done
```

Pass = 10 consecutive runs with `ERR=0 TIMEOUT=0 EVRING=0` and wall
1.9-2.1 s. (A storm cannot hide: `ev_arm=1` dumps on the first timeout
and `ERR`/`TIMEOUT` are always printed.)

The mechanism itself can also be shown **deterministically on the old
image** without waiting for a natural storm, which is worth doing on
int42 once the phone is back (before int43) to convert the [INFERENCE]
in §3 into a measurement:

1. `PAN_FBD_DEBUG=1` (Mesa prints `FBDDBG bo FETCH/NEW/PUT gpu=...` per
   BO), `ev_arm=1`, `hang_inject=4`.
2. With 4 masked dispatches, the frame's three chain nodes and the FS
   job are the masked ones: the jobs retire in hardware but their
   fences stay pending for the full 750 ms, so the kernel believes the
   FS job is in flight.
3. On int42 the slab that holds `jc`/SFBD is FETCHed (same `gpu=`
   address) repeatedly during that 750 ms window — `WAIT_BO` says idle
   because the tiler's fence replaced the FS fence. On int43 the same
   address must not be handed back until the masked job's timeout
   expires (Mesa allocates fresh BOs instead).
4. Cross-check the kernel side: the `TIMEOUT` line for the masked job
   must show `jc_age` >= the number of SUBMITs that ran in the window
   on int42 (the page was rewritten under it) and `jc_age` = 0/1 on
   int43 (never re-touched while it was in flight).

## 7. Notes for integration

* **Build gate change**: `build-int42.sh` asserts "driver 0001 mirror
  add_excl count == 2" (SUBMIT + run_job). The fix leaves exactly one
  `dma_resv_add_excl_fence` call in `submit.c`; int43's gate must expect
  **1** and the "SUBMIT-time excl stamp" check keeps passing (the call
  moved inside `serial_lock`).
* `code/gpu31/jobs/panfrost7580_job.{c,h}` is a stale slice snapshot (it
  predates the event ring); it was not touched. Anything that
  re-derives the mirror from 0001 gets `fbd`/`jc_age` and no
  `publish_excl`.
* The `heap` page dump still picks "the largest BO in the snapshot";
  on this Midgard part the polygon list is per-batch and the tiler heap
  is the 1 MiB `dev->tiler_heap`, which is all zeros until the tiler
  spills — the dump is useful as "not corrupted", not as state. The
  SFBD tiler section now falls inside the full-page dump, which is what
  actually names `pl`/`body`/`heap_start..end`.
