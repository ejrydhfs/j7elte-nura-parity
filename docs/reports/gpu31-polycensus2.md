# gpu31-polycensus2 — polygon list after a completed frame on int36

Date: 2026-11-21. Owner: G31_PolyCensus2. Device: int36 kernel
`3.10.108 #37 SMP PREEMPT` (VM_MIXEDMAP page-referencing mappings), device
Mesa 20.3.5 at `/data/mesabuild/mesa20-b` (diagnostic build). No flash; all
device work under `flock /tmp/gpsdev.lock`, short holds.

Question: after a completed frame, what does the tiler actually leave in the
polygon list that the fragment job's SFBD names, and if no triangle is binned,
what exactly is missing?

Answer, short: the list is **fully initialised, not stale** — every one of the
256 tiles has a header entry pointing at its own 512-byte body slot, and every
body slot contains exactly the empty-list terminator `0xa0000000`. The tiler
ran (proven by fault injection on its position pointer) and read the position
buffer pointer, but binned **zero triangles** out of the three valid
`gl_Position` vertices the vertex shader wrote. The fragment job therefore
invokes no fragment shader and the render target keeps the clear colour. The
remaining defect is the **vertex→tiler consumption of `gl_Position`**, not the
polygon-list allocation, header encoding, body addressing, primitive word,
viewport, varying-buffer descriptor, or tiler heap. A second, independent
defect was seen in these runs: the fragment shader ISA page still reads
all-zero with `count=4` page references, producing the usual `0x52`; with the
diagnostic `PAN_FBD_REUPLOAD` restoring the ISA the fault is gone (`ERR=0`)
and the pixels are still clear, which proves that the ISA-zeroing defect is
*not* the cause of the missing triangle.

## 0. TL;DR

1. **The polygon list is not empty/uninitialised.** After the fragment
   fence, the 64-byte prologue is live (`ffffffff … heap_start …`), and the
   header holds 256 entries that decode as `(body_addr >> 2)` for tiles
   0..255 (entries 0..127 as 8-byte words = 256 u32 values, step `0x80`).
   Entry `i << 2 = body + i*0x200`, exactly one body record per tile.
   Earlier reports' `inrange=0` was a decode artifact: raw entries are
   compressed pointers, not BO addresses.
2. **Every tile is empty.** All 256 body records (512 B each) contain exactly
   one nonzero word, `0xa0000000` at offset 0; the other 127 words are zero
   (256 nonzero words over 256 records; a 512-record scan confirms the same
   shape and no triangle payload anywhere). The 1 MiB tiler heap is
   **0/262144** nonzero words — no spill, nothing to spill.
3. **The vertex shader executed.** The tiler's `Draw.Position` buffer reads
   three valid pixel-space vertices (`w=1`):
   (25.6016, 230.3984), (230.3984, 230.3984), (128.0, 25.6016), z=0.5.
   Primitive word `0x18030008` = TRIANGLES, non-indexed, 3 vertices
   (Index count field 2, minus-1), JTS 6, indices pointer 0, Low/High Depth
   Cull bits 16/17 set (default, no ZS attached). Viewport clip
   is (-inf,-inf,0)..(+inf,+inf,1) with scissor 0,0..255,255.
4. **The tiler executed and dereferenced its inputs.** Replacing the tiler's
   `Draw.Position` pointer with an unmapped address (`0xdeadb000`) makes the
   *chain* submit fault on slot 1 (`ERR-MAP slot=1`), so the tiler job runs
   and reads the field it is given.
5. **Yet it binned nothing.** With byte-correct descriptors, valid positions
   and a working viewport, the tiler's own structures show zero triangles.
   The only remaining variable is what the tiler read *at tiling time*: the
   positions are valid in DRAM after the frame fence, so the tiler does not
   consume the vertex-shader output (intra-chain visibility/ordering of the
   vertex→tiler handoff). `[INFERENCE]` on the mechanism; the zero-coverage
   output itself is measured.
6. **Fragment leg is downstream of this.** One `0x52 INSTR_TYPE_MISMATCH`
   on the fragment job names the FS ISA BO (`0x21d6005`); the FS page reads
   `00000000…` via `0xCF` with `count=4 mapcount=1 vma=1 pin=1 up lru swapb`
   (int36 did not eliminate the shaderbo zeroing in this run). With
   `PAN_FBD_REUPLOAD=1` the ISA is present at emit, `ERR=0`, and the readback
   is unchanged (`corner=26,26,102`, `center=26,26,102`) — so the clear centre
   is caused by the empty tiler output, not by the ISA fault.
