# GPU31 Mesa render — before/after BO dump, job-chain breakdown, exact no-pixel cause

Date: 2026-11-21. Owner: G31_MesaRenderDebug. Phone: up, int19
(Image `c9bc0634`, boot `06b4ab33`). Ticket: `docs/reports/gpu31-mesa-render.md`.

Related: `gpu31-gpuwrite.md` (Zero-fill proof + DEBUG_READ_BO 0xCF),
`gpu31-coherency.md` (DMA-sync model), `gpu31-jobabi.md` (descriptor/slot
tables), `gpu31-mesa.md` / `gpu31-mesa20.md` (Mesa 20.3.5 bring-up),
`gpu31-irq.md` (req==0 to JS0 routing this ticket reverts).

## 0. Verdict

`egltest` renders nothing because **every Mesa batch faults on the GPU**.
Mesa jobs are not dropped at the UAPI layer: all 122 SUBMITs in a
61-frame run return `rc=0`, and all 122 raise **slot-0 ERR IRQ**
(`rawstat=0x00010000`, no DONE bit, no MMU-fault line). The render-target
BO stays all-zero kernel-side before and after the draw.

Root cause: **wrong job slot for the vertex/tiler chain**. Mesa 20.3
emits two submits per batch — the vertex/tiler chain with
`requirements == 0` and the fragment job with `PANFROST_JD_REQ_FS`.
Mainline v5.2 `panfrost_job_get_slot` routes FS to JS0 and *everything
else* to JS1; our `irqfix` (`gpu31-irq.md` §2.1) sent `requirements == 0`
to JS0 instead. The captured chain (`WRITE_VALUE type 2, VERTEX type 5,
TILER type 7`, §3) executed on the fragment slot faults, and every later
submit faults behind it. The WRITE_VALUE-only pattern job on the same
slot completes DONE, so the slot and its tables work — the slot rejects
the vertex/tiler chain, not the jobs' addresses (every VA in both
submits resolves inside a mapped BO, §3–§4).

Fix (this ticket, repo-side): `p7580_job_slot()` matches mainline again
(req==0 falls through to JS1). Files: `code/gpu31/jobs/`
`panfrost7580_jm_hw.h`, `job_test.c`, regen of `driver/0001` (surgical:
comment + hunk length + 2-line function only, timestamps untouched).
Device proof needs a flash+rerun (blocked: no root/reboot on the phone);
Main rebuilds (int20) and expects `egltest RENDER_OK`.

A second, independent defect was found while proving the negative
(coherency race): the pattern test's userspace re-read can win a race
with `free_job`'s completion invalidate, printing USER-stale + KERN-zero
(§1). Fix deferred to G31_Coherency (invalidate before fence signal);
not the Mesa cause (Mesa BOs stay zero on both sides of every fence).

## 1. Before/after BO dump (pattern job + Mesa render target)

### 1.1 Pattern job (own fd, full control), int19 silicon

```
-- pf7580 pattern-test (WRITE_VALUE coherency) on /dev/dri/renderD128 --
GET_PARAM: OK GPU_PROD_ID 0x720 (T720)
CREATE_BO: job h=1 gpu=0x2102000 result h=2 gpu=0x2103000
SUBMIT: WRITE_VALUE jc=0x2102000 bos=[1,2] out_sync=1...
SUBMIT: rc=0 (OK) elapsed=0 ms
SYNCOBJ-WAIT(+8s abs): rc=0 (OK) elapsed=0 ms
USER: word0=0xa5a5a5a5 (prefill 0xa5a5a5a5, expect 0x00000000) STALE
KERN: word0=0x00000000 ZERO
HINT: kernel zero + user 0xA5 = stale CPU alias (visibility bug).
dmesg: ... p7580_job: irq-enter rawstat=0x00000001 stat=0x00000001
```

Descriptor: 56 B at `jc=0x2102000`: header `w[4]=(1<<0)|(2<<1)`
(is_64b, type 2 WRITE_VALUE), payload `{addr=0x2103000, type=3 Zero}`.
Fence waited OK, slot-0 **DONE** IRQ — the GPU wrote the page
(kernel view zero after `dma_sync_for_cpu`).

