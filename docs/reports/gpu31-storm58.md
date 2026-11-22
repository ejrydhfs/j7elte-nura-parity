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
2. **The BO was in flight and the kernel told userspace it was idle.**
   Two independent defects, both in the same contract:
   * **WAIT_BO mapped "budget ran out" to success.** The handler
     returned 0 for both `dma_resv_wait_timeout()` `0` (timeout) and
     `>0` (idle); only `<0` became an error. Mesa's BO-cache fetch
     polls with `panfrost_bo_wait(bo, 0, RW)`, which is exactly the
     0-jiffy wait, so **every cached BO was handed back as ready**,
     fence or no fence. Measured on int42 (§5.1): with one frame's
     dispatches IRQ-masked and a 60 s budget, the same GEM BO
     (`h=6`, 64 KiB, `gpu=0x24bd000`) was PUT to the cache and FETCHed
     back **4 times inside that 60 s window** while its submit's fence
     had not signaled.
   * **BO exclusivity was published twice, out of order.** The 0002
     resv shim's `dma_resv_add_excl_fence()` **replaces** the single
     exclusive fence, and driver 0001 re-published it from `run_job`
     (async, at dispatch). In a frame the FS submit publishes its
     (newer) fence first; the draws submit's **tiler** then dispatches
     ~1.7 ms later and re-publishes its **older** fence over it, so a
     truthful WAIT_BO would still have seen the slab idle once the
     tiler retired while the FS job was queued/executing.
   Together: Mesa rebuilds the next frame into the page the GPU is
   reading (descriptor at `slab+0x5c0`, SFBD at `slab+0x480`, tiler
   section at `slab+0x540`, polygon list named by that section) — a torn
   SFBD is the silent hang; and once the frame pipeline is deep, every
   frame is hit.
3. **Fix (driver 0001)**: (a) `panfrost_ioctl_wait_bo()` maps
   `dma_resv_wait_timeout() == 0` to `-ETIMEDOUT` (v5.2 contract);
   (b) BO exclusivity is published **only at SUBMIT**, in submit order —
   the `dma_resv_add_excl_fence` loop now lives inside the
   `df->serial_lock` section that swaps `serial_prev`; the arm-time
   (`run_job`) re-publish is deleted, `publish_excl` is gone. Host model
   tests T10 (publish order) and T11 (wait mapping) pin both.
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
5. **The `0x58` cascade is not this ticket's bug.** Masking the IRQs of
   one frame with `hang_inject` (no memory corruption at all, the jobs
   retire in hardware) reproduces the same "every later FS job faults
   `0x58` at dispatch, ~113 ms per recovery" sequence. That is the
   pre-existing post-recovery defect already reported by
   gpu31-firstfault/gpu31-readback: it is entered by *any* FS timeout, so
   removing the first fault removes the entry point, but the cascade
   itself is unchanged here.

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

Frame 0 is a valid trigger: both defects apply to the first frame, and
the first frame's FS job is the slowest one (cold shaders), which widens
the window.

### 2.3 The cascade (and why it is a separate defect)

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
set, while slot 1 keeps retiring normally.

**Measured separately (int42, §5.2): masking the IRQs of one frame's
four dispatches with `hang_inject=4` — the jobs retire in hardware, no
memory is corrupted by anyone — produces the same cascade** (29 ERR /
33 TIMEOUT after four masked dispatches, same `jc`, every later FS job
`0x58` at dispatch). So the permanent `0x58` cycle is the known
post-recovery defect (gpu31-firstfault §0.4, gpu31-readback §3), entered
by *any* FS timeout; it is not evidence of the torn SFBD and this ticket
does not change it. What this ticket removes is the *first* fault, and
with it the entry point.

## 3. Mechanism (what the capture + the shim contract prove)

### 3.1 `WAIT_BO` reported a timeout as success (measured)

`panfrost_ioctl_wait_bo()` (driver 0001 before the fix):

```c
	timeout = p7580_gem_wait(dev, file_priv, args->handle, timeout, 1);
	if (timeout < 0)
		return (int)timeout;
	return 0;                     /* <- also returns 0 for timeout == 0 */
```

`dma_resv_wait_timeout()` returns `>0` = idle (jiffies left), `0` = the
budget ran out, `<0` = error. v5.2 `panfrost_wait_bo` maps `0` to
`-ETIMEDOUT`; this handler only mapped `<0>`. The non-blocking form of
the ioctl is `timeout_ns = 0` → "past absolute stamp" → 0 jiffies → and
that is exactly the call Mesa's BO-cache fetch makes:

```c
/* pan_bo.c: "check the cache but without waiting for BO readiness" */
if (!panfrost_bo_wait(entry, dontwait ? 0 : INT64_MAX, PAN_BO_ACCESS_RW))
        continue;                                   /* skip: busy */
...
ret = drmIoctl(bo->dev->fd, DRM_IOCTL_PANFROST_WAIT_BO, &req);
if (ret != -1) { bo->gpu_access = 0; return true; }  /* "ready" */
```

