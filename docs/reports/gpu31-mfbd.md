# gpu31-mfbd — 512² stalls are fault-recovery latency, not FBD type; Mesa 20.3 FBD layout on T720

Date: 2026-11-22. Owner: G31_Mfbd. Device: int38 (`42d3a160…`, kernel
`3.10.108 #37`), `Mali-T72x MP2 r1p0 0x0720`, Mesa 20.3.5 in
`/data/mesabuild/mesa20`/`mesa20-b` (panfrost v5.2 UAPI), driver 0001.
Silicon runs under `flock /tmp/gpsdev.lock`, plain runs only (no
`PAN_MESA_DEBUG=sync|trace`), no flash.

## 0. Verdict (supersedes the ticket's MFBD premise)

1. **Mesa 20.3 has no tile-count threshold that switches to MFBD.** The FBD
   type is a *per-GPU quirk*: `MIDGARD_SFBD` is set for GPU IDs `0x600`,
   `0x620` and `0x720` (`src/panfrost/include/panfrost-quirks.h:76-92`), and
   T720 is `0x720`. Every T720 batch therefore allocates an SFBD
   (`pan_job.c:735-750`), attaches an SFBD (`pan_job.c:1339-1342`) and builds
   the fragment job from `panfrost_sfbd_fragment()`
   (`pan_fragment.c:53-60`). Silicon confirms it at 512×512: the fragment job's
   FBD pointer is `0xc080480`, **bit 0 = 0 (not `MALI_FBD_TAG_IS_MFBD`)**, and
   the payload asks for tiles `0,0..31,31` — one SFBD, one fragment job.
2. **There is no "chained FBD list" in this hardware generation.** `midgard.xml`
   has no chain/next field in either `Single-Target Framebuffer` (320 B) or
   `Multi-Target Framebuffer` (128 B + 64 B per render target); the 64 B
   fragment job payload carries one `Framebuffer` pointer plus 12-bit
   `Bound Min/Max X/Y` tile coordinates. The fragment submit must therefore stay
   **one job consuming one FBD**, covering the whole target; one job per FBD and
   per-FBD tile ranges do not exist. MFBD is only selected on GPUs *without*
   `MIDGARD_SFBD` (`0x750`, `0x820/0x830`, `0x860/0x880`, Bifrost), and is still
   a single descriptor (the render targets are inline descriptors, not a chain).
3. **512×512 is not broken by its tile count.** With no per-frame readback
   (`glFinish` only) a 512×512 pbuffer runs 2/2 frames clean: every job
   (VERTEX, VERTEX, TILER, FRAGMENT) completes `NO_EXCEPTION` with a normal IRQ,
   `0` ERR/TIMEOUT lines.
4. **What the 512² run does change is the polygon-list allocation**: 4 MiB
   instead of 1 MiB, because `panfrost_flat_size()` sizes the flat list with an
   8-px tile base while the hardware uses 16-px tiles. It is a 4×
   over-allocation (also true at 256², where it rounds to 1 MiB), harmless, and
   fully accounted for in §3.
5. **The stalls are fault-recovery latency.** Per-frame `glReadPixels` makes the
   fragment job fault (`DATA_INVALID_FAULT 0x58`) or hang (`NO_EXCEPTION`,
   head == tail); the IRQ ERR path does not signal the job fence, so recovery
   only starts at the 5 s `drm_sched` timeout — one stall per fault, exactly the
   `TIMEOUT slot=… 5.010 s` pattern. This reproduces at **256² too** (same client,
   same code), so it is resolution-independent. G31_Latency measured the
   steady-state at 286-305 fps (3.3-3.5 ms/frame) and owns the fix (kick
   `timeout_work` at delay 0 on the fault IRQ; driver 0001 + 0004).
6. Net driver requirement for >256-tile targets: **none in the FBD code**. Do not
   add MFBD/chaining. The 5 s stalls are Latency's; the stale-readback/BO-recycle
   window in §5.3 is a submit-time fence-publication gap handed to Latency.

## 1. FBD selection and layout (source of truth: `midgard.xml`, Mesa 20.3.5)

`panfrost_get_quirks()` (device tree, stock 20.3.5):

