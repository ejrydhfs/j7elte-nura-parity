# gpu31-ptewalk — fault-time AS0 PT walk: design, evidence, decision table

Date: 2026-11-21. Owner: G31_PteWalk. Phone: on int24; this ticket builds
host-side + int26 image only — Main flashes, integrator owns int26 run.

Related: `gpu31-final.md` (the one open defect), `gpu31-instrtype.md`
(excl-fence serialisation, landed), `gpu31-iova.md` (free-time unmap +
reuse fence, landed), `gpu31-armfence.md` (arm-time fence, landed),
`gpu31-lastjob.md` (descriptor dump: CPU view == kernel view, valid
FRAGMENT bytes), `gpu31-integration26.md` (int26 build, pending).

> Fix 2026-11-21 (G31_PteWalkBytes): NO-OWNER could not decide the case —
> the owner lookup used only the job's SUBMIT-time `bos[]` snapshot, and
> the 8-word GPU-view dump ran only when an owner existed. Walker now
> also scans the live `drv->bo_list` (under `bo_lock`, GEM ref while
> linked) for an IOVA range containing `jc`, falls back to the live BO
> for MATCH/MISMATCH, prints both `snap=` and `live=` ranges on every
> line, and always re-reads the 8 words from `leaf_phys + page_off`
> (even NO-OWNER; `page_off` is then the in-page jc offset).
> Verified: hunk 1542/1542, `git apply --check` clean, GNU patch
> dry-run + apply clean, walker braces balanced. Hand to integrator
> for int30.
+
## 0. Defect (silicon, unchanged through int25)

Per-frame deterministic fault on the last Mesa submit:

```
p7580_job: ERR slot=0 js_status=0x52 INSTR_TYPE_MISMATCH head=tail=jc seq=121 w4=0x00010013
```

Descriptor legal (type 9 FRAGMENT, 64-bit, dep 0,0, next NULL),
byte-identical to 60 DONE fragments; no IOVA overlaps; excl-fence,
arm-time fence, and free-time-unmap fixes all landed and did not clear
it. Decisive test (`gpu31-final.md` §5): at fault time, walk the AS0
page tables for `jc`, compare the leaf's physical page with the owning
BO's current page array, and re-read that physical page via kmap in the
same handler. Differ ⇒ page-table/IOVA lifetime bug (fix map/unmap +
invalidate ordering); match ⇒ GPU-side stale fetch (fix: stronger
invalidate before START).

## 1. Implementation (this ticket)

