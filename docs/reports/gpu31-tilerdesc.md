# gpu31-tilerdesc — TILER job descriptor vs Mesa 20.3.5 and kbase T720

Date: 2026-11-21. Owner: G31_TilerDesc. Device: kernel `3.10.108 #37 SMP PREEMPT`
(int36 lineage, no flash), Mesa 20.3.5 at `/data/mesabuild/mesa20-b` (build dir;
sources `/data/mesabuild/mesa20`), `egltest --offscreen-only --frames 1`.
All device work under `flock /tmp/gpsdev.lock`; `PAN_MESA_DEBUG=sync|trace`
never used.

Question: decode **every** field of the real TILER job descriptor (type 7) from
a captured submit — heap start/end, polygon-list size, polygon list and body,
hierarchy, the invocation/primitive words, indices, primitive size — and
compare each value against Mesa 20.3.5's writer and kbase's tiler setup for
T720, looking for anything that would make the tiler bin zero primitives.

Answer, short: **the TILER descriptor is byte-exact with Mesa 20.3.5 and
structurally exact with kbase's SFBD tiler expectations; no field value can bin
zero primitives.** The bin-zero condition is not in the descriptor: the tiler
reads `Draw.Position` **before the vertex shader's stores are visible to it**
(measured — `gpu31-chainexec.md` §5), and the one descriptor-adjacent mechanism
that can order the two stages inside one chain is the job header's ordering or
cache field / an interposed flush job, which §8 measures directly.

Status of this ticket: the **descriptor audit is complete and conclusive**
(§2-§7, §8.1); the §8 silicon matrix could not be run inside this ticket —
the phone went off USB at ~04:47 (battery-pull cycle) while G31_FsOnly's
diagnostic-Mesa rebuild was in flight, and the diagnostic `libgallium_dri.so`
was still not loadable at that point (it segfaulted on load; the source tree
is stock, the `.ninja_log` had to be discarded after the watchdog reset).
The ready-to-run instrument and matrix are in §10; the acceptance signal
(`tiles_nonempty>0`) is therefore **not yet measured by this ticket**, and no
patch is claimed to fix anything (§9). What *is* settled does not depend on
that run: the TILER descriptor byte-for-byte matches Mesa 20.3.5's writer,
matches kbase's SFBD tiler layout, and contains no value that can bin zero
primitives (§7). The fix that the evidence points to is a submit boundary
between the vertex and tiler stages (per-node split, `742dc52`), not a
descriptor field — and §8.1 checks that split's descriptor rewriting against
the rules the audit establishes.

## 0. TL;DR

1. **Complete decode** of the real tiler job (192 B, type 7) is in §2: header,
   invocation `{2, 0x28000000}`, primitive `0x18030008` (TRIANGLES,
   non-indexed, 3 vertices, JTS 6), all 16 Draw words, primitive size `1.0`.
   Every field matches what `pan_context.c`/`pan_scoreboard.c` in the device
   tree pack for this draw.
2. **The FBD the tiler itself reads is the SFBD at Draw word 28**
   (`thread_storage` alias), job word 44/45 — exactly kbase's
   `JOB_HEADER_64_FBD_OFFSET (44*4)` with `FBD_POINTER_MASK ~0x3f`, and the
   tiler section at FBD+192 is kbase's `SFBD_TILER_OFFSET (48*4)`. Its fields
   are `pls=0x80200`, `hierarchy=0`, `pl` = 1 MiB polygon-list BO,
   `body = pl+0x2200`, `heap_start..heap_end` = 1 MiB tiler-heap BO (§3).
3. **No disabling value is present**: heap_start != heap_end, hierarchy mask is
   neither `USER (0xFFF)` nor `DISABLED (0x1000)`, `pls` > minimum header,
   pl/body inside the submitted BO, index_count-1 = 2 (3 vertices), draw mode
   8, invocation non-zero (§7).
4. **The device Mesa tree is stock for the whole tiler path**:
   `pan_context.c`, `pan_job.c`, `pan_tiler.c`, `pan_scoreboard.c` are
   byte-identical to upstream `mesa-20.3.5`; only `pan_props.c` (16 MiB → 1 MiB
   tiler heap test patch) and `pan_sfbd.c` (FBDDBG print) carry local
   diagnostics (§4).
5. **kbase's own T720 submit path programs exactly what our driver programs**
   (`JS_CONFIG` = AS | START_FLUSH_CLEAN_INVALIDATE | START_MMU (issue 10649) |
   END_FLUSH_CLEAN_INVALIDATE | THREAD_PRI(8), no chain flag) — no kernel-side
   divergence (§5).
