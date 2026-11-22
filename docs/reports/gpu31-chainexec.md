# gpu31-chainexec — chain decode, chain bit, and the vertex→tiler handoff

Date: 2026-11-21. Owner: G31_ChainExec. Device: int36 kernel
`3.10.108 #37 SMP PREEMPT Sat Nov 21 20:45:13 EST 2026`, Mesa 20.3.5 at
`/data/mesabuild/mesa20-b` (diagnostic `-b` build). No flash. All work is
userspace; the `/data/wdarm` watchdog reset the phone in the rejected
split/duplication and bins-present runs described in §6, every other run
completed normally. All device work under `flock /tmp/gpsdev.lock`, short
holds.
`PAN_MESA_DEBUG=sync/trace` was never used.

Question: does our single-submit chain actually execute all jobs in order with
the right JS_CONFIG bits, and if not, is that why the tiler bins no triangles?

Answer, short: **the chain is encoded correctly and all three jobs run**, and
**the chain bit is not the problem**. A new pre-submit prefill probe proves the
remaining cause directly: the tiler consumes the *pre-vertex-shader* content of
its `Draw.Position` buffer. In the same run, at the moment the tiler has
finished, that buffer already holds the vertex shader's three pixel-space
vertices. The tiler therefore reads the VS output buffer without the VS's
stores being visible to it (intra-chain cache/visibility), not because of a
header, dependency, slot, flush-bit, or primitive error. Setting
`JS_CONFIG.JOB_CHAIN_FLAG` (bit 11) for every descriptor-marked submit changes
nothing (measured with the same probe). Userspace chain splitting/duplication —
the only way to put a submit boundary (and thus `START_FLUSH_CLEAN_INV`)
between the VS and the tiler — faults JS1 with `JS_STATUS 0x41 POWER_FAULT` on
int36 (and the same one-shot duplicate issued from Mesa’s own submit path also
fails to complete), so the split/flush variants must be implemented in the
driver and tested on the next image. The independent FS-ISA page defect (Defect B) still blocks
pixels as soon as the tiler does bin.

## 0. TL;DR

1. **Mesa’s chain is byte-correct** (decoded through `0xCF`, full raw words):
   `WRITE_VALUE(idx 2) → VERTEX(idx 1) → TILER(idx 3)`, `next` NULL-terminated,
   tiler `dep1=1` (vertex), `dep2=2` (write value). Both deps appear earlier in
   the linked list, satisfying `pan_scoreboard.c`’s own rule; execution order on
   Midgard is defined by the dependency indices, not by `next`.
2. **What we program matches**: HEAD_NEXT = the WRITE_VALUE job, slot JS1,
   `JS_CONFIG_NEXT = 0x00083700` = AS0 | START_FLUSH_CLEAN_INV (0x300) |
   START_MMU (0x400) | END_FLUSH_CLEAN_INV (0x3000) | PRI(8) (0x80000).
   Bit 11 (JOB_CHAIN_FLAG) is clear, as in the T720 table of
   `gpu31-jobabi.md`; bits 14/15 clear.
3. **Variant (a), chain bit 11, changes nothing.** With `chain_mode=1`, the
   chain dispatch itself carries `cfg=0x00083f00` (bit 11 set) — measured from
   the last-dispatch dump after a fragment-refused run — and the result is
   identical: 0 non-empty tiles, clear pixels. Bit 11 is not an
   ordering/visibility switch on this part.
4. **The vertex shader does write the buffer the tiler reads.** With
   `PC2_ZEROPOS`, the hook zeroes the tiler’s `Draw.Position` buffer before the
   chain submit; after the frame the same address holds the three vertices
   (`41ccd000 43666600 …`). This rules out “the VS never ran / writes
   elsewhere”.
5. **Decisive probe (`PC2_PREFILL`)**: write a tiny CPU triangle
   `(0,0),(8,0),(0,8)` into that same buffer *before* the submit. The VS
   overwrites it (point 4). The tiler then bins **exactly one tile**, whose
   record is the CPU triangle (`98197841 3c00bf7f a0000000`), i.e. it consumed
   the buffer's **pre-VS** contents. Without the prefill the same probe sees
   **0** non-empty tiles even though the buffer holds the VS triangle by the
   time the tiler’s prologue is published. This is the precise remaining cause:
   **the intra-chain vertex→tiler handoff is not visible to the tiler**.
