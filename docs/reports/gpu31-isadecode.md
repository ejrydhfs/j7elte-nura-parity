# gpu31-isadecode — VS/FS ISA from the live submit, Midgard arch-4 legality

Date: 2026-11-21 (device clock 2026-11-22 GMT). Owner: G31_IsaDecode.
Device: int37 (`3.10.108 #37 SMP PREEMPT Sat Nov 21 22:41:00 EST 2026`),
Mesa 20.3.5 diagnostic build `/data/mesabuild/mesa20` → `/data/mesabuild/mesa20-b`.
No flash from this ticket; every device run under `flock /tmp/gpsdev.lock`,
short holds; `PAN_MESA_DEBUG=sync/trace` never used.

Question: are the vertex and fragment Midgard ISAs that the GPU actually
fetches legal for T720 (Midgard arch 4), does the shader meta's low-nibble tag
match the first bundle, and is anything in the stream only valid on a newer
Midgard?

Answer, short: **the compiler output is legal arch-4 ISA** for both stages,
with correct first tags. **The live fragment-shader BO is not**: its first
64-byte line is zero at submit while the meta still carries tag 5, and the
vertex BO has its second 64-byte line zeroed. That is Defect B (stale dirty
zero-line writeback in `p7580_shmem_alloc`), not an ISA-generation or
meta-packing bug. No Mesa compiler change is warranted; the already-committed
kernel side of Defect B must be flashed.

## 0. TL;DR

1. **Captured from the BOs, at a live submit.** The fragment meta
   `0x2d30005` points at BO `0x2d30000`; the vertex meta `0x2d31005` points at
   BO `0x2d31000`. Both are 4 KiB SHMEM shader BOs, both 96-byte ISAs
   (`compile stage=4/0 size=96 first_tag=5`).
2. **The compiled ISAs are legal T720 streams.** Tags are
   `[5 ldst, 3 tex, 9 alu/8, 9 alu/8]` (FS) and `[5 ldst, 9 alu/8, 9 alu/8,
   5 ldst]` (VS); every `next_tag` is the next bundle's tag and both streams
   terminate in `TAG_BREAK (1)`. No `TAG_ALU_*_WRITEOUT` (`0xC..0xF`) appears,
   so the T720 `MIDGARD_NO_UPPER_ALU` rule is respected; the texture word is
   `.last` with no `.ooo`, so `MIDGARD_NO_OOO` is respected. No opcode from the
   T760+ blend/color-buffer family appears.
3. **Meta/tag and counts match the ISA.** FS meta word 0 is
   `0x02d30005` → first tag `5`, and the first ISA word is `0xc87a9835`;
   VS meta is `0x02d31005` and the first ISA word `0xc87a9495`. FS meta
   declares sampler 1 / texture 1 / attribute 0 / varying 1, exactly the
   disassembler's stats; VS declares attribute 2 / varying 2, exactly its
   stats. Meta words 2–3 are the only count carriers (`MALI_SHADER` packs
   sampler|texture and attribute|varying).
4. **The live FS BO is clobbered in its first cache line.** Read through the
   kernel `0xCF` path (`clean`-then-invalidate + `kmap`, i.e. DRAM truth) at
   the submit, its words 0–15 are `00000000`, while words 16–23 are exactly
   the compile-time writeout block. The meta's `shader=0x2d30005 tag=5` no
   longer matches the live first bundle (`word0 & 0xf == 0 == TAG_INVALID`).
   The live VS is the mirror image: words 0–15 are intact, words 16–23 are
   zero (the last two VS bundles lost).
5. **This is Defect B, already specified and already in the driver patch but
   not in the int37 image.** Fix: `__flush_dcache_area(page_address(page),
   PAGE_SIZE)` after each `shmem_read_mapping_page()` in `p7580_shmem_alloc`
   (`docs/reports/gpu31-defectb.md`, `code/gpu31/gem/panfrost7580_gem.c`,
   driver `0001`). The ISA capture below is the acceptance evidence that the
   remaining fault is page content, not ISA legality.
6. **No newer-arch-only instruction was found.** The T720 writeout path is the
   correct low-tag one (`brx.write.always` inside `alu/8`), not the T760+
   `TAG_ALU_*_WRITEOUT`. A decoder-only caveat is recorded in §6 (T720
   interpipe register bases are printed swapped by Mesa 20.3.5); it does not
   affect encodings and was not changed on the device Mesa path.