| GPU ID | FBD | Tiling |
|---|---|---|
| `0x600`, `0x620` | **SFBD** | flat |
| `0x720` (this device) | **SFBD** | flat, no hier tiling (`MIDGARD_NO_HIER_TILING`) |
| `0x750` | MFBD | flat |
| `0x820`, `0x830` | MFBD | flat |
| `0x860`, `0x880` | MFBD | hierarchical |
| Bifrost (`0x6000`, `0x6221`, `0x7093`, `0x7212`, …) | MFBD | hierarchical |

FBD pointer tags (`midgard.xml` `FBD Tag`): `IS_MFBD = 1`, `HAS_ZS_RT = 2`,
mask `0x3f`; the low 6 bits are a tag, the rest is the 64-byte-aligned address.

**SFBD** (`Single-Target Framebuffer`, 320 B, 64-B aligned):

| Offset | Section | Size |
|---|---|---|
| +0x00 | Local Storage | 32 B |
| +0x20 | Parameters | 160 B (`Internal Format`, `Sample Count`, `Swizzle`, `Color Writeback Format`, `Color Block Format`, `Color Write Dithering/Clean Pixel/Write Enable`, `Bound Min/Max X,Y`, `CRC/Color/ZS/S Writeback` buffers, `Clear Color 0..3`, `Z/S Clear`) |
| +0xC0 | Tiler (`Midgard Tiler`) | 40 B |
| +0xE8 | Padding 1 | 24 B |
| +0x100 | Tiler Weights ×8 | 32 B |
| +0x120 | Padding 2 | 32 B |

The Tiler section is `Polygon List Size`, `Hierarchy Mask` (+ flags), `Polygon
List`, `Polygon List Body`, `Heap Start`, `Heap End` (heap start/end =
`dev->tiler_heap`, 1 MiB in this port).

**MFBD** (`Multi-Target Framebuffer`, 128 B fixed + extensions), for reference
only — never used on T720:

| Offset | Section | Size |
|---|---|---|
| +0x00 | Local Storage | 32 B |
| +0x20 | Parameters (`Width-1`/`Height-1`, pixel bounds, `Render Target Count-1`, `Effective Tile Size`, Z/S state) | 24 B |
| +0x38 | Tiler (`Midgard Tiler`) | 40 B |
| +0x60 | Tiler Weights ×8 | 32 B |
| +0x80 | *(optional)* ZS CRC Extension | 64 B |
| after fixed header | Render Target descriptor × N (`RGB` buffer 16 B, Midgard preload/clear overlay, format/swizzle/MSAA bits) | 64 B each |

No `next`/chain member exists in either aggregate, and `pan_mfbd.c` emits one
descriptor (`fb` + inline RT array) — there is nothing to chain and no per-FBD
tile range to split.

## 2. 512² SFBD contents

`PAN_FBD_DEBUG` (already in the device Mesa) at the same client, two sizes:

```
FBDDBG cbuf    bo=0xb118000 slice_off=0 base=0xb118000 stride=1024
               sfbd_stride=16384 tiled=1 fmt=b8g8r8a8_unorm          # 256²
FBDDBG fragjob has_draws=1 job=0xb15a5c0 fbd=0xb15a480 fb=256x256 cbufs=1
               clear=0x4 min=0,0 max=256,256 tiles=0,0..15,15
FBDDBG cbuf    bo=0xbf7d000 slice_off=0 base=0xbf7d000 stride=2048
               sfbd_stride=32768 tiled=1 fmt=b8g8r8a8_unorm          # 512²
FBDDBG fragjob has_draws=1 job=0xc0805c0 fbd=0xc080480 fb=512x512 cbufs=1
               clear=0x4 min=0,0 max=512,512 tiles=0,0..31,31
FBDDBG batch   has_draws=1 has_tiler=1 has_frag=1 first_job=0xc080440
               first_tiler=0x7fb652c000 clear=0x4 poly=0xc0b3000
```

Fields for the 512² SFBD (`fb=512x512`, tiled `b8g8r8a8_unorm`, colour clear):

