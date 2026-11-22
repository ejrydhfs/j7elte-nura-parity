# gpu31-defectb — the Ferrofluid ISA BO page is clobbered by a stale cached zero line

Date: 2026-11-21 (device clock 2026-11-22 GMT). Owner: G31_DefectB.
Device: int36 (`3.10.108 #37 SMP PREEMPT Sat Nov 21 20:45:13 EST 2026`) and
int37 (`#37 … 22:41:00`, int36 + the driver-side chain split; GEM unchanged,
confirmed by Main). Mesa 20.3.5 diagnostic build at
`/data/mesabuild/mesa20` → `mesa20-b`. No flash from this ticket; all device
work under short `flock /tmp/gpsdev.lock` holds.

Scope: the fragment-shader ISA BO (e.g. `0x21d6000`, 4 KiB SHMEM, GEM handle
8) reads **all-zero** through `DEBUG_READ_BO` (0xCF) at submit although the
mmaprefs fix holds real references (`count=4 mapcount=1 vma=1 pin=1 up lru
swapb clean`). Rewriting the ISA at emit removes the `0x52`. The page is not
freed and not truncated: it is **overwritten by the writeback of a dirty
zero cache line that the kernel's cached linear alias still holds from the
shmem allocation clear**. The user mapping is Normal Non-Cacheable
(`pgprot_writecombine`), the linear alias is Normal Write-Back, and this
tree's arm64 `flush_dcache_page()` is not a flush (it only clears
`PG_dcache_clean`), so nothing reconciles the two.

## 0. TL;DR

1. **Reproduced with no GPU and no Mesa.** New client mode
   `--defectb-test` (32 trials: create 4 KiB BO → mmap → write an ISA-shaped
   page → 0xCF read → 3 ms → 0xCF + user read → munmap + GEM_CLOSE, so the
   next trial reuses the just-freed page): **31/32 trials lost the page**,
   and in **31/32 the 0xCF read immediately after the write was already
   zero** while the user mapping read the written words. That is a direct
   observation of the two aliases disagreeing: the kernel's cached map
   returns the stale zero line, userspace's NC map returns DRAM.
2. **Trigger.** A freshly allocated shmem page (page allocator hands the new
   BO a page that the previous BO's `shmem_evict_inode()` freed microseconds
   earlier). `shmem_getpage_gfp()` zeroes it with `clear_highpage()` →
   `clear_page()` → `dc zva`, which leaves a **dirty zero line in the cache
   via the kernel's Normal-WB linear alias**. It then calls
   `flush_dcache_page()`, which on this arm64 tree is a no-op
   (`arch/arm64/mm/flush.c` only clears `PG_dcache_clean`). Userspace writes
   the ISA through the WC/MT_NORMAL_NC VMA: the stores go to DRAM, the
   dirty zero line is untouched. Any later eviction of that line writes
   zeros over the ISA — asynchronously, whole page, 0–5 ms after the write.
3. **Not the BO/page lifetime.** A complete ftrace capture
   (`kmem:mm_page_alloc/free/free_batched`, `filemap:mm_filemap_delete_*`,
   no drops) shows the FS page's pfn allocated once for the BO and then
   **no free, no page-cache delete, no migration**. The page stays
   `UPTODATE|SWAPBACKED|MMAP` (+`LRU` after the pagevec drain) with the
   ledger's four legitimate references. `--defectb-test` makes the same
   point without any GPU: the loss is CPU/page-cache, not GPU and not a
   dropped reference.
4. **Not the GPU.** No `p7580_job` line exists before the loss; the first
   job IRQ in the traced runs is seconds later. Pre-sleeping 2 s *between
   the allocation and the write* makes the write survive every later sample
   (the stale line has been evicted in the meantime), which no GPU-side
   explanation can produce.
5. **Fix (driver, needs the next flash).** In `p7580_shmem_alloc()`, after
   each `shmem_read_mapping_page()`,
   `__flush_dcache_area(page_address(page), PAGE_SIZE);` — clean the
   allocation zeroes to DRAM and invalidate the linear-alias line before
   the page can be mapped to userspace, so the WC mapping is thereafter the
   only copy. Applied to `code/gpu31/gem/panfrost7580_gem.c` and mirrored in
   driver `0001`. A cached (Normal-WB) CPU mapping is the architectural
   alternative; the driver's existing submit clean / completion
   clean-then-invalidate discipline already covers it.
6. **Silicon acceptance is the next flash** (§6): 5 Mesa frames must show
   zero `p7580_job: ERR` and `p7580_pin: sub … w=c87a9835 01403d3d …` for
   the FS ISA BO, plus `--defectb-test` 32/32. The reproduction and the
   mechanism are proven on int36/int37; the fix itself is a kernel change
   and is not on the device yet.