7. No red pixels. Red pixels require fixing (5) (and separately (6) unless
   the page-zeroing is fixed).

## 1. Method

Device-only `LD_PRELOAD` hook, committed as
`code/gpu31/polylist/polycensus.c` (built on the phone with the device
`gcc -shared -fPIC`). It interposes `ioctl`, tracks `CREATE_BO`
(handle → GPU IOVA/size), and on every `SUBMIT` with an `out_sync` (the
fragment submit of each batch) does the following **inside Mesa's process**:

1. waits on the submit's own `out_sync` syncobj (built-in wait, not Mesa's),
2. reads the fragment job header/payload and the SFBD `Tiler` section through
   the **DEBUG-ONLY kernel route** `DRM_IOCTL` nr `0xCF`
   (`p7580_gem_debug_read_word`: clean-then-invalidate + kmap, so the values
   are DRAM truth, not a userspace alias),
3. censuses the polygon-list prologue, 1024 8-byte header slots and the body,
   cross-checking one body tile through `0xCF` against an independent
   `MMAP_BO`+`mmap` read (matches),
4. decodes the preceding chain submit (write-value → vertex → tiler) for the
   Primitive, `Draw` pointers, position buffer and VS/FS state,
5. scans the tiler heap.

Run shape: `PC2_MAX=1` (one completed frame), non-sync Mesa, no
`PAN_MESA_DEBUG` — `sh /data/polylist/run.sh` with
`LD_PRELOAD=/data/polylist/polycensus.so`.

**Do not run the diag Mesa with `PAN_MESA_DEBUG=sync` or `trace`.** The
diagnostic dump block in `pan_job.c` waits on the chain submit's
`out_sync=0` with `INT64_MAX` and hangs the slot; two such runs had to be
rebooted. The census does not need either variable.

## 2. Fragment SFBD → polygon list (0xCF, after the fence)

Representative completed frame (addresses move per run; shape is identical
across runs):

```
PC2 frag jc=0x21c5600 type=9 idx=1
PC2 fbd=0x21c54c0
PC2 sfbd-tiler at=0x2a2a580: 00080200 00000000 02a5a000 00000000
                            02a5c200 00000000 02000000 00000000
                            02100000 00000000
PC2 sfbd pls=0x80200 hier=0x0 pl=0x2a5a000 body=0x2a5c200
         heap=0x2000000..0x2100000
PC2 frag bounds min=0x00000000 max=0x000f000f tiles x=0..15 y=0..15
```

| Field | Value | Meaning |
|---|---|---|
| `polygon_list_size` | `0x80200` | 512 KiB + prologue |
| `hierarchy_mask` | `0` | flat T720 selector (16×16) |
| `polygon_list` | `0x2a5a000` (h133, 1 MiB BO) | header base |
| `polygon_list_body` | `0x2a5c200` | `pl + 0x2200` |
| `heap_start..end` | `0x2000000..0x2100000` (h1) | 1 MiB tiler-heap BO |
| fragment bounds | tiles (0,0)-(15,15) | full 256×256 |

The fragment job and the SFBD are the same objects the earlier
`gpu31-fbdtarget.md` decoded; the change here is that every value is now
read through `0xCF` **after the fragment fence**, and the polygon list is
censed in full.

## 3. Header census and the entry encoding

```
PC2 pl handle=133 gpu=0x2a5a000 off=0x0 size=0x80200 body_off=0x2200
PC2 pl-prologue at=0x2a5a000: ffffffff 00000000 00000000 00000000
                              00000000 00000000 02000000 00000000
                              ffffffff ffffffff 00000000 00000000
                              00000000 00000000 00000000 00000000
PC2 nz entry[0]=0x00a9710000a97080
PC2 census nent=1024 nz=128 inrange=0 first=0 last=127
```

Interpretation:

* The census reads the same 1024 8-byte slots as the earlier diagnostic, and
  finds 128 nonzero ones (indices 0..127). Those are **256 32-bit values**
  starting `0x00a97080, 0x00a97100, 0x00a97180, 0x00a97200, …` step `0x80`
  (the first 8-byte slot shown above is `0x00a9710000a97080`).
* The values are **compressed pointers `address >> 2`**, not addresses:
  `0x00a97080 << 2 = 0x2a5c200` = exactly `polygon_list_body`, and
  `0x80 << 2 = 0x200` = exactly the body stride per tile. So there are
  **256 header entries (4 B each, 16×16 tiles)**, and entry *i* points at
  `body + i*0x200`.