6. **Variants (b)/(c) cannot be exercised from userspace on int36.** JS_CONFIG
   flushes are per submit; a flush/clean “between the VS and the tiler” requires
   a submit boundary, i.e. splitting the chain. Two userspace implementations
   (header-rewriting split, and verbatim re-submit of the same chain) both fault
   JS1 immediately with `JS_STATUS 0x41 POWER_FAULT` (`out_sync=0` and
   `out_sync!=0`, with and without `in_sync`), while Mesa’s own first submission
   of the same bytes is DONE. A one-shot duplicate issued from Mesa’s own
   submit path (diagnostic `PAN_CHAIN_DOUBLE`) also never completes (watchdog
   reset), so this is not an artefact of the preload hook. The split knob must
   live in `p7580_drv_submit` and be tested on the next image; the required
   design is in §7.
7. **Pixels** stay clear. Once the tiler does bin (prefill experiments), the
   fragment job invokes the FS and the independent FS-ISA zero-page defect
   (Defect B) faults `0x52`; with 60 batches each fault stalls the batch ~5 s and
   the watchdog reboots the phone. So red pixels now require Defect B to be
   fixed, not a chain change. The no-bin runs (which never invoke the FS) are
   stable and are what this report measures.

## 1. Method and instruments

Device-only `LD_PRELOAD` hook, committed as
`code/gpu31/polylist/polycensus.c` (extended this ticket; built on the phone
with `gcc -shared -fPIC -O2 -ldl`). It keeps the existing `0xCF`
`DEBUG_READ_BO` census path (clean-before-invalidate + `kmap`, DRAM truth) and
adds, all env-gated and off by default:

| Env | What it does |
|---|---|
| `PC2_RAW` | dump every job-header/draw word of the chain (pre-existing) |
| `PC2_ZEROPOS` | zero the tiler’s `Draw.Position` 64 B before the submit; the post-fence census shows whether the VS rewrote it |
| `PC2_PREFILL` | write a CPU triangle into the tiler’s `Draw.Position` before the submit (tiny `(0,0),(8,0),(0,8)`; `PC2_PREFILL_BIG` selects `(20,140),(236,140),(128,20)`) |
| `PC2_POLLSCAN` | after the chain submit returns, wait (≤500 ms) for the tiler prologue `0xffffffff`, then read the tiler-time position buffer and census the polygon list; first chain only |
| `PC2_SCANLOG` | append the poll-scan lines to a file (fsync per line), so results survive a fragment-leg watchdog reboot |
| `PC2_REFUSE_FRAG` | return `-EIO` for the fragment submit; lets a prefill run measure the tiler without invoking the FS (Defect B) |
| `PC2_COPY0`, `PC2_DOUBLE`, `PC2_DOUBLE_SKIP` | userspace chain duplication controls (§6) |
| `PC2_SPLIT` | userspace header-rewriting split at the vertex→tiler boundary (§6) |
| pre-existing | `PC2_BADPOS` (fault the chain’s position pointer), `PC2_SWAPVARY`, `PC2_POINTS`, `PC2_ZEROPRIM`, `PC2_CPUPOS*`, `PC2_TILERAFTER*` |

Mesa sources read on the device (not copied into the repo): `pan_scoreboard.c`
(job emission and the dependency rules), `pan_job.c`
(`panfrost_batch_submit_jobs`: chain first with `reqs=0`, then the fragment job
with `PANFROST_JD_REQ_FS`), `midgard.xml` (`Job Header`: `Is 64b` bit 0,
`Type` bits 1..7, `Index` bits 16..31 of word 4; `Dependency 1/2` in word 5),
`midgard_pack.h`. The driver-side contract is `gpu31-jobabi.md` §6.

Run shape: `FRAMES=1`, non-sync Mesa, `PC2_MAX=1`, no `PAN_MESA_DEBUG`.
Baseline (`sh /data/polylist/run.sh`) finishes in ~3 s when Defect B does not
fire, or ~8 s when it does (one fragment fault + the 5 s job timeout); pixels
are always the clear colour. `ERR` is 0 or 1 (the known last-fragment `0x52`).

