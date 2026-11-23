# gpu31-fpssplit — 155 fps restored: a 0-jiffy poll of an IDLE fence answered 0, so WAIT_BO told Mesa every cached BO was busy

Date: 2026-11-22. Owner: G31_FpsSplit. Mesa 20.3.5 at
`/data/mesabuild/mesa20-b`, client `gfdemo` / `mfbd/rdtest`. All device
work under `flock /tmp/gpsdev.lock` as root. Driver source of truth
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (unchanged by
this ticket: `04db08d` driver, `98640d5` WAIT_BO 0→-ETIMEDOUT). This
ticket changes the **dma-fence / dma-resv backport shims** only:
commit `5950b7e` (+ `d2c82ab` new-file hunk counts, by G31_Integrator41),
built as int47 (`Image 2a5e9416…`, boot `80ff0f9e…`).

## 0. Verdict

1. **The 155→101 fps drop is one wrong return value in the 3.10
   dma-fence shim.** `dma_fence_wait_timeout()` short-circuits an
   already-signaled fence and returned the bare budget, so a
   **0-jiffy poll of an idle fence returned 0** — the same value as
   "budget ran out". `panfrost_ioctl_wait_bo` correctly maps 0 to
   `-ETIMEDOUT` (gpu31-storm58), so Mesa's BO-cache probe
   (`panfrost_bo_wait(bo, 0, RW)`) was told **every idle BO was busy**:
   `panfrost_bo_cache_fetch()` never reused anything and each frame
   allocated (CREATE_BO + SHMEM pages + map_bo + mmap) and later freed
   a full BO set.
2. **Attribution (int46prof, 256²×300f, per-class ktime counters):** the
   render thread burned **5.2 ms/frame in sys**, of which **WAIT_BO was
   4.88 ms/frame** (794 polls/frame × 6.0 µs); CREATE_BO 0.91,
   map_bo 0.68, SUBMIT 2.05 (wall, mostly pipeline), MMAP 0.004.
   Shmem grew to **235 MB** mid-run (BO churn), 94–110 fps.
3. **Fix (mainline `dma_fence_default_wait` contract
   `timeout ? timeout : 1`):** a signaled fence now returns a positive
   remainder for any budget (1 for a 0-jiffy poll); a pending fence
   still returns 0 → WAIT_BO still `-ETIMEDOUT`; an **empty resv** is
   idle (>0). Two one-line changes + comments in shim 0001/0002, pinned
   by 3 new host checks (412 checks × native + qemu-aarch64, 0 failures).
4. **Silicon (int47):** WAIT_BO **4.3 calls/frame** (all blocking,
   1.09 ms each = the GPU), CREATE_BO **1.0/frame** (cache hits),
   client sys **1.7 ms/frame** (was 5.2), Shmem **flat 3.1–3.3 MB** and
   bounded ≤12.5 MB mid-run (was 235 MB). fps: 8 consecutive 300f runs
   **155.8 / 154.9 / 155.4 / 152.4 / 152.8 / 153.9 / 153.5 / 131.6**;
   an independent locked matrix by Main: 155.7 / 107.6 / 155.0 / 157.4 /
   154.4 / 158.1. Direct probe `WAIT_BO(handle, timeout_ns=0)` on a
   fresh BO returns success on int47 (it returned `ETIMEDOUT` when the
   cache never hit).
5. **Churn SIGSEGV is the client, not the kernel**: fault address was a
   hole in `/proc/self/maps` at a *successful* mmap() return, and the
   op ring showed the same `(map,size)` munmapped twice by a thread that
   could not own the destroyed VMA. The client unmapped a BO in its
   first cleanup loop without clearing `bo[i].map`, so the second loop
   munmapped the range again and could destroy another thread's brand
   new mapping. Fixed client committed (`code/gpu31/stress/churn.c`):
   6/6 1000-iteration runs ok on the fixed images, 0 kernel faults.
