# gpu31-mmaprefs — userspace-mapping page references: design, tests, silicon check

Date: 2026-11-21. Owner: G31_MmapRefs (for integration; no flash in this
ticket). Scope: `code/gpu31/gem/panfrost7580_gem.{c,h}` (mmap / fault /
vm_ops open+close / free), the driver 0001 mirror (gem + submit), the
host model `code/gpu31/gem/maprefs_model.c`, the client device test
`--mmaprefs-test`, and the closure probes specified by
`gpu31-shaderbo.md` §5.1.

Predecessor: `docs/reports/gpu31-shaderbo.md` — the fragment ISA BO page
is populated, mapped into AS0, read by the GPU, and then **zeroed
wholesale while the BO is live and mapped** (`clear_highpage`-class, no
submit/close/munmap/swap in the window). Rewriting the ISA into the
mapping at every meta emit removes the `0x52` completely, so the GPU
really reads that page. The report's fix (§5.2) is implemented here.

## 0. TL;DR

1. **Root cause of the class.** The userspace mapping was
   `remap_pfn_range()` + `VM_PFNMAP`, which installs *special* PTEs and
   refcounts nothing. The only reference keeping a mapped page out of
   the allocator was the BO's own `shmem_read_mapping_page()` pin. The
   mapping contributed zero, so any path that loses/drops the BO's
   reference lets the allocator hand the still-mapped page out and zero
   it — exactly the observed event.
2. **Fix, in the driver.** Local SHMEM mappings are now fault-driven
   `VM_MIXEDMAP`: `p7580_gem_fault()` inserts `bo->pages[off]` with
   `vm_insert_page()`, so every PTE owns a page reference the mm
   releases at zap; and the mapping itself holds one reference **per BO
   page for as long as any VMA maps the BO**, taken on the 0→1 VMA
   transition and dropped on the 1→0 close (`p7580_gem_map_pin` /
   `p7580_gem_map_unpin`). The BO's own page reference, the WC prot,
   the bind-time GPU pin and the clean-then-invalidate discipline are
   untouched.
3. **Shape precedent inside the target tree:** `udl_gem_mmap()` clears
   `VM_PFNMAP`/sets `VM_MIXEDMAP` after `drm_gem_mmap()` and leaves the
   driver's own `gem_vm_ops` installed; `udl_gem_fault()` is exactly
   `vm_insert_page(vma, addr, obj->pages[off])`. No second ops table is
   swapped in. `VM_DONTCOPY` matches the vendor kbase mapping on this
   device (`mali_kbase_mem_linux.c:773`).
4. **Host evidence:** new model test `gem/maprefs_model.c` (**62
   checks, PASS**) — reference identities, the negative control that
   reproduces the zeroing with the old shape, split/mremap/two-mapping
   pairing, the free-path guards, scope (CMA/imported), and the two
   closure probes. `make kcheck` 3 files zero warnings, `make kobj` 3
   objects, `gem_validate` PASS, `cachedir_model` PASS, client
   `MOCK SUITE: PASS` (host + `qemu-aarch64` static). Driver 0001
   `git apply --check` clean, only the gem/submit sections changed.
5. **Silicon acceptance** (needs the flash; §6): 0 `p7580_job: ERR`
   lines over a 5-frame Mesa run, `p7580_pin: sub` ledger for the FS ISA
   BO with `count >= 3`, `mapcount >= 1`, `up`, and the ISA words
   `c87a9835 01403d3d …`; plus `/data/pf7580-selftest
   --mmaprefs-test /dev/dri/renderD128` → `MMAPREFS-TEST: PASS`.

## 1. What the old shape held (the reference ledger)

For a 4 KiB local SHMEM BO page, before this ticket:

| Reference | Who takes it | Who drops it | Covers the page while mapped? |
|---|---|---|---|
| page-cache ref | `shmem_read_mapping_page()` | page cache / eviction | not the mapping |
| BO ref | `shmem_read_mapping_page()` (one per page) | `p7580_shmem_free()` | only while the BO lives |
| mapping ref | — (nothing) | — | **no** |
| PTE ref | — (`remap_pfn_range` special PTE) | — | **no** |

