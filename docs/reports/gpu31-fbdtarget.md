# gpu31-fbdtarget — fragment-job SFBD vs the readback BO

Date: 2026-11-21. Owner: G31_FbdTarget. Device: int34 kernel (#37,
`3.10.108`) + device Mesa 20.3.5 with patch 0007 applied, rebuilt in
`/data/mesabuild/mesa20-b`. No flash, no reboot; all device access under
`flock /tmp/gpsdev.lock`, short holds.

Question: is the missing red centre an FBD/render-target divergence (clear
and draw writing different memory, wrong stride/format/bounds), and is the
draw fragment job even emitted?

Answer: **no divergence.** There is exactly **one** fragment job per batch and
it carries *both* the clear (SFBD clear fields) and the draw (SFBD tiler
section); its SFBD render target is the **same BO** `glReadPixels` maps. The
draw chain *is* emitted (vertex + tiler + fragment, tiler descriptor says
TRIANGLES × 3, non-indexed). The triangle is missing for a different reason,
captured here as two new, evidence-backed leads (§6): the tiler leaves the
polygon list without any entry that points into its body, and the fragment
shader ISA BO reads back as **zeros in the userspace mapping itself**, with
the last fragment submit faulting `0x52 INSTR_TYPE_MISMATCH` whose
`fault_pointer` is that shader BO.

## 0. TL;DR

1. **One fragment job per batch, clear + draw together.** `has_draws=1,
   has_tiler=1, has_frag=1, clear=0x4` for every one of the 60 batches of
   one offscreen run; the trace contains exactly one `Type: Fragment` job per
   frame. Mesa 20.3 does not emit a separate "clear job": `batch->clear`
   becomes SFBD `Clear Color 0..3` + `Clean Pixel Write Enable`, the geometry
   comes from the same SFBD's Tiler section.
2. **SFBD decodes cleanly and matches the surface**: `Color Writeback Base`
   = the pbuffer BO, `Row Stride 16384` (= 1024 × 16 for tiled U-interleaved),
   `Internal Format = Color Writeback Format = R8G8B8A8`, swizzle `1546`,
   bounds 0..255, clear colours `0xff661a1a` = (26,26,102,255).
3. **Address comparison MATCH.** SFBD `Color Writeback Base = 0x5455000`;
   `glReadPixels` maps the *same* resource: `bo=0x5455000`,
   `tex_addr=0x5455000`, 256×256, tiled, `b8g8r8a8_unorm`. The clear colour
   observed at (2,2) is silicon proof that the GPU wrote exactly that BO.
4. **The draw job is emitted.** Tiler job (`type=7`, deps 1,2) primitive word
   `0x18030008`: draw mode 8 (TRIANGLES), index type 0 (non-indexed),
   `index_count = 2` (field is minus-1 ⇒ **3 vertices**), `job_task_split = 6`;
   `Draw.State` non-zero (fragment meta), `Position/UBOs/textures/samplers`
   non-zero. The fragment payload is full-surface (`tiles 0,0..15,15`, no tile
   enable map).
5. **Therefore the FBD is not the bug** — do not patch it. The remaining
   failure is upstream of the FBD and is now pinned by new instrumentation
   (§5.4/§6): the polygon list has **no tiler-written entries** (census: 1024
   entries, `nz=128`, `inrange=0` — all non-zero values are stale BO-cache
   content that does not point into the body), and the fragment-shader ISA BO
   is read as all-zero (CPU view), while the vertex-shader BO in the same
   process stays intact.
6. Delivered: this report + `code/gpu31/mesa/diag/0001-mesa203-fbdtarget-debug-print.patch`
   (env-gated `PAN_FBD_DEBUG` prints: SFBD cbuf, fragment payload, batch
   flags, job-chain raw words, polygon-list census, FS/VS meta + ISA, shader
   compile bytes, BO create/mmap/munmap, shader-BO watch). **Diagnostic only,
   not a product patch.** Kernel-side lead handed to the integrator in §7.