## 2. The chain as Mesa emits it (decoded, `PC2_RAW`)

Representative completed frame (addresses move per run, shape is identical):

```
PC2 chain k=0 job=0x3584480 type=2 idx=2 next=0x35840c0
PC2 job at=0x3584480: 00000001 00000000 00000000 00000000 00020005 00000000 035840c0 00000000
PC2 job+32 at=0x35844a0: 035b4000 00000000 00000003 00000000 ...
PC2 chain k=1 job=0x35840c0 type=5 idx=1 next=0x3584000
PC2 job at=0x35840c0: 00000001 00000000 00000000 00000000 0001000b 00000000 03584000 00000000
PC2 job+64 at=0x3584100: 00000006 00000000 ... state=0x2d32000
PC2 chain k=2 job=0x3584000 type=7 idx=3 next=0x0
PC2 job at=0x3584000: 00000001 00000000 00000000 00000000 0003000f 00020001 00000000 00000000
PC2 job+32 at=0x3584020: 00000002 28000000 18030008 00000000 00000000 00000002 ...
PC2 tiler draw: pos=0x3594040 ... state=0x35843c0
PC2 primitive=0x18030008 draw_mode=8 index_type=0 index_enable=0 restart=0 jts=6 index_count=2
```

Header decode with the genxml packing:

| k | job | word 4 | type | index | word 5 (dep1/dep2) | next |
|---|---|---|---|---|---|---|
| 0 | `0x3584480` | `0x00020005` | 2 WRITE_VALUE | 2 | 0 / 0 | `0x35840c0` |
| 1 | `0x35840c0` | `0x0001000b` | 5 VERTEX (COMPUTE_JOB) | 1 | 0 / 0 | `0x3584000` |
| 2 | `0x3584000` | `0x0003000f` | 7 TILER | 3 | 1 (vertex) / 2 (write value) | 0 |

* The WRITE_VALUE payload at `+32` is `address=0x35b4000` (the SFBD
  `polygon_list`), `type=3` (Zero), i.e. the tiler-enable/zero write emitted by
  `panfrost_scoreboard_initialize_tiler()`.
* The tiler’s two dependency indices both reference jobs **earlier in the
  linked list**, which is exactly what `pan_scoreboard.c` requires (“a job’s
  dependencies must appear earlier in the linked list”; “jobs linking via
  next_job has no bearing on order of execution … the execution order is from
  resolving the dependency fields instead”). Mesa reserves index 2 for the
  write value before the first tiler and numbers jobs in emission order, so the
  list order (wv → vertex → tiler) and the index order (1, 2, 3) are different
  by design; that is not an encoding error.
* The primitive word is TRIANGLES, non-indexed, 3 vertices
  (`Index count` field 2), JTS 6, depth-cull defaults, no face culling
  (`Draw` word 0 = `0x27`: `Front face CCW`, cull-front/back both 0).
* The vertex job’s `Draw.Position` is 0 (unused for the VS); the tiler’s is
  `0x3594040`, which is the same buffer named by the position varying
  descriptor (`SNAP_4`, buffer 1, stride 16, size 48).

## 3. What the kernel programs, and variant (a): bit 11

`p7580_job_hw_submit()` writes HEAD_NEXT = `jc` (the WRITE_VALUE job), routes
`reqs=0` to JS1, and writes `JS_CONFIG_NEXT = p7580_js_config(0) = 0x00083700`.
That is AS 0, START_FLUSH_CLEAN_INVALIDATE (bits 9:8 = 3), START_MMU (bit 10,
t72x issue 10649), END_FLUSH_CLEAN_INVALIDATE (bits 13:12 = 3), THREAD_PRI(8)
(bits 23:16). Bit 11 `JOB_CHAIN_FLAG`, bit 14 `ENABLE_FLUSH_REDUCTION` and bit
15 `DISABLE_DESC_WR_BK` are 0 — identical to the T720 column of
`gpu31-jobabi.md` §6 and to `0x83700` in every earlier dispatch trace.