With the ioctl returning 0, `ret != -1` → the BO is declared ready and
spliced out of the cache, whatever its fences say. Every pool/scratch/
polygon-list/render-target BO is therefore reusable while an in-flight
batch still owns it.

### 3.2 The resv holds one exclusive fence, and it was replaced out of order

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
is not harmless, it is the second bug:

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

Mesa's guard is correct in both halves; the kernel lied to it. 3.1
alone is sufficient for reuse (the poll always says "ready"), and 3.2
makes a truthful answer impossible as well (the BO carries a fence that
is older than the job actually using it), so both must be fixed.

Why only some runs: with a *correct* fence the damage still needs the
next frame's build to land inside the FS job's execution, and the 3.1
poll then decides. At 156 fps (6.4 ms/frame) the draws→FS dispatch
latency is ~3 ms and the FS job runs ~250 µs in the second half of the
frame, i.e. exactly when userspace builds the next frame; a few hundred
µs of jitter (workqueue latency, cold first frame) puts the CPU write
inside the job. Measured hit rate: 1 storm per 2-3 runs of 300 frames.

`serialize_fs=1` does not address this (it moves the FS submit to the
tiler's entity; the tiler's `run_job` still re-publishes over the FS
fence), consistent with gpu31-firstfault measuring it as harmful.

## 4. Fix (driver 0001, folded in)

| site | change |
|---|---|
| `panfrost7580_ioctl.c` `panfrost_ioctl_wait_bo` | map `dma_resv_wait_timeout() == 0` to `-ETIMEDOUT` (v5.2 contract). Without it the 0-jiffy poll Mesa's BO-cache fetch uses always answers "ready". |
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

