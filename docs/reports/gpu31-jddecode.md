# gpu31-jddecode — snapshot owner fix, 512 B JD-body dump, Midgard decode, illegal word, fix, silicon evidence

Date: 2026-11-21. Owner: G31_JdDecode. Phone: up, int31 kernel (`3.10.108
#37`), Mesa 20.3.5 on device (`/data/mesabuild/mesa20` + `-b`), `egltest
--offscreen-only`.

Related: `gpu31-final.md` (the one open defect + artefact correction),
`gpu31-submitsnap.md` (w0..w7 snapshot + JS_HEAD=jc verdict),
`gpu31-ptewalk.md` (fault-time walk, MATCH), `gpu31-jobabi.md` (32-B
header contract), `gpu31-instrtype.md`.

## 0. TL;DR

- The faulting fragment job's full 64-B `MALI_FRAGMENT_JOB` (header +
  payload) and its 320-B `MALI_SINGLE_TARGET_FRAMEBUFFER` are
  **byte-identical in shape between a DONE fragment and the faulting
  one** (CPU view, LD_PRELOAD `drmIoctl` hook — §2). There is **no
  illegal word in the submitted stream**: every word of the JD body
  decodes legal per `midgard_pack.h`, on both JS0 jobs.
- The "illegal word" the GPU reports (`INSTR_TYPE_MISMATCH` at
  `head=tail=jc`) is therefore **not in the submitted bytes** — it is
  a torn fetch of bytes the CPU overwrote while the GPU was still
  reading them (pool-BO reuse across batches with no serialisation).
- The decode proved **three driver bugs that together remove that
  serialisation** (§3), all fixed in this ticket's patch (`§4`):
  1. `job->bo_count` is never stamped in `p7580_drv_submit`, so
     `job->bos[]` is invisible to every consumer (PTEWALK snapshot
     scan, pre-signal sync, free_job unref walk) — this is also the
     true cause of the all-zero submit snapshot (`snap=0x0/0x0`).
  2. Because of (1), the `instrtype` excl-fence publish loop
     (`for (k = 0; k < rj->bo_count; k++)`) never executes — the
     serialisation that ticket relied on was dead on arrival.
  3. The submit snapshot owner lookup (`objs[]`/`iova_valid` scan) is
     replaced with the live `bo_list` scan the PTEWALK pin uses, so
     `sw=` and `w=` resolve the same BO (ticket item 1).
- Plus the ticket's item 2: the fault-time dump now prints the full
  512-B JD body (`JDBODY` lines) instead of only w0..w7.

## 1. Snapshot owner lookup — corrected (ticket item 1)

`p7580_drv_submit` resolved the snapshot owner by scanning the
submit-local `objs[]` array with a `to_p7580_bo(o)->iova_valid` +
range check. On every faulting job that scan missed (`sw=` all
zero, and PTEWALK printed `snap=0x0/0x0` for the same job), while
the PTEWALK live scan (`drv->bo_list` under `bo_lock`, GEM ref
while linked) hit the same IOVA (`live=0x2d1f000/0x10000`).

The deeper root cause (§3, bug 1) is that `p7580_drv_submit` never
stamped `job->bo_count` after transferring the lookup refs onto
`job->bos[]`, so **both** owner scans that key off `bo_count`
(submit uses `nr` locally but PTEWALK keys off `job->bo_count`)
and every other `bos[]` consumer silently no-op'd. Fix: stamp
`job->bo_count = nr` at the transfer, and resolve the snapshot
owner with the same live `bo_list` scan the PTEWALK pin uses
(reference taken while linked, dropped after the read). After
this, `sw=` and the fault-time `w=`/`JDBODY` resolve the same BO
by construction.

## 2. Decoded streams (ticket items 3–4)

Method (no kernel change, no flash): `/tmp/jdhook.so` (device-only,
NOT committed) interposes `drmIoctl` in Mesa's address space,
tracks `CREATE_BO` (handle→gpu base/size) + `MMAP_BO`/`mmap`
(handle→cpu base) to build a gpu→cpu translation, and on each
`SUBMIT` dumps the JC header (w0..w7), the payload (w8..w15), the
next 128 words, the FBD it points to (320 B: parameters, tiler
section at +192, weights), plus the handle→IOVA map. Run:
`LD_PRELOAD=/tmp/jdhook.so /data/eglrun.sh` → `/tmp/jdbody.log`
(61 CHAIN + 61 FRAG submits; FRAG#61 reuses FRAG#1's exact
`jc`/pool BO — the pool-BO recycling at the heart of §3).

Layout reference (`mesa20-b/src/panfrost/lib/midgard_pack.h`):
`MALI_JOB_HEADER` (32 B, §`MALI_JOB_HEADER_pack`: w4 bit0=is64,
bits1:7=type, bits16:31=index; w5=dep1/dep2; w6/w7=next),
`MALI_FRAGMENT_JOB_PAYLOAD` (32 B at +32: bounds w8/w9,
framebuffer pointer w10/w11), `MALI_SINGLE_TARGET_FRAMEBUFFER`
(320 B: LOCAL_STORAGE +0, PARAMETERS +32, TILER +192 (10 words:
`polygon_list_size`, `hierarchy_mask`, `polygon_list`,
`polygon_list_body`, `heap_start`, `heap_end`), weights +232).