## 1. The ledger inversion

The shaderbo/mmaprefs fix is intact: the FS ISA BO's page has
`count=4 mapcount=1 vma=1 pin=1`, `up lru swapb clean`, and the page is the
one 0xCF and the GPU read. Yet `w=` is `00000000 00000000 00000000 00000000`
at every submit while the VS ISA page next to it keeps
`c87a9495 08003c3d …`. Two identical 4 KiB shmem BOs, same mapping mode,
same process, opposite outcome. The difference is *when* each page was
allocated relative to a churn/free burst: the FS page is recycled from the
BO that was just freed, the VS page is not.

## 2. The trigger, measured

### 2.1 ftrace: same pfn, no free after allocation

Run on int37, `events/kmem/mm_page_alloc`, `mm_page_free`,
`mm_page_free_batched`, `filemap/mm_filemap_delete_from_page_cache`,
`buffer_size_kb=32768`, `overwrite=off` (complete: `entries-in-buffer ==
entries-written`). The FS BO's page (`pfn=635179` in that run):

```
alloc  pfn=635179                    1995.210890   (previous BO being freed/evicted)
free   pfn=635179                    1995.210899
alloc  pfn=635179  gfp=GFP_HIGHUSER_MOVABLE
create gpu=0x10bfc000 h=8 size=4096  1995.211004
mmap   gpu=0x10bfc000 h=8 size=4096  1995.211080
post-memcpy marker pfn=0x9b12b w0=c87a9835   1995.211258
```

After `post-memcpy` there is **no further event at all for that pfn** — no
free, no delete, nothing. The page was zeroed in place.

### 2.2 Same process, immediate sampling

Mesa diag probe `PAN_FBD_PAGE_PROBE` (pagemap + kpageflags + kpagecount +
first words) at 1 ms, 3 ms and 200 ms steps after the `memcpy`:

```
post-memcpy        pfn=… kflags=0x4808 w0=c87a9835  0x4808 = UPTODATE|SWAPBACKED|MMAP
delay-step  (1 ms) pfn=… kflags=0x4828 w0=00000000  0x4828 = +LRU
```

The flip and the `PageLRU` set are the same scheduling instant. It is a
plain in-place write of zeros; the page never leaves the page cache.

### 2.3 Pre-sleep before the write

`PAN_FBD_PRESLEEP_US=2000000` sleeps after `panfrost_bo_create()` and
*before* the `memcpy`, then the normal `PAN_FBD_DELAY2` sampling:

```
pre-memcpy        w0=00000000           (fresh page)
post-memcpy       w0=c87a9835
delay-step 0..4   w0=c87a9835           (2 s of samples, all intact)
```

So the clobber is a **one-shot that fires within milliseconds of the page
allocation**, not something that repeats after each write. Waiting for it
before the write makes the write stick — the same state the alloc-time
`__flush_dcache_area()` produces.

### 2.4 No GPU involved

`dmesg` in the traced runs has no `p7580_job`/IRQ line before the loss; the
first job IRQ is 4+ seconds later. The pre-sleep result is likewise
GPU-independent. The new client test (§3) reproduces with **no submit at
all**.

### 2.5 What the arms do, and why nothing cleans the line

* `shmem_getpage_gfp()` allocates the page, `SetPageSwapBacked`, adds it to
  the page cache, `lru_cache_add_anon()`, then `clear_highpage(page)` →
  `clear_page()` → `dc zva` (`arch/arm64/lib/clear_page.S`). That is a
  dirty zero line in the cache via the linear alias.
* `flush_dcache_page(page)` follows, but
  `arch/arm64/mm/flush.c:flush_dcache_page()` is:
  ```c
  void flush_dcache_page(struct page *page)
  {
      if (test_bit(PG_dcache_clean, &page->flags))
          clear_bit(PG_dcache_clean, &page->flags);
  }
  ```
  i.e. **no clean, no invalidate**. `insert_page()`'s `flush_dcache_page()`
  on the fault path is equally a no-op.
* The BO's VMA is `pgprot_writecombine()` = `MT_NORMAL_NC`
  (`p7580_gem_mmap`), so the CPU's ISA stores bypass the cache and land in
  DRAM. The dirty zero line is not updated, and nothing in the driver
  touches the page between allocation and submit (submit clean runs later),
  so the line survives until the cache evicts it — writing zeros over the
  NC-written bytes.

This is the ARMv8 "mismatched memory attributes" hazard: Normal-WB and
Normal-NC mappings of the same page are not coherent, and the kernel's
`flush_dcache_page()` contract assumes the user mapping is cacheable.

## 3. Minimal reproducer, no GPU: `--defectb-test`