The existing `chain_mode` debugfs knob (from `gpu31-lastjob`) was used for
variant (a): mode 1 sets bit 11 whenever the submit-time descriptor snapshot’s
word 4 has bit 0 set (bit 0 is `Is 64b`; every Mesa job sets it, so this is
“all Mesa submits”). To read the *chain* dispatch’s cfg without a fragment
fault, `PC2_REFUSE_FRAG` refuses the fragment submit, so the last dispatch in
`/sys/kernel/debug/p7580/dump` is the chain (JS1, WRITE_VALUE head
`w4=0x00020005`):

```
# chain_mode = 1
last slot=1 seq=60 jc=0x268a480 cfg=0x00083f00 as=0 aff=0x3
sw 00000000 00000000 00000000 00000000 00020005 00000000 0268a0c0 00000000
# chain_mode = 0
last slot=1 seq=62 jc=0x3204480 cfg=0x00083700 as=0 aff=0x3
sw 00000000 00000000 00000000 00000000 00020005 00000000 032040c0 00000000
```

`0x83f00 = 0x83700 | (1<<11)`, i.e. the chain submit really does carry
JOB_CHAIN_FLAG in mode 1 (not just the fragment dispatch). Functional result
with the new probe (§5): **identical to mode 0** — prefill bins the CPU
triangle, control bins nothing, pixels clear. So JOB_CHAIN_FLAG is not the
missing ordering/visibility bit on this part.

(Two side notes for the next investigator: `trace_arm` on int36 consumes its
budget without emitting the dispatch lines — `trace_left` goes from 7 to 0 and
only ERR lines appear in the ring; and the `chain_mode=1` full-run ERR line for
the fragment is the only easy place the cfg is visible. The chain cfg above was
obtained by fault injection, not from the dispatch trace.)

## 4. The vertex shader does produce and store the positions (`PC2_ZEROPOS`)

Baseline frame, first chain, with the hook zeroing the tiler’s 64-byte
`Draw.Position` buffer immediately before the submit ioctl (driver submit-sync
covers the CPU write; `PC2_BADPOS` already proved this mapping route reaches the
GPU):

```
PC2 zeropos: zeroed 64B at 0x57a2040        <- this chain's tiler position
PC2 positions at=0x57a2040: 41ccd000 43666600 3f000000 3f800000
                            43666600 43666600 3f000000 3f800000
                            43000000 41ccd000 3f000000 3f800000
```

i.e. `(25.60,230.40,0.5,1)`, `(230.40,230.40,0.5,1)`, `(128.00,25.60,0.5,1)`
after the frame. The buffer was zeroed by *this* submit, so the VS really did
execute and write the buffer the tiler names. This excludes “the VS is skipped”
and “the tiler points at the wrong buffer”.

## 5. The decisive probe: the tiler consumes the pre-VS content

`PC2_PREFILL` writes a tiny triangle `(0,0),(8,0),(0,8)` (all inside tile 0)
into the tiler’s `Draw.Position` immediately before the chain submit; the VS
then overwrites those same 48 bytes (§4). `PC2_POLLSCAN` waits until the tiler
publishes its prologue (`0xffffffff`) and then reads (a) the tiler-time position
buffer and (b) all 256 polygon-list body records. `PC2_REFUSE_FRAG` stops the
fragment submit so Defect B cannot stall or mask the run. `PC2_SCANLOG` writes
each line to `/data/chainexec/*.txt` with fsync.

Control (`PC2_POLLSCAN` only), first chain, immediately after the chain submit:

```
chain pollscan: prologue=ffffffff waited=0ms
chain pollscan: pos@0x5b9c040 = 41ccd000 43666600 3f000000 3f800000
                                43666600 43666600 3f000000 3f800000
chain: pl=0x631c000 prologue=ffffffff 00000000 ...
chain: tiles_nonempty=0 payload_words=0
```

Prefill (`PC2_PREFILL` + the same probes):

```
chain pollscan: prologue=ffffffff waited=0ms
chain pollscan: pos@0x66f6040 = 41ccd000 43666600 3f000000 3f800000
                                43666600 43666600 3f000000 3f800000
chain: pl=0x6e76000 prologue=ffffffff 00000001 00000001 00000000 00000001 ...
chain: sample tile0: 98197841 3c00bf7f a0000000 00000000 ...
chain: tiles_nonempty=1 payload_words=2
```

