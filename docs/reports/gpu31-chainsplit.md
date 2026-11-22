# gpu31-chainsplit — driver-side chain split at the VS/TILER boundary

Date: 2026-11-21. Owner: G31_ChainSplit. Target image: int37 (built by the
integrator from `code/gpu31/driver/0001-…`), device evidence from int36.
No flash was performed by this ticket; this is a patch/report hand-off.

Question: `gpu31-chainexec` proved that one chained submit runs
`WRITE_VALUE → VERTEX → TILER` but the tiler consumes the **pre-VS**
content of `Draw.Position`: the chain's `END_FLUSH_CLEAN_INV` happens once,
after the last job, so the vertex shader's stores never become visible to
the tiler inside that dispatch. Can the driver split the chain into one
scheduled run per node, each with its own `END_FLUSH_CLEAN_INV`, while the
UAPI still returns a single out-sync?

Answer: **yes, implemented in the SUBMIT path.** The submitted linked list
is walked through the descriptor `next` pointers; every node is copied into
a driver-owned shadow descriptor with `next = NULL`, both dependency
indices cleared and a sequential index, then queued as its own `drm_sched`
job on the same slot entity. Node *i* attaches a `drm_sched` dependency on
node *i-1*'s armed fence, each run programs `p7580_js_config()` (START and
END `FLUSH_CLEAN_INV`), and the ioctl's `out_sync` is installed once, on
the **last** queued node's fence. BO references and the runtime-PM hold are
taken per queued node and released in that node's `free_job`. A one-node
chain with no dependency indices (Mesa's fragment submit) keeps the
original descriptor and allocates no shadow BO.

## 0. TL;DR

1. **Chain walk.** `p7580_drv_submit` reads the 32-byte header at `jc`,
   follows `word 6/7` (64-bit form when word 4 bit 0 is set, otherwise
   `word 6`) until `next == NULL`, and caps the walk at 16 nodes/type-checks
   every node (`WRITE_VALUE`, `VERTEX`, `TILER`, `FUSED`, `FRAGMENT`,
   `NULL`).
2. **Isolation.** Each node is copied (header + inline payload) into one
   slot of a single driver-owned SHMEM BO: `next = NULL`, `dep1 = dep2 = 0`,
   `index = position + 1`. Payload fields are absolute GPU addresses, so
   the copy is relocatable. The shadow is mapped into AS0 and cleaned to
   DRAM before queueing.
3. **Per-node scheduling.** One `drm_sched_job` per node on the SUBMIT's
   existing per-slot entity. Node 0 carries the external ordering
   (per-file serial predecessor, in-sync fences, BO exclusion fences);
   node *i>0* carries `drm_sched_job_add_dependency(prev_fence)`. All nodes
   use the same slot (`FS → JS0`, everything else → JS1), so each node is a
   fresh `START` on the same JM slot.
4. **Single out-fence.** The UAPI `out_sync` handle is validated once,
   before anything is queued, and `drm_syncobj_replace_fence` is called
   exactly once with the last queued node's armed fence. One wait therefore
   covers the whole split. On a mid-chain queue failure the last node the
   GPU accepted owns the out-fence, and that fence is also published as the
   file's serial predecessor.
5. **Per-job lifetime.** Every queued node owns one reference per submitted
   BO, one reference on the shared shadow BO, and one job-lifetime PM ref
   (armed before `drm_sched_job_submit`, released in `free_job`). Nodes
   that never queued release their shadow reference in the error path.
6. **Host tests.** `code/gpu31/driver/chainsplit_test.c` compiles the real
   pure-C chain helpers from `jobs/panfrost7580_jm_hw.h` and models the
   dispatch: chain decode/isolate, ordering on one slot, single out-fence,
   partial-failure unwinding with PM/BO/shadow ledgers, pass-through and
   the 32-bit `next` form. `make check` in `code/gpu31/driver` prints
   `chainsplit_test: all checks passed`.
7. **Silicon acceptance (next image).** With `PC2_POLLSCAN` +
   `PC2_REFUSE_FRAG`, the control census must move from
   `tiles_nonempty=0` to the VS triangle's tile set (non-zero and not
   exactly one tile), and the `PC2_PREFILL` run must bin the VS triangle
   rather than the tiny prefill. Once Defect B (FS ISA page) is fixed the
   full frame must show the centre red and the corner clear.