* Consequently the older `inrange=0` assertion (comparing the raw 64-bit
  entry against `[pl, pl+size)`) is a **decode artifact**: with `entry << 2`
  every one of the 256 pointers is in range, inside the body.
* The prologue is live tiler output, not leftover BO-cache content: the
  write-value job zeroes the list base, and word 0 is `ffffffff` after the
  tiler ran, with `heap_start` in prologue word 6 and `ffffffff` at words
  8/9. (The diag read performed right after the *chain* submit, before a
  fence, can still show the previous frame's entries — that is why this
  census is taken after the fragment fence.)

Size note: the T720 flat sizing in Mesa 20.3 uses an 8-pixel base
(`panfrost_flat_size`: `0x200 + 8*ceil(W/8)*ceil(H/8)`) and 8 B/tile, i.e.
space for 1024 tiles, body at `0x2200`. The hardware here enumerates 256
16×16 tiles with 4-byte entries. The allocation is oversized but the SFBD
body offset is explicit, so this is harmless; it does mean a census that
assumes 8 B/tile and 1024 tiles mis-reads the header.

## 4. Body and tiler heap

```
PC2 mapscan body_off=0x2200 tiles=512 covered=256 nonzero_words=256
PC2 bscan tile=0 nz=1: a0000000 00000000 ... (127 zeros)
PC2 bscan tile=1 nz=1: a0000000 00000000 ...
...
PC2 bscan-0xcf at=0x2a5c200: a0000000 00000000 ...   (matches-map=1)
PC2 heap-map range=0x2000000..0x2100000 nonzero=0/262144
```

* Scanned 512 of the body records (the first 256 KiB; `body_off=0x2200`,
  stride `0x200`): **256 records have content, each with exactly one nonzero
  word**, the `0xa0000000` empty-list terminator; everything else is zero.
  (The remaining allocation is zero; the scan cap does not hide payload —
  no record in the run shows a second nonzero word, and the 0xCF cross-check
  of the first record matches the mapping byte-for-byte.)
* The tiler heap is entirely zero (`0/262144` words): the tiler needed no
  spill, as expected for an empty bin set.
* No triangle record exists anywhere in the body. The list is a complete,
  well-formed set of **empty** per-tile lists.

## 5. Geometry leg: the vertex shader did produce the triangle

```
PC2 chain k=0 job=...480 type=2 idx=2 next=...0c0   (write-value, Zero)
PC2 chain k=1 job=...0c0 type=5 idx=1 next=...000   (vertex, dep ...)
PC2 chain k=2 job=...000 type=7 idx=3 next=0x0      (tiler, dep 1,2)
PC2 vertex draw: pos=0x0 ubos=0x...3a0 state=0x...000
PC2 vertex draw: attr_bufs=0x...320 attrs=0x...360
                 vbuf=0x...2e0 varyings=0x...2c0 viewport=0x0
PC2 tiler draw: pos=0x2a3a040 ubos=0x2a2a420 tex=0x2a2a428
                samp=0x2a2a440 push=0x0 state=0x2a2a3c0
PC2 tiler draw: attr_bufs=0x0 attrs=0x0 vbuf=0x2a2a2e0
                varyings=0x2a2a2d0 viewport=0x2a2a400
PC2 tiler primitive-size @+184: 3f800000 00000000
PC2 viewport at=0x2a2a400: ff800000 ff800000 7f800000 7f800000
                          00000000 3f800000 00000000 00ff00ff
PC2 viewport floats min=(-inf,-inf,0) max=(inf,inf,1)
                   scissor=(0,0)-(255,255)
PC2 varying-buffer at=0x2a2a2e0: ...0001 0 00000004 0000000c
                                 ...0041 0 00000010 00000030
PC2 primitive=0x18030008 draw_mode=8 index_type=0
               index_enable=0 restart=0 jts=6 index_count=2   (minus-1 ⇒ 3)
PC2 tiler position=0x2a3a040 state=0x2a2a3c0 flags=0x27
PC2 positions at=0x2a3a040: 41ccd000 43666600 3f000000 3f800000
                           43666600 43666600 3f000000 3f800000
                           43000000 41ccd000 3f000000 3f800000
                           00000000 ... (rest of the 64 B slot is zero)
```

Decoded:

* Positions: (25.6016, 230.3984, 0.5, 1.0), (230.3984, 230.3984, 0.5, 1.0),
  (128.0, 25.6016, 0.5, 1.0) — the same triangle the clear-frame test expects,
  written by the vertex shader into the varying buffer.