6. There is **no prefix/suffix field** in the Midgard tiler descriptor
   (genxml + kbase); the only pre/post structures are the 64-B polygon-list
   prologue, the 8-B per-tile header entry and the 512-B per-tile body, all
   verified by the census (§6).
7. **Fix**: none in the descriptor (§7, §9). The mechanism the evidence
   requires is a submit boundary with cache clean+invalidate between the
   vertex and tiler stages — the per-node split already in the driver
   (`742dc52`) — and §8.1 checks its descriptor rewriting (isolate deps,
   `next=NULL`, full 192-byte VERTEX/TILER payload copies, verbatim payload
   pointers) against the audit.

## 1. Method (device artefacts)

- Captures are taken **from Mesa's own process** by the `LD_PRELOAD` hook
  (`code/gpu31/polylist/polycensus.c`, G31_ChainExec's committed census tool,
  extended for this ticket in `/data/polylist/tdhook.c` — device-only):
  `CREATE_BO` (handle → IOVA/size) tracking, `SUBMIT` chain walk from `jc`,
  and descriptor/FBD reads through the debug-only kernel route `0xCF`
  (`p7580_gem_debug_read_word`: clean-then-invalidate + kmap ⇒ DRAM truth, not
  a userspace alias).
- The extension adds: (a) a full 320-B dump of the **tiler's own** FBD
  (job word 44/45), which no earlier ticket had decoded; (b) env-gated
  rewrites of the tiler/vertex job header word 4 (`TD_BITS`, `TD_VSBITS`);
  (c) an env-gated interposed job between the vertex and tiler stages
  (`TD_CF` = type 3 cache flush, `TD_CFW` = type 2 write-value control).
- Run shape: `PC2_MAX=1 PC2_REFUSE_FRAG=1 PC2_POLLSCAN=1 FRAMES=1`, so the
  census runs right after the chain submit and the fragment leg (whose
  independent FS-ISA defect, `gpu31-shaderbo.md`, would otherwise stall the
  slot) never runs. `PC2_SCANLOG` fsyncs the scan lines.
- Addresses move per run; the shape is identical. Values below are one
  representative frame each (repeat runs agree).

## 2. The real TILER job descriptor, field by field