## 1. Why the split must live in the driver

`gpu31-chainexec` §0/§7 established, on int36:

* Mesa's chain is byte-correct: `WRITE_VALUE(idx 2) → VERTEX(idx 1) →
  TILER(idx 3)`, `next` NULL-terminated, tiler deps `1`/`2`.
* The JM executes all three jobs in one dispatch and the tiler consumes the
  **pre-VS** `Draw.Position` buffer. The VS stores are present in that
  buffer when the tiler completes, so the defect is visibility/ordering,
  not execution, addressing, primitive or the chain bit.
* The only lever that inserts a submit boundary (and therefore a second
  START/END `FLUSH_CLEAN_INV`) between the VS and the tiler is the driver:
  userspace chain splitting/duplication issues from Mesa fault JS1 with
  `JS_STATUS 0x41 POWER_FAULT`.

The driver-side split below avoids the userspace experiment's failure
shape (duplicate `WRITE_VALUE` submits): each original node is dispatched
exactly once, in original chain order, and no extra node is synthesized.

## 2. Design

### 2.1 Chain walk (`p7580_drv_submit`)

After the existing BO lookup/map/sync loop (which places every submitted BO
on `drv->bo_list` and cleans it to DRAM), the driver walks the chain:

```
walk = args->jc
for (;;) {
    read 32-byte header at walk from the owning BO (clean+invalidate first)
    type = (word4 >> 1) & 0x7f
    reject type 0 / unknown / reserved (no grounded payload size)
    append header + address
    next = (word4 & 1) ? word6 | word7 << 32 : word6
    if (!next) break;
    require p7580_jc_valid(next)
    walk = next
}
```

`p7580_drv_gpu_read()` resolves `[walk, walk+len)` against the live BO list
(taking a reference), runs the existing clean-then-invalidate CPU-read
sequence, and copies page by page through `kmap_atomic`; a descriptor that
crosses a page boundary is handled. A descriptor outside every live BO, a
cycle longer than `P7580_CHAIN_MAX_JOBS` (16), or an unsupported job type
fails the ioctl with `-ENOENT`/`-E2BIG`/`-EINVAL` and stage `chain`.

### 2.2 Shadow descriptor (`p7580_chain_hdr_isolate`)

A node cannot be dispatched with its original header: its `next` pointer
would make the JM walk the rest of the chain inside the same dispatch, and
its `job_index` dependencies name nodes absent from this dispatch.
`p7580_chain_hdr_isolate(hdr, pos)` therefore writes:

| Header field | Split value |
|---|---|
| `next_job` (word 6/7) | `0` (terminate after this node) |
| `dep1` / `dep2` (word 5) | `0` (ordering is now a `drm_sched` dependency) |
| `job_index` (word 4 bits 16..31) | `pos + 1` |
| `Is64`, type, barrier bits | preserved |

One shadow BO per submitted chain (`p7580_drv_shadow_create`) holds all
nodes at `shadow_iova + pos * P7580_CHAIN_SLOT_STRIDE` (256 B stride; the
largest Midgard aggregate is the 192-byte tiler job, so 16 nodes fit one
page). Header **and inline payload** are copied; the payload is not
relocated because every field in it is an absolute GPU address. The whole
shadow BO is cleaned once with `p7580_gem_submit_sync()` after the fill.

Payload lengths are grounded per type (`p7580_chain_job_bytes`):
`NULL 32`, `WRITE_VALUE 64`, `VERTEX 96`, `TILER/FUSED 192`,
`FRAGMENT 64`; anything else is rejected rather than guessed.

### 2.3 Per-node sched job and ordering

`p7580_drv_split_queue(ctx, pos, jc, hdr, prev, &fence)` builds and queues
one node:

1. `struct p7580_submit_job` with `jc = shadow base + pos*256`, the
   isolated header in `desc[8]`, `chain_shadow/chain_pos/chain_len` stamps,
   and (last node only) the UAPI `out_sync` handle.
2. One reference per submitted BO **per node** (`drm_gem_object_reference`)
   plus the per-node `iova_*` diagnostic snapshot.
3. `drm_sched_job_alloc` on the submit's slot entity, the ops round-trip
   guard, `sched_job->slot = slot`, `drm_sched_job_arm`.
4. Dependencies: `prev` (node 0: the per-file serial predecessor; node
   *i*: the armed fence of node *i-1*) first, then — node 0 only — the
   explicit in-sync fences and implicit BO exclusion fences.
5. `p7580_drv_job_pm_arm`, `drm_sched_job_submit`; the helper returns one
   caller reference on the armed fence for the next node/out-sync.

The shim's entity FIFO plus the explicit previous-fence dependency gives
strict `WRITE_VALUE → VERTEX → TILER` order across separate dispatches:
the pump will not dequeue node *i* while node *i-1*'s fence is unsignaled.
`run_job` then programs the shadow `jc` and `p7580_job_hw_submit()` writes
`p7580_js_config()` = AS0 | START_FLUSH_CLEAN_INV | START_MMU |
END_FLUSH_CLEAN_INV | PRI(8), i.e. **every node gets its own cache
clean+invalidate bracket**.

### 2.4 One out-fence and the file serial chain

The ioctl resolves and validates `out_sync` once before queueing. After the
loop, the driver calls `drm_syncobj_replace_fence` **once**, with the last
queued node's fence (partial failure: the last node the GPU accepted). The
same fence becomes `df->serial_prev`, so Mesa's following fragment submit
(JS0) still orders behind the whole vertex/tiler split through the
per-file serial dependency. Intermediate node fences are only internal
`drm_sched` dependencies.

### 2.5 BO / shadow / PM lifetime per node

| Resource | Acquired | Released |
|---|---|---|
| user BO refs | one per node in `p7580_drv_split_queue` | each node's `free_job` (after `job_done_sync`) |
| shadow ref + object ref | `P7580_CHAIN_MAX_JOBS`-bounded, one per node at create | each node's `free_job`; last release runs `p7580_gem_gpu_unmap` then drops the object |
| runtime-PM ref | `p7580_drv_job_pm_arm` before queue | `p7580_drv_job_pm_release` in `free_job` / queue-failure unwind |
| caller lookup + in-fence refs | while building the split | dropped at the single exit after the split is queued (or fails) |

Nodes that never queue (allocation/dependency/queue failure) release their
shadow reference and per-node BO references immediately; already-queued
nodes remain owned by the scheduler and retire normally. The shadow is
unmapped (`p7580_gem_gpu_unmap`) by the last retiring/erroring node, never
while a queued node may still read it.

### 2.6 Unchanged paths

* A one-node chain with `dep1 == dep2 == 0` (the Mesa fragment submit) is
  dispatched from the original `args->jc` exactly as before; no shadow BO
  is allocated.
* Slot selection, `run_job`, IRQ completion, timeout/recover, fence
  signalling, BO completion sync and `chain_mode` dispatch tracing are
  untouched. The shadow `jc` is what those paths now observe for split
  chains.

## 3. Files changed

| File | Change |
|---|---|
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | regenerated `panfrost7580_submit.c` hunk: chain walk, shadow BO, `p7580_drv_split_queue`, single out-fence, per-node unwind; `panfrost7580_job.h` hunk: `chain_shadow`, `chain_pos`, `chain_len`; `panfrost7580_jm_hw.h` hunk: chain helpers |
| `code/gpu31/jobs/panfrost7580_jm_hw.h` | pure-C chain surface: `P7580_CHAIN_*`, job-type numerics, `p7580_chain_hdr_{is64,type,index,next,dep1,dep2}`, `p7580_chain_hdr_isolate`, `p7580_chain_job_bytes` |
| `code/gpu31/jobs/panfrost7580_job.h` | per-node split stamps in `struct p7580_submit_job` |
| `code/gpu31/driver/chainsplit_test.c` | new host test (real helpers + scheduler model) |
| `code/gpu31/driver/Makefile` | builds/runs `chainsplit_test` in `make check` |
| `docs/reports/gpu31-chainsplit.md` | this report |

`git apply --check` of the regenerated driver `0001` is clean, and the
applied `panfrost7580_submit.c` is byte-identical to the staged source
(verified against a scratch git apply during regeneration).

## 4. Host tests

`code/gpu31/driver/Makefile`:

```
make -C code/gpu31/driver check
./submit_fix_test
submit_fix_test: all checks passed
./chainsplit_test
chainsplit_test: all checks passed
```

`chainsplit_test` uses the *real* header helpers, not a copy:

| Test | What it pins |
|---|---|
| T1 chain decode/isolate | Mesa sample `0x00020005 / 0x0001000b / 0x0003000f`: types 2/5/7, indices 2/1/3, 64-bit `next` chain, tiler deps 1/2; isolate clears `next`/deps, sets index `pos+1`, preserves type/Is64; 256-B slots are 64-B aligned/do not overlap and 16 fit one page |
| T2 ordering | 3 nodes queued on one slot; node 0 has no dependency, node 1 depends on node 0's fence, node 2 on node 1's |
| T3 single out-fence | exactly one `replace_fence`, target is node 2's fence; `serial_prev` is node 2 |
| T4 error unwinding | failure at node 1: only node 0 queued, out-fence and serial published with node 0, unqueued nodes' shadow refs released; after retire: PM arms == releases, user BO refs back to caller-only, last shadow release unmaps once. Failure at node 0: nothing queued, no out-fence install, no serial publish, all shadow refs released |
| T5 pass-through | one node + zero deps → no shadow; one node + dep indices → shadow needed |
| T6 forms/limits | 32-bit `next` (Is64 clear) reads word 6 only; unknown type 6/0 returns 0 bytes; `P7580_CHAIN_MAX_JOBS == 16` |

## 5. Silicon verification (int37, exact)

Build/flash by the integrator (no flash from this ticket). The only changed
applied file is driver `0001`; all prior int36 gates/routing are unchanged.

### 5.1 Boot evidence that the split is active

The driver prints, for the first 8 split submits (`p7580_chain_trace`) and
for the first 8 nodes (`p7580_sched_trace`):

```
p7580_sched: chain jc=0x<user jc> nodes=3 shadow=0x<iova> stride=256
p7580_sched: split seq=<n> pos=0/2 slot=1 jc=0x<iova+0x000>
p7580_sched: split seq=<n+1> pos=1/2 slot=1 jc=0x<iova+0x100>
p7580_sched: split seq=<n+2> pos=2/2 slot=1 jc=0x<iova+0x200>
```

plus the existing `p7580_sched: run_job seq=… jc=…` /
`p7580_job: dispatch … cfg=0x00083700 …` lines showing three distinct JS1
STARTs and the END_FLUSH_CLEAN_INV bits, and `p7580_job: dispatch` `sw`
words for the split nodes with `w5=0/w6=0` (next and deps cleared).
The Mesa fragment submit remains one node (no `split` lines).

### 5.2 Acceptance probe (no fragment): control

```
rootshell /data/wdarm 15 env PC2_POLLSCAN=1 PC2_REFUSE_FRAG=1 \
    PC2_SCANLOG=/data/chainsplit/scan_ctrl.txt PC2_MAX=1 FRAMES=1 \
    sh /data/polylist/run.sh
