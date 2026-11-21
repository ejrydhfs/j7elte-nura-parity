# GPU31 tiler chain — jc/FBD/heap payload decode, divergence hunt, verdict

Date: 2026-11-21. Owner: G31_TilerChain. Phone: up, int20
(`295ff9b0`, slotfix req0→JS1 live). Mesa 20.3.5 on device
(`/data/mesabuild/mesa20` + `-b`), `egltest --offscreen-only`.

Related: `gpu31-jobabi.md` (descriptor/slot/FBD tables),
`gpu31-mesa-render.md` (slotfix, chain/FBD/BO evidence),
`gpu31-coherency.md` (DMA-sync model).

## 0. Verdict

1. **Slot routing verified live.** One `--frames 1` run after
   `dmesg -c`: ~122 `p7580_job: irq-enter` lines strictly alternating
   `rawstat=0x00000002` (slot 1 DONE) / `0x00000001` (slot 0 DONE),
   **zero ERR lines, zero MMU-fault lines**. Chain (req=0) runs on JS1,
   fragment (FS) on JS0 — exactly the §5 contract.
2. **Clear colour is GPU-rendered and exact.**
   `corner(2,2) RGBA=26,26,102,255 (expect ~26,26,102)` —
   the fragment job, FBD, and RT writeback path all work end to end.
3. **Triangle still missing.**
   `center(128,128) RGBA=26,26,102,255 (expect ~255,0,0)`,
   `RENDER_MISMATCH`. Centre reads clear, not black: no fragment ran
   inside the triangle — the polygon list is empty or never consumed.
4. **No descriptor-contract divergence found.** Tiler jc, fragment jc,
   FBD, tiler heap/size, polygon-list refs, DRAW pointers, and
   `JS_CONFIG` bits all check out against `gpu31-jobabi.md` and the
   Mesa 20.3 sources on device (details §2–§3). **No fix landed** —
   the defect is downstream of the submit contract (geometry
   execution), and the acceptance criterion (centre red) is **not
   met**. Next steps in §5.

## 1. Method (device artefacts, not committed)

- `/tmp/g31trace.c` → `/tmp/g31trace.so` (device gcc, `-shared -fPIC
  -ldl`): `LD_PRELOAD` shim hooking **both** `drmIoctl` (what Mesa
  20.3 actually calls via libdrm — hooking only `ioctl` sees nothing)
  and `ioctl`. Tracks CREATE_BO (handle→iova/size), MMAP_BO
  (handle→fake), `mmap` (fake→cpu), GET_BO_OFFSET, and on SUBMIT walks
  the 32-B header chain from the process's own CPU maps.
- Decode offsets come from the generated pack header
  (`mesa20-b/src/panfrost/lib/midgard_pack.h`), not guesses:
  DRAW section @ job+64 both job kinds
  (`MALI_*_SECTION_DRAW_OFFSET 64`); SFBD `LOCAL_STORAGE @0`,
  `PARAMETERS @32` (40 words), `TILER @192` (10 words:
  `size, hier, polygon_list u64, body u64, heap_start u64, heap_end
  u64`), `WEIGHTS @256`.
- Log: `/tmp/g31jc.log` on device (one `--frames 1` run ≈ 3000
  lines; every SUBMIT appears twice — both hooks fire, harmless).
  IOVAs vary per run (allocator order), so addresses below are one
  representative frame. Pool BOs (job/FBD/descriptor memory) are
  GPU-only and never CPU-`mmap`'d after fill, so the shim marks their
  *contents* UNMAPPED — that is a shim blind spot, **not** a driver
  residency gap (the driver `map_bo`s every `bo_handles` entry into
  AS0 at SUBMIT; all pointers below resolve into listed handles).

## 2. Decoded payloads (representative frame)

### 2.1 Tiler-chain submit (`req=0x0`, JS1, DONE)

```
SUBMIT jc=0x104cd480 req=0x0 nbos=12
  job0 @jc type=2 64b idx=0 dep=? next=+0x40 : WRITE_VALUE addr=heap+0 type=3 (Zero)
  job1 @+0x40  type=5 64b idx=0 dep=? next=+0xC0 : VERTEX  (COMPUTE_JOB, DRAW @+64)
  job2 @+0xC0  type=7 64b idx=1 dep=? next=0   : TILER   (MIDGARD_TILER_JOB, DRAW @+64)
```