* Primitive (`genxml Primitive`, word 10): `0x18030008` = Draw mode 8
  (TRIANGLES), Index type 0 (non-indexed), Primitive Index Enable 0,
  Primitive restart none, Job Task Split 6; Base vertex 0, restart index 0,
  Index count (`minus(1)`) = 2 → **3 vertices**, Indices pointer 0.
* `Draw.Position` = the position buffer; the position varying-buffer
  descriptor (entry 1) says `ptr = that buffer, stride = 0x10, size = 0x30`
  (3 × 16 B) — exact match.
* Viewport: no clipping (min/max ±inf, z 0..1), scissor (0,0)-(255,255),
  i.e. the triangle at x 25..230 / y 25..230 is fully inside.
* `Draw.FBD` (Draw word 28, aliasing Thread Storage) is the SFBD that
  carries the tiler section above.

Nothing in the geometry descriptors is wrong, and the vertex shader output
is present in the buffer.

## 6. Proof that the tiler job runs and reads its position pointer

Diagnostic (env-gated, one run): the hook rewrote the chain tiler's
`Draw.Position` to `0xdeadb000` (unmapped) in the pool BO before the submit
ioctl. Result:

```
p7580_job: ERR-MAP slot=1 seq=0 nbos=12 ...
p7580_job: ERR slot=0 js_status=0x41 POWER_FAULT ... head=...600 ... (collateral)
```

The **chain** submit (slot 1) faulted at the address, so the tiler job
executes and reads the `Draw.Position` value it is given. Combined with the
live prologue/body writes, the tiler's geometry stage is running, not being
skipped by a slot/dependency problem.

A control that only swapped the tiler's `Draw.Varyings` from `fs_vary` to
`vs_vary` (the position attribute descriptor is `MALI_SNAP_4`, buffer 1 in
`vs_vary`; `fs_vary` holds the general varying at buffer 0) changed nothing:
`ERR=0` (with FS re-upload) and the pixels stayed clear. So the varying
*list* pointer is not the blocker either.

## 7. Fragment leg and the independent FS-ISA defect

Baseline clean run (no re-upload):

```
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH) seq=121
           jc=0x...600 ... sw=… 00010013 …
PC2 fs meta shader=0x21d6005 tag=5
PC2 isa at=0x21d6000: 00000000 ... (24/24 words zero)
p7580_pin: 0xcf iova=0x21d6000 idx=0 count=4 mapcount=1 vma=1 pin=1
           up lru swapb clean mapping=0000000000000000 pgidx=0
           w=00000000 00000000 00000000 00000000
```

The VS ISA page at the same instant is intact
(`c87a9495 08003c3d dc860940 …`). So the fragment ISA page is (still)
wholesale zero while its BO holds `count=4` references and the mapping is
pinned — the `gpu31-shaderbo`/`gpu31-mmaprefs` symptom, intermittent on this
boot (a plain 5-frame run in `SESSION-STATE.md` saw `ERR=0`; the census runs
reproduced it). The fault pointer is the FS ISA address the fragment
`Renderer State` names.

A/B with the diagnostic `PAN_FBD_REUPLOAD=1` (ISA re-copied at every meta
emit, with `PAN_FBD_DEBUG=1`; the re-upload code is gated by both):

```
dmesg: p7580_job: ERR count = 0
egltest: corner(2,2) RGBA=26,26,102,255   (clear exact)
egltest: center(128,128) RGBA=26,26,102,255
```

So even with a valid fragment shader and no job fault, the triangle does not
appear. The ISA-zeroing defect is real and must still be fixed, but it is
**not** the cause of the missing red centre.

## 8. Conclusion — two defects, and the exact remaining cause

There are **two distinct defects** in this run:

**Defect A (the missing triangle): the tiler bins nothing.** Its output is a
complete but uniformly empty polygon list, so the fragment job has no work.
This is upstream of the fragment job, in the vertex→tiler leg:

1. The tiler produces a complete, correctly sized, correctly addressed
   polygon list: 256 header entries (`body_addr >> 2`), 256 body records,
   live prologue, `heap_start` in place, and an empty tiler heap.
2. Every body record is the empty-list terminator `0xa0000000`. There is no
   triangle record anywhere, so the fragment job has nothing to invoke the
   fragment shader for; the render target keeps the SFBD clear colour
   (`0xff661a1a` = 26,26,102,255) at both the corner and the centre.
3. All static tiler inputs are byte-correct (primitive/vertex count,
   non-indexed indices pointer, position pointer, position varying format
   `SNAP_4`/buffer 1/stride 16/size 48, viewport/scissor, FBD tiler section),
   and the tiler provably executes and reads `Draw.Position` (fault
   injection).