```

Pass: the scan's `tiles_nonempty` for the VS triangle's tile set is
**non-zero and not exactly 1** (the same triangle covers many 16×16 tiles),
and the scan reads the VS triangle from `Draw.Position`. Failure signature
to report instead: `tiles_nonempty=0` (no boundary effect), exactly `1`
(pre-VS/prefill content still consumed), or a JS1 `0x41 POWER_FAULT`
(the split itself is rejected by the JM; capture `p7580_job: irq-err`
`sw`/`PTEWALK` and the `split`/`dispatch` lines).

### 5.3 Acceptance probe: prefill must be superseded

```
rootshell /data/wdarm 15 env PC2_PREFILL=1 PC2_POLLSCAN=1 \
    PC2_REFUSE_FRAG=1 PC2_SCANLOG=/data/chainsplit/scan_prefill.txt \
    PC2_MAX=1 FRAMES=1 sh /data/polylist/run.sh
```

Pass: the census now reflects the **VS triangle** (many tiles), not the
tiny `(0,0),(8,0),(0,8)` prefill's single tile 0. This is the decisive
change from int36 (`gpu31-chainexec` §8: prefill path binned exactly one
CPU triangle; control binned zero).

### 5.4 Pixel acceptance (after Defect B is fixed)

```
rootshell /data/wdarm 15 env PC2_MAX=1 FRAMES=1 sh /data/polylist/run.sh
```

With bins present the tiler no longer has to be refused; once the FS ISA
page defect is fixed the centre pixel must be red and the corner clear
(the int36 blocker is the independent `0x52` FS fault, not this split).

Any of the three probes must be run under the existing device lock
(`flock /tmp/gpsdev.lock`) with short holds, as in `gpu31-chainexec`.

## 6. Risks / limits

* **Cost:** one SHMEM page + one AS0 mapping per split submit (not per
  node). The fragment path and all one-node submits allocate nothing.
* **Cap:** 16 chain nodes; longer chains fail `-E2BIG` (Mesa's Midgard
  chains are 3 nodes in the measured frame).
* **CPU-readable source:** the walk reads descriptor bytes through the BO's
  SHMEM pages; a descriptor placed in a non-CPU-readable/imported BO would
  fail `-EFAULT`. Mesa puts batch job descriptors in a local CREATE_BO pool.
* **Hardware acceptance is open:** if the JM rejects standalone nodes or a
  standalone tiler still bins pre-VS data, §5.2 tells exactly which
  signature was observed; the fallback experiments remain the two-submit
  `chain_mode` shapes from `gpu31-chainexec` §7, but per-node isolation is
  the shape that avoids re-submitting `WRITE_VALUE`.
* `chain_mode == 2` (the old in-place descriptor-bit patch experiment) is
  dormant for split submits; it is superseded by per-node isolation and is
  not used by this path.

## 7. Hand-off

Integrator rebuild: int37 is int36 plus the regenerated
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (the 0002 wiring
and every other patch are unchanged). Suggested post-build gates, in
addition to the existing int36 gate set:

1. `git apply --check` of driver `0001` on the pristine tree: clean.
2. `drivers/gpu/drm/panfrost/panfrost7580_submit.o` carries
   `p7580_drv_split_queue`, `p7580_drv_gpu_read`, `p7580_drv_shadow_create`,
   and exactly one `p7580_drv_shadow_put` call site from `free_job`.
3. `Image`/`System.map` strings contain `p7580_sched: chain jc=`,
   `p7580_sched: split seq=`, `p7580_sched: submit_err stage=chain`.
4. `make -C code/gpu31/driver check` passes (this report's host tests).

Then run §5.1–§5.3 on the device; §5.4 only after Defect B is fixed.