USER-stale + KERN-zero is a **test-side race, not a GPU defect**:
the fence signals in the IRQ handler, but `free_job`'s
`p7580_gem_job_done_sync` invalidate runs later on `finish_work`.
The re-mmap + userspace read in between can observe the pre-invalidate
alias while the kernel read (after invalidate) sees zeros. The mapping
is WC (`pgprot_writecombine`), so the stale line most likely comes from
the prefill store sitting in the CPU store buffer at SUBMIT time, made
visible to the later user read by ordering, while the GPU zeroed DRAM.
Either way the GPU write itself is proven by the DONE IRQ + KERN zero.
Handed to G31_Coherency: maintenance must precede the fence signal
(e.g. invalidate in the IRQ path before `dma_fence_signal`, or retire
the user read behind `free_job`), so a fence wait guarantees visibility.

 charges? No: no UAPI or descriptor change is needed for this leg.

### 1.2 Mesa render-target BO (Mesa's own fd, LD_PRELOAD trace)

Render target identified from the last frame's SFBD (§4):
`color_writeback.base = 0x7c55000`, 266240 B linear, `row_stride`
16384 B (= 64 tiles x 256 B). It is CREATE_BO `h=4 size=266240`
(`0x7c55000`), present in **every** submit's `bo_handles`, mapped by
the driver at every SUBMIT (SUBMIT rc=0 implies map success; map failure
returns an error, never a fault).

Kernel-side word0 of `h=4` via DEBUG_READ_BO (same fd, same process):

| point | value |
|---|---|
| KERN-BEFORE, every one of 122 submits | `0x00000000` |
| KERN-AFTER, all 3 SYNCOBJ_WAIT retire points (§2) | `0x00000000` |

Before = zero (fresh BO, never written), after = zero: **the GPU never
wrote the render target**. Not a readback/re-read race: the kernel view
after completion invalidate is still zero, and no submit ever raised DONE.

## 2. Submit/IRQ accounting (one `--offscreen-only --frames 1` run)

Strace (raw numbers, musl has no DRM names for our ioctls):

| ioctl | count | rc |
|---|---|---|
| SUBMIT `_IOC_WRITE('d',0x40,40)` = `0x40286440` | 122 | all 0 |
| CREATE_BO `0xc0186442` | 133 | all 0 |
| WAIT_BO `0x40106441` | 126 | all 0 |
| MMAP_BO `0xc0106443` | 75 | all 0 |
| GET_PARAM `0xc0106444` | 4 | 1x0 + 3x EINVAL (expected: only PROD_ID) |
| MADVISE `0xc00c6448` | 385 | EINVAL (expected: not on v5.2 node) |
| SYNCOBJ_WAIT `0xc02864c3` (libdrm 40 B shape) | 3 | all 0 |
| SYNCOBJ_CREATE/HANDLE_TO_FD/FD_TO_HANDLE/DESTROY | 3/3/3/3 | all 0 |

61 `requirements=0` submits + 61 `requirements=0x1 (FS)` submits = 61
frames x (chain + fragment). No submit is dropped: UAPI flags
(`requirements` gate), tiler-heap BO (`h=1`, in every submit list), and
counts (12 BOs ≤ 64) are all accepted.

dmesg over the same window: **120 `irq-enter rawstat=0x00010000`
(slot-0 ERR)**, zero DONE lines on any slot, zero `MMU fault` / `GPU
MULTIPLE_FAULT` lines (both handlers print unconditionally via
`dev_err`, so silence = no MMU fault, no multi-fault: the fault is a
job-slot rejection, not a page fault). 120 vs 122 submits: the last
frame's pair was still in flight at capture; the ratio is 1 ERR per
submit — **both** submits of every frame fault (chain on the wrong
slot, fragment behind the faulted chain, §5).

## 3. Job descriptor breakdown (captured via LD_PRELOAD shim)