4. The three `gl_Position` vertices are valid in the position buffer after
   the frame fence.

Therefore the only surviving mechanism is that the tiler did not observe the
vertex shader's output when it ran: the chained write-value → vertex → tiler
runs in one submit, and the position data is only observed valid after the
frame. `[INFERENCE]` The remaining cause is the **intra-chain
vertex→tiler visibility/ordering of the gl_Position varying buffer**
(the tiler reads the buffer the VS wrote, in the same submit, with no
inter-job flush). The static format/count/pointer hypotheses are excluded by
the census above.

Next measurement to close it (kernel/Mesa, not a userspace decode): split or
order the vertex and tiler stages so the tiler runs after the vertex output
has been made visible (e.g. a cache-flush/ordering point between the two
jobs, or two submits with a fence between on a device where a standalone
tiler submit is accepted), then re-run this census: the body must show a
triangle record in the tiles x1..14/y1..14 and the centre must turn red
(with the FS-ISA issue fixed or `REUPLOAD` active). The two rejected attempts
in this ticket (standalone post-fence tiler submit; redirecting the tiler to
a CPU-written triangle in the polygon-list BO) either timed out or faulted
and are not evidence; they are listed here so nobody repeats them blindly.

**Defect B (independent): the fragment-shader ISA page is still zeroed.**
`p7580_pin` reports `count=4 mapcount=1 vma=1 pin=1 up lru swapb clean` for
the FS ISA BO page, yet the page reads all-zero through the kernel `0xCF`
route, and the fragment job faults `0x52` with that address as the pointer.
`PAN_FBD_REUPLOAD=1` restores the ISA at emit, removes the fault (`ERR=0`)
and leaves the pixels unchanged, so B is not the cause of the clear centre,
but it will kill fragments even after the tiler starts binning. It must be
fixed independently (`gpu31-shaderbo.md` / `gpu31-mmaprefs.md`); int36's
page references did not eliminate it in this reproduction (while a plain
5-frame run recorded in `SESSION-STATE.md` did not hit it — the event is
intermittent).

## 9. Reproduce

```
# build the hook on the device (musl gcc)
scp code/gpu31/polylist/polycensus.c user@172.16.42.1:/data/polylist/
ssh user@172.16.42.1 'cd /data/polylist && gcc -shared -fPIC -O2 \
    -o polycensus.so polycensus.c -ldl'

# one completed frame, 0xCF census after the fence
rootshell env PC2_MAX=1 FRAMES=1 sh /data/polylist/run.sh
```

`/data/polylist/run.sh` sets the Mesa 20.3.5 `mesa20-b` library paths,
`EGL_PLATFORM=surfaceless`, `MESA_LOADER_DRIVER_OVERRIDE=panfrost`,
`LD_PRELOAD=/data/polylist/polycensus.so`, clears dmesg, runs
`/data/egltest --offscreen-only`, prints `RC`, the `p7580_job: ERR` count
and the readback.

Warnings:
* Never use `PAN_MESA_DEBUG=sync` or `=trace` with the diagnostic Mesa: the
  diag dump block waits on the chain submit's `out_sync=0` and hangs the
  slot (needed a reboot twice in this ticket).
* The hook also contains env-gated fault-injection experiments
  (`PC2_BADPOS`, `PC2_SWAPVARY`, `PC2_POINTS`, `PC2_ZEROPRIM`,
  `PC2_CPUPOS*`, `PC2_TILERAFTER*`). They were used to exclude hypotheses;
  two of them (`PC2_CPUPOS2`, `PC2_TILERAFTER`) fault/hang the slot and are
  **not** part of the normal path. Run them only on a disposable boot.
* Keep device work under `flock /tmp/gpsdev.lock` and never flash/reboot from
  a worker slice.

## 10. Artefacts

* `code/gpu31/polylist/polycensus.c` — the 0xCF census/decode hook
  (diagnostic; all probes default-off except the census; experiments are
  env-gated).
* Device logs from the runs quoted above lived in `/tmp/pc5.log` (stderr),
  `/tmp/pc5.out` (readback) and the kernel ring; they are repro-only and not
  committed.
* The `0xCF` route, `p7580_pin` ledger and the diag Mesa
  (`PAN_FBD_DEBUG`/`PAN_FBD_REUPLOAD`) are pre-existing diagnostic artefacts
  documented in `gpu31-mmaprefs.md` / `gpu31-shaderbo.md`; they remain
  debug-only and must be stripped before cutover.