- WRITE_VALUE target = tiler-heap BO base (`h=133 …/0x100000`,
  `addr=0x10d62000` class): the polygon-list head zero that
  `pan_scoreboard.c` emits iff tiling is present. ✓
- Header shape (64-bit `next`, NULL-terminated, WRITE_VALUE→
  VERTEX→TILER) matches the `gpu31-mesa-render.md` §3 capture exactly.
- Dep/index 16-bit split in the shim yields implausible values
  (e.g. `dep=61632`); the `JOB_HEADER` bit packing was **not**
  cross-checked against genxml, so dep wiring is reported as
  **unverified**, not as a divergence (§4.3).

VERTEX DRAW (@job+64, `MALI_DRAW` `cl[]` order):

```
state=BO(h=10) OK  ubo=pool OK   tex=0   smp=0
abuf=pool OK  attr=pool OK  vbuf=pool OK  vary=pool OK  pos=0 (expected: vertex uses COMPUTE_JOB DRAW, position unused)
state[0..3]=…00020002 class (shader meta present)
```

TILER DRAW:

```
state=pool OK  ubo=pool OK  tex=pool OK (trampoline→view BO OK)  smp=pool OK
abuf=0 attr=0 (expected: tiler consumes varyings, not attributes)
vbuf=pool OK  vary=pool OK  vp=pool OK
pos=pool-BO(h=13)+0x40 — GPU-only pool BO, in bo_handles, shim-blind but driver-mapped
state[0..3]=…00010001 class (frag meta present)
vp[0..7]=ff800000 ff800000 7f800000 7f800000 00000000 3f800000 00000000 00ff00ff (sane viewport)
```

Every non-null pointer resolves inside one of the 12 submitted BOs.
No `UNMAPPED` (driver-side) link, no NULL `state`/`varyings`/
`viewport`. Texture trampoline resolves to the sampler-view BO
(**contents read all-zero** — second-order suspect, §4.2 — but a
zero texture would sample black, not clear, so it cannot explain a
clear centre).

### 2.2 Fragment submit (`req=0x1`, JS0, DONE)

```
job0 type=9 64b idx=0 : FRAGMENT bounds tiles (0,0)-(15,15) = 256x256
FBD @pool+0x… (SFBD, 320 B):
  PARAMETERS: cl0=0xb04d8281 (RAW, 1x, swizzle 0x60a, 4x32b RGBA, LINEAR, dither,
              clean_pixel_write=1, color_write_enable=1)
              bounds (0,0)-(255,255)
              color_writeback.base = RT BO (h=4, 0x41000 B) row_stride=16384 surface_stride=0
              crc base = pool scratch (Mali-managed), zs/s bases = 0 (no ZS in egltest)
              clear = ff661a1a x4  (== corner clear ~(26,26,102)+alpha ✓)
  TILER: pls_size=0x80200 hier_mask=0x0
         polygon_list = heap base, body = base+0x2200 (off 0x2200)
         heap_start..heap_end = 1 MiB window (test-patched heap, see §4.1)
  LOCAL_STORAGE: tls_size shift 5, tls = scratch pool BO, wls = 0
  WEIGHTS: 8x 0x00000000
```

### 2.3 Tiler heap (`h=1` + `h=133` mirror, `h=132` scratch)

- `heap_start..heap_end` size `0x100000` (1 MiB): the device-local
  TEST-ONLY `pan_props.c` 1 MB heap (`gpu31-mesa20.md` §4; upstream
  16 MB does not fit pre-shmem CMA). `polygon_list_size 0x80200`
  (524 800 B) fits comfortably — no `OUT_OF_MEMORY` signature, and
  OOM would not explain a DONE chain anyway.
- `polygon_list` (heap BO) and `polygon_list_body` (+0x2200) both
  inside the heap BO, which is in every submit's handle list.
  `hierarchy_mask 0x0` = flat 16×16 tiling selector on
  `MIDGARD_NO_HIER_TILING` (T720), per `pan_tiler.c` — not a mask
  error.

## 3. Contract check vs `gpu31-jobabi.md`