So `page_count` for a mapped page was 2, neither of which the mapping
owns. After the fix it is 3 before the first fault and 4 with a mapped
PTE:

| Reference | Who takes it | Who drops it |
|---|---|---|
| page-cache ref | `shmem_read_mapping_page()` | page cache |
| BO ref | `shmem_read_mapping_page()` | `p7580_shmem_free()` |
| **mapping ref** | `p7580_gem_map_pin()` (0→1 VMA) | `p7580_gem_map_unpin()` (1→0 close) |
| **PTE ref** | `insert_page()`'s `get_page()` + `page_add_file_rmap()` | `zap_pte_range()`'s `page_remove_rmap()` + `put_page()` |

Everything in the last two rows is new. Both are released by *different*
owners (mm for the PTE, driver for the mapping), which is why the model
keeps them apart.

## 2. Design

### 2.1 Fault-driven `VM_MIXEDMAP` (per-PTE reference)

`p7580_gem_mmap()` after `drm_gem_mmap()` for a local SHMEM BO
(`bo->pages && !bo->sgt`):

```c
vma->vm_flags &= ~VM_PFNMAP;
vma->vm_flags |= VM_MIXEDMAP | VM_DONTCOPY;
mutex_lock(&obj->dev->struct_mutex);
p7580_gem_vm_get(bo);          /* mapping ref for this VMA */
mutex_unlock(&obj->dev->struct_mutex);
return 0;                      /* PTEs come from the fault handler */
```

* `drm_gem_mmap()` (3.10) installed `VM_IO|VM_PFNMAP|VM_DONTEXPAND|
  VM_DONTDUMP` and the driver's `gem_vm_ops`; flipping the flags is legal
  and is what `udl_gem_mmap()` does in this same tree. (The old note in
  the file header — "post-hoc MIXEDMAP breaks the core contract" — was a
  mis-attribution: the `-EINVAL`/`-ENXIO` of `gpu31-mmap.md` R2 /
  `gpu31-mmap2.md` §2 was `dma_mmap_attrs()` reading the fake
  `vma->vm_pgoff` as a buffer page index. It is fixed by the `vm_pgoff =
  0` reset that is kept here.)
* `p7580_gem_fault()` → `p7580_map_page()` → `vm_insert_page(vma, addr,
  bo->pages[off])`. `insert_page()` (mm/memory.c) rejects anon/compound
  pages, `flush_dcache_page()`s, `get_page()`s, `page_add_file_rmap()`s
  and installs a **normal** (non-special) PTE. A raced second fault on
  the same address returns `-EBUSY`, which `p7580_fault_err()` folds
  into the retry arm: no reference taken, no page leaked.
