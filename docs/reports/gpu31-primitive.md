# GPU31 primitive — untextured control, invocation/primitive decode, verdict

Date: 2026-11-21. Owner: G31_PrimitiveDebug. Phone: up, kernel
`3.10.108 #37 SMP PREEMPT Fri Nov 20 20:49:07 EST 2026` (= int21 build,
coherency pre-signal c207bdd live). Mesa 20.3.5 on device
(`/data/mesabuild/mesa20` + `-b`), EGL surfaceless. Prior state:
int20 tiler-chain audit (86f82b9) — clear exact, triangle missing, no
payload divergence; coherency fix c207bdd committed but int21 not yet
confirmed at ticket start (it is running now, `#37`).

## 1. Control results

| Test (device, under `flock /tmp/gpsdev.lock`) | Corner | Centre | Verdict |
|---|---|---|---|
| Textured `egltest` repro (`/data/egltest --offscreen-only --frames 1`) | `26,26,102,255` (expect ~26,26,102) | `26,26,102,255` (expect ~255,0,0) | `RENDER_MISMATCH` — reproduces tilerchain §6: clear exact, centre **clear** (not black) |
| Clear-only `q2wait` (3× `glClear` + `glReadPixels`, transfer-map readback path) | `26,26,102,255` | `26,26,102,255` | clear path writes RT and is CPU-visible (WAIT_BO + `bo_wait` retires) |
| Clear-only `q1sync` (`glClear` + `glFenceSync` + `ClientWaitSync` + `glFinish`) | `0,0,0,0` | `0,0,0,0` | fence-only wait without transfer flush does **not** retire the batch on this driver — readback-path artefact, not GPU |
| **Untextured solid control** `/tmp/eglsolid` (new this ticket: `pos`-only VS, `gl_FragColor=vec4(1,0,0,1)`, no sampler/texture, 60 draws + `glFinish`, repo-`egltest` flow) | `0,0,0,0` (expect ~26,26,102) | `0,0,0,0` (expect ~255,0,0) | `RENDER_MISMATCH` — **worse than textured**: not even the clear lands |
| `llvmpipe` clear (`GALLIUM_DRIVER=llvmpipe /tmp/q2wait`) | `26,26,102,255` | `26,26,102,255` | software baseline sane (no GPU IRQ expected) |

Solid-control source kept on device (`/tmp/eglsolid.c`, `/tmp/q2wait.c`,
`/tmp/q0clear.c`); device `/tmp` is tmpfs and was wiped once mid-ticket
(`g31trace.so`/`g31jc.log` lost — the int20 shim decode is preserved in
`gpu31-tilerchain.md` §2 and re-derived below from genxml, not from a
new shim run).

Texture-path isolation: the solid triangle has **no sampler, no texture,
no UVs**, yet still draws nothing — and additionally loses the clear.
The zero-texture trampoline (`texdesc[0..7]=0`, tilerchain §2.1/§4.2)
therefore **cannot** be the root cause (a zero texture would sample
black, and the textured centre reads clear, not black). Geometry leg is
the suspect; texture path is exonerated as primary.

Caveat: solid `0,0,0,0` ≠ textured `clear` because the readback paths
differ in batch retirement: `q2wait`/textured-`egltest` retire through
the transfer-map `flush_batches_accessing_bo + bo_wait` path, while the
solid binary's bare `glFinish` + `glReadPixels` does not force the same
retire on this driver (cf. `q1sync` zeros). The solid control still
proves the no-sampler program does not paint red, but its all-zero
readback must not be over-interpreted as "GPU wrote black".

## 2. Invocation (+32) / primitive (+40) decode vs Mesa expectation