| SFBD field | 512² value | How obtained |
|---|---|---|
| Parameters.Width/Height implied by Bound Max | 511 / 511 | emitter (`key.width-1`), payload `tiles 0..31` observed |
| Internal / Writeback Format | R8G8B8A8, swizzle 1546 | same emitter path as the 256² silicon decode (`gpu31-fbdtarget.md` §2) |
| Color Block Format | Tiled U-Interleaved | `tiled=1` observed |
| Dithering / Clean Pixel / Color Write Enable | 1 / 1 / 1 | `clear=0x4`, `writeback.base` observed |
| Color Writeback Base | 0xbf7d000 (this run) | `FBDDBG cbuf base` |
| Color Writeback Row Stride | 32768 (=2048 B × 16) | `FBDDBG cbuf sfbd_stride` |
| Clear Color 0..3 | `0xff1f0d0d` (=13,13,31,255, `glClearColor(0.05,0.05,0.12,1)`) | emitter |
| Tiler.Polygon List Size | `0x200200` = 0x200 + 4096·0x200 | `panfrost_flat_size()`, mask 0 |
| Tiler.Hierarchy Mask | 0 (T720 flat field: 16×16 hardware tiles) | `panfrost_choose_tile_size()` |
| Tiler.Polygon List | 0xc0b3000 (this run) | `FBDDBG batch poly` |
| Tiler.Polygon List Body | list + 0x8200 (0x200 + 4096·8) | `panfrost_tiler_header_size()` |
| Tiler.Heap Start..End | 0x2000000..0x2100000 (1 MiB) | `pan_tiler.c` emit + device heap |
| Fragment payload | `bound_min 0,0`, `bound_max 31,31`, FBD 0xc080480 (tag 0) | `FBDDBG fragjob` |

Polygon-list allocation math: `flat_size` decodes the T720 tile-size field with
an 8-px base (`(1 << (dim & 7)) * 8`), so 512² is sized as 64×64 = 4096 tiles:
header + body = `0x8200 + 0x200200 = 0x208400`, `util_next_power_of_two` ⇒
**4 MiB**. At 256² it is 1024 tiles ⇒ `0x82400` ⇒ 1 MiB. Both match the BO
sizes in the kernel `ERR-MAP` (`12=…/0x400000` at 512², `…/0x100000` at 256²).
The hardware still tiles at 16×16 (32×32 = 1024 tiles at 512²), so the 4 MiB is
a 4× over-allocation — this is the only 256→512 change in the FBD path, and it
is why an early reading mistook it for an "MFBD threshold".

## 3. Silicon: 512² renders clean without per-frame readback

`GFDEMO_BENCH=1 GFDEMO_PX=512 GFDEMO_NOREAD=1` (glFinish per frame), driver
trace armed (`trace_arm=64`), 2 frames:

```
p7580_job: dispatch slot=1 jc=0xdd49000 … sw=…00010005 …  post-start js_status=0x00000008 (NO_EXCEPTION)
p7580_job: irq slot=1 stat=0x00000002 js_status=0x00000000 (NO_EXCEPTION) has_job=1
p7580_job: dispatch slot=1 jc=0xdd49100 … sw=…0002000b …  NO_EXCEPTION → DONE
p7580_job: dispatch slot=1 jc=0xdd49200 … sw=…0003000f …  NO_EXCEPTION → DONE   (TILER)
p7580_job: dispatch slot=0 jc=0xd9165c0 … sw=…00010013 …  NO_EXCEPTION → DONE   (FRAGMENT, tiles 0..31)
… second frame: same four jobs, all NO_EXCEPTION/DONE
BENCH 512x512 frames=2 time=0.073s fps=27.4
```

dmesg after the run: **0** `p7580_job: (ERR|TIMEOUT)` lines. So one SFBD
covering 1024 hardware tiles executes to completion: the fragmentation in the
ticket was not the FBD.

## 4. Silicon: the failing job under per-frame `glReadPixels`

Same client and size, readback every frame.

### 4.1 512×512, 2 frames — frame 2 fragment job faults

