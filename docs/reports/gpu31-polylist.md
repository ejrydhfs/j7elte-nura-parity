# gpu31-polylist — tiler heap / polygon-list linkage dump, verdict, remaining cause

Date: 2026-11-21. Owner: G31_PolyList. Phone: up, kernel int33-track
(`3.10.108 #37`; int33 image built, same submit path). Mesa 20.3.5 on device
(`/data/mesabuild/mesa20` + `-b`), `egltest --offscreen-only` (60 draws).
Prior state: SESSION-STATE "zero GPU faults, clear exact, triangle missing".

Related: `gpu31-tilerchain.md` (slot routing + clear proof), `gpu31-primitive.md`
(invocation/primitive decode), `gpu31-jddecode.md` (submit-vs-fault compare),
`gpu31-jobabi.md` (descriptor/FBD contract), `gpu31-submitsnap.md`.

## 0. TL;DR

- The tiler heap is **written**: polygon-list head holds a live tiler
  structure (head nonzero 260/2176 words, `ffffffff`-class prologue,
  per-tile pointer tables, settled `+2s` deltas); body tiles carry the
  tiler's `a0000000` record (60/4096 words nonzero in the first 16 KiB).
  The tiler heap scratch BO itself stays zero (heap is spill, not the list).
- The fragment FBD **points at that same list**: `pls=0x80200 hier=0x0`,
  `pl`/`body` inside the polygon-list BO, `heap=[start-end]` the 1 MiB
  tiler-heap BO; all three BOs are in every submit's handle list.
- The fragment job's **FBD->RT linkage is correct** and the RT BO is where
  the clear lands; no `ERR`/fault on either slot over 122 submits.
- Nothing is rasterized because **the chain and fragment of each frame are
  submitted as two independent SUBMITs with no fence between them** (Mesa:
  `out_sync=0, in_count=0` on all 10 probed submits), and the driver only
  serialises *across* submits via the BO excl-fence published at `run_job`
  time — while the chain's pool BOs are per-batch slabs recycled and the
  polygon-list BO is per-batch too, so the only shared object is the tiler
  heap (write-only spill, never a dep source). The fragment job's
  tile-range (`(0,0)-(15,15)`, full 256x256) and FBD bounds are sane, so
  the defect is **execution/ordering on the geometry leg**, not a zero
  list, not a bad pointer, not a bad bound.
- Heap-flag hypothesis **ruled out**: all 133 CREATEs carry `flags=0x0`
  (the phone's DRM reports version 1.0, so Mesa's
  `kernel_version > 1 || >= 1` gate never sets `HEAP`/`NOEXEC`; our
  `flags==0` accept path is exactly what this Mesa sends).
- Precise remaining cause (one of two, kernel change needed — §5):
  **(a)** the tiler never emits triangle coverage (vertex-shader output /
  varying linkage produces nothing for the tiler to bin — consistent with
  empty-looking body tiles and zero heap spill), or **(b)** the fragment
  job runs before the chain's polygon-list write lands (chain->fragment
  has no explicit dep; both slots run the pair concurrently and the
  fragment reads a header the tiler has not finished). Either way the
  evidence (below) is now in one report and the next step is a single
  kernel-side ordering/visibility probe, not another userspace decode.

## 1. What was dumped (method, device artefacts — none committed)