6. **Residual (not this fix's regression):** the silent `js_status
   0x08` hang survives int47 at roughly **1 per 4–6 300-frame runs**;
   each costs the 750 ms sched timeout + a recovery power-cycle and
   drops that run (and often the next) to 90–133 fps with the client's
   sys rising back to 8.5 ms/frame (BO-cache misses behind the slow
   GPU). It also occurs with the Mesa cache disabled and on the pre-fix
   image (int46measure 512×10 had 2 ERR/TIMEOUT), so it is the known
   storm class, not a shim regression; int48's `submit_err` attribution
   is needed to close it. Details §6.

## 1. Instrumentation (measurement-only, never committed)

Per-class ktime counters in `struct p7580_drv` (indices: 0
`p7580_gem_create`, 1 `p7580_shmem_alloc`, 2 `p7580_drv_map_bo`, 3
SUBMIT ioctl hot block, 4 `.mmap`, 5 WAIT_BO (`p7580_gem_wait`), 6
`p7580_gem_free_object`, 7 sched `free_job`, 8 `job_done_sync`, plus 9/10
ioctl wrappers unused and 11 `p7580_drv_submit` core). One windowed
`pr_info` every 100 SUBMITs prints `class=calls/ns-per-call`; 100 submits
= 50 frames (measured 2 submits/frame). Windows were captured by
streaming `/dev/kmsg` (`cat /dev/kmsg > file &`), because the 128 KB
kernel ring loses the first windows to the always-on `p7580_pin` probes.

Images: `Image-int46prof` (`bafeea1e…`, pre-fix shims) and
`Image-int47prof` (`e2b0c04f…`, fixed shims) — the same driver as
int46/int47 plus this instrumentation. The shipped acceptance image is
int47 (no instrumentation).

## 2. Baseline: where the 5.2 ms/frame sys went (int46prof, pre-fix)

18 steady windows, 256²×300f, medians; calls/frame = calls/50:

| class | calls/frame | ns/call | ms/frame | note |
|---|---|---|---|---|
| create | 4.1 | 216 491 | **0.91** | every frame allocates a new BO set |
| shmem_alloc | 4.1 | 213 748 | 0.89 | nested in create; 272 pages + `__flush_dcache_area` |
| map_bo | 21.0 | 32 399 | 0.68 | 21 BOs/submit walked; bo_list grows with churn |
| SUBMIT | 2.0 | 1 025 930 | 2.05 | wall; includes queue/serial |
| MMAP | 1.0 | 4 222 | 0.00 | one new mapping/frame |
| WAIT_BO | **794.1** | **5 992** | **4.88** | **render thread, pure poll storm** |
| free_object | ~1 (bursty) | ~57 000 | 0.06 | frees lag their windows |
| free_job | 4.0 | 1 373 924 | 5.50 | kworker |
| job_done_sync | 80.0 | 106 473 | 8.52 | kworker (also pre-signal) |

Timeline run: 300 frames 2.94 s, **user 0.22 s / sys 1.61 s**
(1200 GPU IRQs) = 5.4 ms/frame sys; Shmem 1.1 MB → **235 MB** mid-run.
fps 93.7 / 99.3 / 94.5, 512² readback 10f `errtimeout=2`.

Why 794 polls/frame: each cache miss leaves a BO in Mesa's bucket, so
the bucket grows; `panfrost_bo_cache_fetch()` scans every same-size
entry with a 0-jiffy WAIT_BO, and every one of them answered
"-ETIMEDOUT". The poll count *is* the bucket size.

## 3. Root cause, byte for byte

3.10 has no dma-fence; shim `0001` implements it.

```
dma_fence_wait_timeout(fence, intr, timeout):
        if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
                return timeout;              /* <- 0 for a 0-jiffy poll */
```

Mainline's contract is the remaining budget, minimum 1:
`dma_fence_default_wait()` starts with `signed long ret = timeout ?
timeout : 1` and returns it for a signaled fence. Returning 0 makes
"idle" and "timed out" indistinguishable to `dma_resv_wait_timeout()`;
gpu31-storm58 then (correctly) maps 0 → `-ETIMEDOUT`, so the cache probe
could never succeed. int41 was fast precisely because the poll always
answered "ready" (the same lie that caused the storms); int44 made the
answer honest but, with this shim, always "busy".

## 4. Fix and host evidence

* `0001-dma-fence-minimal-subset.patch`: signaled short-circuit returns
  `timeout ? timeout : 1`; `dma_fence_default_wait()` gained mainline's
  early signaled check with `ret = timeout ? timeout : 1`.
* `0002-dma-resv-shim.patch`: remainder initialised
  `timeout ? timeout : 1`, and kept for a resv with **no fences** (an
  empty resv is idle); a waited fence set still reports the remaining
  budget, so pending → 0 → WAIT_BO `-ETIMEDOUT`.
* Mirrors: `code/gpu31/series/*`, `code/gpu31/sched/{src,patches}/*`;
  patch body == `sched/src` byte-for-byte for both files.
* Host: `code/gpu31/sched/tests/sched-host-test.c` +3 checks —
  signaled+0 → 1 (>0), pending+0 → 0, empty resv+0 → >0.
  `make check`: **412 checks, 0 failures** native and qemu-aarch64;
  gates + 3.10 `-fsyntax-only`/`-c` kcheck PASS.
* `d2c82ab` fixed the new-file hunk counts (253/307) I under-declared;
  int47's pre-apply gate now scans every applied patch.

## 5. Silicon after the fix

Direct semantic probe on int47 (`waitprobe`, fresh BO, empty resv):
`WAIT_BO(handle=1, timeout_ns=0) ret=0 errno=0` →
`VERDICT: idle BO reported READY`.

Per-class (int47prof, 18 steady windows, same units):

| class | calls/frame | ns/call | ms/frame | delta |
|---|---|---|---|---|
| create | **1.0** | 22 727 | **0.02** | −0.89 |
| shmem_alloc | 1.0 | 18 560 | 0.02 | −0.87 |
| map_bo | 21.0 | **1 840** | 0.04 | −0.64 |
| SUBMIT | 2.0 | 696 336 | 1.39 | −0.66 |
| MMAP | 1.0 | 4 578 | 0.00 | — |
| WAIT_BO | **4.3** | **1 091 378** | 4.76 | 794 polls → 4.3 *blocking* waits for the GPU (wall, not CPU) |
| free_object | 1.0 | 38 956 | 0.04 | — |
| free_job | 4.0 | 1 399 623 | 5.66 | kworker |
| job_done_sync | 80.0 | 106 969 | 8.70 | kworker (unchanged) |

Client CPU (timeline, 300f): **user 0.12 s / sys 0.51–0.54 s** =
**1.7–1.8 ms/frame sys** (was 5.4); GPU IRQs 1199. Shmem mid-run
**1.8–9.9 MB** (was 235 MB); before/after a run 3.1–3.3 MB.

fps, 256²×300f NOREAD, plain runs, quiet window:

* int47, 8 back-to-back runs: **155.8 / 154.9 / 155.4 / 152.4 / 152.8 /
  153.9 / 153.5** / 131.6, 0 TIMEOUT/ERR, Shmem 3.1–3.3 MB flat.
* Main's independently locked int47 matrix: 155.7 / 107.6 / 155.0 /
  157.4 / 154.4 / 158.1.
* Pre-fix control, same harness: 93.7 / 99.3 / 94.5.

Acceptance 3×300f: runs 1–3 of the 8-run series are **155.8/154.9/155.4
fps with 0 ERR/0 TIMEOUT**, Shmem flat — the ≥150 target is met.
(Second independent sequence 158.0/153.9/152.8 on int47prof.)

**10×512² readback correct:** `rdtest --size 512 --frames 10 --read 1
--pre-finish 0` with `ev_arm=1`: `TOTAL frames=10 ok=10 zero=0
mismatch=0`, 0 joberr; repeated clean (3/3 clean in one session, 2/3 in
another where the residual hang hit one run).

**Churn clean:** `code/gpu31/stress/churn.aarch64 4 250 6` ×3 on int47:
`CHURN ok iters=1000 threads=4 errs=0 stale_reused=55/76/86`; kernel
`faults=0`, `Could not find map=1372` (the deliberate stale-offset
probes returning -EINVAL). Also 3/3 ok on int47prof.

## 6. The churn SIGSEGV and the residual silent hang

### 6.1 Churn SIGSEGV = client bug (resolved)

Crash (4 threads × 250 iters × ≤6 BOs): `SIG11 addr=0x7fb5f05000
code=0x1` (SEGV_MAPERR); `/proc/self/maps` had **no VMA at that
address** (a hole between two live `renderD128` mappings), yet the op
ring showed `mmap ... map=0x7fb5f05000` succeeded and `touch0` at that
exact address faulted. The same ring also contained two `munmap1` of one
`(map,size)` from another thread, and a 1 MiB `munmap1` of
`0x7fb5e25000..0x7fb5f25000` covers `0x7fb5f05000`. Mechanism: the
first cleanup loop unmapped without clearing `bo[i].map`, the second
loop munmapped again, and the window between the two (which contains the
stale-offset mmap probe) is long enough for another thread to be given
the freed address range — the stale munmap then destroys that new VMA.
The kernel log stayed clean throughout (no `dead0000`, no oops, no
`MMAP_BO` error), and the sequential phase always passed. Fix:
`bo[i].map = NULL` after every munmap (committed client). 6/6 runs
(4 threads × 250 iters × 6 BOs: 3 on int47prof, 3 on int47) pass
afterwards, userspace and kernel.

### 6.2 Residual silent 0x08 hang (open, measured, not this shim)

With the polls honest, BO reuse resumes and the storm58 class still
fires occasionally: `p7580_job: TIMEOUT slot=0 js_status=0x00000008
(NO_EXCEPTION)` with `head == tail`, followed by `recover-done …
cycle=1` (a full power-cycle recovery, 750 ms budget). Measured with
kmsg capture: 1 hang in 6×300f (int47prof); 1 + 2 hangs in three 300f
runs in one ev_arm-armed window (133.4 / 92.3 / 99.8 fps); Main reports
2/4 runs ending in Mesa `Assertion failed: !ret` after a silent timeout,
and 512² readback ×30 shows occasional hangs (`pre-finish` mode is the
worst: 0/10 in one run). It is **not** a regression of this shim:
(a) it reproduces with Mesa's BO cache disabled (`PAN_FBD_NOCACHE=1`,
1 bad run of 5), (b) the pre-fix image also hung (int46measure 512×10
`errtimeout=2`), and (c) unaffected windows show the same per-frame
kernel numbers. After a hang, the recovery power-cycle leaves the next
runs in a slow state (90–133 fps, client sys back to ~8.5 ms/frame,
Shmem mid-run 9–12.5 MB) until the device re-settles. Not thermal:
45–55 °C, `thermal-gpufreq-0` cur_state 0, MIF fixed 825 MHz, CPUs
1.5 GHz. Closing it needs int48's per-error `submit_err` attribution
plus the fault capture (`ev_arm`) from the failing run; this ticket
records the rate and the window.

## 7. Artifacts / device state

External disk (`…/j7k/gpu31/`): `Image-int46prof` (`bafeea1e…`,
`kernel-boot-int46prof.img` `79ab379b…`), `Image-int47prof`
(`e2b0c0 4f…`, `kernel-boot-int47prof.img` `83b17044…` — fixed shims +
instrumentation), and the shipped `Image-int47` (`2a5e9416…`,
`kernel-boot-int47.img` `80ff0f9e…`). Logs: `logs/gpu31-int47*`.
On the phone: `/data/pfssplit/{a47,af,clean47,thermal,busprobe,clkprobe,
hangrate,series}*`, `/data/mfbd/rdtest`, `/tmp/churn`, `/tmp/waitprobe`.
Commits: `5950b7e` (shim fix + host checks), `d2c82ab` (hunk counts,
G31_Integrator41), `cf2e9fc` (churn client), this report.
Device at hand-off: int47 (`#37 19:33:43`), booted by Integrator41;
`ev_arm` disarmed; MemFree > 1.0 GB.
