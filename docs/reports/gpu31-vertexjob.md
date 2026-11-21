# gpu31-vertexjob — VERTEX job decode, shader-meta pointer chain, INSTR fault

Date: 2026-11-21. Owner: G31_VertexJob. Static ticket: no flash, no device
writes. Phone was off USB during the whole ticket (owns the JDBODY log).
Sources: Mesa 20.3.5 (fetched from the mesa-20.3.5 tag, same tree as the
device's `/data/mesabuild/mesa20`), the generated `midgard_pack.h`
(`gen_pack.py genxml/midgard.xml`, run in this ticket — every `_LENGTH` and
`bit(a,b)` below is quoted from that generator, not from memory), and our
driver canon `code/gpu31/driver/0001-…`.

Related: `gpu31-microdraw.md` (the faults this ticket explains),
`gpu31-jddecode.md` (JDBODY/PTEWALK), `gpu31-jobabi.md` (32-B header),
`gpu31-polylist.md` (tiler heap), `gpu31-serialfence.md` (int34).

## 0. TL;DR

1. **The chain-head VERTEX job is a well-formed Midgard v5 `COMPUTE_JOB`**,
   192 B (0xC0) — `Header@0 / Invocation@32 / Parameters@40 / Draw@64`,
   `job_task_split = 5`, type 5, `is64 = 1`. **Every one of its 48 words
   decodes legally** under `midgard_pack.h`; nothing in it is a Bifrost-class
   encoding. Decode table in §2.
2. **Two numbers in `gpu31-microdraw.md` were misread** (fixed here, §5):
   `head = jc + 0xC0` is wrong — the observed pair
   (`head=0x88d60c0`, `jc=0x88d6580`) has **head = jc − 0x4C0**, i.e. the
   vertex job is at pool + 0xC0 (tiler#0 at pool + 0x00, vertex#0 right after
   it) and `jc` is the WRITE_VALUE job allocated *after* the varying/FBD
   data. And `sw[4] = 0x00020005` does **not** decode as VERTEX: it is
   `type=2` (**WRITE_VALUE**), `is64=1`, `index=2`. `sw[]` is the snapshot of
   `jc` (the chain head), so it can never describe the faulting vertex job.
   Consequence: **the faulting VERTEX job has never been dumped**. The
   JDBODY of the microdraw chain starts at `jc` = BO+0x580, while the fault
   is at BO+0xC0 — a 0x4C0-byte miss.
3. `0x51 INSTR_INVALID_ENC` / `0x52 INSTR_TYPE_MISMATCH` are **vertex/tiler
   shader-core instruction faults** (`mali_midg_regmap.h:459-466`,
   `code/gpu31/jobs/panfrost7580_jm_hw.h:166-188`). No job-header or
   pointer-syntax error can produce them. The only datum the vertex core
   decodes as *instructions* is the ISA reachable through
   **`DRAW.State` (JD word 30/31, +0x78) → Renderer State +0 → 
   `Renderer State.Shader.Shader` (word 0/1, low nibble = first bundle tag)
   → ISA bytes**. That chain is the offending pointer region (§3).
4. Reachability is *correct by construction*: both the Renderer-State upload
   BO and the ISA BO are added to the batch (`panfrost_batch_add_bo`) and are
   therefore in `submit.bo_handles`, and the driver maps every handle. The
   memattr index (0 = kbase `AS_MEMATTR_INDEX_DEFAULT`, 0x48) is the same
   default kbase uses on this silicon, so it is eliminated (§4).
5. Delivered: **driver patch 0003** — fault-target decode at JS_HEAD
   (HEADTYPE/VDRAW/VDRAW2/VMETA/VISA), which *is* the missing capture; and
   **Mesa 20.3.5 patch 0007** — a real out-of-bounds write + garbage
   descriptors in `panfrost_emit_vertex_data` for exactly the
   `gl_VertexID`-only shader shape that produced `0x51` (the attribute shapes
   produced `0x52`). §6.
6. Silicon verification is one run + one dmesg grep, no flash (§7/§8). The
   new lines decide between "not in the submit set" (`own=MISS`), "meta
   wrong" (VMETA counts/state), and "ISA bytes/tag wrong" (VISA).

## 1. Where the vertex job comes from

Mesa 20.3.5 `src/gallium/drivers/panfrost/pan_context.c`:

- `panfrost_draw_vbo()` allocates, per draw, a **tiler** job
  (`MALI_MIDGARD_TILER_JOB_LENGTH` = 192) and then a **vertex** job
  (`MALI_COMPUTE_JOB_LENGTH` = 192), both 64-B aligned (`:474-480`). Tiler
  first, so in the transient pool the tiler is at offset 0 and the vertex at
  offset 0xC0. This is the origin of the observed `head` at BO+0xC0.
- `panfrost_draw_emit_vertex()` (`:279-311`) fills the COMPUTE_JOB:
  `job_task_split = 5`, then the DRAW fields
  `state / attributes / varyings / varying_buffers / thread_storage` plus the
  `pan_emit_draw_descs()` set (`offset_start`, `instance_size`,
  `uniform_buffers`, `textures`, `samplers`, `push_uniforms`).
- `panfrost_emit_vertex_tiler_jobs()` (`pan_cmdstream.c:1819-1834`) adds
  VERTEX (index N) then TILER (index N+1), and `panfrost_add_job()` chains
  them in call order. The *first* tiler of a batch additionally reserves the
  WRITE_VALUE index before taking its own (`pan_scoreboard.c:121-131`), so
  the first chain's indices are V=1, WRITE_VALUE=2, T=3; later draws are
  V=N, T=N+1. At submit, `panfrost_scoreboard_initialize_tiler()` prepends
  the WRITE_VALUE job and hands **its** address to the ioctl as `jc`
  (`pan_job.c:1003-1052`, `1018`).
  Chain order is therefore `WRITE_VALUE(jc) → VERTEX(head) → TILER`, with
  the *addresses* not monotonic: the WRITE_VALUE job was allocated last.
- Job header packing: `MALI_JOB_HEADER_pack` → word 4 =
  `is64|type<<1|barrier<<8|invalidate<<9|suppress<<11|texmap<<12|relax<<14..15
  |index<<16`; word 5 = `dep1|dep2<<16`; words 6/7 = next.

## 2. Decoded VERTEX job (MALI_COMPUTE_JOB, 192 B)

Offsets are byte offsets from the job base (= JS_HEAD). Word indices are JD
words. `Draw` word *n* is JD word `16+n` — verified against
`MALI_DRAW_pack` (`cl[0..31]`), not inferred.

| Off | JD w | Field | Value / source | Notes |
|---|---|---|---|---|
| 0x00 | 0 | exception_status | 0 (HW writes on fault) | in DRAM stays 0 |
| 0x04 | 1 | first_incomplete_task | 0 | |
| 0x08 | 2-3 | fault_pointer | 0 | HW-latched |
| 0x10 | 4 | is64=1, **type=5 VERTEX**, index=N | `0x000N000B` for N=1 | legal |
| 0x14 | 5 | dep1=0, dep2=0 | 0 | vertex has no deps |
| 0x18 | 6-7 | next | TILER job VA (pool + 0x00) | chain link |
| 0x20 | 8 | Invocation.invocations | packed WGS/WG triplet | see below |
| 0x24 | 9 | shifts | `workgroups_z_shift=32`, `unknown_shift=2` | graphics quirk |
| 0x28 | 10 | Parameters.job_task_split (bits 26:29) | **5** = vertex | `MALI_COMPUTE_JOB_PARAMETERS_pack` |
| 0x2C-0x3C | 11-15 | Parameters padding | 0 | section is 24 B |
| 0x40 | 16 | Draw.flags | bit1 `draw_desc_64b`, bit2 `tex_desc_64b` (=0x6) | Midgard sets both |
| 0x44 | 17 | Draw.offset_start | `ctx->offset_start` (=start) | |
| 0x48 | 18 | Draw.primitive_index_base | 0 | |
| 0x4C | 19 | — | 0 | |
| 0x50 | 20-21 | Draw.position | **0 in the vertex job** | only the tiler job sets it |
| 0x58 | 22-23 | Draw.uniform_buffers | pool VA or 0 | VS UBO array |
| 0x60 | 24-25 | Draw.textures | 0 (no VS textures here) | |
| 0x68 | 26-27 | Draw.samplers | 0 | |
| 0x70 | 28-29 | Draw.push_uniforms | pool VA or 0 | |
| **0x78** | **30-31** | **Draw.state** | **upload-BO VA + 64-aligned off** | **→ §3** |
| 0x80 | 32-33 | Draw.attribute_buffers | pool VA (S) | ATTRIBUTE_BUFFER[attr_count+1] |
| 0x88 | 34-35 | Draw.attributes | pool VA (T) | ATTRIBUTE[attr_count] |
| 0x90 | 36-37 | Draw.varying_buffers | pool VA | ATTRIBUTE_BUFFER[xfb+n+1] |
| 0x98 | 38-39 | Draw.varyings | pool VA (`vs_vary`) | ATTRIBUTE[vs_count] (+FS appended) |
| 0xA0 | 40-41 | Draw.viewport | **0 in the vertex job** | tiler-only |
| 0xA8 | 42-43 | Draw.occlusion | 0 | |
| 0xB0 | 44-45 | Draw.thread_storage | **SFBD VA** (`reserve_framebuffer`) | SFBD+0 = Local Storage (TLS) |
| 0xB8 | 46-47 | — | 0 | `cl[30]/cl[31]` are unwritten |

Invocation (words 8/9) for the microdraw case (3 vertices, 1 instance,
workgroup size 1,1,1, `quirk_graphics` true): values
`{POSITIVE(1)x4, POSITIVE(3), POSITIVE(1)} = {0,0,0,0,2,0}`, so
`invocations = 2`, `size_y/z_shift = 0`, `workgroups_x/y_shift = 0`,
`workgroups_z_shift = 32`, `unknown_shift = 2` → **w8 = 0x00000002,
w9 = 0x28000000** `[INFERENCE: derived from pan_invocation.c +
panfrost_pack_work_groups_compute(1,3,1,1,1,1,true)]`. Small counts change
w8 only.

Consequence of the decode: the only fields that can make the *shader core*
raise `INSTR_*` are the ISA and the descriptors it consumes. Everything else
(header, type, split, invocation) is legal and matches what kbase-based
Midgard drivers submit.

## 3. Pointer inventory, and the offending word

| Pointer | JD offset | Points at | Lives in | In the submit BO list? |
|---|---|---|---|---|
| `Draw.state` | 0x78/0x7C | 64-B Renderer State | `ss->upload.rsrc` (u_upload BO) | **yes** — `panfrost_emit_compute_shader_meta()` calls `panfrost_batch_add_bo()` for `ss->upload.rsrc->bo` |
| `Draw.attribute_buffers` | 0x80 | ATTRIBUTE_BUFFER array | batch pool BO | yes (`batch->pool` handles) |
| `Draw.attributes` | 0x88 | ATTRIBUTE array | batch pool BO | yes |
| `Draw.varying_buffers` | 0x90 | ATTRIBUTE_BUFFER array (incl. the always-present `position` buffer slot) | batch pool BO | yes |
| `Draw.varyings` | 0x98 | ATTRIBUTE array (VS varyings; FS's follow) | batch pool BO | yes |
| `Draw.uniform_buffers` / `push_uniforms` | 0x58 / 0x70 | UBO / push-constant descriptors | pool / uniform BO | yes |
| `Draw.thread_storage` | 0xB0 | SFBD → Local Storage (TLS) at +0 | pool | yes |
| `header.next` | 0x18 | TILER job | pool | yes |
| **index buffer** | — | **not in the vertex job** | TILER job Primitive word 4/5 (TILER+0x38) | tiler job's submit |
| **tiler heap / polygon list** | — | **not in the vertex job** | SFBD Tiler section (SFBD+192: `polygon_list`, `polygon_list_body`, `heap_start/end`) + WRITE_VALUE payload word 0/1 | SFBD is in the pool; heap is `dev->tiler_heap` (appended to every submit) |
| `Shader.Shader` (in the meta) | meta +0x00/0x04 | ISA VA with `first_tag` in bits 3:0 | `ss->bo` (`PAN_BO_EXECUTE`) | **yes** — same emitter adds `ss->bo` |

Static answer to "is the shader-meta region reachable at the encoded IOVA?":
**yes.** `panfrost_emit_compute_shader_meta()` (`pan_cmdstream.c:543-560`)
does `panfrost_batch_add_bo(batch, ss->bo, …)` and
`panfrost_batch_add_bo(batch, pan_resource(ss->upload.rsrc)->bo, …)` *before*
returning the VA, and `panfrost_batch_submit_ioctl()` (`pan_job.c:924-997`)
emits every `batch->bos` handle plus the pool/invisible-pool handles. Our
`p7580_drv_submit()` maps each handle with `p7580_mmu_map(…, RD|WR,
P7580_MEMATTR_IDX_DEFAULT)` before queueing. So the encoded IOVA resolves,
is 4 KiB-granular, executable (NX clear) and read/write.

**Offending word (named):** `DRAW.State` — JD word 30/31 at +0x78 — and,
one indirection down, `Renderer State.Shader.Shader` (word 0/1 of the
64-B meta). Rationale is exact: `0x51/0x52` are instruction-decode
exceptions; the ISA address is the *only* quantity in the vertex job that
feeds instruction decode; the `first_tag` in the low nibble of
`Shader.Shader` is what the core uses to schedule the first bundle, so a
wrong tag produces `INSTR_TYPE_MISMATCH`/`INVALID_ENC` with **no** memory
fault and **no** illegal job word — exactly the observed signature (data
reads such as the clear colour still work, so DRAM/PTE/IOVA are fine for
data).

## 4. What is eliminated (with the evidence)

| Candidate | Verdict | Ground |
|---|---|---|
| Illegal job header / type / index | **No** | The header words decode `type=5, is64=1`; `sw[4]` "VERTEX" quote was a misread, §5. |
| Bifrost-class encoding on T720 | **No** | Bifrost would emit a 256-B `BIFROST_TILER_JOB` (Draw@128) and take `is_bifrost` branches (`panfrost_draw_emit_tiler`, `texture_descriptor_is_64b=0`). Observed chain pitch is 0xC0 (192-B tiler + 192-B vertex), the SFBD is a 320-B `MALI_SINGLE_TARGET_FRAMEBUFFER` (MFBD would carry the `MALI_FBD_TAG_IS_MFBD` bit), and the Compute-Job Draw is at +0x40 not +0x80. Nothing in the body sets a Bifrost-only field. **Nor can a job word be *re-interpreted* as a newer-arch encoding**: `0x51/0x52` are raised by the vertex/tiler *shader core*, after the JM has accepted and executed the descriptor; the only arch-selecting bits in the job are `Draw.flags[2:1]` (descriptor width), which mean the same thing on v4/v5 Midgard and cannot switch the core's instruction decoder. The newer-arch interpretation can only apply to the ISA itself, which is why the fix targets `Shader.Shader`. |
| Wrong `job_task_split` | **No** | 5 = vertex, same value the tiler(6)/kbase split uses; word 10 bits 26:29. |
| Wrong GPU id ⇒ wrong compiler/quirks | **No** | `GET_PARAM(GPU_PROD_ID)` returns 0x720 (`P7580_GPU_PROD_ID`, ioctl 0x04 gate accepts only that param; enum zero on both sides). Mesa 20.3 maps 0x720 → arch 4, `MIDGARD_QUIRKS|SFBD|NO_HIER_TILING|NO_AFBC` and midgard quirks `INTERPIPE_REG_ALIASING|OLD_BLEND|BROKEN_LOD|NO_UPPER_ALU|NO_OOO`; `MIDGARD_NO_UPPER_ALU` is consumed in the scheduler (`midgard_schedule.c:1329`), i.e. the workaround that prevents exactly `INSTR_INVALID_ENC` is active. |
| Memattr/cache policy | **No** | `P7580_MEMATTR_IDX_DEFAULT = 0`; kbase's own table is index 0 = `AS_MEMATTR_LPAE_IMPL_DEF_CACHE_POLICY 0x48` = `AS_MEMATTR_INDEX_DEFAULT` (verified against kbase `mali_midg_regmap.h:524-555` / `mali_kbase_mmu_mode_lpae.c:73-90`). That is the shipping policy on this SoC. |
| BO flags / CREATE_BO | **No** | Driver reports DRM version 1.0, so Mesa 20.3.5's `panfrost_bo_alloc` skips the `NOEXEC`/`HEAP` flags entirely (`pan_bo.c:65-71`); `CREATE_BO.flags == 0` is enforced and satisfied. |
| IOVA alignment / `first_tag` collision | **No** | `p7580_iova_alloc` uses `drm_mm` in pages ⇒ 4 KiB-aligned; `size` is `round_up(PAGE_SIZE)` at create. Low nibble is free for the tag. |
| Submit BO-list truncation | **No** | `P7580_MAX_SUBMIT_BOS = 64`, observed `nbos≈12`; over-limit is a hard `-EINVAL`, not a silent drop. |
| Not in the submit set | **Open, decidable** | Structurally yes (see §3), but nothing in the log has ever *proved* it for the vertex job — VMETA/VISA do. |

## 5. Corrections to `gpu31-microdraw.md`

1. `head = jc + 0xC0` → **`head = pool + 0xC0`**, with `head = jc − 0x4C0`
   for the observed numbers (`head=0x88d60c0`, `jc=0x88d6580`). Arithmetic:
   `0x88d6580 − 0x88d60c0 = 0x4C0`. `jc` is the WRITE_VALUE job, allocated
   after the varying descriptors/FBD; the vertex job is the second pool
   allocation of the batch (after the 192-B tiler job).
2. `sw[4] = 0x00020005` → `is64=1`, **type=2 (WRITE_VALUE)**, `index=2`,
   deps 0/0 — i.e. the chain head, not the vertex job. (The
   `00010013` fragment sample in the same report *is* type 9, because for a
   fragment submit `jc` **is** the fragment job.)
3. Consequence: the JDBODY captured at `jc` (BO+0x580) never covered the
   faulting descriptor (BO+0xC0). Any conclusion drawn from those bytes about
   the VERTEX job is unfounded. This ticket's driver patch removes the gap.
4. §4's "compare `state`/TLS/`attributes` across JDBODYs" was therefore
   impossible with the old instrumentation; the new `VDRAW2/VMETA/VISA`
   lines make it a one-shot read.

## 6. The fix

### 6.1 Driver (committed): `code/gpu31/driver/0003-panfrost7580-fault-head-vertexjob-decode.patch`

Applies on top of the applied 0001 (+0002). Verified with
`git apply --check` and `patch -p1 --dry-run` on a freshly applied 0001 tree;
brace/paren balance 0; `patch -p1` and `git apply -p1` both accept it.

- `panfrost7580_job.h`: `struct p7580_submit_job` gains
  `u64 fault_head` + `u32 fault_status` (stamped at every fault verdict).
- `panfrost7580_job.c`: stamps `fault_head = JS_HEAD` / `fault_status` in the
  IRQ fault leg, the "ERR alongside DONE" leg, and the TIMEOUT path. This is
  the piece that makes every later dump target the *faulting* descriptor.
- `panfrost7580_submit.c`:
  - `p7580_drv_ptewalk_pin()` now walks/dumps **`target = fault_head ?:
    jc`** (new `TARGET` banner prints jc, head and the walked address; the
    JDBODY/PTEWALK address token changed `jc=` → `at=` so the log never
    again implies the chain head was dumped). Owner lookup and page math
    follow `target`.
  - new `p7580_vj_read()` (snapshot-first, live-`bo_list`-fallback reader,
    ≤2 kmap pairs per call) and `p7580_drv_vertexjob_pin()`:
    - `HEADTYPE head=… own=SNAP|LIVE|MISS type=… VERTEX is64=… idx=… dep=…,…
      next=… js_status=0x51`
    - `VDRAW head=… split=5 inst=… off=… flags=… inv=…/… pos=… ubo=… tex=…
      smp=… push=…`
    - `VDRAW2 head=… state=0x… attrbuf=… attr=… varbuf=… vary=… vp=… tls=…`
    - `VMETA head=… state=0x… own=… bo=0x…/0x… shader=0x… tag=N samp=…
      tex=… attr=… vary=… props=…`
    - `VISA head=… shader=0x… isa=0x… own=… bo=0x…/0x… got=N w=…`
  Runs in the same process-context window as the existing walk (timedout
  workqueue, PM held), no MMIO, no sleeps beyond kmap.

This is diagnostics with teeth: `own=MISS` on `state` or `shader` is a
provable mapping/submit-set bug; `own=SNAP` + mathematically wrong VMETA
counts/state is a Mesa packing bug; `own=SNAP` + legal meta + VISA words that
a host `midgard_disasm` rejects is an ISA-generation/tag bug.

### 6.2 Mesa 20.3.5 (committed): `code/gpu31/mesa/patches/0007-mesa203-vertex-attribute-array-bounds.patch`

The only *shader-shape-dependent* defect in the vertex-job emission path, and
it lives in exactly the shape that produced `0x51` (`gl_VertexID` only, no
attributes) while the attribute shapes produced `0x52`:

`panfrost_emit_vertex_data()` sizes the ATTRIBUTE array as
`MALI_ATTRIBUTE_LENGTH * attribute_count`, but the special-input block below
runs whenever `attribute_count >= PAN_VERTEX_ID` (16) and writes **both**
`out[PAN_VERTEX_ID]` (16) and `out[PAN_INSTANCE_ID]` (17). For a
`gl_VertexID`-only VS `attribute_count = PAN_VERTEX_ID + 1 = 17`
(`pan_assemble.c:293-298`), so `out[17]` writes 8 bytes past the allocation
into the next pool object (the viewport descriptor at the next 32-B aligned
offset); for exactly 16 attributes both specials are out of bounds. The
remaining `out[0..15]` and `bufs[2..]` are **uninitialised pool memory**,
while the shader meta declares `attribute_count = 17` to the hardware.

Patch: reserve `attribute_count + 2` records for both arrays and zero both
before filling. Applies cleanly to upstream `mesa-20.3.5`
`src/gallium/drivers/panfrost/pan_cmdstream.c` (verified `patch -p1
--dry-run` + `git apply -p1 --check`); rebase if the device tree carries
Mesa 20.3 patches in this hunk.

Honest scope: this patch removes a real pool overflow and non-deterministic
descriptors; it is **not** claimed to be the proven cause of every `0x52`
(the attribute shapes never touch the special block). The decision comes from
§7's capture.

## 7. Exact dmesg capture procedure (rootshell, no `dmesg -c` as user)

`dmesg -c` as the `user` account is permission-denied on this image, which is
how the earlier ticket got a stale buffer. Always clear through rootshell.

1. Build/flash is Main's call; for the diagnostic build only 0003 is needed
   on top of int34 (0001+0002 unchanged otherwise). After boot:
   `uname -r` must show the rebuilt revision (`3.10.108 #<new>`), otherwise
   stop — you are reading the old kernel's lines.
2. Clear **as root**, then verify the clear took effect:
   ```
   /usr/local/bin/rootshell -c 'dmesg -c > /dev/null; echo cleared'
   /usr/local/bin/rootshell -c 'dmesg | wc -l'
   ```
   The second command must print `0`.
3. Arm the trace budget and run the failing client (surfaceless):
   ```
   /usr/local/bin/rootshell -c 'echo 8 > /sys/kernel/debug/p7580/trace_arm'
   /data/wdarm 14 /data/eglrun.sh       # known-good watchdog wrapper (jddecode)
   # alternatives: /tmp/microdraw (attr-less ES3), /data/egltest --offscreen-only
   sync; sleep 1
   ```
4. Capture **to a file, without `-c`** (so nothing races the read):
   ```
   /usr/local/bin/rootshell -c 'dmesg > /tmp/vj.log; wc -l /tmp/vj.log'
   ```
5. Pull the evidence:
   ```
   grep -E 'p7580_job: (ERR|TARGET|HEADTYPE|VDRAW2?|VMETA|VISA|JDBODY|PTEWALK)' /tmp/vj.log
   ```
6. Also grab the register/status context for the same seq and the recovery
   outcome:
   ```
   grep -E 'p7580_job: (err|irq|TIMEOUT|dump)' /tmp/vj.log | tail -40
   /usr/local/bin/rootshell -c 'cat /sys/kernel/debug/p7580/dump'
   ```
7. Do **not** re-run before copying the log: the JDBODY window is 512 B and
   the pool BO is recycled per batch, so a second run overwrites the bytes
   the first run captured.

Expected shape of a useful capture (one fault):

```
p7580_job: ERR slot=1 js_status=0x00000051 (INSTR_INVALID_ENC) … head=0x88d60c0 … jc=0x88d6580 …
p7580_job: TARGET slot=1 seq=0 jc=0x88d6580 head=0x88d60c0 walk=0x88d60c0 js_status=0x51
p7580_job: HEADTYPE head=0x88d60c0 own=SNAP bo=0x8xxxxxxx/0x10000 type=5 VERTEX is64=1 idx=1 dep=0,0 next=0x88d6000 js_status=0x51
p7580_job: VDRAW  head=0x88d60c0 split=5 inst=0 off=0 flags=00000006 inv=00000002/28000000 pos=0 ubo=0 tex=0 smp=0 push=0
p7580_job: VDRAW2 head=0x88d60c0 state=0x… attrbuf=0x… attr=0x… varbuf=0x… vary=0x… vp=0 tls=0x…
p7580_job: VMETA  head=0x88d60c0 state=0x… own=SNAP bo=0x…/0x… shader=0x… tag=N samp=… tex=… attr=17 vary=… props=…
p7580_job: VISA   head=0x88d60c0 shader=0x… isa=0x… own=SNAP bo=0x…/0x… got=8 w=…
```
`next=0x88d6000` is the tiler job of the same draw (pool + 0x00) - if `next`
is not `pool+0x00`-like, the chain itself was rewritten.

## 8. Silicon verification plan (what each line decides)

| Observation | Verdict | Action |
|---|---|---|
| `TARGET` shows `head != jc` and `walk=head` | Confirms §5; the old dump was blind | proceed |
| `HEADTYPE own=MISS` or `got<8` | The faulting descriptor is not in the job's BO set | submit-set bug: dump `ERR-MAP nbos`, compare against `bo_handles` |
| `VDRAW2 state=0` | No shader meta pointer in the submit | Mesa/chain-emission bug (would also explain 0x51) |
| `VMETA own=MISS` | The Renderer State IOVA is not mapped/submitted | mapping/submit-set bug (fix in `panfrost_emit_compute_shader_meta` caller / driver list) |
| `VMETA own=SNAP` but `attr/vary/samp/tex` do not match the program | Meta packing/upload race | re-check `pan_upload_shader_descriptor` + upload BO lifetime |
| `VMETA own=SNAP`, counts sane, **`VISA own=MISS`** | ISA pointer is wrong/unmapped | `Shader.Shader` encoding or `ss->bo` lifetime; ISA never fetched from a valid BO |
| `VISA own=SNAP`, `w=` bytes differ between two runs of the same shader, or `tag` ≠ tag of first bundle (host `midgard_disasm`) | ISA bytes/tag wrong | compiler-target or coherency; compare the captured 8 words against a host-side `midgard_disasm` of the same shader |
| `VISA own=SNAP`, words match, still `0x51` | Hardware rejects a legal stream | re-open quirk coverage for 0x720 (`MIDGARD_NO_UPPER_ALU`/`OLD_BLEND`/`NO_OOO` usage), then escalate with the disassembly |

Success criterion for the whole line of work is unchanged: `egltest`
RENDER_OK with a red centre pixel and **zero** `ERR` lines after a clean
`rootshell` clear. The `0x51/0x52` must disappear before the polygon-list
census is meaningful (`gpu31-microdraw.md` §3).

## 9. Handoff to integrator

Take (on top of int34):

- `code/gpu31/driver/0003-panfrost7580-fault-head-vertexjob-decode.patch`
  (applies after 0001+0002 to `drivers/gpu/drm/panfrost/`; verified with
  `git apply --check` and `patch -p1 --dry-run`)
- `code/gpu31/mesa/patches/0007-mesa203-vertex-attribute-array-bounds.patch`
  (**device Mesa 20.3.5 tree**, not the Mesa-27 kbase tree; rebase only this
  hunk if `/data/mesabuild/mesa20` carries local patches in
  `panfrost_emit_vertex_data`)
- this report.

Do NOT take the `/tmp/vj/**` scratch trees or the fetched Mesa 20.3.5 copy.
The JDBODY log that motivated this ticket remains on the phone and is now
superseded by a capture at `head`.