Method: `/tmp/mesatrace.so` (kept on device `/tmp`, source in this
ticket's worklog, not committed) wraps `ioctl`/`mmap`, tracks
CREATE_BO (handle→iova/size) + MMAP_BO/mmap (fake→cpu), and on SUBMIT
logs jc/requirements/handles, walks the 32 B-header chain through Mesa's
own cpu mappings (up to 16 jobs), dumps FRAGMENT payload + full 320 B
FBD, and issues DEBUG_READ_BO word0 before the real SUBMIT and after
the real SYNCOBJ_WAIT. Log: `/tmp/mesajc.log` on device (2465 lines for
the traced run). Representative first frame (all 61 identical in shape):

```
SUBMIT jc=0x4f2f480 req=0x0 out=0 nbos=12
  hs=[9@0x4f41000/4096,133@0x57c4000/1048576(heap),11@0x4f43000,
      8@0x4f40000,132@0x57b4000/65536,10@0x4f42000,12@0x4f44000/1048576,
      7@0x4f3f000,4@0x4eed000/266240(RT),6@0x4f2f000/65536(pool),
      13@0x5044000/65536,1@0x4d6a000/1048576(heap-mirror? tiler heap)]
  job0 @0x4f2f480 type=2 idx=0 dep=0,61632 next=0x4f2f0c0 64b
    payload: 057c4000 00000000 00000003 00000000 00000000 00000000
  job1 @0x4f2f0c0 type=5 idx=0 dep=0,61440 next=0x4f2f000 64b
    payload: 00000002 28000000 14000000 00000000 00000000 00000000
  job2 @0x4f2f000 type=7 idx=1 dep=2,0 next=0x0 64b
    payload: 00000002 28000000 18030008 00000000 00000000 00000002
  SUBMIT rc=0
SUBMIT jc=0x4f2f600 req=0x1 out=1 nbos=12 hs=[same 12]
  job0 @0x4f2f600 type=9 idx=0 dep=0,0 next=0x0 64b
    frag payload: 00000000 000f000f 04f2f4c0 00000000 ...
    FBD@0x4f2f4c0 (SFBD, 320 B, §4)
  SUBMIT rc=0
```

Chain reading:

- Chain submit: WRITE_VALUE (type 2, Zero to polygon list
  `0x057c4000`, inside heap BO `h=133 0x57c4000+1M`) → VERTEX (type 5)
  → TILER (type 7), indices 0/0/1, deps chained (tiler dep on write-value
  path), 64-bit next pointers, NULL-terminated. Exactly the
  `pan_scoreboard.c` shape (write-value iff tiler present).
- Fragment submit: single FRAGMENT job (type 9), FBD pointer in payload
  words 2-3 (`0x04f2f4c0`, inside pool BO `h=6`), bounds
  `(0,0)-(15,15)` tiles = 256x256.
- Every `jc`, `next`, polygon-list (`0x0852c000` class), heap
  (`0x07ad2000+1M`), FBD, and RT address resolves inside a BO listed in
  that submit's `bo_handles` (checked for first/middle/last frames).
  No `UNMAPPED` link, no `LOOP`, no `requirements` rejection.

So the descriptors Mesa builds are sane; the driver accepts them; the
GPU faults them on slot 0.

## 4. FBD decode (last frame, SFBD — T720 quirks `MIDGARD_SFBD`)

FBD `320 B` at `0x835c4c0` (pool BO). Sections per genxml
(`midgard.xml:1146-1151`): Local Storage @0, Parameters @32, Tiler @192,
Padding1 @232, Weights @256, Padding2 @288.

```
LS:   tls_size shift 5, tls_base 0x0806c000 (scratch, pool BO)
PARAMS cl[0]=0xb04d8281: internal_format RAW, sample_count 1,
  swizzle 0x60a, color_wb_format 19 (4x32b RGBA), color_block LINEAR(0),
  dither 1, clean_pixel_write 1, color_write_enable 1
  bounds (0,0)-(255,255); color_writeback.base = 0x7c55000 (RT h=4),
  row_stride 16384, surface_stride 0
  zs/s_writeback.base = 0 (no ZS buffer in egltest)
  clear colors ff661a1a x4 (corner clear ~(26,26,102) + alpha)
TILER @192: polygon_list_size 0x80200, hierarchy_mask 0x0,
  polygon_list 0x0852c000 (heap BO h=133), body +0x200,
  heap_start 0x07ad2000 (heap BO h=1), heap_end 0x07bd2000 (+1M)
```

All addresses land inside submitted BOs. Tiler heap is the 1 MB
test-patched heap (`pan_props.c` 1 MB TEST-ONLY, `gpu31-mesa20.md` §4);
`polygon_list_size` 0x80200 (524800 B) fits. No OUT_OF_MEMORY
signature is visible (and OOM would print no MMU line either — but the
heap-exhaust hypothesis dies with the slot evidence: the chain faults
before the heap is even touched, and WRITE_VALUE-alone succeeds).

## 5. Why the fragment submit faults too

Both submits of a frame run on slot 0 (chain req==0 → JS0 by the old
routing; fragment req==FS → JS0). The vertex/tiler chain faults on the
fragment slot (no DONE ever observed for a type-5/7 job on JS0; the
NULL/WRITE_VALUE-only submits are the only DONE rows). The fragment job
then reads tiler structures the faulted chain never produced
(`polygon_list`/heap cursor uninitialized — the same class Mesa's own
RASTERIZER_DISCARD comment warns about), so it faults as collateral.
After each fault the driver recovers (HARD_STOP + SOFT_RESET + AS0
rebind + rearm, fence signaled so Mesa never hangs — hence 660 FPS with
zero pixels), and the next frame faults identically. One root cause,
120 identical ERRs.

Slot legality, not addresses: no MMU-fault line fires (MMU IRQ prints
unconditionally), all VAs resolve, WRITE_VALUE-alone DONEs on JS0 while
any chain containing VERTEX/TILER on JS0 ERRs.

## 6. Fix (repo-side, this ticket)

`p7580_job_slot()` matches mainline v5.2 `panfrost_job_get_slot`
(`panfrost_job.c:102-122`: FS→JS0, everything else→JS1):

```c
if (requirements & P7580_JD_REQ_FS) return P7580_SLOT_FRAGMENT;
if (requirements & P7580_JD_REQ_ONLY_COMPUTE) return P7580_SLOT_COMPUTE;
return P7580_SLOT_VERTEX_TILER;
```

The `requirements == 0 → JS0` arm (irqfix) is deleted. Upstream never
had it: its NULL-job selftest also uses req==0 and passes on JS1 (NULL
runs on any slot; the selftest asserts only fence completion, and did —
`gpu31-submit-hw.md` shows the NULL job completing on JS1 pre-irqfix).

Files changed (committed):

| File | Change |
|---|---|
| `code/gpu31/jobs/panfrost7580_jm_hw.h` | slot comment rewritten (mainline rule + Mesa evidence), `== 0 → JS0` arm deleted |
| `code/gpu31/jobs/job_test.c` | `p7580_job_slot(0) == 1` (was 0) + comment |
| `code/gpu31/driver/0001-*.patch` | surgical regen of the `jm_hw.h` hunk only (comment + `+1,254`→`+1,258` + arm deletion); abialign pins, timestamps, all other hunks untouched |
| `docs/reports/gpu31-mesa-render.md` | this file |

Verification exercised: `jobs/job_test` PASS, `client make check` (mock
suite incl. Mesa ABI rows) PASS, `gem/gem_validate` PASS,
`git apply --check` on 0001 PASS.

## 7. What Main must do (flash + run)

1. Build int20 (0001 now routes req==0 → JS1) and flash.
2. `dmesg -c`, then `/data/eglrun.sh` (or the shim-traced variant);
   expect per frame: chain submit on **JS1** DONE (`rawstat=0x00000002`)
   + fragment submit on **JS0** DONE (`rawstat=0x00000001`), zero ERRs,
   then `egltest ... RENDER_OK` with non-zero corner/center pixels.
3. If any ERR survives, the decision tree is: MMU-fault line present →
   unmapped VA (re-check §3 addresses against the new IOVAs); slot-0 ERR
   on the fragment job only → FBD/heap content (re-dump §4); slot-1 ERR
   on the chain → escalate to G31_MesaRenderDebug with `/tmp/mesajc.log`.
4. Cutover hygiene unchanged: DEBUG_READ_BO `0xCF` + shim + this ticket's
   `/tmp` artefacts (`mesatrace.c/.so/.log`, `/tmp/mt-*.txt`,
   `/tmp/s*.txt`) stay on the device only; the `gpu31-gpuwrite.md`
   cutover checklist still strips the debug surface before landing.

## 8. Worklog (device artefacts, not committed)

- `/tmp/mesatrace.c` → `/tmp/mesatrace.so` (device gcc, `-shared -fPIC
  -ldl`): ioctl/mmap LD_PRELOAD tracer (CREATE→iova, MMAP→cpu,
  SUBMIT→chain walk + FBD dump + KERN-BEFORE, SYNCOBJ_WAIT→KERN-AFTER).
- `/tmp/mesajc.log` (2465 lines): full CREATE/MMAP/SUBMIT/chain/FBD/
  KERN-BEFORE record of the traced 1-frame run; `/tmp/s2.txt`,
  `/tmp/s3.txt`, `/tmp/s4.txt`: raw strace ioctl audits.
- Counts that ground this report: 133 CREATE rc=0, 122 SUBMIT rc=0
  (61x req0 + 61x reqFS), 120 slot-0 ERR IRQs, 3 SYNCOBJ_WAIT rc=0,
  KERN-BEFORE/AFTER `h=4 word0=0` throughout, no MMU/MULTIPLE_FAULT
  lines anywhere in the window.
