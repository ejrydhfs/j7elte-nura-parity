# GPU31 INSTR_TYPE_MISMATCH — descriptor dump, type/slot table, fix, evidence

Date: 2026-11-21. Owner: G31_InstrType. Phone: up, int22-era kernel
(`3.10.108 #37`, errtrace live). Mesa 20.3.5 on device
(`/data/mesabuild/mesa20` + `-b`), `egltest --offscreen-only`.

Related: `gpu31-jobabi.md` (descriptor/slot/FBD tables),
`gpu31-mesa-render.md` (slotfix req0→JS1), `gpu31-tilerchain.md`
(jc/FBD/heap decode), `gpu31-primitive.md` (geometry-fault verdict).

## 0. Fault (silicon, int22 errtrace)

```
p7580_job: ERR slot=0 js_status=0x00000052 (INSTR_TYPE_MISMATCH)
  stat=0x00010000 rawstat=0x00010000 head=0x21c5600 tail=0x21c5600
  seq=121 jc=0x21c5600
p7580_job: TIMEOUT slot=0 js_status=0x52 ... head=0x21c5600
```

Reproduced live (5+ runs): exactly ONE submit faults per run, always the
last fragment (`seq=121` of 122 submits = 61 chain+fragment pairs),
always on slot 0. All 60 earlier identical pairs DONE both slots.
Fault code varies run to run (`0x52 INSTR_TYPE_MISMATCH` /
`0x58 DATA_INVALID_FAULT`) — the GPU reads bad bytes, not a fixed
type rejection.

## 1. Descriptor dump (same-fd LD_PRELOAD shim + DEBUG_READ_BO 0xCF)

Method: `/tmp/g31dbg.so` (device `gcc -shared -fPIC -ldl`) hooks
`drmIoctl` (Mesa 20.3 path): tracks CREATE_BO (handle→iova/size),
MMAP_BO/mmap (CPU maps), walks the 32-B chain on SUBMIT, then issues
the 0xCF readback on the same fd for every handle word0 + the owning
handle's jc words. Log `/tmp/g31dbg.log` (device-only, not committed).

Faulting submit (representative, IOVAs vary per run):

```
SUBMIT#122 jc=0x65e1600 req=1 rc=0
  h[0]=11 ... h[6]=4(RT 0x41000) h[7]=133(heap 1M) h[9]=6(pool 64K)
  h[10]=71 h[11]=1(heap-mirror 1M)
  job0 @0x65e1600 type=9(FRAGMENT) idx=1 dep=0,0 w4=00010013 64b
  frag tiles=(0,0)-(15,15) fbd=0x65e14c0
```

Faulting descriptor words (CPU view, `w4=0x00010013`):
`is_64b=1, type=9 (FRAGMENT), index=1, dep=0,0, next=NULL` —
byte-identical to all 60 DONE fragments. `w[0..7]` and the FBD
(§2) match the `gpu31-mesa-render.md` §3 / `gpu31-tilerchain.md` §2
captures exactly.

0xCF kernel-side readback of the same submit (same fd, same handles):
ALL `rc=0` — every handle resolvable — but every word reads
`0x00000000`, including `jc+0..60`. The CPU map shows the real bytes
(`00010013` etc.); the kernel SHMEM page view is zeros. I.e. at
SUBMIT time the kernel page cache for the pool BO does not contain
the CPU-written descriptor bytes (visibility defect), and/or the GPU
PTW walks a stale page.

## 2. Type/slot table (grounded)

Mesa emit (`pan_job.c:1015-1052`): vertex/tiler chain `req=0`,
fragment `req=PANFROST_JD_REQ_FS`. Job types (`midgard_pack.h:226-236`):

| Type | Value | Mesa submit | Slot (driver `p7580_job_slot`) | Result |
|---|---|---|---|---|
| WRITE_VALUE | 2 | chain (`req=0`) | JS1 | DONE |
| VERTEX | 5 | chain (`req=0`) | JS1 | DONE |
| TILER | 7 | chain (`req=0`) | JS1 | DONE |
| FRAGMENT | 9 | fragment (`req=FS`) | JS0 | DONE x60, fault x1 (last) |

Routing is mainline-exact (`panfrost_job_get_slot`: FS→JS0, else→JS1;
`ONLY_COMPUTE`→JS2 kernel-internal). The faulting `jc` is a type-9
FRAGMENT job on JS0 — the correct slot. The mapping is **not** the bug.

## 3. Root cause

The driver never publishes a completion fence on the BOs' `dma_resv`:
`p7580_drv_submit` takes implicit dependencies (`dma_resv_get_excl` →
`sched_job_add_dependency`, patch hunk ~+6600) but nothing ever calls
`dma_resv_add_excl_fence` — `git grep add_excl` hits only the shim's
own definition. Upstream `panfrost_job_push` attaches
`reservation_object_add_excl_fence` at submit, so same-BO submits
serialise; here every excl stays NULL and every dependency add is
skipped (`get_excl` returns NULL → no dep).