Genxml anchors (device `midgard_pack.h`): `INVOCATION @ job+32` (2 w,
`invocations` + 5 shift fields), `COMPUTE_JOB_PARAMETERS @ job+40`
(`job_task_split` @ bits 26–29), `PRIMITIVE @ tiler-job+40` (6 w:
`cl0[draw|index_type|...|jts<<26]`, `base_vertex_offset`,
`restart_index`, `index_count-1`, `indices lo/hi`).
Mesa emitters: `pan_invocation.c panfrost_pack_work_groups_compute`
(`MALI_POSITIVE(d)=d-1`, `util_logbase2_ceil` semantics verified against
`u_math.h`), `pan_context.c panfrost_draw_emit_vertex/tiler`
(`jts=5` vertex, `jts=6` tiler, `draw_mode=pan_draw_mode(TRIANGLES)=8`).

Non-indexed triangle (`count=3`, `instance=1`): invocation inputs
`(1,3,1)/(1,1,1)` → `packed = 0x00000002`, shifts all 0,
`w1 = wz=32<<22 | unk=2<<28 = 0x28000000`.
Observed shim payload words (t348dchain §2.1, all frames identical):

- VERTEX `w8..w13 = 00000002 28000000 14000000 00000000 00000000 00000000`
- TILER  `w8..w13 = 00000002 28000000 18030008 00000000 00000000 00000002`

Decode:

- `invocations=2` (`num_y-1=2` → `vertex_count=3`), `w1=0x28000000`
  (`sy=sz=wx=wy=0`, `wz=32` quirk, `unk=2`) — **exact Mesa match**
  (recomputed by hand from `pan_invocation.c` in `/tmp/jcwords.c`).
- Vertex `PARAMS w10=0x14000000` = `job_task_split 5 << 26` — **match**
  (`panfrost_draw_emit_vertex`: `job_task_split=5`).
- Tiler `prim0=0x18030008`: `draw=8 TRIANGLES`, `index_type=0`,
  `idx_en=0`, `first_provoking=0`, `low/high_depth_cull=1/1`,
  `jts=6`, `restart=0` — **match** (`panfrost_draw_emit_tiler`,
  flatshade-first off, restart off). `prim1=0` (`base_vertex_offset`),
  `prim2=0`, `prim3=2` (`index_count-1` → `index_count=3`) — **match**
  non-indexed triangle. (`prim4/5` indices `0/0` — correct for
  non-indexed.)
- Job headers: 64-bit, `WRITE_VALUE(idx0) → VERTEX(idx0) → TILER(idx1)`,
  NULL-terminated. `dep` 16-bit split in the old shim disagrees with
  plausible scoreboard values, but `midgard_pack.h MALI_JOB_HEADER`
  confirms `dependency_1 = bits 160–175, dependency_2 = 176–191`; the
  shim never re-derived the header packing, so dep wiring is
  **unverified, not divergent** (same caveat as tilerchain §4.3).

**No invocation/primitive divergence.** The words the GPU reads are the
words Mesa means to send for a 3-vertex non-indexed triangle.

## 3. Kernel-side probes (this ticket, raw ioctls, int21 running)

- `0xCF DEBUG_READ_BO` route **works**: WC-mmap write `0xDEAD0000` is
  visible via kernel view (`val=0xdead0000`, `/tmp/wcvis`).
- Raw `WRITE_VALUE Zero` (`/tmp/wvvis`, req=0 → JS1): `SUBMIT rc=0`,
  `WAIT_BO rc=0`, **USER and KERN views both `0x00000000`** — GPU write
  visible on int21 for the trivial job. (The repo-client
  `/data/pf7580-submit --pattern-test` concurrently reports STALE
  `0xA5` both views with `SYNCOBJ-WAIT rc=0` — same binary that passed
  pre-int20; ring shows no fresh `p7580_job` line for that run, so the
  discrepancy is a stuck-slot / stale-fence artefact, not a coherency
  regression. See §4.)