`code/gpu31/client/pf7580-selftest.c` (built by the existing Makefile,
mock suite unaffected). Each trial: CREATE_BO 4 KiB → MMAP_BO → `mmap` →
write an ISA-shaped page (`0xc87a9835`, marker `0xcafe1234` at 0x400, full
deterministic fill) → 0xCF read → `usleep(3000)` → 0xCF word0 and 0x400 and
user word0 → `munmap` + `GEM_CLOSE` (so the next trial reuses the pfn).

Silicon, int37:

```
TRIAL 30: immediate 0xCF word0=0x00000000 (want 0xc87a9835)
TRIAL 30: LOST after 3000 us  k0=0x00000000 kmark=0x00000000 user0=0xc87a9835
TRIAL 31: immediate 0xCF word0=0x00000000 (want 0xc87a9835)
...
DEFECTB: 1/32 trials kept the page; lost-immediate=31 lost-after-3000us=31 first-bad=2
DEFECTB-TEST: FAILED
```

Trial 1 passes (its page was not freshly cleared/reused in the same way),
trials 2–31 fail. `user0` still showing `c87a9835` while `k0` reads zero is
the alias mismatch directly: the user NC read hits DRAM (ISA), the kernel
kmap read hits the stale cached zero line. This also confirms the earlier
"kernel and user agree after the fact" observations: they agree **after**
the dirty line has been evicted to DRAM (both then read zeros).

## 4. Ruled out (silicon, this ticket)

| Candidate | Verdict |
|---|---|
| Page freed / reallocated under the BO | No — complete ftrace: no free/alloc/delete/migrate for the pfn after our allocation; kflags never show BUDDY/free; no `Bad page state` |
| Lost reference (page array/pin) | No — `count=4 mapcount=1 vma=1 pin=1` at submit; page stays in the page cache; `--defectb-test` loses it with all refs held |
| BO close / eviction / GEM_CLOSE | No — no close in the window; test keeps the handle open through both reads |
| Truncate / hole punch / shmem_writepage | No — no `mm_filemap_delete_from_page_cache` for the pfn, `PageUptodate` stays set, no swap (`SwapTotal: 0`) |
| GPU write (clear/job/UAF) | No — no job before the loss; reproduces with no submit; pre-sleep (no GPU activity) prevents it |
| Wrong BO/address/meta/tag/cache-direction | No (previous tickets; unchanged) |
| "Whole-page zeroing by clear_highpage of a free page" | The *allocation* clear is the source of the zero line; the page is not free when it is clobbered |

## 5. Fix

### 5.1 Driver (applied in `code/gpu31/gem/panfrost7580_gem.c`, mirrored in driver `0001`)

In `p7580_shmem_alloc()`, immediately after each page comes back from
`shmem_read_mapping_page()` and before it can be mapped to userspace:

```c
/* gpu31-defectb: flush_dcache_page() is not a flush on this arm64
 * tree, so the clear_highpage() zeroes are still dirty lines in the
 * kernel's cached linear alias. Clean+invalidate before the page can
 * be handed out: the WC/NC userspace mapping is then the only copy. */
__flush_dcache_area(page_address(page), PAGE_SIZE);
```

`__flush_dcache_area()` (`arch/arm64/mm/cache.S`, clean+invalidate to PoC)
writes the zeros to DRAM and drops the line, so a later eviction cannot
overwrite the NC stores. It is a one-shot per page at BO creation, dwarfed
by the page-cache lookup; no hot path changes. The BO's own reference, the
mapping pin, the PTE reference, the WC prot and the submit/completion
clean-then-invalidate discipline are untouched.

The architectural alternative (recorded, not taken here to keep the
coherency discipline unchanged) is to map local SHMEM BOs **cached**
(Normal-WB) — then userspace stores and the kernel linear alias share the
cache line and the existing submit clean / completion clean+invalidate
already handle device visibility. Mixing attributes is what must not happen.

### 5.2 Why this matches all the evidence

* The clobber is a one-shot right after allocation (the line is evicted):
  the fix removes the line at exactly that point.
* It is GPU-independent (the client test): the fix is CPU-side.
* The page is never freed/truncated: the fix does not touch lifetime.
* `PAN_FBD_REUPLOAD` (write the ISA right before emit) works because by
  then the stale line is long gone; the fix makes the *first* write
  equivalent to that rewrite.

## 6. Silicon acceptance (next flash; Main)

Kernel: int36/int37 + this ticket's driver change (GEM only). Exact
commands, short lock holds:

```
# 0. boot + node
rootshell /bin/sh -c 'cat /proc/version; ls -l /dev/dri/renderD128'

# 1. plain page-lifetime loop (no GPU, must be 32/32)
rootshell /bin/sh -c '/data/pf7580-selftest --defectb-test /dev/dri/renderD128'
# expect: DEFECTB: 32/32 trials kept the page; lost-immediate=0 …;
#         DEFECTB-TEST: PASS

# 2. the Mesa reproducer, RECHECK off, 5 frames
rootshell /bin/sh -c 'dmesg -c >/dev/null'
rootshell /bin/sh -c 'B=/data/mesabuild/mesa20-b; \
  export LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri; \
  export LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri; \
  export MESA_LOADER_DRIVER_OVERRIDE=panfrost EGL_PLATFORM=surfaceless; \
  /data/egltest --offscreen-only --frames 5'
rootshell /bin/sh -c 'sleep 6; dmesg | grep -E "p7580_job: ERR |p7580_pin: sub"'
# expect: zero ERR lines; the FS ISA BO's "sub" line with
#         count>=3 mapcount>=1 up and w=c87a9835 01403d3d 00000030 …
```

Interpreting a failure after the flash: if `--defectb-test` is still not
32/32, the clean did not reach the line (check that the built gem.o contains
the `__flush_dcache_area` call in `p7580_shmem_alloc`); if the loop passes
but Mesa still loses the page, compare the FS BO's ledger line and whether
Mesa's diag `PAN_FBD_PAGE_PROBE` shows the immediate 0xCF disagreement
(§3) — that would mean a second alias/writer exists.

## 7. Diagnostics added (env-gated, strip before cutover)

Mesa diag patch `code/gpu31/mesa/diag/0003-mesa203-defectb-page-probe.patch`
(hosts `src/panfrost/lib/pan_bo.c`,
`src/gallium/drivers/panfrost/pan_assemble.c`; the device tree
`/data/mesabuild/mesa20` currently carries it):

| Knob | Effect |
|---|---|
| `PAN_FBD_PAGE_PROBE=1` | per-sample `PAGEPROBE <where>: va pfn kcount kflags w0 w256` from `/proc/self/pagemap`, `/proc/kpageflags`, `/proc/kpagecount`, plus an ftrace `trace_marker` line |
| `PAN_FBD_DELAY2_US=<us>`, `PAN_FBD_DELAY2_LOOP=<n>` | sample the shader page while the process does nothing |
| `PAN_FBD_PRESLEEP_US=<us>` | sleep after BO create, before the `memcpy` (separates allocation-time one-shot from our write) |
| `PAN_FBD_KREAD_AFTER=1` | 0xCF (kernel kmap) read immediately after the write, next to the user-alias word: the alias-disagreement measurement |
| `PAN_FBD_MPROTECT=1` | two-step `mprotect` to try to force cached PTEs (returned no change on this tree; kept as a control) |
| `pan_fbddbg_marker(create/mmap/FREE)` | BO lifecycle lines in the ftrace ring, interleaved with the `kmem`/`filemap` events |

Note: enabling `vmscan`+`migrate`+`compaction` tracing in one run wedged the
device (watchdog reset) — keep trace sets narrow (`kmem` + `filemap` only);
this is why the run that would have captured `mm_vmscan_writepage` is not in
this report.

## 8. Files / state

| File | Change |
|---|---|
| `code/gpu31/gem/panfrost7580_gem.c` | `__flush_dcache_area(page_address(page), PAGE_SIZE)` after each `shmem_read_mapping_page()` in `p7580_shmem_alloc()`, with the rationale comment |
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | same hunk mirrored (gem.c section); `make kcheck` PASS (3 TUs, zero warnings) |
| `code/gpu31/client/pf7580-selftest.c` | new `--defectb-test` (32-trial create/write/0xCF/sleep/close loop) |
| `code/gpu31/mesa/diag/0003-mesa203-defectb-page-probe.patch` | new diag patch (page probe, deltas, pre-sleep, kernel-read-after, mprotect control, BO lifecycle ftrace markers) |
| `docs/reports/gpu31-defectb.md` | this report |

Device note at hand-off: after the int37 flash + the watchdog reboot that
interrupted the last trace run, `/data/egltest` (the diagnostic Mesa build)
segfaults during `eglInitialize` right after the loader prints
`Native platform type: surfaceless`, while `/data/pf7580-selftest`
(`--pattern-test`, `--defectb-test`) runs and the 0xCF ledger is live. The
Mesa diag build needs a clean relink/rebuild before the §6 step 2 run; the
0xCF/defectb silicon evidence in this report was collected before that
state.

Host checks run: `make kcheck` (gem: 3 files, zero warnings);
`client make` + `./build/pf7580-selftest` → `MOCK SUITE: PASS`; aarch64
static client rebuild. Device: `--defectb-test` FAILED 1/32 (reproduction),
as above. No flash.