`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(submit.c new-file hunk, now 1470 lines):

- `static noinline void p7580_drv_ptewalk_pin(drv, job)` — bounded,
  PM-held, process-context only. Walks the 4-level LPAE tables from
  `drv->mmu.root_dma` over ≤4 `kmap`/`kunmap` pairs (3 PTE levels +
  leaf ATE); finds the owning BO by `jc` inside one submitted BO's
  IOVA (`job->bos[]` referenced pointers, F-JT2, stable across close);
  compares `leaf_phys` vs `p7580_bo_dma_addr(owner, page_idx)`; re-reads
  32 descriptor bytes from the leaf phys page via `kmap` in the same
  handler. Prints exactly one `pr_err` line:

```
p7580_job: PTEWALK slot=%d seq=%u jc=0x%llx leaf=0x%llx leaf_phys=0x%llx bo_phys=0x%llx MATCH|MISMATCH iova=0x%lx npages=%u w=%08x ×8
```

  Degenerate shapes print one line each and return: `level=%u UNMAPPED`
  (a level entry is not a PTE), `NOT-LEAF` (level-3 entry is not an
  ATE), `NO-OWNER` (jc inside no submitted BO), `CROSS-PAGE` (32-B
  header would cross a page — asserts the dispatch snapshot contract).
- Call site: `p7580_drv_timedout_job`, PM-held window, BEFORE
  `p7580_job_timedout(&drv->jd, job)` → recover rebinds AS0. This is
  the only point where tables, BO pages, and power are all valid
  together: the IRQ ERR leg is atomic (no kmap of HIGHMEM-adjacent
  pages, no BO deref), and post-recover the tables are rebound.
- Includes: `linux/highmem.h` + `linux/mm.h` added to the submit.c
  include block (`kmap`/`pfn_to_page`/`PAGE_MASK`).

Locking/power discipline: runs under the timedout path's
`p7580_drv_pm_get` hold (fails closed dark — no line, no MMIO); CPU
reads only (`kmap`/`kunmap` on lowmem table pages, same discipline as
the GEM `0xCF` debug-read leg); no locks taken (job owns its BO refs);
no TLB access (pure table-memory comparison, so stale-TLB vs
stale-table is distinguishable).

`code/gpu31/series/build-int26.sh` (copy of int25 + §6m ptewalk gates:
walker symbol/string/call-site checks, nm-or-disasm for the inlined
case, Image `PTEWALK` string, applied-tree grep evidence).

## 2. Evidence (host + build; silicon pending Main flash)

Host: driver `make check` PASS (`submit_fix_test: all checks passed`);
`git apply --check` clean on the full 19-block patch; `-fsyntax-only`
clean of the applied `panfrost7580_submit.c` against the real 3.10.108
headers + staged 0001–0004 shims (cross `aarch64-linux-gnu-gcc`).

Build int26: `-j4` Image RC=0, all prior gates PASS (ops layout,
irq/completion, slot routing, ABI-align, gpuwrite, coherency, errtrace,
instrtype excl publish, iova free-time unmap, armfence 6l) + new 6m
ptewalk gate PASS (walker present, comparison + re-read live, call
inside timedout path, string in Image). Outputs (external disk, kept):

- `Image-int26`: `acbaa043276912a53dfb3775dacf7d80d4cb0e6ecc306ef2abbe01fcdf0bc2a4`
- `kernel-boot-int26.img`: `0aca9dc175c093df3b71470eaf1d5b3e696fa2c6b61a14534df7cabce94e9728`
- Logs: `.../gpu31/logs/gpu31-int26-build.log`,
  `.../gpu31/logs/gpu31-int26-evidence.log`

Silicon run (Main: flash + `/data/wdarm 14 /data/eglrun.sh`, return
verbatim `ERR` + `TIMEOUT` + `PTEWALK` lines and RENDER_OK status):
requested via Main handoff; integrator is build-only/no-flash.

## 3. Decision table (decisive on the PTEWALK line)

- `PTEWALK ... MISMATCH leaf_phys=0x... bo_phys=0x... (differ)` ⇒ the
  table points at a page the BO no longer owns: **PT/IOVA lifetime
  bug**. Minimal next change: free-time path must leaf-clear +
  PM-held range invalidate + `bo_list` unlink BEFORE `iova_free`
  returns the range (iova.md contract), AND map must
  invalidate-before (purge any dark-free survivor) + map + invalidate
  publish — then re-verify with one egltest (expect zero ERR).
- `PTEWALK ... MATCH leaf_phys == bo_phys, w[] == 0x00010013 ...` ⇒
  tables are correct and the bytes are correct in DRAM; the GPU
  fetched stale data through its own cache/TLB: **GPU-side stale
  fetch**. Minimal next change: pre-START GPU-side invalidate —
  `LOCK(jc-range) + FLUSH_PT + wait !ACTIVE` (mmu `FLUSH_PT` op,
  same shape as the map/unmap publish leg) programmed between
  CONFIG_NEXT and COMMAND_NEXT=START in `p7580_job_hw_submit`,
  PM-held — then re-verify with one egltest (expect zero ERR).
- `PTEWALK ... UNMAPPED / NOT-LEAF` ⇒ the mapping is gone at fault
  time (unmap raced execution): lifetime bug, same fix as MISMATCH
  but audit the free/timeout ordering first.
- `PTEWALK ... NO-OWNER` ⇒ jc belongs to no submitted BO (stale jc or
  snapshot bug): audit the `job->bos[]`/snapshot stamping, not the
  MMU.
- No PTEWALK line at all ⇒ the fault did not take the timeout path
  on this run (IRQ-only retire or dark PM-get failure): capture
  `ERR`/`ERR-MAP` + `dump` and re-run with `trace_arm` re-armed.

Silicon verdict + fix/verification land as `gpu31-integration26.md`
(Main flash + egltest) on the PTEWALK line.