- Raw `FRAGMENT`-only (`/tmp/rtprobe{,2,3}`: Mesa-exact `PARAMS
  cl0=0xb04d8281`, bounds `(0,0)-(255,255)`, FBD tagged `&~63`, TLS
  scratch + zero weights in `rtprobe3`): `SUBMIT/WAIT rc=0` but
  **RT stays `0xA5A5A5A5`** in both views, ring `rawstat=0x00010000`
  (**slot-0 ERR**). A hand-built FBD misses something Mesa provides
  (working Mesa clear-only `q2wait` retires `rawstat=0x00000001`
  **DONE** on JS0) — raw-fragment is not a valid clear control without
  the full batch state. Not pursued further; the Mesa-emitted fragment
  is the valid probe and it DONEs on int20 (tilerchain §2.2).
- Raw `WV+VERTEX+TILER` chain with **Mesa-exact §2 words**
  (`/tmp/vtprobe2`, req=0 → JS1): all submits `rc=0`, `WAIT rc=0`, but
  ring shows **`rawstat=0x00020000` (slot-1 ERR)** and a subsequent
  `WARNING at panfrost7580_job.c:322 p7580_job_hw_submit`
  (`COMMAND_NEXT != 0` — slot stuck, recovery pending). Bisection
  `WV-only → WV+VERTEX → WV+VERTEX+TILER` could not separate VERTEX vs
  TILER because the first ERR wedges the slot for the later submits in
  the same process. Heap `0xCF` readback (`heap[0]=0`, `body=0`) is
  therefore **inconclusive** (zeros are the prefill, and no DONE ever
  retired).

## 4. Verdict: geometry present in descriptors, faulted in execution

- Invocation + primitive + DRAW + FBD + heap + slot routing all check
  out; both slots DONE on int20 for the Mesa chain (tilerchain §2/§3).
- On the running int21 kernel the same-shaped raw chain raises
  **slot-1 ERR** (`0x00020000`), and raw/​Mesa fragment probes that
  depend on tiler output raise **slot-0 ERR** (`0x00010000`); `WRITE_VALUE`
  alone still DONEs. The fault follows **job type (VERTEX/TILER
  execution)**, not addresses, slots, heap size, coherency order, or
  texture contents.
- The precise missing piece is the **JS_STATUS fault code + HEAD/TAIL
  at the slot-1 ERR** (CONFIG vs INSTR vs TILE_RANGE vs
  ADDRESS_RANGE vs OUT_OF_MEMORY — `jm_hw.h:159-190` table). The
  first-8-jobs `p7580_job_trace` budget is long exhausted by egltest
  startup (no `dispatch`/`post-start`/`irq slot` lines remain in the
  ring), and `0xCF` reads only BO bytes, not registers — so the fault
  code is unobservable with the current kernel. Polygon-list non-emptiness
  is likewise undecidable until a chain DONEs.

## 5. What Main must do (kernel change requested → int22 build + flash)

1. Extend observability (debug-only, strip at cutover): raise/reset
   `p7580_job_trace` budget (or add a one-shot `DEBUG_READ_BO`-style
   sysfs dump) to capture, for the faulting submit, `JS_STATUS`
   (`js_status_name`), `JS_HEAD/TAIL`, and the tiler-heap head +
   `polygon_list_body` words after chain DONE/ERR.
2. Re-run: `dmesg -c`, textured `egltest --frames 1` (expect per-frame
   chain JS1 + fragment JS0 lines with fault code), `/tmp/eglsolid`
   and `/tmp/vtprobe2` bisects (WV-only vs WV+VERTEX vs WV+TILER in
   **separate processes** to avoid the stuck-slot WARN contamination).
3. Decide by fault code: `TILE_RANGE`/`OUT_OF_MEMORY` → heap sizing;
   `INSTR_*` → shader-state pointers; `ADDRESS_RANGE`/`READ/WRITE_FAULT`
   → descriptor VA residency; `CONFIG/AFFINITY` → slot programming.

No repo kernel change in this ticket (probes are device `/tmp` only).
Cutover hygiene unchanged: `0xCF` route, `/tmp/eglsolid*`,
`/tmp/q*`, `/tmp/*probe*`, `/tmp/jcwords*` stay on-device; this report
is the only repo artefact.