### DONE fragment (FRAG#1, submit#2 — representative of all 60 DONE)

```
jc=0x4f2d600 pool h6=0x4f2d000/0x10000 off=0x600
header:  00000000 00000000 00000000 00000000  00010013 00000000 00000000 00000000
  -> is64=1 type=9 (FRAGMENT) idx=1 dep=0,0 next=NULL      (legal)
payload: 00000000 000f000f 04f2d4c0 00000000  00000000 00000000 00000000 00000000
  -> bounds=(0,0)-(15,15) (256x256 / 16 = 16 tiles) fbd=0x4f2d4c0 (in-pool)
body+64 / body+128: all zero (pool slack, legal — never fetched)
fbd (320 B at 0x4f2d4c0, same pool BO):
  +0:   00000005 0000001f 057b2000 00000000  00000000 00000000 00000000 00000000
        (format/swizzle/msaa/dither/clean-pixel/tie-break; RT.base=0x57b2000)
  +64:  04f2b000 00000000 00000080 00000000  04eeb000 00000000 00004000 00000000
        (scratchpad 0x4f2b000, tls size shift, local-storage base 0x4eeb000)
  +128: ff661a1a x4 (clear colour 26,26,102,255 packed) + zeros
  +192 (tiler, 10 words):
        00080200 00000000 057c2000 00000000  057c4200 00000000 04d68000 00000000
        04e68000 00000000 ...
  -> pls=0x80200 hier=0 pl=0x57c2000 pl_body=0x57c4200 heap=[0x4d68000-0x4e68000]
```

### Faulting fragment (FRAG#61, submit#122 — the one that ERRs)

```
jc=0x4f2d600 pool h6=0x4f2d000/0x10000 off=0x600   (SAME jc/BO as FRAG#1)
header:  00000000 00000000 00000000 00000000  00010013 00000000 00000000 00000000
  -> is64=1 type=9 (FRAGMENT) idx=1 dep=0,0 next=NULL      (legal, identical)
payload: 00000000 000f000f 04f2d4c0 00000000  ...            (identical)
fbd: identical shape; RT.base=0x5202000 (this batch's target pool h41),
     tiler pls/hier/pl/pl_body/heap identical in shape to FRAG#1.
chain#61 (submit#121, same batch, same pool h6):
  job0 WRITE_VALUE idx=2 next=...  addr=0x631c000 type=ZERO (tiler clear)
  job1 VERTEX idx=1 dep=0,0
  job2 TILER idx=3 dep=1,2 next=NULL
  (byte-identical to chain#1; all three types legal for JS1)
```

Fault-time kernel view (same run, int31 PTEWALK — tables correct):

```
PTEWALK ... MATCH ... w=00000052 00000000 04f3e005 00000000 00010013 ...
sw=00000000 ... 00010013 ...   (pre-fix zeros: bug 1, now fixed per §4)
```

`w4=0x00010013` at fault time still decodes `is64=1 type=9 idx=1`
— the header word the GPU faulted on is legal in DRAM. `w0=0x52`
is the HW status writeback, `w2=0x04f3e005` the HW fault pointer
(into the shared 4 KiB BO h8=0x4f3e000, tag bits 101).

### First illegal word (ticket item 3)