## 1. Capture

Two independent, env-gated instruments were used; both read the *same* BO the
meta names.

### 1.1 Mesa side (diag patch `0004-mesa203-isadump.patch`)

`panfrost_emit_frag_shader()` and `panfrost_emit_compute_shader_meta()` now
write, with `PAN_FBD_ISADUMP=1`:

* `/tmp/isadump-fs-meta.bin` / `/tmp/isadump-vs-meta.bin` — the packed
  `MALI_RENDERER_STATE` the submit consumes (64 B);
* `/tmp/isadump-fs-isa.bin` / `/tmp/isadump-vs-isa.bin` — the first 24 words
  (96 B) of the shader BO CPU mapping at emit;
* a text line with `shader` (pointer|tag), `first_tag`, BO address and words.

`panfrost_shader_compile()` additionally writes the pristine compiler buffer
(`/tmp/isadump-s<stage>-compile.bin`) so a live/compile diff isolates a BO
content loss from a compilation.

### 1.2 Submit side (`code/gpu31/polylist/polycensus.c`, `dump_state()`)

At the fragment submit the hook parses the tiler/vertex descriptors, takes the
`Renderer State` pointer, reads the state and the ISA through the kernel's
`0xCF` `DEBUG_READ_BO` (clean-before-invalidate + `kmap`; DRAM), and now
prints **all 24 ISA words** (was 12) and writes
`/tmp/pc2-FS-isa.bin`, `/tmp/pc2-VS-isa.bin` plus the 64-byte metas. The
kernel read is the authority for "what the GPU fetches"; the user mapping can
be Normal-NC and is not.

Run (device, diag Mesa):

```
B=/data/mesabuild/mesa20-b
export LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri
export LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri
export MESA_LOADER_DRIVER_OVERRIDE=panfrost EGL_PLATFORM=surfaceless
export LD_PRELOAD=/data/polylist/isadump/pc2.so PC2_MAX=1
export PAN_FBD_ISADUMP=1 PAN_FBD_DEBUG=1
flock /tmp/gpsdev.lock -c 'timeout -s KILL 30 /data/egltest --offscreen-only --frames 1'
```

Captured addresses (one run; addresses move per run, shape identical):

| Stage | Meta (`shader=`) | ISA BO | live BO bytes |
|---|---|---|---|
| FS | `0x2d30005` (tag 5) | `0x2d30000` | first 64 B zero, last 32 B intact |
| VS | `0x2d31005` (tag 5) | `0x2d31000` | first 64 B intact, last 32 B zero |

## 2. Raw words

Pristine compiler ISAs (96 B each; these are the bytes the compiler produced):

```
FS stage=4 first_tag=5 w0=c87a9835
  c87a9835 01403d3d 00000030 00000000
  210a1193 00e43c40 00000000 00000000
  08200019 02300018 ff2e4800 0000004f
  0000ffff 00000000 00000000 00000000
  08020019 027b0018 ffae4100 fffffc4f
  0000ffff 00000000 00000000 00000000

VS stage=0 first_tag=5 w0=c87a9495
  c87a9495 08003c3d dc860940 000003c3
  00800099 04300358 00009001 00000000
  00000000 00000000 00000000 00000000
  00a20059 6ed862e0 0214ecf8 3f2a4052
  40520210 84303f2a 0000d000 00000000
  a9e0d415 08003d3c dc9e1d40 000003d3
```

Live BOs through `0xCF` at the submit:

```
FS live  (meta 0x2d30005, tag 5)
  00000000 00000000 00000000 00000000
  00000000 00000000 00000000 00000000
  00000000 00000000 00000000 00000000
  00000000 00000000 00000000 00000000
  08020019 027b0018 ffae4100 fffffc4f   <- equals compile 0x40 block
  0000ffff 00000000 00000000 00000000

VS live  (meta 0x2d31005, tag 5)
  c87a9495 08003c3d dc860940 000003c3   <- equals compile 0x00 block
  00800099 04300358 00009001 00000000
  00000000 00000000 00000000 00000000
  00a20059 6ed862e0 0214ecf8 3f2a4052
  00000000 00000000 00000000 00000000   <- compile had 2 more bundles
  00000000 00000000 00000000 00000000
```