At the tiler-time sample the buffer **already contains the VS triangle** in both
runs, yet the tiler bins nothing in the control and bins the *CPU prefill* in
the prefill run. The only way both can be true is that the tiler’s reads of
`Draw.Position` do not observe the VS stores: it consumed the buffer’s pre-VS
contents (zeros in the control; the CPU triangle when present). This is a
memory-visibility/ordering failure inside the job chain, not a descriptor
error.

Repeating the pair with `chain_mode=1` (bit 11 set) reproduces the result
exactly (control 0; prefill 1 tile, `981c4ec1 3c00bf7f a0000000`), so the chain
bit neither fixes nor changes the handoff.

### Why a CPU prefill is the right discriminator

* If the tiler read *after* the VS and coherently, it would see the VS triangle
  in both runs and the control would bin the many tiles the VS triangle covers.
  It does not.
* If the tiler read the buffer at submit time (before the VS), it must see the
  prefill when present and zeros/garbage otherwise. That is exactly what the
  two runs show.
* The prefill writes through the same MMAP_BO alias and the driver’s
  submit-time cache clean; the same route’s GPU visibility is already proven by
  `PC2_BADPOS` (chain fault on slot 1 when the pointer is redirected).

Therefore the remaining defect (Defect A) is: **the tiler job’s read of the
vertex shader’s varying output is not ordered/coherent with the vertex job’s
stores within one chained submit**. The chain’s dependency fields are correct
but do not (on this driver/part as programmed) make the VS output visible to
the tiler.

## 6. Variants (b) cache flush before the tiler, (c) split submits

`JS_CONFIG` cache maintenance is per **submit**, not per job: the same
`START/END_FLUSH_CLEAN_INV` pair is applied at the chain head and at the chain
tail. There is no per-job flush bit in the descriptor that we could set instead
(`Invalidate Cache` at word 4 bit 9 exists in genxml but is job-local cache
invalidate; Mesa leaves it 0 and it is not an L2 write-back point). A
“flush/clean immediately before the tiler” therefore requires a submit
boundary between the VS and the tiler — variant (c) — and then variant (b) is
simply the second submit’s `START_FLUSH_CLEAN_INV`.

Two userspace attempts were implemented in the hook (both off by default):

1. `PC2_DOUBLE` / `PC2_COPY0`: re-submit the batch’s **own unmodified chain**
   once more after the first copy’s fence. The intention: the first run’s VS
   has already produced `gl_Position`, so the second run’s tiler has the data
   in DRAM at submit time regardless of intra-chain ordering. `PC2_COPY0`
   submits one extra copy with `out_sync=0` (Mesa’s own chain shape) and then
   lets the original run; `PC2_DOUBLE` submits two copies with `in_sync`
   chaining and waits on each.
2. `PC2_SPLIT`: rewrite the chain in the pool BO as
   part 1 `WRITE_VALUE → VERTEX` (vertex `next=NULL`), fence and wait, then
   part 2 `WRITE_VALUE → TILER` with the tiler’s `dep2` pointing at the write
   value. Part 2 starts with `START_FLUSH_CLEAN_INV` — the explicit
   cache-clean-before-tiler variant. Headers are restored afterwards.

Both were expected to be safe (identical bytes / valid deps). Measured instead:

* Every extra chain submit faults JS1 immediately: `p7580_job: ERR slot=1
  js_status=0x00000041 (POWER_FAULT)`, `head=tail=jc`, with `cfg=0x00083700`.
  It reproduces with `out_sync=0` and with an `out_sync`, with `in_sync`
  copied from the original (0) or chained to the copy’s fence, on the first and
  on the second chain of the run (skip control), with `chain_mode=0` and `1`.
  Mesa’s own submission of the same descriptor bytes in the same process is
  DONE; the only difference is that the extra ioctl is issued from inside the
  census hook.
* After the first such fault the GPU/MJ stays faulted: the next fragment and
  the next chain both report `POWER_FAULT` until the process exits; the app
  then stalls (fragment fences never signal promptly) and the watchdog reboots.
* The split’s part 2 (`WRITE_VALUE → TILER`, tiler `dep2=2`) behaves the same
  way; it never produced a completed tiler fence.