## 1. How the data was obtained (no kernel change)

Mesa 20.3.5 `PAN_MESA_DEBUG=trace` walks the submitted chain with pandecode
and prints the fragment job's SFBD, the tiler job's FBD, the shader
disassembly and the vertex job's descriptors
(`src/panfrost/lib/decode.c:1280+`, dump files `pandecode.dump.NNNN`;
`PANDECODE_DUMP_FILE=stderr` keeps it on stderr). That gives the submission
view with zero kernel changes.

To compare against the *readback* BO and to see what pandecode does not
print (`MIDGARD_TILER_JOB.PRIMITIVE`, the polygon list, the shader
BOs), the diagnostic patch adds, behind `PAN_FBD_DEBUG`:

| print | where | answers |
|---|---|---|
| `FBDDBG cbuf` | `panfrost_sfbd_set_cbuf()` | exact RT base/stride/format written to the SFBD |
| `FBDDBG fragjob` | `panfrost_fragment_job()` | has_draws, FBD ptr, batch min/max, tile bounds |
| `FBDDBG batch` | `panfrost_batch_submit_jobs()` | has_draws/has_tiler/has_frag, clear mask, polygon list |
| `FBDDBG chain k=…` | `panfrost_batch_submit_ioctl()` after the sync wait | raw 128 B of every job in the chain (write-value, vertex, tiler incl. Primitive and Draw.State) |
| `FBDDBG polylist` / `pcol` | same | 1024-entry header census + bitmap; body dump |
| `FBDDBG meta`/`isa` | same | FS/VS Renderer State word 0 (ISA + first tag) and the first 96 B of each ISA |
| `FBDDBG readback` | `panfrost_ptr_map()` | the BO `glReadPixels` reads, its tex_addr/stride/box |
| `FBDDBG compile`/`fsbo`/`vsbo` | `panfrost_shader_compile()`, `panfrost_prepare_midgard_fs_state()`, `panfrost_emit_compute_shader_meta()` | compile-time bytes vs. what the meta points at later |
| `FBDDBG bo create/mmap/munmap` + `WATCH` | `pan_bo.c` | BO allocation/recycling and *when* a shader BO's first 16 B change |

`PAN_FBD_REUPLOAD=1` additionally re-copies the compiled FS binary into its BO
at every meta emit (experiment, §5.4).

## 2. Decoded fragment SFBD (silicon, real submission)

Trace, 256×256 surfaceless pbuffer, `clear=0x4` (colour), one draw. Byte
offsets are from `genxml/midgard.xml`: Local Storage @0 (32 B), Parameters
@32 (40 × 32-bit words), Tiler @192 — SFBD length 320.

| SFBD field | Word | Value (decoded) | Meaning |
|---|---|---|---|
| Parameters.Internal Format | +0x20[2:0] | 1 = R8G8B8A8 | tilebuffer format |
| Parameters.Sample Count | +0x20[5:3] | 1 | single-sampled |
| Parameters.Swizzle | +0x20[17:6] | 1546 | inverted RGBA swizzle for b8g8r8a8 |
| Parameters.Color Writeback Format | +0x20[22:18] | 1 = R8G8B8A8 | writeback format |
| Parameters.Color Block Format | +0x20[27:26] | 1 = Tiled U-interleaved | tiled RT |
| Parameters.Dithering / Clean Pixel / Color Write Enable | +0x20 | 1 / 1 / **1** | colour is written |
| Parameters.Bound Min/Max X,Y | +0x28/+0x2C | 0,0 / 255,255 | full 256×256 |
| **Parameters.Color Writeback.Base** | +0x60 | **0x4eeb000** (run A) / **0x6045000** (run C) | render target IOVA |
| **Parameters.Color Writeback.Row Stride** | +0x64 | **16384** | 1024 px·B × 16 (U-interleaved) |
| CRC Write Enable | +0x24[15] | 0 | no checksum |
| ZS Write Enable / ZS Format | +0x24 | 0 / D24S8 (default field) | no depth |
| **Clear Color 0..3** | +0x90..0x9C | `0xff661a1a` ×4 | (26,26,102,255) = glClearColor(0.1,0.1,0.4,1) |
| Tiler.Polygon List Size | +0xC0 | 524800 = 0x80200 | 0x200 + 1024 × 0x200 |
| Tiler.Hierarchy Mask | +0xC4[15:0] | 0 | T720 tile-size field: 0 ⇒ 16×16 |
| Tiler.Polygon List | +0xC8 | 0x57c2000 (run A) / 0x691c000 (run C) | list base |
| Tiler.Polygon List Body | +0xD0 | list + 0x200 (run A) / +0x2200 (run C) | body offset |
| Tiler.Heap Start..End | +0xD8/+0xE0 | 0x4d68000..0x4e68000 / 0x5ec2000..0x5fc2000 | tiler heap |