The loss is exactly one 64-byte line per BO, at *different* lines: this is the
`flush_dcache_page()`-is-not-a-flush defect of `gpu31-defectb.md` (the
allocation's dirty zero line is written back asynchronously through the
kernel's WB alias over the WC user stores). The FS BO is the one that matters
for the first tag: the meta's tag 5 addresses a bundle whose live tag is 0.

Meta words (little-endian; word 0 is `shader|first_tag`, word 2 is
`sampler_count|texture_count`, word 3 is `attribute_count|varying_count`):

```
FS: 02d30005 00000000 00010001 00010000 ...
         shader=0x2d30005 tag=5     samp=1 tex=1  attr=0 vary=1
VS: 02d31005 00000000 00000000 00020002 ...
         shader=0x2d31005 tag=5     samp=0 tex=0  attr=2 vary=2
```

## 3. Disassembly (Mesa 20.3.5 `disassemble.c`/`midgard_ops.c`, gpu_id 0x720)

Built from the device tree's own sources (`disassemble.c`,
`midgard_ops.c`, `midgard_print_constant.c`, `util/half_float.c`,
`util/softfloat.c`) and run on the captured 96-byte files.

FS entry (offset 0x00):

```
ld_vary_32 r26.xy, 0, 0x9E, 0x1E /* A0 */

texture.2d.last hr26, texture0, fsampler0, r0.xyxx,

vadd.fmov r0, r24.xxxx, hr0.xyzw
brx.write.always +0 -> alu/8
break

FS writeout block (offset 0x40; reached by the `brx.write`, not by fallthrough):

vmul.imov r0, sext(r24.xxxx), r0
brx.write.always -2 -> alu/8
```

VS entry (offset 0x00):

```
ld_attr_32 r26.xy, 1, 0x1E, 0x1E /* 0 */
ld_attr_32 r0.xy, 0, 0x1E, 0x1E /* 0 */

smul.fmov r0.z, r24.x, #0
uconstants 0x0, 0x0, 0x0, 0x0

vmul.fmul r24.xyz, r0.xyzz, r23.xyzz
vadd.fadd r27.xyz, r24.xyzz, r22.xyzz
smul.fmov r27.w, r24.x, #1

st_vary_32 r26, 1.xyyy, 0x9E, 0x1E /* 0 */
st_vary_32 r27, 0, 0x9E, 0x1E /* 0 */
```

Stats from the disassembler: FS `instr=6 bundles=4 qwords=6 attrs=0 vary=1
tex=1 samp=1 helper=1`; VS `instr=8 bundles=4 qwords=6 attrs=2 vary=2
tex=0 samp=0`. These match the meta counts in §2.

## 4. Arch-4 legality, bundle by bundle

| Bundle | Word | Tag | Size | Next tag | Verdict |
|---|---|---|---|---|---|
| FS 0 | `c87a9835` | 5 `ldst` | 1 qw | 3 | legal; `ld_vary_32` |
| FS 1 | `210a1193` | 3 `tex` | 1 qw | 9 | legal; `.last`, **no `.ooo`** |
| FS 2 | `08200019` | 9 `alu/8` | 2 qw | 1 `break` | legal; lower-tag writeout branch |
| FS 3 | `08020019` | 9 `alu/8` | 2 qw | 1 `break` | legal; writeout loop target |
| VS 0 | `c87a9495` | 5 `ldst` | 1 qw | 9 | legal; two `ld_attr_32` |
| VS 1 | `00800099` | 9 `alu/8` | 2 qw | 5 | legal |
| VS 2 | `00a20059` | 9 `alu/8` | 2 qw | 5 | legal |
| VS 3 | `a9e0d415` | 5 `ldst` | 1 qw | 1 `break` | legal; `st_vary_32` pair |

Checks against the T720 (arch 4) quirks in
`src/panfrost/midgard/midgard_quirks.h`:

* `MIDGARD_NO_UPPER_ALU`: no bundle tag is `0xC..0xF`; the writeout path is
  the arch-4 form (`brx.write.always` inside `alu/8`) in both shaders.
* `MIDGARD_NO_OOO`: the single texture word has no out-of-order field
  (`texture.2d.last`, no `.oooN`).
* `MIDGARD_OLD_BLEND`/`MIDGARD_BROKEN_LOD`: not exercised (no blend/color
  buffer load, no gradient), so no T760-only opcode can be hiding there.
* `MIDGARD_INTERPIPE_REG_ALIASING`: the shaders use the aliased classes
  (`ld_attr_32`/`st_vary_32` on r26, texture input on texture0 and output
  r0), which is exactly the T720 layout.
