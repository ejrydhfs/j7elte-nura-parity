# gpu31-submitsnap — SUBMIT-time w0..w7 snapshot + fault-time compare; JS_HEAD offset verdict

Date: 2026-11-21. Owner: G31_SubmitSnapshot. Ticket: extend the SUBMIT snapshot
from w4/w5 to w0..w7, print it next to the fault-time bytes on the ERR/TIMEOUT
lines, and verify whether JS_HEAD should point at jc or jc+0x10 for Midgard
fragment jobs (panfrost 5.2 submit code vs our dispatch). Add the comparison;
fix the offset if wrong. Hand to integrator.

Related: `gpu31-final.md` (the one open defect), `gpu31-ptewalk.md` (fault-time
walk + leaf re-read, MATCH => tables correct), `gpu31-lastjob.md` (w4/w5 snapshot
+ chain-bit experiment), `gpu31-jobabi.md` (32-B header contract), `gpu31-instrtype.md`.

Starting evidence (from Main, int30 silicon):

```
PTEWALK slot=0 seq=121 jc=0x21c5600 leaf_phys=0x9efec000 bo_phys=0x9efec000 MATCH
  live=0x21c5000/0x10000 npages=16
  w = 00000052 00000000 021d6005 00000000 00010013 00000000 00000000 00000000
```

The page tables and TLB are correct (MATCH), so no IOVA-lifetime bug. The first
word at jc reads `0x52` — the same value as the `INSTR_TYPE_MISMATCH` status —
so the job header the GPU read is not a valid header at fault time, OR the GPU
wrote the fault status back into a valid header. This ticket decides between
those two by grounding the SUBMIT-time bytes.

## 1. JS_HEAD offset verdict: jc (offset 0) is correct — no fix needed

All three submit implementations agree byte-for-byte: HEAD_NEXT = the
descriptor address itself.

- kbase t7xx r15p0 (on the external disk):
  `drivers/gpu/arm/t7xx/r15p0/backend/gpu/mali_kbase_jm_hw.c:58`
  `u64 jc_head = katom->jc;`, then `:78-81`
  `kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_HEAD_NEXT_LO), jc_head & 0xFFFFFFFF, kctx);`
  `kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_HEAD_NEXT_HI), jc_head >> 32, kctx);`
  And `katom->jc = user_atom->jc` (`mali_kbase_jd.c:917`) — the userspace
  descriptor address passes through untouched.
- kbase t72x r7p0: identical (`backend/gpu/mali_kbase_jm_hw.c:67,80-83`).
- mainline v5.2 `panfrost_job_hw_submit` (`drivers/gpu/drm/panfrost/panfrost_job.c`):
  `u64 jc_head = job->jc; ... job_write(pfdev, JS_HEAD_NEXT_LO(js), jc_head & 0xFFFFFFFF);`
  `job_write(pfdev, JS_HEAD_NEXT_HI(js), jc_head >> 32);` — same verbatim copy.
- Our dispatch (`panfrost7580_job.c:p7580_job_hw_submit`):
  `lo/hi = job->jc; HEAD_NEXT_LO/HI = lo/hi` — matches all three.

There is no `+0x10` (or any other) offset anywhere on the path: Mesa builds the
32-B `job_descriptor_header` (`mali_kbase_defs.h:1689-1712`, "identical for all
GPU archs": exception_status + first_incomplete_task + fault_pointer +
bitfield bytes 16/17 + index + deps + next_job), passes its address as `jc`,
and the JM fetches word 0 (`exception_status`) at exactly that address. The
`w4` name in the old prints is bits 16..19 of that header (byte 16 =
`job_descriptor_size` bit0 + `job_type` bits), not a "+16 pointer" — the
confusion this ticket was asked to rule out. `jobabi.md` §1 tables the full
layout; `jm_hw.h` `struct p7580_job_desc` is the same 32 bytes (host test pins
`sizeof == 32`).

Conclusion: the dispatch offset is correct; no code change on the HEAD path.
The fault is not "header lands at the wrong offset in our dispatch".

## 2. What the int30 words actually say (submit-vs-fault decode)

The int30 fault-time words again, with the jobabi header overlaid:

```
w0=0x00000052  w1=0x00000000  w2=0x021d6005  w3=0x00000000
w4=0x00010013  w5=0x00000000  w6=0x00000000  w7=0x00000000
```

- `w4 = 0x00010013`: bit0=1 (64-bit `next_job`), type=9 (FRAGMENT), index=1,
  `w5 = 0` (dep 0,0), `w6/w7 = 0` (next=NULL). Legal fragment header,
  byte-identical to the 60 DONE fragments (`gpu31-lastjob.md` §1,
  `gpu31-instrtype.md` §1: CPU view, kernel 0xCF view, and faulting view all
  agree at SUBMIT time).
- `w0 = 0x52`: `exception_status` low byte = `INSTR_TYPE_MISMATCH` — the exact
  code in the ERR line's `js_status=0x52`. Per `jobabi.md` §1, `w0` is
  *written by HW on fault* (low byte exception code, bits 9:8 access type,
  bits 31:16 source ID; `0` = `JOB_NOT_STARTED`).
- `w2 = 0x021d6005`: nonzero `fault_pointer` — also HW-written at fault time
  (a GPU VA latched at fault, type-dependent: shader/tiler PC or data
  address). A valid submitted header has `fault_pointer == 0` (Mesa zeroes
  it; cf. `lastjob.md` §1 `cpu w0..7=00000000 00000000 00000000 00000000
  00010013 ...`).

So the fault-time header is a *valid submitted header with the HW fault
writeback applied* (w0=0x52 status, w2=fault pointer), not a header read from
the wrong address. If the GPU had fetched at jc+0x10, w4-position bytes would
not decode as a legal fragment header AND the status writeback would land at
the wrong word — instead the status sits exactly in `exception_status` and
the type/index/dep/next words still decode. Combined with PTEWALK MATCH
(tables correct, bytes correct in DRAM), the remaining suspect is GPU-side
stale fetch (its own cache/TLB), per the `ptewalk.md` §3 decision table — the
compare this ticket adds makes that verdict printable in one line.

## 3. Implementation (this ticket)

Canon sources (host-tested) + driver-0001 patch hunks regenerated from them
(`git apply --check` clean; GNU `patch -p1 --dry-run` clean on all three
touched hunks; every hunk `@@` count re-audited, zero skew across all 19):

- `code/gpu31/jobs/panfrost7580_job.h`: `struct p7580_submit_job` drops the
  `desc_w4`/`desc_w5` pair for a single `uint32_t desc[8]` (SUBMIT-time words
  w0..w7; `desc[4]` carries the chain_mode bit0 consumer, `desc[5]` the dep
  pair). `struct p7580_job_last` gains `uint32_t sw[8]` (the SUBMIT-time words
  copied at dispatch). All-zero when the snapshot was unavailable (cross-page
  jc, unmapped page, jc outside every submitted BO) — same contract as before.
- `code/gpu31/jobs/panfrost7580_job.c`:
  - `p7580_job_err_pin` takes `const uint32_t *sw` instead of
    `desc_w4/desc_w5`; ERR line becomes
    `... jc=0x%llx cfg=0x%08x sw=%08x x8` (NULL-safe: prints zeros).
    Old `w4=/w5=` tokens are gone from every line.
  - `p7580_job_hw_submit`: copies `job->desc[0..7]` into `jd->last.sw[0..7]`
    (8 plain u32 stores under hw_lock, IRQ-safe); `chain_mode==1` tests
    `job->desc[4] & 0x1`; dispatch line prints `sw=%08x x8`.
  - Both IRQ legs (DONE+ERR-sticky, ERR-only) pass `job->desc` (live job) or
    `jd->last.sw` (post-budget fallback) as `swp`.
  - `p7580_job_timedout` TIMEOUT line prints `sw=%08x x8` from `job->desc`.
  - `p7580_job_dump_show` prints a second `sw %08x x8` line after `last ...`.
  - One repair inside the restored diff: the DONE-leg
    `if (p7580_js_status_is_fault(js_status))` guard had been dropped by a
    bad range edit; restored (else the ERR pin would fire on every DONE IRQ).