Note `Polygon List Size = 0x80200` and (in the dump that printed it) body at
`+0x2200`: `panfrost_flat_size()` decodes the T720 hierarchy field with an
8-px base (`(1 << (dim&7)) * 8` in `pan/tiler.c`), so mask 0 is sized as
32×32 = 1024 tiles of the *smaller* tile, while `panfrost_choose_tile_size()`
uses a 16-px base for the same field. The allocation is therefore 4×
oversized; harmless for the hardware (which uses 16×16 tiles), but it does
mean the *body offset* Mesa hands the SFBD is 0x2200, not 0x200 — relevant
for anyone decoding the list (§5.3).

Fragment job payload (`type=9`, 64 B, at `jc`):

```
bound_min = 0,0   bound_max = 15,15   (tile units, 16-px)   tile_enable_map = 0
framebuffer = 0x4f2d4c0  (tag 0 = SFBD; not MFBD)
```

The `// XXX: tile coordinates overflow` lines pandecode prints are its own
false positives here: `info.width/height` are 0 for SFBD, so `max_x+1 > 0`
always fires. Decoded by hand, 15<<4 = 240 < 256 — in range.

## 3. Clear and draw: one job, one address

Every batch of the plain run (`PAN_FBD_DEBUG=1`, 60 batches × 2 submits):

```
FBDDBG batch   has_draws=1 has_tiler=1 has_frag=1 clear=0x4 poly=0x5dc2000
FBDDBG cbuf    bo=0x5455000 slice_off=0 base=0x5455000 stride=1024
               sfbd_stride=16384 tiled=1 fmt=b8g8r8a8_unorm
FBDDBG fragjob has_draws=1 job=0x5497600 fbd=0x54974c0 fb=256x256 cbufs=1
               clear=0x4 min=0,0 max=256,256 tiles=0,0..15,15
```

The RT base is **identical for all 60 frames** (the pbuffer resource is
reused); the SFBD/FBD *address* moves with the transient pool, the target does
not. There is no second fragment job and no second RT address anywhere in the
trace: `grep -c "Type: Fragment"` = 1 per frame, and every SFBD carries both
clear colours **and** the tiler section (polygon list + heap).

## 4. Comparison with the BO the client reads back

Readback is `glReadPixels` → `panfrost_ptr_map(PIPE_MAP_READ)` →
`panfrost_load_tiled_image()` from `rsrc->bo->ptr.cpu + slice offset` (tiled
BOs are detiled in software; there is no blit for this path).

```
FBDDBG readback rsrc=0x7f76bcc380 bo=0x5455000 tex_addr=0x5455000
               stride=1024 tiled=1 fmt=b8g8r8a8_unorm 256x256 box=2,253 1x1
FBDDBG readback rsrc=0x7f76bcc380 bo=0x5455000 tex_addr=0x5455000
               stride=1024 tiled=1 fmt=b8g8r8a8_unorm 256x256 box=128,127 1x1
```