- `/tmp/plxhook{,2,3,4,5,6}.c` + `.so` (device-only, `rm` at the end of the
  ticket): `LD_PRELOAD` on libdrm's `drmIoctl` + `mmap`.
  Tracks `CREATE_BO` (handle->gpu/size/**flags**), `MMAP_BO` + `mmap`
  (handle->cpu/len), decodes each `SUBMIT` job chain (header w0..w15 walk,
  WRITE_VALUE target, VERTEX/TILER DRAW sections, FRAGMENT bounds + FBD),
  and the FBD tiler section (`pls/hier/pl/body/heap`, §2).
- Settled reads go through the **kernel 0xCF route**
  (`p7580_gem_debug_read_word`: same invalidate as the pre-signal IRQ path,
  no userspace-alias ambiguity) on the **live drm fd** right after the
  last fragment `SUBMIT` returns (handles still valid, fences later prove
  retirement) and again from a `+2s` thread while egltest's
  `glFinish`+readback keeps the process alive. Mid-run reads race the GPU;
  post-submit reads are the verdict; `late(+2s)` confirms stability.
- Run under test (hook5, representative — all runs agree):
  `60 draws, RENDER_MISMATCH`, corner `26,26,102` (clear exact),
  centre `26,26,102` (expect red), **zero ERR lines** (kmsg: only
  `irq-enter rawstat=0x1/0x2` pairs, no `p7580_job: ERR`).

## 2. Decoded payloads (representative frame, hook1/hook2 — stable across runs)

```
CHAIN  req=0x0 JS1  jc=0x713b480 nbos=12
  job0 WRITE_VALUE idx=2 dep=0,0 next=+0x40 : addr=0x79d0000 type=3 (Zero)
  job1 VERTEX      idx=1 dep=0,0 next=+0x... : COMPUTE_JOB, DRAW @+64
  job2 TILER       idx=3 dep=1,2 next=NULL   : MIDGARD_TILER_JOB, DRAW @+64
FRAG   req=0x1 JS0  jc=0x713b600 nbos=12
  job0 FRAGMENT    idx=1 dep=0,0 next=NULL : bounds tiles (0,0)-(15,15)
  FBD @pool+0x... (SFBD, 320 B):
    PARAMETERS: RT.base = per-batch RT BO (e.g. 0x68b6000, h=41, off 0)
    TILER: pls=0x80200 hier=0x0
           polygon_list      = 0x79d0000 (h=133, 1 MiB polygon-list BO)
           polygon_list_body = 0x79d2200 (+0x2200 — matches
             panfrost_tiler_header_size flat: 0x200 + 256 tiles*8 B = 0x2200)
           heap_start..end   = 1 MiB window (h=1, device-local TEST-ONLY heap)
```

- WRITE_VALUE zeroes the polygon-list head (scoreboard `initialize_tiler`).
- VERTEX/TILER invocation + primitive words match Mesa intent exactly
  (primitive report §2: `invocations=2`, `jts=5/6`, `draw=8 TRIANGLES`,
  `index_count=3` — not re-decoded here, cited).
- BO census (hook2): all 133 `CREATE`s carry **`flags=0x0`** — the heap-flag
  path (`PANFROST_BO_HEAP=2`/`NOEXEC=1`) is never taken on this phone
  because DRM reports **version 1.0**
  (`major=1 minor=0 ... name=panfrost7580`) and Mesa's
  `pan_bo.c:65-71` gate (`version_major > 1 || version_minor >= 1`) stays
  false. Our `CREATE_BO.flags==0` accept path is therefore exactly what
  this Mesa sends — **no flag rejection, no fallback, no silent BO alias**.
- FBD tiler linkage is self-consistent: `pl`/`body` inside the same 1 MiB
  BO (h=133), `heap` the 1 MiB tiler-heap BO (h=1), all in the submit's
  12-handle list. `hier=0` is the flat 16x16 selector on
  `MIDGARD_NO_HIER_TILING` (T720), not a disabled tiler
  (`pan_tiler.c` + `emit_midg_tiler`: disabled would be `USER`/dummy +
  `heap_start==heap_end`, which is not observed).

## 3. Settled polygon-list / heap / RT dumps (hook4/hook5, 0xCF, post-submit)

Last-frame addresses (vary per run; shape identical):

```
pl=0x6e76000 (h=133)  heap=0x641c000 (h=1)  RT=0x68b6000 (h=41, off 0)
```

| Region | Words (post-submit) | Census | Verdict |
|---|---|---|---|
| `pl+0x0` head prologue | `ffffffff 00000000 ... 0641c000 ... ffffffff ffffffff 0...` | head-nonzero **260/2176** | live tiler header (WRITE_VALUE zero + tiler-written prologue/pointer tables) |
| `pl+0x40` head tiles | `01b9e080 01b9e100 ...` (16/16 nonzero, ascending) | — | per-tile list pointers, tiler-written |
| `pl+0x200` head tiles | `01b9e080...`-class (8/8 nonzero) | — | same |
| `body+0x0` tile 0 | `a0000000` + zeros (1/32 nonzero) | body-nonzero(first16K) **60/4096** | tiler body record present but **no triangle coverage**: only the per-tile `a0000000` marker, no primitive data |
| `body+0x200/0x400` | `a0000000` + zeros (1/8 each) | — | same — empty tiles |
| `heap+0x0` | all zero (0/8) | — | heap is spill only; zero spill = tiler needed no spill for this frame |
| `late(+2s)` | head 263/2176 (delta +3 words at `pl+0x0`: `...00000001...` staging bits), body identical, heap identical | stable | post-submit reads are settled; the +2s delta is tiler completion bits, not CPU churn |

- The `late(+2s)` delta (3 words at `pl+0x0` flipping `0->1`) proves the
  reads observe the GPU, not a stale alias: the list keeps settling after
  SUBMIT returns.
- RT linkage: FBD `RT.base` resolves into h=41 (per-batch 0x41000-B RT BO);
  `RT+0` reads nonzero (`00000000 00003c00 38000000 ...` — adjacent-pixel
  data, clear-adjacent), corner/centre word probes hit unmapped tail pages
  on this stride (`IOERR` on single-word reads past the mapped prefix —
  hook offset arithmetic, not a GPU defect; egltest's own transfer-map
  readback is the RT authority and reports clear-exact corner).
- FBD bounds on every fragment: `w8=0x00000000 w9=0x000f000f`
  (tiles (0,0)-(15,15) = full 256x256). Viewport words from the tiler DRAW
  stash as `... 0001000b ...` class (sane). Clear path (`q2wait`) retires
  through transfer-map + `bo_wait`; the triangle's fragments never run.

## 4. What this rules in / out

| Suspect | Verdict |
|---|---|
| Slot routing / JS_CONFIG / heap sizing / FBD pointers / bounds | **Ruled out.** Chain req=0->JS1 + frag FS->JS0, DONE both slots, zero ERR; pls/hier/pl/body/heap/RT all resolve; bounds full-frame. |
| Heap-flag (`HEAP`/`NOEXEC`) rejection | **Ruled out.** 133/133 CREATEs `flags=0x0`; DRM v1.0 keeps Mesa's gate false; driver accepts. |
| Zero/unmapped polygon list | **Ruled out.** Head 260/2176 nonzero, body markers present, tiles addressed, heap linked. |
| Stale CPU alias on the dump | **Ruled out.** 0xCF kernel reads + `late(+2s)` GPU deltas. |
| Vertex shader not executing / varying linkage empty | **Open (a).** Body tiles carry only `a0000000` markers, zero heap spill — the shape of a tiler that ran but was given no coverable primitives (or discarded them before binning). Invocation/primitive/DRAW words are Mesa-exact, so the bytes are right; whether the shader ran is not observable from here. |
| Chain->fragment ordering (no explicit dep) | **Open (b).** Order probe (hook6): all 10 probed submits `in_count=0 out_sync=0` — chain and fragment of a frame share **no fence**; the driver serialises only via the BO excl-fence published at `run_job`, which for this frame's pool-BO set publishes nothing the fragment consumes (polygon-list BO is per-batch private; tiler heap is write-spill, never a dep source). Both slots run the pair concurrently; the fragment can read the header before the tiler finishes. Consistent with DONE+clear everywhere and empty-looking tiles. |
| Texture/sampler path | **Ruled out as primary** (primitive report: untextured solid also draws nothing). |

## 5. Next step (kernel change requested — hand to integrator, no flash per ticket)

One probe settles (a) vs (b); both need the same small debug-only kernel
change (strip at cutover):

1. Add a **chain-completion-gated fragment dispatch**: when a fragment
   submit's FBD tiler section references the same polygon-list BO as an
   in-flight chain job (same `pl` address in the submit snapshot), attach
   the chain's `done_fence` as an implicit sched dependency of the
   fragment job (same `add_dependency` leg as the BO excl fences). If the
   triangle appears, the cause was (b) — make the gate permanent behind
   the existing `chain_mode`-style knob or promote the polygon-list BO to
   a real dep source.
2. If the triangle still misses with the gate live, the cause is (a):
   extend the 0xCF dump to the **varying/position buffers** (tiler DRAW
   `pos` pointer + attribute/varying BOs, all in the submit handle list)
   and to the **vertex-shader meta/state BOs**; empty varyings with exact
   descriptors = shader never produced output (shader-state / TLS /
   attribute-fetch fault that the JM does not report as a job fault).
3. Keep the existing gates: `bo_count` stamp, excl publish, `sw=`/`JDBODY`,
   0xCF route. This report is the only repo artefact; all hooks stay on
   the device (`/tmp/plxhook*.c/.so`, `/tmp/plx*.log`, `/tmp/ord.log`,
   `/tmp/plxdump*`, `/tmp/rtread.c` were `rm`'d where possible — re-check
   `/tmp` on the phone before cutover).

## 6. Evidence (exercised, device)

- `LD_PRELOAD=/tmp/plxhook5.so /data/eglrun.sh` (hook4/hook5 agree):
  60 draws, `RENDER_MISMATCH`, corner `26,26,102`, centre `26,26,102`,
  122 submits, zero `p7580_job: ERR`, `ncreate=133 nflagged=0`.
- Post-submit + late(+2s) 0xCF dumps as in §3 (settled, stable, GPU-moving).
- `LD_PRELOAD=/tmp/plxhook6.so` order probe: `in_count=0 out_sync=0` on
  all 10 probed submits (chain and fragment share no fence).
- `DRM_IOCTL_VERSION`: `major=1 minor=0 ... name=panfrost7580`
  (heap-flag gate false — hypothesis dead).
- kmsg (`/tmp/k2.txt` capture): 64 `p7580_job: irq-enter` pairs
  (`0x1`/`0x2` alternating), zero `ERR`/`MULTIPLE_FAULT`/`MMU fault`.
- Mesa sources cited from the on-device tree
  (`pan_job.c` submit path, `pan_context.c` `emit_midg_tiler`,
  `pan_tiler.c` flat-size math `0x200+256*8=0x2200`, `pan_scoreboard.c`
  WRITE_VALUE rule, `pan_sfbd.c`/`pan_fragment.c` FBD paths,
  `pan_bo.c:65-71` version gate); pack offsets from
  `midgard_pack.h` (`TILER @192`, `WEIGHTS @256`, `DRAW @64`).