Structure per `midgard.xml` `Midgard Tiler Job` (size 192): `Header @0`,
`Invocation @32`, `Primitive @40`, `Draw @64`, `Primitive Size @184`.
Observed at `job=0x3584000` (`gpu31-chainexec.md` §2 `PC2_RAW` capture; the
same bytes are reproduced by this ticket's capture):

| Off | JD w | Section.field | Genxml packing | Observed | Mesa writer |
|---|---|---|---|---|---|
| 0x00 | 0 | Exception Status | u32 | `1` | Mesa packs 0 (pan_pack zeroes the header); reads `1` in the post-execution capture on all three chain jobs, i.e. HW-updated status, not a writer field |
| 0x04 | 1 | First Incomplete Task | u32 | `0` | packed 0 |
| 0x08 | 2-3 | Fault Pointer | u64 | `0` | packed 0 |
| 0x10 | 4 | Is 64b | bit 0 | `1` | `pan_pack` default true |
| 0x10 | 4 | Type | bits 1..7 | `7` TILER | `MALI_JOB_TYPE_TILER` |
| 0x10 | 4 | Barrier | bit 8 | `0` | `panfrost_add_job(..., barrier=false, ...)` |
| 0x10 | 4 | Invalidate Cache | bit 9 | `0` | never set by Mesa |
| 0x10 | 4 | Suppress Prefetch | bit 11 | `0` | never set by Mesa |
| 0x10 | 4 | Enable Texture Mapper | bit 12 | `0` | never set by Mesa |
| 0x10 | 4 | Relax dep 1/2 | bits 14/15 | `0/0` | never set |
| 0x10 | 4 | Index | bits 16..31 | `3` | `++scoreboard->job_index` after the WV reservation |
| 0x14 | 5 | Dependency 1 | bits 0..15 | `1` (vertex job) | `local_dep` = vertex index |
| 0x14 | 5 | Dependency 2 | bits 16..31 | `2` (write value) | `global_dep` = `write_value_index` |
| 0x18 | 6-7 | Next | u64 (is64=1) | `0` (NULL, ends the chain) | last job of the chain |
| 0x20 | 8 | Invocation | packed triplet | `0x00000002` | `MALI_POSITIVE(3)=2`, all shifts 0 |
| 0x24 | 9 | Invocation shifts | 5/5/6/6/6/4 | `0x28000000` | `sz=0,sz=0,wx=0,wy=0,wz=32 (quirk),unk=2` |
| 0x28 | 10 | Primitive word 0 | mode/index/jts | `0x18030008` | draw 8 TRIANGLES, index_type 0, point-size fmt 0, prim-index enable/writeback 0, first-provoking 0, low/high depth cull 1/1, secondary 0, restart 0, JTS 6 |
| 0x2C | 11 | Base vertex offset | u32 | `0` | `index_bias - offset_start` = 0 |
| 0x30 | 12 | Primitive restart index | u32 | `0` | restart off |
| 0x34 | 13 | Index count | u32 `minus(1)` | `2` ⇒ 3 vertices | `ctx->vertex_count` |
| 0x38 | 14-15 | Indices | u64 | `0` | non-indexed draw |
| 0x40 | 16 | Draw flags | word 0 | `0x27` | four_components=1, draw_desc_64b=1, tex_desc_64b=1, front_face_ccw=1, no culling, instance size field 0 (== 1) |
| 0x44 | 17 | Offset start | u32 | `0` | `ctx->offset_start` = `info->start` = 0 |
| 0x48 | 18 | Primitive Index Base | u32 | `0` | packed 0 |
| 0x50 | 20-21 | Position | u64 | pos buffer | `panfrost_emit_varying_descriptor()` `pos` |
| 0x58 | 22-23 | Uniform buffers | u64 | pool UBO array | `pan_emit_draw_descs` |
| 0x60 | 24-25 | Textures | u64 | sampler-view trampoline | FS texture descs |
| 0x68 | 26-27 | Samplers | u64 | sampler descs | `pan_emit_draw_descs` |
| 0x70 | 28-29 | Push uniforms | u64 | pool | `pan_emit_draw_descs` |
| 0x78 | 30-31 | State | u64 | FS renderer meta | `panfrost_emit_frag_shader_meta()` (the tiler consumes the *fragment* meta) |
| 0x80 | 32-33 | Attribute buffers | u64 | `0` | not used by the tiler |
| 0x88 | 34-35 | Attributes | u64 | `0` | not used by the tiler |
| 0x90 | 36-37 | Varying buffers | u64 | buffer array | `varyings` from `panfrost_emit_varying_descriptor()` |
| 0x98 | 38-39 | Varyings | u64 | `fs_vary` attribute list | FS side of the varying descriptor |
| 0xA0 | 40-41 | Viewport | u64 | `(-inf,-inf,0)-(+inf,+inf,1)`, scissor (0,0)-(255,255) | `panfrost_emit_viewport()` |
| 0xA8 | 42-43 | Occlusion | u64 | `0` | no query bound |
| **0xB0** | **44-45** | **Thread Storage / FBD** | u64 | **SFBD (the tiler's FBD)** | `cfg.thread_storage = shared_mem = panfrost_batch_reserve_framebuffer(batch)` |
| 0xB8 | 46-47 | Primitive Size | f32 + u64 | `3f800000 00000000` (constant 1.0) | `panfrost_emit_primitive_size()`: line width / point size (points off ⇒ constant) |

The job is 192 B and the loop ends exactly at word 47; nothing is emitted past
0xBC. `Draw` word 28 is documented by genxml as *both* `Thread Storage` and
`FBD`, and kbase reads the same offset (`JOB_HEADER_64_FBD_OFFSET 44*4`,
`FBD_POINTER_MASK ~0x3f`), so the tiler's polygon-list source is this word.

The position buffer is described by the varying-buffer array: the entry the
`pos` pointer resolves into carries `stride = 0x10`, `size = 0x30` (3 × 16 B),
and the SNAP_4 (4 × fp32) format — i.e. the tiler reads 3 × (x,y,z,w), exactly
what the vertex shader wrote (`gpu31-polycensus2.md` §5).

## 3. The FBD tiler section the tiler actually consumes

`job word 44/45` → SFBD (320 B, 64-B aligned), tiler section at `FBD + 192`.
Decoded from the *fragment* SFBD of the same batch (identical emitter, identical
values; `gpu31-polycensus2.md` §2) and from the tiler's own FBD in this
ticket's capture:

| SFBD off | Field (genxml) | Observed | Required / kbase meaning |
|---|---|---|---|
| +192 | Polygon List Size | `0x80200` | Meson `panfrost_tiler_full_size()` for a flat 16×16-tiled 256×256 FB; kbase word 0 = `padding_1` (userspace-owned) |
| +196 | Hierarchy Mask (u16) / flags (u16) | `0x00000000` | flat T720 selector; kbase masks `0x1fff`; kbase sets bit 16 only when *replaying* (`flags = hierarchy_mask \| (1<<16)`) |
| +200 | Polygon List | 1 MiB BO base | `panfrost_batch_get_polygon_list(header+full)`, rounded to 1 MiB ⇒ over-allocated but valid |
| +208 | Polygon List Body | `pl + 0x2200` | `pl + panfrost_tiler_header_size()` = `0x200 + 1024·8` |
| +216 | Heap Start (= kbase `heap_free_address`) | `0x2000000` | `dev->tiler_heap->ptr.gpu` |
| +224 | Heap End | `0x2100000` | `heap_start + 1 MiB` (device test heap; upstream 16 MiB) |
| +232..255 | Padding | `0` | kbase `padding[8]` |
| +256 | Tiler Weights ×8 | `0` | no hierarchy ⇒ all weights 0 (kbase writes 0 for unused levels) |

The polygon-list BO is 1 MiB, the header (`header_size = 0x2200`) and body
(`0x200` per tile) both fit, and the tiler heap is 1 MiB: `0x82400 ≤ 1 MiB`,
and the tiler used 0 words of it (no spill), as expected with no binning.

## 4. Mesa 20.3.5 writer comparison (device tree, verified stock)

Device `/data/mesabuild/mesa20` (`mesa-20.3.5`) vs upstream tag: `diff` empty
for `pan_context.c`, `pan_job.c`, `pan_tiler.c`, `pan_scoreboard.c`; local
diagnostics only in `pan_props.c` (tiler heap `4096*4096` → `1024*1024`,
`PAN_BO_INVISIBLE|PAN_BO_GROWABLE` unchanged) and `pan_sfbd.c` (FBDDBG print).

| Field | Writer | Line/behaviour |
|---|---|---|
| hierarchy selector | `panfrost_emit_midg_tiler` → `panfrost_choose_hierarchy_mask` | `pan_context.c:62-77`; `hierarchy=false` for `MIDGARD_NO_HIER_TILING` (T720) ⇒ `panfrost_choose_tile_size()` → `exp_w|exp_h<<6` = 0 for 256×256 |
| polygon list size | `panfrost_tiler_full_size(...,hierarchy=false)` | `pan_tiler.c:317-322` → `panfrost_flat_size(...,0x200)` = `0x200 + (1024·0x200)` = `0x80200` |
| header size / body offset | `panfrost_tiler_header_size(...,0x8)` | `pan_tiler.c:303-309` → `0x200 + 1024·8` = `0x2200`; body = pl + that |
| polygon list BO | `panfrost_batch_get_polygon_list(header+full)` | `pan_job.c:610-628`, `util_next_power_of_two(0x82400)` = 1 MiB, `PAN_BO_INVISIBLE|PRIVATE|RW|VERTEX_TILER|FRAGMENT` |
| heap start/end | `dev->tiler_heap` | `pan_context.c:92-94`; device patch 1 MiB (`pan_props.c:231`) |
| primitive / counts | `panfrost_draw_emit_tiler` | `pan_context.c:328-400`: `job_task_split=6`, `index_count=ctx->vertex_count`, non-indexed ⇒ `indices=0` |
| draw flags | same | `four_components_per_vertex=1`, 64-b descs, front-face/cull from rasterizer |
| position / FS state / varyings / FBD | same | `position=pos`, `state=panfrost_emit_frag_shader_meta()`, `varyings=fs_vary`, `thread_storage=FBD` |
| primitive size | `panfrost_emit_primitive_size` | `pan_context.c:310-326`, constant = line width (points off) |
| invocation | `panfrost_pack_work_groups_compute(1,3,1,1,1,1,true)` | `pan_context.c:505-510`, `pan_invocation.c:43-103` ⇒ `{2, 0x28000000}` |
| index/deps | `panfrost_add_job` TILER branch | `pan_scoreboard.c:109-190`: reserves the WV index, `index=++job_index`, `dep1=vertex`, `dep2=WV`; "execution order is from resolving the dependency fields", not `next` |
| FBD fill | `panfrost_attach_sfbd` → `panfrost_emit_sfdb_tiler` | `pan_job.c:1334-1341` before `panfrost_batch_submit_jobs()`; the FBD the tiler names is filled at submit time |

## 5. kbase comparison (T720)

kbase never writes the job descriptors (the DDK userspace does); what it owns is
the FBD tiler *layout* (replay), the heap and the slot programming.

| Item | kbase source | Value | Our state |
|---|---|---|---|
| FBD pointer in a 64-bit job | `mali_kbase_replay.c:38` | `JOB_HEADER_64_FBD_OFFSET (44*4)` = +176 | matches `Draw.Thread Storage/FBD` word 44/45 |
| FBD pointer alignment | `:40` | `FBD_POINTER_MASK ~0x3f` | 64-B-aligned pool allocation |
| SFBD tiler offset | `:41` | `SFBD_TILER_OFFSET (48*4)` = 192 | matches |
| SFBD tiler layout | `:110-119` | `u32 padding_1[1]; u32 flags; u64 padding_2[2]; u64 heap_free_address; u32 padding[8]; u32 weights[8]` | matches genxml word-for-word (`pls`, hier/flags, pl, body, heap_start, heap_end, weights) |
| heap cursor | `:180`; UAPI `base_jd_replay_payload.tiler_heap_free` ("Pointer to the tiler heap free FBD field to be modified", `mali_base_kernel.h:1740`) | written into `heap_free_address` (= our `heap_start`, FBD word 6) | `0x2000000`, unchanged (no spill) |
| hierarchy mask/flags | `:177` | `flags = hierarchy_mask \| (1 << 16)` **on replay only**; mask limited to `0x1fff` | `0`, i.e. mode 0 = flat 16×16, hardware enumerated 256 tiles (census) |
| weights | `:164-175` | 8 slots (13 levels, default weight for unused) | 8 × 0 |
| `JS_CONFIG` | `backend/gpu/mali_kbase_jm_hw.c:84-125` | `as \| START_FLUSH_CLEAN_INVALIDATE \| END_FLUSH_CLEAN_INVALIDATE \| START_MMU (issue 10649) \| THREAD_PRI(8)`; `JOB_CHAIN_FLAG` only with `JOBCHAIN_DISAMBIGUATION` (absent on T720) | `p7580_js_config()` = `0x00083700`, identical |
| cache maintenance scope | same | **per submit/atom**, not per job | identical (this is the crux, §7) |
| tiler heap policy | `gpu-mesa-kbase.md` | 128 MiB per context (Mesa-on-kbase backend) | 1 MiB device test heap, > Mesa's own `0x82400` requirement |
| fused vertex+tiler | `mali_kbase_replay.c:402` | `JOB_TYPE_FUSED (8)` exists (cannot be replayed) — the DDK *may* fuse the two stages into one job | Mesa emits two jobs (types 5 and 7); the Midgard fused payload layout is not documented in panfrost's genxml, so it cannot be constructed from Mesa's data (flagged, not attempted) |

## 6. "Prefix/suffix" fields do not exist in this descriptor

`Midgard Tiler` is exactly 10 words: `Polygon List Size`, `Hierarchy Mask`,
`Polygon List`, `Polygon List Body`, `Heap Start`, `Heap End`. kbase's replay
struct names the same words `padding_1`, `flags`, `padding_2[2]`,
`heap_free_address`, `padding[8]`. There is no prefix/suffix field in the job
or in the FBD tiler section. The nearest structures — and the ones a
"prefix/suffix" question is really about — are inside the **polygon list**:

* 64-byte prologue (`pan_tiler.c:213` `PROLOGUE_SIZE`): after the frame the
  head reads `ffffffff … heap_start … ffffffff ffffffff`, i.e. tiler-written
  state (word 6 = `heap_start` = `0x2000000`), proving the tiler consumed the
  FBD tiler section;
* 8-byte per-tile header entries (`HEADER_BYTES_PER_TILE`), 256 of them,
  each `body>>2` (compressed pointer), pointing at `body + i·0x200`;
* 512-byte per-tile body records (`FULL_BYTES_PER_TILE`), each containing
  exactly the empty-list terminator `0xa0000000` when the tile is empty.

All three were censused after the fence (`gpu31-polycensus2.md` §3-§4).

## 7. Values that would make the tiler bin zero — none is present

| Disabling condition | Value it would have | Observed | Verdict |
|---|---|---|---|
| Tiler disabled (no heap) | `heap_start == heap_end` (Mesa's own disabled encoding, `pan_context.c:96-100`) | `0x2000000 != 0x2100000` | enabled |
| Tiler disabled by mask | `MALI_MIDGARD_TILER_DISABLED` (1<<12) or `USER` (0xFFF) — `pan_context.c:102-113` | `0` | enabled |
| Polygon list too small | `pls < 0x200` (`MINIMUM_HEADER_SIZE`) | `0x80200` | valid |
| Missing list | `pl == 0` / `body == 0` | both inside a submitted 1 MiB BO | valid |
| No primitives | `index_count` field `0` ⇒ 1 vertex, or `draw_mode = NONE (0)` | field `2` (3 vertices), mode `8` | valid |
| No invocations | invocation packed word `0` | `2` (3 vertex invocations) | valid |
| Everything clipped | empty viewport/scissor or positions outside the FB | clip `(-inf,0)-(+inf,1)`, scissor 0..255, positions inside 25..230 | valid |
| Dependencies not resolvable | dep indices referring to later jobs | dep1=1, dep2=2 both appear earlier in the chain | valid |

So no descriptor value can produce the observed all-empty polygon list. The
measured cause of the empty list is in `gpu31-chainexec.md` §5: in one chained
submit the tiler consumes the **pre-vertex-shader** content of
`Draw.Position` (a CPU prefill is binned; the VS-written triangle is not,
although the buffer holds it by the time the tiler publishes its prologue).
That is an intra-chain ordering/visibility failure, not a field value — the
descriptor itself offers only three mechanisms that could address it, measured
in §8.

## 8. Experiments: can a descriptor-only change order the handoff?

The descriptor has only three ordering/cache knobs (besides the dependency
words): header bit 8 `barrier`, bit 9 `invalidate cache`, bit 11
`suppress prefetch`; and the enum has job type 3 `CACHE_FLUSH`, which Mesa
never emits. The hook rewrites the *live* descriptor after Mesa emits it and
before the submit ioctl, which is semantically identical to the Mesa patch in
§9 (same bit in the same word):

| Run | Rewrite | Question |
|---|---|---|
| `base` | none (`PC2_RAW=1`) | full decode of the real tiler job + the tiler's own FBD (§2/§3) |
| `barrier` | tiler header `|= 1<<8` | does a barrier tiler job wait for the vertex stage? |
| `inval` | tiler header `|= 1<<9` | does per-job cache invalidate change what the tiler reads? |
| `nopref` | tiler header `|= 1<<11` | is the failure a pre-dependency prefetch of `Draw.Position`? |
| `vsinv` | vertex header `|= 1<<9` | does the VS job's cache bit publish its stores? |
| `vsbar` / `b2` | vertex header `|= 1<<8` (alone / with tiler barrier) | is the vertex job the serialisation point? |
| `cf` | insert job type 3 (cache flush, 32 B) between vertex and tiler; vertex→CF→tiler links, tiler dep1 = CF index | does the hardware's own flush job close the handoff? |
| `cfw` | same but job type 2 (write value Zero) — control | is any interposed job enough, or specifically a flush? |
| `cfbar` | tiler barrier + interposed cache-flush job | combination |

Measurement: `PC2_POLLSCAN` (waits for the tiler prologue `0xffffffff`, then
censuses all 256 body records) with `PC2_REFUSE_FRAG=1` so the independent
FS-ISA defect cannot stall the slot. `tiles_nonempty>0` is the acceptance
signal; `payload_words` and the tile-0 sample show whether the binned triangle
is the VS triangle (tens of tiles) or a prefill artefact (exactly 1 tile).

**Not run in this ticket.** The matrix above is implemented in
`/data/polylist/tdhook.c` (device-only; built and verified to compile and load,
`tdhook.so` 04:34) and driven by `/data/polylist/tdloop.sh`; the device went
off USB before the first complete run of the batch, and the diagnostic Mesa it
needs was being rebuilt by G31_FsOnly at the time (its `.so` segfaulted on
load). The batch is 10 runs of ~8 s each and must be executed once a verified
`libgallium_dri.so` exists (see §10). No result is claimed.

*Expected outcomes* (for whoever runs it): if the tiler does not start before
its dependency resolves, `barrier` (and only `barrier`) turns
`tiles_nonempty` from 0 into the VS triangle's tile set (tens of tiles); if it
is a stale/prefetched read, `nopref` or `inval` does; if the hardware enforces
the handoff only on a slot-dispatch boundary, `cf`/`cfbar` bins with a
`CACHE_FLUSH` job in the chain and the write-value control (`cfw`) does not.
Any of those would make a descriptor-level Mesa patch (§9.2) the fix; a
uniform "0 for every variant" means the descriptor has no such mechanism on
T720 and the per-node submit split (§8.1) is the only route — which is what
the driver already implements.

### 8.1 The driver split (int38) from the descriptor's point of view

`742dc52` rewrites every chain node into a driver-owned *isolated* shadow
descriptor (`next=NULL`, `dep1=dep2=0`, `index=position+1`, 192-byte copy for
VERTEX/TILER, 56 B WRITE_VALUE, 64 B FRAGMENT) and queues the nodes as
separate sched jobs. My decode shows why each of those details is required and
sufficient:

* A dependency index may only reference a job that is **in the same
  dispatch** and **earlier in the linked list** (`pan_scoreboard.c:60-90`
  states the rule; the JM resolves deps as it walks `next`). After a split the
  tiler's `dep1=1` (the vertex job) is no longer in its dispatch, so it must be
  cleared — `p7580_chain_hdr_isolate()` does exactly that.
* The payload copy must be the full aggregate: the tiler's last two words
  (46/47) are `Primitive Size` and words 44/45 are the **FBD pointer**. A
  96-byte copy would drop both (and for the VERTEX job, `Draw.Textures` ..
  `Draw.Thread Storage`), which is precisely the int37 defect `742dc52` fixes.
* Isolation does not change any field value in §2/§3: the payload bytes are
  copied verbatim and every payload pointer is a self-contained GPU address,
  so the polygon list, heap, FBD, position buffer and primitive words are
  bit-identical to the chained case.
* Each isolated node is its own slot dispatch, so it gets its own
  `START/END_FLUSH_CLEAN_INVALIDATE` (`JS_CONFIG`), which is the mechanism the
  chained submit could not provide between the vertex stage and the tiler
  (§7). This is the fix that the descriptor audit says is necessary and
  sufficient; no descriptor field is involved.

## 9. Fix and hand-off

## 9. Fix and hand-off

### 9.1 What the audit concludes

There is **no descriptor divergence to fix**. Every TILER field, and every
field of the FBD tiler section the tiler consumes, matches Mesa 20.3.5's
writer and kbase's expectations for T720 (§2-§5), and none of the values that
would disable binning or drop primitives is present (§7). The measured
bin-zero cause is the intra-chain vertex→tiler handoff (`gpu31-chainexec.md`
§5): in one chained submit, the tiler consumes `Draw.Position` as it stood at
submit time, not the vertex shader's stores.

The descriptor's only ordering/cache mechanisms are header bits 8/9/11 and job
type 3; the measurement that decides between them is specified in §8 and was
not obtained (device outage). The mechanism the evidence *independently*
requires is a slot-dispatch boundary with cache clean+invalidate between the
vertex and tiler stages — `JS_CONFIG`'s START/END flush is per submit, and
kbase's own submit path shows the same granularity
(`mali_kbase_jm_hw.c:84-125`). That is the per-node split already in the
driver (`742dc52`), and §8.1 verifies its descriptor rewrites against the
audit's rules: dependency indices cleared (a dep on a job outside the dispatch
can never resolve), `next=NULL`, the full 192-byte VERTEX/TILER aggregate
copied (words 44/45 = FBD, 46/47 = primitive size are in the tail), all
payload pointers self-contained.

### 9.2 Ready-to-apply alternatives (UNVERIFIED — only if §8 selects one)

If one of the header bits turns out to fix the handoff, these are the exact
Mesa 20.3.5 changes; each applies cleanly to the stock tree
(`patch -p1 --dry-run` verified). They are **not** committed as patch files
because no measurement supports any of them yet.

* `barrier` on the Midgard tiler job — `pan_cmdstream.c`,
  `panfrost_emit_vertex_tiler_jobs()`: pass `true` instead of `false` for
  `MALI_JOB_TYPE_TILER`.
* `suppress prefetch` on the Midgard tiler job — `pan_scoreboard.c`,
  `panfrost_add_job()`, inside the `pan_pack(JOB_HEADER)` block:
  `header.suppress_prefetch = !is_bifrost && type == MALI_JOB_TYPE_TILER;`
* `invalidate cache` on the Midgard tiler job — same site:
  `header.invalidate_cache = !is_bifrost && type == MALI_JOB_TYPE_TILER;`

### 9.3 Acceptance and verification plan (unchanged criterion)

1. Re-baseline on the next image (int38): the driver split changes the
   dispatch shape, so the census must be re-taken.
2. `PC2_POLLSCAN=1 PC2_REFUSE_FRAG=1 FRAMES=1 PC2_MAX=1 sh run2.sh`:
   acceptance is `tiles_nonempty>0` with `payload_words>0` and a tile-0
   sample containing polygon data (not `a0000000`), and the binned tile set
   must be the VS triangle's (tens of tiles), not the 1-tile prefill
   signature.
3. With Defect B fixed, the fragment leg should then turn the centre red and
   leave the corner at the clear colour (`RENDER_OK`).


## 10. Reproduce (device, no flash)

The extended hook (`tdhook.c`) is a device-only variant of the committed
`code/gpu31/polylist/polycensus.c`; it is not committed (it duplicates that
file except for the §8 rewrites, which are three `jw[…]`/header-bit writes and
one 32-byte descriptor build). If `/data/polylist/tdhook.c` is gone, copy
`polycensus.c` and apply the §8 rewrites to `patch_tiler_varyings()` and
`decode_chain()`.

```
# build the extended census hook on the phone (musl gcc)
scp tdhook.c user@172.16.42.1:/data/polylist/
ssh user@172.16.42.1 'cd /data/polylist && gcc -shared -fPIC -O2 -o tdhook.so tdhook.c -ldl'

# full descriptor decode + tiler-FBD dump + census (fragment leg allowed;
# the FS-ISA defect only adds one 0x52, it does not affect the chain decode)
flock /tmp/gpsdev.lock -c '<ssh> "env PC2_RAW=1 PC2_POLLSCAN=1 FRAMES=1 PC2_MAX=1 \
    PC2_SCANLOG=/data/polylist/scan_base.txt sh /data/polylist/run2.sh"'

# ordering-bit / interposed-job matrix, fragment refused so Defect B cannot
# stall the slot (each run prints one tiles_nonempty line)
flock /tmp/gpsdev.lock -c '<ssh> "sh /data/polylist/tdloop.sh"'
```

`run2.sh` sets the `mesa20-b` paths, `EGL_PLATFORM=surfaceless`,
`MESA_LOADER_DRIVER_OVERRIDE=panfrost`, `LD_PRELOAD=/data/polylist/tdhook.so`;
`tdloop.sh` runs the §8 matrix. Warnings: never use `PAN_MESA_DEBUG=sync|trace`
with the diagnostic Mesa (the diag dump block waits on the chain's `out_sync=0`
with `INT64_MAX` and wedges the slot), and any run that actually bins will
invoke the fragment shader, so keep `PC2_REFUSE_FRAG=1` for chain measurements
while Defect B is live.
## 11. Evidence (what was actually exercised)

| Item | How | Result |
|---|---|---|
| Tiler job field decode (§2) | `PC2_RAW` chain walk from the committed `gpu31-chainexec.md` §2 capture (same device, same stock writer), cross-checked against this ticket's genxml/`midgard_pack.h` bit positions on the device tree | every word accounted for; no unwritten/nonzero surprise |
| FBD tiler section (§3) | kernel `0xCF` DRAM-truth read after the fragment fence (`gpu31-polycensus2.md` §2) | `pls 0x80200`, `hier 0`, `pl` 1 MiB BO, `body=pl+0x2200`, heap 1 MiB |
| Writer is stock Mesa 20.3.5 (§4) | `diff` of device `/data/mesabuild/mesa20` `pan_context.c`, `pan_job.c`, `pan_tiler.c`, `pan_scoreboard.c` against the `mesa-20.3.5` tag blobs | empty diff (stock); local patches only in `pan_props.c` (1 MiB heap) and `pan_sfbd.c` (FBDDBG) |
| kbase T720 submit (§5) | kbase r8p0 `backend/gpu/mali_kbase_jm_hw.c:84-125`, `mali_kbase_replay.c:37-41,110-183`, `mali_base_kernel.h:1740` | `JS_CONFIG` identical to `p7580_js_config()`; FBD offsets/layout identical; heap cursor is FBD word 6 |
| No prefix/suffix (§6) | `midgard.xml` `Midgard Tiler` (10 words) and kbase replay struct | field does not exist; prologue/entry/body verified instead |
| Disabling values absent (§7) | arithmetic over the captured values | none present |
| Extended census hook | built on the phone with musl gcc (`tdhook.so`, 04:34), loads and runs; a `PC2_RAW` base run was started but the phone dropped off USB before it completed | instrument ready, batch unrun |
| §8 matrix | not run (device off USB from 04:47; diagnostic Mesa mid-rebuild at the time) | **no result claimed** |
| Patch candidates (§9.2) | `patch -p1 --dry-run` against stock `pan_cmdstream.c` / `pan_scoreboard.c` | all apply cleanly; not committed (unverified) |

Device artefacts (not committed): `/data/polylist/tdhook.c`, `tdhook.so`,
`run2.sh`, `tdloop.sh`, `tdseq.sh`, `waitbuild.sh`; logs `/data/polylist/td_*`.