```
dispatch slot=0 jc=0xe2c75c0 … sw=…00010013 …   (frame-2 FRAGMENT)
post-start slot=0 rawstat=0x00000000 js_status=0x00000008 (NO_EXCEPTION)
irq-enter rawstat=0x00010000 stat=0x00010000
ERR  slot=0 js_status=0x00000058 (DATA_INVALID_FAULT) stat=0x00010000 rawstat=0x00010000
     head=0xe2c75c0 tail=0xe2c75c0 seq=7 jc=0xe2c75c0 cfg=0x00083700
ERR-MAP slot=0 seq=7 nbos=10 h6=0xe2c7000/0x10000 h4=0xe1c4000/0x102000
     12=0xe2fa000/0x400000 …
JDBODY slot=0 seq=7 jc=0xe2c75c0 words=128
     00010258 00000000 0e2c75c0 00000000 00010013 00000000 00000000 00000000
     00000000 001f001f 0e2c7480 00000000 …
     (exc=0x58, fault_ptr=jc, type=9 FRAGMENT, bounds 0,0..31,31, FBD 0xe2c7480)
PTEWALK slot=0 seq=7 … MATCH snap=0xe2c7000/0x10000 live=0xe2c7000/0x10000
TIMEOUT slot=0 js_status=0x00000058 … head=0xe2c75c0 tail=0xe2c75c0 seq=7   (+5.006 s)
```

`BENCH 512x512 frames=2 time=0.070s fps=28.5` — i.e. `glReadPixels` returned in
70 ms while the fragment job had faulted and was not recovered until 5.0 s
later. Frame 1 (`seq=3`) had completed normally, so the fault is on a *pending*
submit whose buffers had already been handed back to Mesa; the page-table walk
says `MATCH` (not a table lifetime bug).

### 4.2 256×256, 2 frames — frame 1 fragment job hangs

```
dispatch slot=0 jc=0xe8bf5c0 … sw=…00010013 …
post-start slot=0 rawstat=0x00000000 js_status=0x00000008 (NO_EXCEPTION)
(no IRQ at all)
JDBODY … bounds 0,0..15,15, FBD 0xe8bf480
TIMEOUT slot=0 js_status=0x00000008 (NO_EXCEPTION) head=0xe8bf5c0 tail=0xe8bf5c0 seq=3  (+5.009 s)
dispatch slot=1 jc=0xe9f2000 … post-start rawstat=0x00020000 js_status=0x00000041 (POWER_FAULT)
ERR slot=1 js_status=0x00000041 (POWER_FAULT) stat=0x00020000  (post-recovery state)
```

So the stall reproduces at 256² with the same client: **not** a 512²/MFBD
property. Two outcomes are seen, a `DATA_INVALID_FAULT` or a no-IRQ hang; both
are only unwound by the 5 s sched timeout, and the first recover leaves the
next submit reporting `POWER_FAULT`.

### 4.3 `egltest` (256², documented-good client), 3 consecutive runs

```
run 1: 60 draws 0.712 s, readback 0,0,0,0 / 0,0,0,0 → RENDER_MISMATCH  (fault path)
run 2: 60 draws 0.715 s, readback 0,0,0,0 / 0,0,0,0 → RENDER_MISMATCH  (fault path)
run 3: 60 draws 0.217 s (277 FPS), corner 26,26,102 / centre 255,0,0 → RENDER_OK
```

A run that takes the fault path reads back zeros (the fragment job never wrote)
and pays the 5 s recovery; a clean run is ~3.6 ms/frame. This is the same
fault-recovery-latency signature G31_Latency quantified (286-305 fps
steady-state; 2 stalls + 300×3.5 ms = 10.4 s = 28.7 fps).

### 4.4 Why each fault costs 5.01 s

The fault IRQ only freezes the slot and clears the sticky bit; the job fence is
signalled by `timedout_job → recover` at `P7580_JOB_TIMEOUT_MS = 5000`, so
Mesa's `glFinish`/`WAIT_BO`/syncobj wait sits on an unsignalled fence for the
full timeout. G31_Latency's fix (kick `timeout_work` at delay 0 on the fault
IRQ, driver 0001 + 0004) turns that 5 s into milliseconds; it does not change
how many faults occur.

## 5. Readback path: what the evidence says

1. **Same size, only readback differs**: 512² no-readback = 0 faults, all jobs
   complete (§3); 512² with per-frame readback = fault/hang (§4.1). 256² shows
   the same split (§4.2).
2. **`glReadPixels` returns before the pending fragment job's fence** at 512²:
   the process printed `BENCH … time=0.070s` while the job's recovery
   `TIMEOUT` landed 5.0 s later (§4.1). Mesa's read path
   (`panfrost_ptr_map`, `pan_resource.c:906-916`) does
   `panfrost_flush_batches_accessing_bo(bo, false)` then
   `panfrost_bo_wait(bo, INT64_MAX, false)`; a wait that returns with the job
   still pending means the BO's reservation held no unsignalled fence for that
   submit.