* No lock is taken in the fault handler: `bo->pages`, `bo->cma_paddr`,
  `bo->sgt` are immutable for the BO's lifetime, `vm_insert_page`
  takes the pte lock itself, and the VMA's GEM reference keeps the BO
  (and an imported BO's attachment) alive. Same shape as udl.
* CMA and PRIME-imported VMAs keep their immediate mapping and their
  core flags: `dma_mmap_attrs()` (WC scanout, `dma_alloc`-owned memory)
  and `remap_pfn_range()` over the exporter's live attachment
  (`bo->sgt`). Their (normally unreachable) fault path uses
  `vm_insert_pfn()`, the only insert helper legal on a PFNMAP-only VMA.

### 2.2 Mapping-owned references (`put_page` on close)

`VM_MIXEDMAP` alone already pins every page that has a PTE. The ticket
(and §5.2 of the shaderbo report) asks for the **mapping** to hold the
pages, which also covers the pages of a partly-touched BO, so the driver
keeps a per-BO pin:

```c
static int  p7580_bo_cpu_mapped(bo);   /* SHMEM && pages && !sgt */
static void p7580_gem_map_pin(bo);     /* get_page() per page, map_pinned = 1 */
static void p7580_gem_map_unpin(bo);   /* put_page() per page */
static void p7580_gem_vm_get(bo);      /* vma_refs++ , pin on 0->1 */
static void p7580_gem_vm_put(bo);      /* vma_refs--, unpin on 1->0 */
```

`vma_refs` is incremented **once per VMA that will be closed once** — the
mmap success path and `vm_ops->open` — and decremented by
`vm_ops->close`:

* 3.10 `__split_vma()` (`mm/mmap.c:2524`) and `copy_vma()`
  (`mm/mmap.c:2939`) call `->open` on the new VMA, and every VMA gets
  exactly one `->close` (split parts, `mremap`'s old range, `munmap`,
  exit). The count therefore reaches 0 exactly when the last VMA goes
  away.
* `dup_mmap()` does **not** call `->open`, so a fork-inherited VMA would
  close without a take and release the parent's references. Hence
  `VM_DONTCOPY` — the same flag the vendor kbase mapping sets on this
  device (and DRM's own GEM-reference pairing already assumes the same
  open/close symmetry).
* The pin deliberately covers the **whole BO**, not "the mapped byte
  range": after a split/mremap the per-VMA ranges partition the original
  range, so a range-keyed pin would leak references on the split-off
  part. A fixed whole-BO set is the only bookkeeping that is invariant
  under splits, and it is what the previous partial-fault case needs.
* Lock discipline: both transitions are serialised by
  `dev->struct_mutex`, which every DRM VMA hook already takes, so a 0→1
  take can never interleave with a 1→0 put. The driver's hooks close
  their critical section *before* calling `drm_gem_vm_open/close` (those
  take the same mutex themselves): this VMA's GEM reference is taken
  inside the core call and dropped inside the core call, so the BO
  cannot go away in between.

### 2.3 Free path

`p7580_gem_free_object()` cannot run while a VMA exists (each VMA holds
a GEM reference), so it asserts exactly that and cannot leak:

```c
if (WARN_ON_ONCE(bo->vma_refs != 0)) bo->vma_refs = 0;
if (WARN_ON_ONCE(bo->map_pinned))     p7580_gem_map_unpin(bo);
```

then the existing order is unchanged: GPU unmap/IOVA free, DMA unbind,
attachment destroy, mmap-offset release, `drm_gem_object_release`,
`resv` fini, backing free (`p7580_shmem_free` drops the BO's own page
references last).

## 3. Closure probes (`gpu31-shaderbo.md` §5.1)

`p7580_gem_debug_page_ledger(obj, tag, idx)` (DEBUG-ONLY, strip before
cutover) prints one line:

```
p7580_pin: <tag> obj=%lu iova=0x%llx idx=%u count=%d mapcount=%d vma=%u pin=%u
           up|lru|swapb|dirty mapping=%p pgidx=%lu w=%08x %08x %08x %08x
```

* `count` = `page_count()` (cache + BO + mapping + PTE),
  `mapcount` = `page_mapcount()` (= mapped PTEs), `vma`/`pin` = the
  mapping pin state, then `PageUptodate/PageLRU/PageSwapBacked/
  PageDirty`, then the page-cache identity `page_mapping()` +
  `page_index()`, then the first four words of the page.
* Call sites: `tag="0xcf"` from `p7580_gem_debug_read_word()` (the
  client's DEBUG_READ_BO 0xCF path, *after* the completion
  clean-then-invalidate, so the words are DRAM truth) and `tag="sub"`
  from `p7580_drv_submit()` per BO after `p7580_gem_submit_sync()`,
  budget 8 lines per submit (its own budget — the job/IOVA budgets are
  untouched).
* Interpretation (same decision tree as the report, plus the identity
  pair G31_ShaderBo asked for): `count` = 1 means only the page cache
  holds the page (BO ref lost); `count >= 3` with the mapping live means
  both mapping owners are intact; `!up`/`!lru`/`swapb` identifies a
  shmem page-cache removal rather than a BO reference drop; and if
  `mapping=`/`pgidx=` change across lines (or the words are gone while
  `up`/`lru` and `count >= 3` hold) the page was **replaced and
  re-referenced** — allocator reuse of the struct page — not merely
  unfreed. Same identity + zeroed words means the content was
  overwritten **in place**, and the shape of the zero separates the
  writers: `w=` all zero (and the `0x400` marker gone in a second 0xCF
  read) is a wholesale `clear_highpage()`-class clear of a page that
  still has `count >= 3` owners — i.e. someone holds a stale/duplicate
  reference to the same `struct page`, localised by the caller's tag
  (`0xcf` vs `sub`); only the first words zero is a targeted write.

## 4. Host tests (all run, all green)

`code/gpu31/gem/maprefs_model.c` — **54 checks, 0 failures**:

| Test | What it pins down |
|---|---|
| `mapped_page_cannot_be_recycled` | create 2 refs → mmap 3 → fault 4, `mapcount` 1; second fault `-EBUSY` with no extra ref; worst-case freer leaves 2; allocator recycle refused; ISA words intact; close returns the page to 0; recycle then allowed |
| `old_shape_loses_the_page` | **negative control**: with `remap_pfn_range` (no pin, no PTE ref) the same freer leaves 0 and the allocator zeroes the live page — the observed failure, reproduced |
| `split_keeps_and_balances` | `->open` copy does not double-pin; first part's close keeps the pin; last close unpins exactly once; gets == puts |
| `mremap_balances` | open-before-unmap ordering keeps and then releases the pin exactly once |
| `two_mappings` | second independent mapping (report `magic2`): pin once, `mapcount` 2, both views survive the freer, pin survives the first close |
| `closure_ledger` | the §5.1 ledger at the 0xCF read and at SUBMIT: `count >= 3`, `mapcount >= 1`, `up`, `pin=1 vma=1`, ISA words present; and after the freer `count >= 2`, words still there |
| `ledger_identity_names_replacement` | a lost reference keeps `mapping`/`page_index` and the words; a recycled + re-added page changes the pair and reads zero while `up`/`lru` are set — the third verdict the flash check needs |
| `free_path_guards` | close without open is refused + warned (no put); a leaked pin at free is dropped; BO teardown leaves only the page-cache ref |
| `scope_cma_imported` | CMA and `sgt`-backed BOs take no mapping pin |

Checks run in this ticket:

```
cd code/gpu31/gem    && make check            # gem_validate + cachedir_model (27) + maprefs_model (62)
cd code/gpu31/gem    && make kcheck           # 3 TUs -fsyntax-only, zero warnings
cd code/gpu31/gem    && make kobj             # 3 objects
cd code/gpu31/client && make check            # MOCK SUITE: PASS (host)
cd code/gpu31/client && make check-aarch64    # MOCK SUITE: PASS (qemu-aarch64, static)
# driver 0001: git apply --check on an empty tree  -> clean;
#   only panfrost7580_gem.c/.h and panfrost7580_submit.c sections differ;
#   applying it reproduces the repo's gem.c/.h byte-for-byte.
# extra: panfrost7580_submit.c compiles -Wall -Werror -c against the real
#   3.10.108 headers with the 0001-0004 shims staged; submit.o references
#   p7580_gem_debug_page_ledger (U) next to p7580_gem_submit_sync.
```

Note on negative controls: `old_shape_loses_the_page` is the one that
would fail if the fix were reverted (no pin, no PTE ref); the ledger
test would fail if the pin were dropped early (split/mremap cases would
fail if `vm_ops->open` forgot to take the extra VMA count).

## 5. Client device test (`--mmaprefs-test`)

New mode in `code/gpu31/client/pf7580-selftest.c` (built by the existing
Makefile; mock suite unaffected):

1. CREATE_BO 4 KiB (job/ISA BO) + MMAP_BO + `mmap()`; fill an ISA-shaped
   page (`0xc87a9835 01403d3d …` at 0x100 via `pf7580_mr_pattern`, a Zero
   WRITE_VALUE descriptor at 0, a `0xcafe1234` marker at 0x400);
   CREATE_BO 4 KiB target prefilled 0xA5A5A5A5, mapped.
2. **Churn window**: six CREATE_BO/MMAP_BO/`mmap`/memset/`munmap`
   cycles of other 4 KiB BOs — the exact window in which the shaderbo
   trace shows the page being zeroed pre-fix.
3. Verify the original mapping word-for-word (1024/1024), then a
   **second** independent mapping of the same BO, then the kernel 0xCF
   read of word 0 and of the 0x400 marker.
4. Submit (`bos=[job, target]`, `jc=job_gpu`, `out_sync`), wait on the
   fence (+8 s absolute), re-verify the user mapping and the 0xCF read,
   and check the target's word 0 became 0 (the job really ran). This also
   produces the submit-side ledger in dmesg.
5. `MMAPREFS-TEST: PASS` iff every view agrees and every value is
   nonzero. Any mismatch prints the offset and both values.

## 6. Silicon verification (exact, for the integrator)

Kernel: build with driver 0001 (this ticket) on top of the current int
series; nothing else changes. Device work under `flock /tmp/gpsdev.lock`,
short holds, no flash from this ticket.

```
# 0. after the flash, confirm the boot and the VMA shape
rootshell /bin/sh -c 'dmesg -c >/dev/null'
rootshell /bin/sh -c 'cat /proc/version'

# 1. Mesa fragment run (the shaderbo reproducer, no RECHECK patch)
rootshell /bin/sh -c 'B=/data/mesabuild/mesa20-b; \
  export LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri; \
  export LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri; \
  export MESA_LOADER_DRIVER_OVERRIDE=panfrost EGL_PLATFORM=surfaceless; \
  /data/egltest --offscreen-only --frames 5'
rootshell /bin/sh -c 'sleep 6; dmesg | grep -E "p7580_job: ERR |p7580_pin: sub|p7580_pin: 0xcf|p7580_gem_mmap: ok"'
```

Acceptance for step 1 (all three must hold):

* `dmesg | grep -c 'p7580_job: ERR '` → **0** across the 5 frames (the
  old `0x52` on `fault_pointer = 0x<fs>005` is gone **without** the
  `PAN_FBD_RECHECK` rewrite patch and without any Mesa change);
* every `p7580_gem_mmap: ok mixedmap …` line (no more
  `remap_pfn_range` path), and for the FS ISA BO's
  `p7580_pin: sub … idx=0` line: `count >= 3`, `mapcount >= 1`,
  `up` (and not `!lru`/`swapb`), `w=c87a9835 01403d3d …`;
* the `p7580_pin: 0xcf` ledger (from any `DEBUG_READ_BO`, e.g. the
  cachedir test) shows `vma>=1 pin=1` and the same non-zero words.

`PAN_FBD_RECHECK` stays available as the control, not as a workaround:
with the fix in place it must be a no-op for faults, and with it
*disabled* the FS submit must be clean on its own — that is exactly what
step 1 above requires. If the page is still zeroed at the next
`CREATE_BO` (the compile-time `0x400` marker gone too), the freer is
above the mapping reference (shmem inode/page-cache removal) and the
ledger's `!up` / `!lru` / `swapb` bits name it; the acceptance stays
"zero `ERR` lines plus the ISA words at the FS submit", never merely
"no crash". The empty polygon list (`gpu31-fbdtarget.md` §5.3) keeps the
triangle out of the readback and is a separate ticket.

```
# 2. mapping-lifetime client test (no GL)
rootshell /bin/sh -c '/data/pf7580-selftest --mmaprefs-test /dev/dri/renderD128'
# expect: CHURN-VIEW OK 1024/1024, SECOND-MAP OK, KERN-VIEW OK,
#         POST-SUBMIT-VIEW OK, POST-SUBMIT OK, MMAPREFS-TEST: PASS

# 3. regressions that share the pages: completion/cache direction
rootshell /bin/sh -c '/data/pf7580-selftest --cachedir-test /dev/dri/renderD128'
rootshell /bin/sh -c '/data/pf7580-selftest --pattern-test /dev/dri/renderD128'
# expect: CACHEDIR-TEST: PASS, PATTERN-TEST: PASS (GPU writes visible)

# 4. pin/mapcount closure (the report's §5.1 measurement)
rootshell /bin/sh -c 'dmesg | grep "p7580_pin:" | tail -20'
# FS ISA BO: count 4 while the mapping has a PTE (cache+BO+pin+PTE),
# 3 if read before its first fault; mapcount 1 (or 2 with two mappings);
# the words are the compiled ISA, never 00000000.
```

If step 1 shows `count` at 2 for a mapped BO, the mapping ref is missing
(check `p7580_gem_mmap` reached the `ok mixedmap` return); if it shows
`count >= 3` but the ISA is still zero, compare the ledger lines: a
changed `mapping=`/`pgidx=` (or zeroed words with `up`/`lru` and
`count >= 3`) means the page was replaced and re-referenced by the
allocator, while `!up`/`!lru`/`swapb` with the same identity means the
page left the shmem page cache and was not re-used yet. Same identity
with `w=` all-zero plus a zeroed `0x400` marker (one extra
`DEBUG_READ_BO` at `0x400`) is the wholesale in-place clear of a page
that still has `count >= 3` owners — a stale/duplicate reference to the
same `struct page` — and the tag of the line that sees it (`0xcf` vs
`sub`) says which side is touching it; only the leading words zero is a
targeted write.

## 7. What is proven here, and what still needs the flash

* Proven on the host: reference identities and arithmetic, VMA
  open/close pairing across split/mremap/two mappings, the free-path
  guards, the negative control (old shape loses the page), the identity
  verdict (lost reference vs replaced page), and the exact ledger values
  the closure probes must show. The kernel code compiles
  against the real 3.10.108 headers (syntax + objects), the driver patch
  applies cleanly, and the client paths build and pass their mock suite
  on host and qemu-aarch64.
* Needs the flash (Main): `vm_insert_page()`/`vm_ops->close` on real
  hardware, and the ledger's absolute `count` values on silicon. The
  ticket's silicon acceptance is §6 steps 1–4.
* Not changed: the WC/cached prot selection, `pgprot_writecombine`, the
  bind-time GPU pin, `dma_sync` clean-then-invalidate discipline, the
  CMA/scanout path, PRIME attachment ownership, IOVA/GPU mapping, and
  the BO's own page pin.

## 8. Files / state

| File | Change |
|---|---|
| `code/gpu31/gem/panfrost7580_gem.c` | fault-driven `VM_MIXEDMAP` mmap + `vm_insert_page` fault; `p7580_gem_vm_get/put` + `p7580_gem_map_pin/unpin`; `vm_ops` open/close wrappers; free-path guards; header comment; `p7580_gem_debug_page_ledger()` + 0xCF call; fault path no longer takes `struct_mutex` |
| `code/gpu31/gem/panfrost7580_gem.h` | `vma_refs`/`map_pinned` in `struct p7580_bo`; ledger prototype (DEBUG-ONLY) |
| `code/gpu31/gem/maprefs_model.c` | new host model test (54 checks) |
| `code/gpu31/gem/Makefile` | `maprefs_model` in `check`/`clean` |
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | gem.c/.h sections regenerated; submit.c gains the `tag="sub"` ledger probe with its own 8-line budget |
| `code/gpu31/client/pf7580-selftest.c` | new `--mmaprefs-test` device test (+ usage line) |
| `docs/reports/gpu31-shaderbo.md` | §5.2 marked implemented (points here); §7 state updated |
| `docs/reports/gpu31-mmap2.md`, `docs/reports/gpu31-cachedir.md` | status notes pointing at this ticket |

DEBUG-ONLY to strip at cutover: `p7580_gem_debug_page_ledger()`, its two
call sites, and the `p7580_gem_mmap` entry/exit `pr_info`s (pre-existing
from `gpu31-mmap2`, one of which now says `ok mixedmap`).