| | SFBD (GPU write) | glReadPixels (CPU read) | |
|---|---|---|---|
| BO base | 0x5455000 | 0x5455000 | **MATCH** |
| slice offset | 0 | 0 | MATCH |
| layout | Tiled U-interleaved, row stride 16384 | tiled, CPU detile | MATCH |
| format | R8G8B8A8 (writeback) | b8g8r8a8_unorm | MATCH |
| size | bound 0..255 = 256×256 | 256×256, box (2,253)/(128,127) | MATCH |

So the clear colour the CPU reads *is* the bytes the fragment job's SFBD
targets. Since the centre reads the same clear colour, the fragment job ran
and wrote the whole surface — the draw contributed nothing to it.

## 5. Is the draw emitted, and what does the tiler see?

### 5.1 Chain

Per frame the trace shows `WRITE_VALUE(idx 2) → VERTEX(type 5, idx 1) →
TILER(type 7, idx 3)` on the vertex/tiler slot plus the FRAGMENT job
(`type 9, idx 1`) on the fragment slot. Raw words confirm the report's earlier
correction: write-value at `jc`, `next` = vertex, vertex `next` = tiler at
pool + 0x000, vertex at pool + 0xC0.

### 5.2 Tiler descriptor (pandecode does not print this section)

`FBDDBG chain k=2` (pool + 0x000):

```
+00: 00000000 00000000 00000000 00000000 0003000f 00020001 00000000 00000000
+20: 00000002 28000000 18030008 00000000 00000000 00000002 00000000 00000000
+40: 00000027 00000000 00000000 00000000 084d0040 00000000 083bb420 00000000
+60: 083bb428 00000000 083bb440 00000000 00000000 00000000 083bb3c0 00000000
```

- `w4 = 0x0003000f`: `is64=1`, `type=7` TILER; `w5 = 0x00020001`: dep1=1
  (vertex), dep2=2 (write-value) — correct ordering.
- Primitive (`w10..w15`, genxml `Primitive`): `0x18030008` ⇒ draw mode 8
  (TRIANGLES), index type 0 (**non-indexed**, as the client draws),
  `job_task_split = 6`, base vertex 0, restart index 0, **index_count field
  = 2 (minus-1) ⇒ 3 vertices**, indices 0. Correct for `glDrawArrays(GL_TRIANGLES,0,3)`.
- Draw (`w16..`): `flags=0x27`, `position=0x84d0040` (16 B/vertex fp32 vec4
  output), `uniform_buffers=0x83bb420`, `textures=0x83bb428`,
  `samplers=0x83bb440`, **`state=0x83bb3c0`** (fragment Renderer State).
  Nothing in the draw is zero or missing.

### 5.3 Polygon list census — no tiler output

`FBDDBG polylist` after the fragment submit's wait (the driver's own fence has
fired), reading the 1024 8-byte header entries that follow the 64-B prologue:

```
FBDDBG polylist bo=0x8c50000 size=1048576 nent=1024 nz=0    inrange=0
               pro=00000000 … (all zero)
FBDDBG polylist bo=0x80f6000 size=1048576 nent=1024 nz=128  inrange=0
               first=0 last=127 pro=ffffffff … 0769c000(heap start) …
FBDDBG pcol row=00 ########................................  (stale rows 0..3)
```

Across runs the result is the same in kind: either the prologue is all zero
(WORK_VALUE job ran and zeroed word 0; tiler wrote nothing at all), or the
header contains 128 non-zero entries at indices 0..127 whose values are
**outside** the polygon-list BO (`inrange=0`) and are byte-identical for all
60 batches — i.e. recycled BO-cache content from an earlier use, not this
frame's geometry. In no run does an entry point into the list body
(`list + 0x2200 + tile·0x200`), which is what the tiler writes for every
covered tile.

Verdict: the tiler job is submitted with a correct descriptor but leaves the
polygon list empty. The fragment job therefore clears tiles and invokes no
fragment shader for them — matching "clear colour at the centre".

### 5.3a Patch 0007 was NOT in the device tree (applied during this ticket)