Host model tests `code/gpu31/driver/submit_fix_test.c` gain **T10**
(publish order: with a replace-semantics resv, the fixed sequence leaves
the FS fence on the BO and the BO stays "busy" after the tiler signals;
the old dispatch-time re-publish leaves the tiler's fence) and **T11**
(wait mapping: 0 -> -ETIMEDOUT, >0 -> idle, <0 -> propagated).

## 5. Verification

### 5.1 Host checks

* `0001-panfrost7580-driver-sources.patch` applies cleanly to the
  3.10.108 tree (`git apply --check`, all 17 files) and every new-file
  hunk header's line count matches its body.
* `aarch64-linux-gnu-gcc -std=gnu89 -fsyntax-only` against the real
  3.10.108 headers with the 0001-0004 shims staged: `ioctl.c`,
  `submit.c`, `job.c` (and `gem.c`, `mmu.c`, `power.c`, `syncobj.c`)
  clean. (`main.c` fails on `DRIVER_RENDER_ONLY`, a backport define from
  a patch outside this slice; untouched by this ticket.)
* `submit_fix_test`: `all checks passed` (T1-T11).

### 5.2 Silicon: the BO is handed back while the submit is in flight

Deterministic, on int42 (the *unfixed* image), script
`storm58/proof2.sh` on the external disk; logs `st58-proof2.{log,out}`:

```
set: serialize_fs=0 ev_arm=1 hang_inject=4 job_timeout_ms=60000
5 frames, PAN_FBD_DEBUG=1
ring:   EV start   slot=1 seq=0 jc=0x25f0000  a=0 b=0 c=0x08
        EV timeout slot=1 seq=0 jc=0x25f0000  dt=60137825 us
        hang_inject slot=1 seq=0/1/2 ... (the 4th, slot 0, was never
        dispatched: the chain below it was still pending)
out:    FBDDBG bo PUT   gpu=0x24bd000 h=6 size=65536
        FBDDBG bo FETCH gpu=0x24bd000 h=6 size=65536 flags=0x0
        (four PUT/FETCH pairs, lines 31/33 50/52 69/71 88/90)
```

One frame's four dispatches are IRQ-masked and the budget is 60 s, so the
first frame's submit fence is provably pending for the whole client run
(the ring's single timeout lands 60.1 s after its START). Inside that
window the *same GEM BO* (handle 6, the 64 KiB slab that holds the FS
descriptor and SFBD) is returned to Mesa's cache and re-fetched four
times. That is the invariant violation, measured, with no storm and no
corruption involved.

### 5.3 The `0x58` cascade is reproduced without any corruption

Same script, `job_timeout_ms=750` (proof run 1, `st58-proof.{log,out}`):
4 masked dispatches (slot 1 seqs 0-2 = the draw chain, slot 0 seq 3 =
the FS job), each `TIMEOUT ... rawstat=0x2/0x1` (the IRQ was masked; the
jobs retired in hardware), `js_status=0`, descriptors intact. After the
first recovery the run continues with `TIMEOUT slot=0 js_status=0x58`
for seqs 7, 11, 15, ... every ~113 ms — 29 ERR / 33 TIMEOUT in 30
frames. So the cascade only needs an FS timeout, not a torn SFBD.

## 6. Silicon acceptance (int44) and current state

Builds: int43 (`b769c29`, publish-order half only, driver sha `58972560…`,
boot img `f9b6f10f…`) is superseded; **int44 is the acceptance image** —
`88e011a`, driver sha `1c295834…`, 28/28 gates pass, boot
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/kernel-boot-int44.img`
(27 348 992 B, sha256
`e32de3ac213b4f7d843a299ecc2728b6f0fb07e924c9ce5bc2dc9b45e617155b`),
Image `Image-int44` (18 209 976 B, sha256
`bf834883694a588b2b9af240896b1354eb5ba42674f03ab17ab7e70fdbe8c6a9`).
int44's gate 6ae scopes the new mapping to the
`if (timeout == 0)` -> `return -ETIMEDOUT;` pair in
`panfrost_ioctl_wait_bo()` (driver 0001 already has two unrelated
`-ETIMEDOUT` returns in the MMU/GEM wait helpers, so a global grep is
wrong); `ioctl.o` shows `bl p7580_gem_wait` followed by
`mov w1, #0xffffff92` (-ETIMEDOUT) in the next slot.

int44 is flashed and boots (`3.10.108 #37 17:29:16`; phone
`dmesg`/`ls` run on UTC while the banner is build-time EST, so the
22:31:38 UTC reboot = 17:31:38 EST, i.e. the flash reboot right after
the 17:29:25 EST Image). The first acceptance attempt produced **no valid
datum**: that reboot landed in the middle of section A (runs 1-4
straddle the old and new image; runs 7-10 hit a booting system with
0.1-0.9 s walls and no BENCH
line), and after that boot the phone was in boot-time memory pressure
(shmem 1.2 GB unevictable, load 12.8 with Xorg/fbwin in D, 23+
lowmemorykiller kills, rootfs 96 % full): every `gfdemo` launch was
SIGKILLed within 0.1-0.3 s even at 3 frames, and sshd closed most
sessions right after the session-open. The kill is the LMK, not an
int44-induced abort: the WAIT_BO mapping can only surface through an
ioctl return (a Mesa error print and a normal nonzero exit), whereas the
observed termination is SIGKILL with `lowmemorykiller` /
`... Killing 'Xorg' ...` lines in dmesg (grep count 23 within the first
two minutes of that boot). Device work was stopped (Main);
acceptance re-runs on the next boot with `storm58/accept44.sh`.

### 6.1 Why even a clean 10x300 cannot fit yet (separate defect)

Measured by Main on int44 while chasing the `gfdemo` SIGKILLs: the
kernel leaks **~1.16 MB of shmem pages per frame** (389 MB after one
300-frame run, +35 MB per 30-frame run, persisting after the client
exits; `gem_names` empty, so pages, not GEM objects). One 300-frame run
costs ~350 MB on a 1.4 GB/no-swap part, which is what starved the phone
and starved earlier attempts at this acceptance. It also explains why the
"thrash" state returned between boots without any client running.

That leak is **not** touched by this ticket and is owned by G31_BOLeak
(page refs / mmap pin-unpin / free path in driver 0001). Nothing in this
patch allocates per submit (`fbd`/`jc_age` are fixed fields, the age
ledger is a 64-entry array inside `p7580_drv_file`, and the extra
`p7580_drv_gpu_read()` for the SFBD word takes and drops exactly one GEM
ref). The 10x300 acceptance therefore runs on the kernel that carries
the leak fix (int45); the storm proof in §5 does not depend on it (§5.2
used a 60 s fence window, not memory pressure, and §2's first faults
happened at frames 0 and ~88, long before the leak could matter).

Note for the leak work: the per-frame BO set is *identical* every frame
in every storm capture (same `h6` slab, same IOVA, same 10-BO map), so
the growth is in per-page refs, not in new BOs — consistent with
Main's `gem_names` observation.

Acceptance command (Main runs it on the flashed image), `ev_arm=1`, the
pre-existing clean cost is 1.98-2.03 s so any regression shows as wall
time plus `ERR`/`TIMEOUT`:

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
1.9-2.1 s (a storm cannot hide: `ev_arm=1` dumps on the first timeout and
`ERR`/`TIMEOUT` are unconditional prints).

Optional confirmation that the WAIT_BO fix bites, on the new image:
`PAN_FBD_DEBUG=1` + `hang_inject=4` + `job_timeout_ms=60000` — the slab
that holds `jc` must show **no** `bo FETCH` while the masked fence is
pending (Mesa allocates fresh BOs instead), in contrast to the four
fetches in §5.2.

Device state at hand-off: int42 was left with
`job_timeout_ms=750, ev_arm=0, serialize_fs=0, hang_inject=0` before the
int43 flash; the earlier OOM (no-cache A/Bs) was cleared by the battery
pull Main arranged. No reboots/unbinds were done by this ticket.

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