* Tag chain: every `next_tag` equals the following bundle's tag; both
  streams end in `TAG_BREAK`. No `TAG_INVALID` in the compiled streams.
* Meta: low nibble == `words[0] & 0xf` for both stages (5 == 5); the meta's
  `shader` base is exactly the BO the ISA was read from; counts match §3.

**Nothing in either compiled ISA is only valid on a newer arch.** The live
FS stream, however, fails the first check (`words[0] & 0xf == 0`,
`TAG_INVALID`) because of the zeroed line — a content defect, not an encoding
defect.

## 5. Fix

1. **No Mesa compiler or compat-patch change.** The T720 compiler path is
   correct: arch-4 tags, lower-tag writeout, no OoO, correct meta. Changing the
   Mesa path to "avoid" anything here would be treating the clobbered page as
   an ISA symptom.
2. **Kernel: apply the Defect B fix at the next flash.** In
   `p7580_shmem_alloc()`, after each `shmem_read_mapping_page()`, add
   `__flush_dcache_area(page_address(page), PAGE_SIZE);` (clean the
   allocation zeroes to DRAM and invalidate the WB linear-alias line before
   the page can be mapped). This is already committed in
   `code/gpu31/gem/panfrost7580_gem.c` and mirrored in driver `0001`; int37
   does not carry it, which is why the live BO shows the 64-byte zero line
   above. `docs/reports/gpu31-defectb.md` §6 has the silicon acceptance:
   `PAN_FBD_DEBUG` + the `0xCF` read must show `w=c87a9835 01403d3d …` for
   the FS BO at every submit, and `--defectb-test` must pass 32/32.
3. **Decoder-only caveat (not applied to the device Mesa path).** For
   `MIDGARD_INTERPIPE_REG_ALIASING`, `midgard_ra.c` puts tex inputs
   (`REG_CLASS_TEXR`) at the LDST base r26 and tex outputs (`TEXW`) at the
   work base r0, but `disassemble.c` calls
   `print_texture_word(..., interpipe ? 0 : REG_TEX_BASE, interpipe ?
   REGISTER_LDST_BASE : REG_TEX_BASE)`, i.e. the two bases are swapped for
   T720. It affects only the printed register numbers (the disassembly above
   is annotated accordingly); it cannot change an encoding and is not a
   legality finding. If a future ticket touches the disassembler, swap the
   two arguments.

## 6. Evidence files and reproduce

On the device / captured to the host:

* `/tmp/isadump-s4-compile.bin`, `/tmp/isadump-s0-compile.bin` — pristine
  96-byte compiler buffers;
* `/tmp/isadump-fs-isa.bin`, `/tmp/isadump-vs-isa.bin` — live BO first 24
  words at emit;
* `/tmp/pc2-FS-isa.bin`, `/tmp/pc2-VS-isa.bin` — same through the submit-side
  `0xCF` kernel read;
* `/tmp/isadump-fs-meta.bin`, `/tmp/isadump-vs-meta.bin` (and
  `/tmp/pc2-*-meta.bin`) — packed renderer state;
* repo artifacts: `code/gpu31/mesa/diag/0004-mesa203-isadump.patch`,
  `code/gpu31/polylist/polycensus.c` (24-word ISA + raw-blob dump).

Reproduce the decode on the host with a decoder built from the device tree:

```
gcc -std=gnu99 -O1 -DHAVE_PTHREAD -DHAVE_ENDIAN_H \
  -Wno-error=implicit-function-declaration -o midgard_disasm main.c \
  src/panfrost/midgard/{disassemble,midgard_ops,midgard_print_constant}.c \
  src/util/{half_float,softfloat}.c -Iinclude -Isrc -Isrc/panfrost \
  -Isrc/panfrost/include -Isrc/mesa -Isrc/compiler -Isrc/gallium/include \
  -Isrc/gallium/auxiliary -Isrc/mapi -Isrc/util
./midgard_disasm 0x720 4 fs-isa.bin    # fragment
./midgard_disasm 0x720 0 vs-isa.bin    # vertex
```

Host build note: the T720 path needs `-DHAVE_PTHREAD -DHAVE_ENDIAN_H` (and
`-std=gnu99`), exactly as used; `midgard_print_constant.c` pulls in
`util/half_float.c`/`util/softfloat.c`.