`grep "unsigned specials" /data/mesabuild/mesa20/src/gallium/drivers/panfrost/pan_cmdstream.c`
returned nothing *before* this ticket's work: the tree the running Mesa was
built from did **not** contain patch 0007's hunk, despite the session note
that it had been applied. The patch applies cleanly
(`patch -p1 --dry-run`: "Hunk #1 succeeded at 1136, offset 30 lines") and has
now been applied and rebuilt (`ninja`: 7 steps, `libgallium_dri.so` relinked).
Result: pixels are unchanged (corner and centre both 26,26,102,255; 4
`ERR/TIMEOUT` lines in a clean window), so 0007 is not the cause of the empty
polygon list either — but the vertex attribute-overflow fix is now genuinely
in the tree, which removes one uncontrolled variable from every later run
(and is worth keeping).

### 5.4 The fragment shader BO is read as zeros

`FBDDBG compile` shows both shaders compiled and uploaded (96 B each, first
tag 5 matching the low nibble of the first ISA word, as Midgard requires):

```
compile stage=4(F) size=96 tag=5 bo=0x9d46000 bytes=c87a9835 01403d3d 00000030 …
compile stage=0(V) size=96 tag=5 bo=0x9d47000 bytes=c87a9495 08003c3d dc860940 …
```

Seconds later, in the same process, the FS BO's **CPU view** is zeros while
the VS BO is intact:

```
FBDDBG fsbo gpu=0x9d46000 size=4096 isa=c87a9835 …   ← valid
FBDDBG fsbo gpu=0x9d46000 size=4096 isa=00000000 …   ← later frames
FBDDBG vsbo gpu=0x9d47000 size=4096 isa=c87a9495 …   ← unchanged throughout
```

The `WATCH` probe (first 16 B of every EXECUTE BO) localises it: no BO
create/mmap/munmap of that address and no submit happens across the
transition — the change is observed between two ordinary Mesa calls, and one
run showed it already at the first emit (before any submit). Re-uploading the
compiled binary into the BO at every meta emit (`PAN_FBD_REUPLOAD=1`) does
**not** restore the triangle (still 26,26,102 everywhere), which says the
geometry path is empty independently of the FS bytes — but it is the FS BO
that the one remaining fault names.

### 5.5 The one remaining fault names the FS ISA

Clean run (dmesg cleared as root, one plain client run, no trace):

```
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH) … seq=121
           jc=0xb135600 sw=… 00010013 …        ← type 9 FRAGMENT
p7580_job: JDBODY … 00000052 00000000 07032005 00000000 00010013 …
                    │        │
                    │        └ fault_pointer = 0x7032005 = 4 KiB BO + tag 5
                    └ exception_status
```

`fault_pointer` has the shape of a shader address (`base | first_tag`, tag 5),
and the fault is on the *fragment* submit only, at the very last batch
(`seq=121`; the 120 working submits produce no error). No vertex faults in
this window. Earlier `DATA_INVALID_FAULT`/`POWER_FAULT` bursts in the ring
came from runs that were killed under `timeout` while a job was pending, and
are not reproducible in a clean run.

## 6. Verdict

| Hypothesis | Verdict | Evidence |
|---|---|---|
| Clear and draw use different RT addresses | **No** | one fragment job/batch, one SFBD, one base (§3) |
| RT base/stride/format/bounds wrong | **No** | §2 table; clear colour lands in the readback BO (§4) |
| Readback BO differs from the RT BO | **No** | bo == tex_addr == SFBD base == 0x5455000 (§4) |
| Draw job not emitted (clear-only fragment job) | **No** | has_tiler=1; tiler descriptor TRIANGLES × 3, non-indexed, FS state/UBO/textures set (§5.2) |
| Geometry divergence in the FBD | **No** | payload tiles 0,0..15,15 == 0..255 px; SFBD bounds 0..255 |
| Tiler produces polygons | **No** | polygon-list census: no entry points into the body (§5.3) |
| Fragment shader bytes reach the GPU | **Doubtful** | FS BO reads all-zero from the userspace mapping while the VS BO does not (§5.4); `0x52` fault_pointer is that BO (§5.5) |