3. **Driver mechanism** [INFERENCE, matches the code]: the submit's completion
   fence is published as the exclusive fence on every submitted BO inside
   `run_job` (arm time, driver 0001 §"gpu31-instrtype"), i.e. *after* the
   `SUBMIT` ioctl has returned and installed the fence on `out_sync`. A
   `WAIT_BO` issued in that window (Mesa does it immediately after the submit)
   observes only the previous fence and returns. The same driver comment
   already names the consequence of a second writer entering that window:
   "the GPU reads a torn chain/FBD -> INSTR_TYPE_MISMATCH/DATA_INVALID_FAULT".
   That is the observed `DATA_INVALID_FAULT 0x58`.
4. **Minimal fix (handed to G31_Latency, 5 lines)**: in `p7580_drv_submit`,
   after the split-queue loop and before returning, walk `objs[0..nr)` and
   `dma_resv_add_excl_fence(&to_p7580_bo(objs[i])->resv, last_fence)` with the
   split's `last_fence` (the same fence installed on `out_sync`). Publishing at
   ioctl time removes the window; the arm-time stamp can stay as a
   belt-and-braces dependency publish for later submits.
5. **Mesa-side workaround usable without a driver change**: `glFinish()` before
   `glReadPixels()` — `glFinish` waits on the submit's `out_sync` fence, which
   *is* installed during `SUBMIT`, so it does not depend on the BO stamp. The
   no-readback runs already show that ordering submission before CPU reuse is
   sufficient. `code/gpu31/mesa/mfbd/run.sh` measures all three variants.

## 6. Verification (prepared; silicon re-run pending)

`code/gpu31/mesa/mfbd/` adds:

* `rdtest.c` — 256²/512²/1024² offscreen GLES2 pbuffer, one line per frame with
  ms, corner/centre RGBA, non-zero byte count, checksum and `OK/ZERO/MISMATCH`;
  `--read 0` (clean control), `--pre-finish 1` (`glFinish` then readback),
  `--pre-finish 0` (bare `glReadPixels`).
* `run.sh` — builds `rdtest` on the phone and runs the matrix
  `sizes × {noread, read+finish, read}`, printing per-config kernel
  `ERR|TIMEOUT` counts, `js_status` histogram, the first `FBDDBG fragjob`
  `fbd=`/`tiles=` (bit 0 = MFBD tag) and `slow_frames(>=1000ms)`.

Acceptance criteria encoded there:

| Config | Expected |
|---|---|
| 512² noread, 10 frames | rc 0, `faults=0`, `slow_frames=0`, all frames drawn |
| 512² read+finish, 10 frames | rc 0, `faults=0`, every frame `OK` (pixels correct) |
| 512² read (bare), 10 frames | reproduces: `faults>0`, `slow_frames>0` |
| 1024² noread | `faults=0`; fragment payload tiles `0,0..63,63`; polygon list 16 MiB BO, 1 MiB heap unsplit (prediction, not yet run) |

Status: the device hung at the end of the 512² readback experiment
(`ssh: No route to host`, RNDIS up, `p7580` recovery pending) and needs a
physical power-cycle; no device commands were issued after that. The script and
client above are committed so the matrix runs unchanged once the phone is back.
The 512² clean-render claim (§3) is already silicon-measured; the pixel verdict
for 512² (`read+finish`) and 1024² are the two runs still outstanding.

## 7. Recommendations

* Do **not** add an MFBD path or FBD list walking for T720: 1024/4096 tiles fit
  one SFBD and one fragment job; MFBD is quirk-selected and would need a
  different descriptor entirely (`MIDGARD_SFBD` also gates MRT support in
  `pan_screen.c`).
* Land G31_Latency's fault-IRQ timeout kick; it is what converts every observed
  fault from a 5 s stall into a millisecond recovery.
* Land the submit-time BO fence publication (§5.4) if bare `glReadPixels` after
  a frame must be safe without `glFinish`; otherwise document
  `glFinish`-before-readback as the supported ordering.
* Keep the 4 MiB flat-tile polygon-list over-allocation in mind when auditing
  memory: it is `panfrost_flat_size`'s 8-px tile base, not a second FBD.