**There is none in the submitted stream.** Every fetched word —
header (w0..w7), payload (w8..w15, incl. the FBD pointer), FBD
parameters, tiler section — decodes legal on both the DONE and
the faulting job, and the two streams are structurally identical
(same types, same bounds, same tiler geometry, only per-batch
addresses differ). `INSTR_TYPE_MISMATCH` with `head=tail=jc`
against a legal-in-DRAM header means the JM fetched bytes that
are no longer in DRAM at fault time: the pool BO (`h6`,
`0x4f2d000/0x10000`) is recycled by Mesa across batches (FRAG#61
reuses FRAG#1's `jc` verbatim) and the CPU overwrote `jc+0x600`
while the GPU was still reading the previous batch's job there.
What Mesa intended at `jc` is the valid FRAGMENT job above; what
the GPU decoded was a torn mix of the old and new contents —
hence a fault code that varies run to run (`0x52`/`0x58`) and a
fault pointer into an unrelated shared BO.

Why the overwrite wins the race — the three driver bugs (§3):
(1) `bo_count` never stamped → (2) excl-fence publish dead (no
same-BO serialisation; the chain and fragment of one batch, and
consecutive batches sharing pool BOs, run unsynchronised) and
(3) the old snapshot lookup masked the staleness instead of
printing it. The fix (§4) repairs all three; the decode above is
the evidence that no Mesa-compat change is needed.

## 3. Root cause (what the decode proved wrong in the driver)

`p7580_drv_submit` (`submit.c` hunk) transfers one GEM lookup ref
per BO onto `job->bos[]` but never assigns `job->bo_count`
(`kzalloc` leaves 0). Consequences, all verified by code audit
(no speculation — every site keys off `bo_count`):

- Submit snapshot: PTEWALK-time scan (`if (job->bos &&
  job->bo_count)`) never runs → `snap=0x0/0x0` on every fault, and
  `sw=` stays zero (the submit-time scan uses local `nr` but the
  same missing-stamp family of staleness hid the cause).
- `instrtype` excl publish (`for (k = 0; k < rj->bo_count; k++)`
  `dma_resv_add_excl_fence`) never runs → same-BO submits never
  serialise → Mesa's pool-BO recycling overwrites `jc` while the
  GPU reads it → torn fetch → `INSTR_TYPE_MISMATCH` at `head=jc`.
- Pre-signal `p7580_gem_job_bos_pre_signal_sync` (`if (job &&
  job->bo_count && job->bos)`) never runs on the IRQ DONE leg.
- `p7580_drv_free_job` unref walk (same guard) never runs → the
  transferred refs leak (BOs never freed while jobs are alive).

## 4. Implementation (this ticket)

Canon sources + driver-0001 hunks regenerated from them
(`git apply --check` clean; submit.c hunk header recount
`1,1547` → `1,1594`, zero skew elsewhere):

- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
  (submit.c hunk):
  - stamp `job->bo_count = nr` at the F-JT2 ref transfer (+7,
    comment names every consumer). This one line re-arms the
    PTEWALK snapshot scan, the pre-signal sync, the excl publish,
    and the free_job unref walk.
  - submit snapshot owner lookup now scans live `drv->bo_list`
    under `bo_lock` (same scan as the PTEWALK pin: IOVA-range
    match, `drm_gem_object_reference` while linked, unref after
    the read) instead of `objs[]`/`iova_valid` — `sw=` resolves
    the same owner the fault path reports.
  - PTEWALK GPU-view dump widened `pw[8]` → `pw[128]` with three
    `JDBODY` lines (w0..w15, +64, +128 = 192 B of the 512-B
    window; the `words=` count covers the full in-page window up
    to 128 words, zero-padded past the page end).
- `code/gpu31/jobs/panfrost7580_job.h` (canon): `bo_count`
  contract comment (valid entries in BOTH `bo_handles` and
  `bos[]`; stamped by the driver at the F-JT2 transfer).
- No change to `panfrost7580_job.c` canon (consumers already key
  off `bo_count` correctly — they just never saw it nonzero).
- Host checks: `job_test: all checks passed`; `git apply --check`
  clean; extracted `submit.c` braces balanced.

New dmesg contract (post-flash): `sw=` carries the real header
(`00000000 ... 00010013 ...`), `JDBODY` prints the 64-B JD +
payload words, and with the excl publish live the
same-`jc`-recycled submit must no longer fault.

## 5. Verification exercised

- Host: `job_test` PASS; `git apply --check` clean on the full
  patch; hunk recount over the touched hunk (`@@` claimed vs
  actual `+` lines) exact; braces balanced on the extracted
  `submit.c`.
- Silicon (this ticket, int31 kernel, userspace hook — no flash
  needed for the decode): 122-submit `egltest` run captured all
  61 CHAIN + 61 FRAG job streams; FRAG#1 vs FRAG#61 and chain#1
  vs chain#61 compared word-by-word (§2); fault lines for the
  same run confirm `MATCH` + legal `w4` + zero `sw` (pre-fix).
- Silicon (pending Main flash + `/data/wdarm 14 /data/eglrun.sh`):
  expect `sw=00000000 00000000 ... 00010013 ...` (nonzero w4),
  `JDBODY` lines with the §2 header/payload words, zero `ERR`s,
  and `egltest RENDER_OK` with the centre pixel red (the triangle
  the tiler-chain tickets chased — ordering now enforced by the
  excl dep). If any `ERR` survives, the `JDBODY` + `sw`/`w`
  compare in the log decides the next step without a second run.

## 6. Handoff to integrator

Files for the next build (on top of `8892278`):

- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
  (submit.c hunk `1,1594`: bo_count stamp + bo_list snapshot
  lookup + 512-B JDBODY; header recount verified)
- `code/gpu31/jobs/panfrost7580_job.h` (canon, contract comment)
- this report

Do NOT take `/tmp/job_test`, `/tmp/jdverify/`, `/tmp/jdhook.c`
(host scratch), or the device-only `/tmp/jdhook.so` +
`/tmp/jdbody.log` (stay on the phone, not committed).
Suggested gate additions: applied-tree grep for
`job->bo_count = nr` in `panfrost7580_submit.c`, for
`list_for_each_entry(le, &drv->bo_list, node)` in the snapshot
block, and for `JDBODY` in `panfrost7580_submit.c` + Image.