To rule out an artefact of the preload hook, the same one-shot duplicate was
also added to the diagnostic Mesa (`PAN_CHAIN_DOUBLE`, inside
`panfrost_batch_submit_jobs()` right after the first chain ioctl). It behaves
the same: the run that normally finishes in ~3 s was still in flight when the
watchdog reset the phone (~120 s). The dmesg of that run was lost with the
reset, so the explicit `POWER_FAULT` evidence remains the hook-side runs above,
but the failure is not hook-specific. A split necessarily contains such a
re-submit, so it is not expected to work from userspace/Mesa either
`[INFERENCE]`.

Conclusion: userspace/Mesa chain splitting or re-submission is **not viable on
int36**; the extra chain submit fails at the JM level independently of the
bytes. The variants must be implemented in the driver (`p7580_drv_submit`),
where the split can be issued as two real submits with the driver's own fences,
and measured on the next image. This is a kernel item, not a Mesa item: Mesa
already emits a legal chain that the JM executes (both jobs DONE, tiler
provably runs).

## 7. Remaining cause and the next measurement

**Precise remaining cause (measured).** In one chained submit,
`WRITE_VALUE → VERTEX → TILER`, the tiler’s reads of `Draw.Position` see the
buffer as it was at submit time, not the vertex shader’s stores. The VS stores
are in the buffer by the time the tiler’s prologue is published (poll-scan
reads the VS triangle at tiler completion), so the failure is the handoff’s
visibility/ordering, not a missing VS execution, wrong address, wrong format,
wrong primitive, wrong slot, or missing chain bit. With no prefill the tiler
therefore bins the buffer’s pre-submit (zero) content and the polygon list is
256 empty `0xa0000000` lists; with a prefill it bins exactly the prefill.

**Fix direction.** Put a real submit boundary with cache clean+invalidate
between the VS and the tiler, i.e. split the batch’s chain in the driver:

* `chain_mode=3` (proposed): in `p7580_drv_submit`, when the submitted chain is
  the Mesa Midgard chain (`WRITE_VALUE → VERTEX+ → TILER+`), build two submits
  from the same descriptor BOs:
  1. the chain with the last vertex job’s `next` set NULL and its out-fence
     installed (driver-owned fence), dispatched to JS1;
  2. after submit 1’s fence, the original chain from `WRITE_VALUE` (or from the
     tiler with its `dep1` cleared and `dep2` kept on the re-emitted write
     value), dispatched to JS1 with `START_FLUSH_CLEAN_INV`.
  Both submits must carry the same BO list; the fragment submit must take an
  `in_sync` on submit 2 (Mesa currently relies on the implicit order).
* `chain_mode=4` (cheaper if it works): same split, but only clean (bit 12,
  not bit 13) at the end of submit 1 / start of submit 2, to test whether the
  invalidate side is what matters.

**Acceptance for that image** (the same probe, no FS needed):
`PC2_POLLSCAN` control must change from `tiles_nonempty=0` to the tile set of
the VS triangle (tens of tiles, not 0 and not exactly 1); the prefill run must
then bin the VS triangle rather than the tiny prefill. Once Defect B is also
fixed, the fragment should turn the centre red and leave the corner clear; with
bins present today the FS `0x52` fault stalls each batch ~5 s (§0.7).

## 8. What was measured, in one table