- `code/gpu31/driver/0001-...-driver-sources.patch`:
  - `job.c` hunk (`@@ 1,1439`) + `job.h` hunk (`@@ 1,289`) rewritten
    byte-identical from canon (verified `match=True`; a duplicated 22-line
    footer that crept into the hunk-7 body during the first swap was removed).
  - `submit.c` hunk (`@@ 1,1547`): SUBMIT snapshot reads all 8 words
    (`for wi in 0..8: job->desc[wi] = *(kv + poff + wi*4)` after the same
    `job_done_sync` invalidate; `kzalloc` already zeroes, so no memset);
    chain_mode==2 clears `job->desc[4] &= ~0x1u` and patches the stored BO
    word identically. CROSS-PAGE / unmapped / outside-BO leaves zeros.
  - PTEWALK MATCH + NO-OWNER lines gain the submit side:
    `... w=%08x x8 sw=%08x x8` with `job->desc[0..7]` appended — the
    submit-vs-fault compare reads off one line. `job` is in scope (the
    walker takes `struct p7580_submit_job *job`); no signature change.
- `code/gpu31/jobs/job_test.c`: unchanged (pins `sizeof(desc)==32`,
  `p7580_jc_valid`, slot/config/fault tables — none of which moved).
  Host run: `job_test: all checks passed`.

New dmesg contract (post-flash):

```
p7580_job: dispatch slot=0 jc=0x... cfg=0x83700 ... seq=121 sw=00000000 00000000 00000000 00000000 00010013 00000000 00000000 00000000 chain_mode=0
p7580_job: ERR  ... seq=121 jc=0x... cfg=0x83700 sw=00000000 ... 00010013 ...
p7580_job: TIMEOUT ... sw=00000000 ... 00010013 ...
p7580_job: PTEWALK ... MATCH ... w=00000052 00000000 021d6005 00000000 00010013 ... sw=00000000 00000000 00000000 00000000 00010013 ...
```

Decision rule for Main's run: `w0: 0x00000000 -> 0x52` with `w4` identical
(`00010013 == 00010013`) between `sw=` and `w=` ⇒ HW status writeback onto a
correctly-fetched header (stale-fetch-then-fault or post-fault writeback
racing the leaf re-read), NOT a wrong-offset fetch and NOT a CPU-visibility
defect (SUBMIT bytes already proven good). `w4` differing ⇒ revisit the
submit-sync/coherency path. Either way the answer is now in the log without a
second run.

## 4. Verification exercised (host; silicon pending Main)

- `cd code/gpu31/jobs && cc -Wall -Wextra -o /tmp/job_test job_test.c && /tmp/job_test`
  → `job_test: all checks passed`.
- `git apply --check` on the full 19-block patch: clean.
- Hunk-count audit over all 19 blocks (`@@` claimed vs actual `+` lines):
  zero skew (submit.c `1,1547` recount after the -2/+0 line repair).
- GNU `patch -p1 --dry-run` on the three touched hunks (job.c/job.h/submit.c
  extracted): `checking file ...` x3, no rejects.
- No build-int script touched; integrator's int31 (or next) build picks the
  patch up with existing gates (the old `w4=` token appears in no gate grep;
  verified: no `build-int*.sh` references `desc_w`/`w4=`/`w=`).
- Silicon: needs Main flash + `/data/wdarm 14 /data/eglrun.sh`, return the
  `dispatch` + `ERR`/`TIMEOUT` + `PTEWALK` lines and RENDER_OK status. The
  decisive comparison (`w=` vs `sw=`) is in the log already on the first run.

## 5. Handoff to integrator

Files for the next build (on top of `b79c502`):

- `code/gpu31/jobs/panfrost7580_job.{c,h}` (canon)
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
  (job.c 1439 / job.h 289 / submit.c 1547 hunks + PTEWALK sw compare)
- this report

Do NOT take the `/tmp/job_test` binary or any `build/` output (stray
untracked artifacts were removed from the tree before commit; the two
firmware tarballs under `code/pmaports/...` are pre-existing and untouched).
Suggested gate additions for the build script (optional, cheap): grep the
applied tree for `sw=%08x` in `panfrost7580_job.c` (ERR/dispatch/TIMEOUT),
`job->desc[wi]` in applied `submit.c`, and `sw=%08x` in the PTEWALK strings
of applied `submit.c` + Image.