Do not change the FBD emission or the Mesa-compat patch for this symptom.
The two open causes are upstream of it and are handed off below.

## 7. Handoff / next steps

1. **Kernel (integrator): audit the BO data path's cache operation
   directions.** The 4 KiB shader BO is in every fragment submit's BO list.
   In `0001` the submit path does
   `p7580_gem_submit_sync(obj)` → `dma_sync_single_for_device(…,
   DMA_BIDIRECTIONAL)` per page, and the completion path does
   `dma_sync_single_for_cpu(…, DMA_BIDIRECTIONAL)` (invalidate) before the
   fence signals. Additionally, the submit path calls
   `p7580_gem_job_done_sync(&jbo->base)` on the jc-owning BO *before* reading
   the JD body for `job->desc[]` (`submit.c`, "Same invalidate the completion
   path runs, so the snapshot sees CPU bytes"). An **invalidate** at submit
   time discards CPU-dirty lines instead of writing them back; that is the
   wrong direction for a buffer the CPU just filled. Two concrete asks:
   - prove user-mmap writes are in DRAM before the GPU reads (a debug knob
     that forces `__dma_clean_range` over a named BO would settle it), and
   - check whether the completion invalidate can run while a *later* CPU
     write is only in the write-combining buffer.
   Symptom to reproduce without GL: write a pattern into a shader-sized BO in
   userspace, submit/retire a job that references it read-only, re-read the
   mapping — the FS BO losing its 96 bytes is exactly this.
2. **Mesa (next ticket):** the last fragment submit's `0x52` with
   `fault_pointer` = the FS ISA address is the fragment-side twin of the
   vertex `0x51/0x52` fixed by patch 0007. Once (1) is settled, capture
   `FBDDBG meta FS` + `FBDDBG isa FS` at the *faulting* submit and compare the
   bytes with the host disassembly of the same shader.
3. **Patch 0007** is now applied to `/data/mesabuild/mesa20` and the DRI
   library rebuilt (verified by hunk + `unsigned specials` in
   `panfrost_emit_vertex_data`); keep it there. Re-run the polygon-list
   census (`FBDDBG polylist`) after any future driver change: a covered tile
   must leave an entry pointing into `list + body_offset + tile·0x200`.
4. **Polygon list**: if (1) turns out to be a Mesa-side/BO-lifetime issue
   instead, the census (`FBDDBG polylist`) is the assertion to keep: any
   covered tile must leave an entry pointing into `list + body_offset +
   tile·0x200`.
5. **Instrumentation hygiene:** the device tree
   `/data/mesabuild/mesa20` currently carries the diagnostic patch (all
   prints are behind `PAN_FBD_DEBUG`, so a normal run is byte-identical in
   behaviour). Apply/revert with
   `code/gpu31/mesa/diag/0001-mesa203-fbdtarget-debug-print.patch`
   (`patch -p1 -R` to revert) before any product Mesa build is made from that
   tree. It is not part of the shipped series.

## 8. Artefacts

- `code/gpu31/mesa/diag/0001-mesa203-fbdtarget-debug-print.patch` —
  diagnostic prints (`PAN_FBD_DEBUG`, `PAN_FBD_REUPLOAD`), 8 files, no
  behaviour change when the variables are unset.
- Device logs used above (on the phone, may be recycled):
  `/tmp/pandecode.dump.0001` (trace), `/tmp/d9.log` … `/tmp/d15.log`
  (`FBDDBG`), `/tmp/o16.log` (re-upload experiment).
- Reproduce: `PAN_FBD_DEBUG=1 PAN_MESA_DEBUG=sync /data/eglrun.sh` (2 files,
  first batch is enough); add `PAN_MESA_DEBUG=trace` for pandecode's SFBD
  decode. Clear dmesg through `rootshell` first, and do not `timeout`-kill a
  run you intend to read faults from (it manufactures POWER_FAULT/timeout
  lines).