| Item | Required | Observed | Result |
|---|---|---|---|
| Tiler heap address/size | heap BO in submit list, `heap_end-heap_start` sane | 1 MiB BO, in all 12-handle lists | ✓ |
| FBD output address | RT BO, stride 64 tiles × 256 B | `color_wb = h=4`, `row_stride 16384` | ✓ (clear renders) |
| Fragment polygon-list/FBD ref | FBD ptr in frag payload w2–3, tagged `&~63` | `0x…4c0`, SFBD decode clean | ✓ |
| `JS_CONFIG` CHAINED/JOBCHAIN/PROTECTED | T720 has no disambiguation/protected HW; `START_MMU` + flush/inv + pri 8 | `p7580_js_config` sets exactly that; DONE on both slots | ✓ nothing missing |
| `heap_free` handling | Midgard SFBD carries **no** heap-free cursor (only `heap_start/end`); cursor is a kbase replay payload | no cursor field exists to program | N/A — nothing to fix |
| Weights | Mesa emits all-zero | 8× 0 | ✓ |

## 4. Divergence hunt (what was ruled out, what remains)

1. **Slot/requirements — ruled out (already fixed, int20).**
   Chain `req=0` → JS1 DONE, fragment `FS` → JS0 DONE, no ERR.
2. **Texture descriptor zeros — noted, not causal.**
   `texdesc[0..7]=0` at the trampoline target. A zero texture
   samples black; the centre is *clear*, so the triangle's fragments
   never ran. Separate ticket material (texture-upload path), not
   this one.
3. **Job dep/index wiring — unverified, not claimed.**
   The shim's `dep` 16-bit split disagrees with plausible
   scoreboard values; `JOB_HEADER` bit layout was not re-derived
   from genxml in this pass. Kernel-side jc dump (§5.1) settles it.
4. **Heap size — ruled out.** 1 MiB ≫ 0x80200 polygon list; chain
   DONEs.
5. **Remaining shape of the bug.** Descriptors in, DONE out both
   slots, clear exact, centre clear: the vertex→tiler leg produces
   no polygon list, or the fragment leg never consumes it. Suspects
   in priority order: (a) vertex shader output/varying linkage
   (attribute→varying path for this textured program); (b) tiler
   invocation config (`invocations`/primitive words, not yet
   decoded — shim stops at DRAW); (c)65535 (b) is the cheapest next
   decode: 8 invocation words + 6 primitive words sit at known
   offsets (+32/+40) and were captured but not interpreted.

## 5. Next steps (for Main / whoever takes the follow-up)

1. **Kernel-side jc dump.** Extend the existing `p7580_job_trace`
   budget (first-8-jobs only, exhausted at egltest startup) or add a
   one-shot `DEBUG_READ_BO`-style jc-header dump at SUBMIT: proves
   the bytes the GPU read == the §2 bytes (rules out
   CPU→GPU visibility on pool BOs once and for all).
2. **Decode invocation+primitive words** (+32/+40 of each job) and
   compare VERTEX vs TILER `invocations`/`index_count`/`draw_mode`.
3. **Untextured control.** A solid-colour (no-sampler) triangle
   build of egltest isolates the texture path from the geometry
   path; if the solid triangle draws, the tiler chain is innocent
   and the sampler-view BO (§4.2) becomes prime suspect.
4. **Polygon-list readback.** After chain DONE, dump the heap head
   + `polygon_list_body` via `DEBUG_READ_BO` (0xCF route): empty
   list ⇒ vertex/tiler produced nothing; non-empty ⇒ fragment
   consumption fault (but no ERR fires, so the former is likelier).
5. Cutover hygiene unchanged: shim (`/tmp/g31trace.c/.so`,
   `/tmp/g31jc.log`) and `0xCF` route stay device-only; this report
   is the only repo artefact.

## 6. Evidence (exercised, device)

- `egltest --offscreen-only --frames 1` on int20:
  `corner(2,2) RGBA=26,26,102,255`,
  `center(128,128) RGBA=26,26,102,255`, `RENDER_MISMATCH`.
- Same run, post-`dmesg -c` ring: alternating
  `irq-enter rawstat=0x00000002/0x00000001`, no `0x…0000` ERR bit,
  no `MMU fault`/`MULTIPLE_FAULT` lines (both handlers print
  unconditionally).
- `/tmp/g31jc.log` chain/FBD/DRAW decode as in §2 (shim + offsets
  described in §1; re-runnable with the artefacts listed there).