| Run (int36) | Probe | tiler-time `pos` buffer | `tiles_nonempty` | Pixels | JS faults |
|---|---|---|---|---|---|
| baseline, `PC2_MAX=1` | census after fragment fence | VS triangle | 0 (256 terminators, heap 0) | clear (26,26,102) | 1× `0x52` (FS ISA, last fragment) |
| `chain_mode=1` | same | VS triangle | 0 | clear | 1× `0x52`, cfg `0x83f00` |
| `PC2_ZEROPOS` | zero pos pre-submit | VS triangle after frame | — | clear | 0 |
| `PC2_POLLSCAN` (control, no fragment) | poll tiler, census | VS triangle | **0** | n/a | 0 |
| `PC2_PREFILL` + `PC2_POLLSCAN` | prefill tiny CPU triangle | VS triangle | **1** (tile 0 = CPU triangle) | n/a | 0 |
| same, `chain_mode=1` | prefill | VS triangle | **1** (CPU triangle) | n/a | 0 |
| `PC2_PREFILL_BIG`, no fragment | prefill big CPU triangle | — | — | n/a | 0 |
| `PC2_PREFILL_BIG` + FS reupload, fragment allowed | bins exist | — | (bins) [INFERENCE] | no readback; watchdog reboot before output [INFERENCE: each fragment FS `0x52` stalls ~5 s] | not captured (reboot) |
| `PC2_COPY0` / `PC2_DOUBLE` / `PC2_SPLIT` | extra submits from the hook | — | — | — | `0x41 POWER_FAULT` on JS1, then all slots fault |
| Mesa `PAN_CHAIN_DOUBLE` (diagnostic build) | one extra chain submit from `panfrost_batch_submit_jobs` | — | — | — | run did not complete; watchdog reset (~120 s) [log lost with the reset] |

Every scan line quoted above is preserved on the device in
`/data/chainexec/scan_ctrl3.txt`, `scan_prefill3.txt`, `scan_ch1_pre.txt`,
`scan_ch1_ctrl.txt` (fsynced before the fragment leg runs).

## 9. Reproduce

```
# build the extended hook on the device (musl gcc)
scp code/gpu31/polylist/polycensus.c user@172.16.42.1:/data/polylist/
ssh user@172.16.42.1 'cd /data/polylist && gcc -shared -fPIC -O2 \
    -o polycensus.so polycensus.c -ldl'

# control (tiler-time read + census, no fragment)
rootshell /data/wdarm 15 env PC2_POLLSCAN=1 PC2_REFUSE_FRAG=1 \
    PC2_SCANLOG=/data/chainexec/scan_ctrl.txt PC2_MAX=1 FRAMES=1 \
    sh /data/polylist/run.sh

# decisive prefill
rootshell /data/wdarm 15 env PC2_PREFILL=1 PC2_POLLSCAN=1 \
    PC2_REFUSE_FRAG=1 PC2_SCANLOG=/data/chainexec/scan_prefill.txt \
    PC2_MAX=1 FRAMES=1 sh /data/polylist/run.sh

# variant (a): read the chain dispatch's cfg from the last-dispatch dump.
# Refusing the fragment makes the last dispatch the JS1 chain; no fault.
rootshell sh -c 'echo 1 > /sys/kernel/debug/p7580/chain_mode'
rootshell /data/wdarm 15 env PC2_REFUSE_FRAG=1 PC2_MAX=1 FRAMES=1 \
    sh /data/polylist/run.sh
rootshell cat /sys/kernel/debug/p7580/dump | head -2   # cfg=0x00083f00
rootshell sh -c 'echo 0 > /sys/kernel/debug/p7580/chain_mode'
```

Warnings: never use the diagnostic Mesa with `PAN_MESA_DEBUG=sync` or `trace`
(the diag dump block waits on out_sync=0 with `INT64_MAX` and wedges the slot).
Any run where the tiler actually bins will invoke the FS; with Defect B live,
the per-batch 5 s job timeout (60 batches) exceeds the watchdog and reboots the
phone — the big-prefill runs did exactly that, while no-bin runs finish in
seconds. Use
`PC2_REFUSE_FRAG` for chain measurements and keep `/data/wdarm` armed.

## 10. Artefacts

* `code/gpu31/polylist/polycensus.c` — extended census hook: full-chain raw
  decode (pre-existing), `PC2_ZEROPOS`, `PC2_PREFILL[_BIG]`, `PC2_POLLSCAN` +
  `PC2_SCANLOG`, `PC2_REFUSE_FRAG`, and the rejected `PC2_COPY0/DOUBLE/SPLIT`
  experiments (all off by default; no effect on the default census path).
* This report. Device-only: `/data/chainexec/scan_*.txt`, the Mesa diag build
  (now also carrying the env-gated `PAN_CHAIN_DOUBLE` resubmit probe in
  `pan_job.c`), `/data/polylist/run.sh`, `/data/wdarm`. No kernel change was
  made by this ticket; the split knob is a next-image driver item (§7).