Mesa recycles pool/scratch BOs across batches (the traced run reuses
pool-BO IOVAs across all 61 batches; the faulting submit's IOVA also
appears in frame 1 with a different handle list). With no excl fence,
two live batches write/read the same pool BO concurrently: the later
batch's CPU writes can land while the earlier batch's GPU job is still
queued, and the GPU fetches a torn chain/FBD. The victim reads garbage
as the descriptor — random fault codes (`0x52`/`0x58`) at HEAD — while
60 identical earlier submits DONE. `head==tail==jc` + `DONE-bit-less`
RAWSTAT is the JM rejecting the first fetch.

Contributors ruled out: FBD/TILER/LS decode all sane (§1 + tilerchain
§2–§3); heap 1 MiB ≫ `pls 0x80200`; `JS_CONFIG` exact
(`START_MMU`+flush/inv+pri8); WC mapping + `submit_sync` drain proven
(pattern-test DONE + KERN-zero); texture path exonerated (primitive §1).

## 4. Fix (repo-side, this ticket)

`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(+30, `p7580_drv_run_job` after the out_sync install): publish the
dispatch fence as the exclusive fence on every submitted BO —

```c
if (!IS_ERR(fence) && fence) {
    struct p7580_submit_job *rj = job;
    uint32_t k;
    for (k = 0; k < rj->bo_count; k++) {
        struct drm_gem_object *o =
            (struct drm_gem_object *)rj->bos[k];
        if (!o)
            continue;
        dma_resv_add_excl_fence(&to_p7580_bo(o)->resv, fence);
    }
}
```

Placed at dispatch (not queue): `sched_job->fence` is NULL at queue
time (assigned from `run_job`'s return at dispatch, 0004 shim
`job->fence = run_fence`); `job->bos` carries one referenced pointer
per BO (F-JT2, stable across close). Locking: the 0002 shim locks
`resv->lock` inside `add_excl` — same no-outer-lock discipline as the
existing `get_excl` leg (SUBMIT-DEADLOCK rule). Later submits then
take the newest unsignaled excl as an implicit dep, serialising
same-BO batches; retired fences don't gate (dep leg skips signaled).

Also corrected `docs/SESSION-STATE.md`: the int22 fault entry blamed
the requirements→slot mapping; the mapping is correct (this report §2)
and the defect is the missing excl-fence serialisation.

No kernel change in the slices (`jobs/`, `gem/`, `mmu/`) — the only
code change is the driver-0001 hunk above. No flash per ticket
(Main rebuilds int23).

## 5. Verification required (Main, int23 build + flash)

1. Rebuild int23 (0001 hunk must survive `--check` + gate: `grep
   dma_resv_add_excl_fence panfrost7580_submit.c`).
2. `dmesg -c`, `/data/eglrun.sh` (or shim-traced variant); expect per
   frame chain JS1 DONE + fragment JS0 DONE, **zero ERRs**, no TIMEOUT,
   then `egltest RENDER_OK` (centre red — the triangle the tiler-chain
   tickets chased; ordering now guaranteed by the excl dep).
3. If any ERR survives: capture `head/tail/jc/seq` + fault code; a
   still-varying code with `dep` wiring intact reopens the visibility
   leg (§1 0xCF zeros), not the slot map.

Device-only artefacts (not committed, stay on phone): `/tmp/g31dbg.c`
(source lost with the env — rebuild from this §1 description),
`/tmp/g31dbg.so`, `/tmp/g31dbg.log`, `/tmp/g31type.*`, `/tmp/g31fbd.*`.

## 6. Worklog

- Reproduced int22 fault live ×5: always last submit (`seq=121/122`),
  slot 0, varying `0x52`/`0x58`.
- Shim chain-walk: faulting `jc` = type-9 FRAGMENT, `req=FS` → JS0,
  bytes identical to 60 DONE fragments; full 320-B FBD sane
  (`pls=0x80200 hier=0`, 1 MiB heap, SFBD params exact).
- 0xCF same-fd reads: `rc=0` everywhere, all zeros vs nonzero CPU
  view → kernel page view stale at SUBMIT.
- `git grep`: `dma_resv_get_excl` consumed at submit, zero
  `add_excl` producers → implicit-dep path dead → batches sharing
  recycled pool BOs run concurrently → torn fetch → fault.
- Fix: dispatch-time excl publish hunk in driver 0001 (+30 lines,
  comment + loop only). `git diff` reviewed; patch context verified
  against surrounding `run_job` code.
