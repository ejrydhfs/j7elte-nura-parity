# gpu31-mmlock — the mmap-offset manager is now driver-owned and serialized; the 155→101 fps gap is NOT the per-frame teardown

Date: 2026-11-22. Owner: G31_MMLock. Device: int45
(`3.10.108 #37 SMP PREEMPT Sun Nov 22 18:10:25 EST 2026`) for the
negative control, `int46measure` (`#37 18:44:21`) for the measurements.
Mesa 20.3.5 at `/data/mesabuild/mesa20-b`, client `gfdemo`; all device
work under `flock /tmp/gpsdev.lock`. Driver source of truth
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`
(commit `04db08d`, sha256 `e7474670…`). Status: **race fixed and built
(0 errors); the CRASH reproduction is gone from the kernel log under
the 1000-iteration churn, but the churn client itself still dies with
SIGSEGV (open, §6.1) so the stress is NOT claimed as a pass; the fps
target is not reached and the measurements below show the per-frame
teardown is not the cause (§5).**

## 0. Verdict

1. The observed oops is a **double-allocation in the 3.10 core offset
   manager**, not a GEM or MMU bug. `drm_gem_create_mmap_offset()`
   (drm_gem.c:334) does `drm_mm_search_free()` and then a separate
   `drm_mm_get_block()`; neither the `drm_mm` nor the `drm_hashtab` has
   any locking. Two creators can both take the same hole; the second
   `drm_mm_insert_helper()` walks the first node's `LIST_POISON2` next
   pointer (`dead000000000130`) — exactly the reported PC
   (`drm_mm_insert_helper+0x30 <- drm_mm_get_block_generic <-
   drm_gem_create_mmap_offset+0x8c <- p7580_gem_create <-
   panfrost_ioctl_create_bo`). Free now really runs from the sched
   `free_job` workqueue (gpu31-boleak), so create and free are genuinely
   concurrent.
2. The reader is unsafe too: `drm_gem_mmap()` walks the same
   `offset_hash` under `dev->struct_mutex` and takes the object with an
   unconditional `drm_gem_object_reference()`, so it can resurrect a
   BO already at refcount 0 (free_object in progress). `struct_mutex`
   cannot serialize the writer side: `gem_free_object` is reachable
   with `struct_mutex` already held (and must never take it itself).
3. **Fix:** the driver owns the whole fake-offset space — its own
   `drm_mm` (page window equal to the core's 64-bit
   `DRM_FILE_PAGE_OFFSET_*` layout), its own `drm_open_hash`
   (offset → BO) and one `mmap_offset_lock` around alloc, free,
   err-undo and the `.mmap` lookup. `.mmap` no longer calls
   `drm_gem_mmap`; it resolves the offset in the driver table and takes
   the object with `kref_get_unless_zero`, then installs the VMA and
   `drm_vm_open_locked` under `struct_mutex` with no lock nesting. The
   core manager is no longer called anywhere in the driver.
4. The **fps gap is not the per-frame teardown.** Measured on the fixed
   image: free path 213 µs/BO × ~2.7 frees/frame ≈ **0.57 ms/frame**
   (PTE clear 71 µs, TLB+PM 14.6 µs, dma_unbind 117 µs, put_page 7 µs,
   fput 0.3 µs per BO) against a **~3.5 ms/frame** gap. The frame time
   is dominated by client-side kernel CPU (5.2 ms/frame sys at 256²)
   and GPU/pixel work (fps scales 161 → 109 → 101 → 45 fps at
   32²/128²/256²/512²). Details and the next instrumentation step are
   in §5.

## 1. The race, byte for byte (3.10.108 tree)

```
drm_gem_create_mmap_offset(obj)                 drivers/gpu/drm/drm_gem.c:334
  list->map = kzalloc(...)                      /* obj->map_list, core-owned */
  hole = drm_mm_search_free(&mm->offset_manager, size>>12, 0, false)
  list->file_offset_node = drm_mm_get_block(hole, size>>12, 0)   /* insert */
  drm_ht_insert_item(&mm->offset_hash, &list->hash)
```

* `drm_mm_search_free()` returns a hole node without modifying the
  tree; `drm_mm_get_block()` then links a **fresh** node the core
  allocated. Two threads in the window between search and insert both
  receive the same hole; the first insert relinks it, the second
  traverses the first node's poisoned `node_list` (`LIST_POISON2`).
* `drm_hashtab.c` has no locks at all (verified: no
  `spin_lock`/`mutex` in the file), and `drm_gem_free_mmap_offset()`
  does `drm_ht_remove_item()` + `drm_mm_put_block()` unguarded.
* `drm_gem_mmap()` (drm_gem.c:658) locks `dev->struct_mutex` for the
  lookup only; create/free never take it. `p7580_gem_free_object()` is
  entered from `drm_gem_object_unreference*` in several contexts,
  including with `struct_mutex` held, so it must not take that lock.

The driver's writers are: `p7580_gem_create()` (CREATE_BO ioctl),
`p7580_gem_prime_import_sg_table()` (dma_buf import), the two
error-unwind legs, and `p7580_gem_free_object()` (client close/exit,
sched `free_job`). The reader was `p7580_gem_mmap()` →
`drm_gem_mmap()`. Shadow BOs do not create offsets of their own — they
go through `p7580_gem_create()` (`p7580_drv_shadow_create`), so fixing
the create/free/lookup sites fixes them too; `panfrost_ioctl_mmap_bo()`
was a third reader of `obj->map_list.hash.key` with no lock.

## 2. The fix (driver 0001, commit `04db08d`)

`struct p7580_dev` gains:

* `struct drm_mm offset_mm` — page-index window
  `P7580_OFFSET_START = (0xFFFFFFF >> PAGE_SHIFT) + 1`,
  `P7580_OFFSET_SIZE = (0xFFFFFFF >> PAGE_SHIFT) * 16` (the core's
  64-bit layout, so `MMAP_BO` hands out the same page-start encoding as
  before and Mesa is unaffected);
* `struct drm_open_hash offset_hash` — `offset_node.start` → BO;
* `struct mutex mmap_offset_lock` — the only serializer.

`struct p7580_bo` gains `offset_node`, `offset_hash`, `offset_valid`.

New helpers (`panfrost7580_gem.c`): `p7580_offset_alloc()` (search +
insert + hash insert, all inside the lock, hash insert failure removes
the mm node), `p7580_offset_free()` (hash remove + node remove under
the lock), `p7580_offset_lookup()` (hash find + `kref_get_unless_zero`
under the lock; a refcount-0 object is dying — its hash entry is
removed before `kfree`, both under this lock, so the failed get is the
correct "stale offset" answer).

Call sites switched: `p7580_gem_create()` (alloc, `err_offset:` free),
`p7580_gem_prime_import_sg_table()` (alloc with a fail-closed
`dev_private` check, IOVA-alloc unwind frees the offset),
`p7580_gem_free_object()` (`if (bo->offset_valid) p7580_offset_free()`),
`panfrost_ioctl_mmap_bo()` (reads `offset_node.start` under the lock),
and `p7580_gem_mmap()`:

```
obj = p7580_offset_lookup(gd, vma->vm_pgoff);   /* referenced, or NULL */
if (!obj) return drm_mmap(filp, vma);           /* not ours: core legacy path */
if (vm_size > obj->size) { unref; return -EINVAL; }
vma->vm_flags |= VM_IO|VM_PFNMAP|VM_DONTEXPAND|VM_DONTDUMP;
vma->vm_ops = dev->driver->gem_vm_ops;
vma->vm_private_data = obj;
vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
mutex_lock(&dev->struct_mutex); drm_vm_open_locked(dev, vma); mutex_unlock(...);
```

No core `drm_gem_create_mmap_offset`, `drm_gem_free_mmap_offset`,
`drm_gem_mmap` or `obj->map_list` access remains in the driver (grep:
only comments). Lock discipline: `mmap_offset_lock` is a leaf; create /
err-undo / free / lookup take it alone. `free_object` may be called with
`struct_mutex` held → order `struct_mutex → mmap_offset_lock`; the mmap
path releases `mmap_offset_lock` (inside the lookup) before taking
`struct_mutex`, so there is no inverse order and no ABBA. The per-mmap
`pr_info` pair (two lines per mmap, once per frame) is gone.

Driver 0001 hunk counts: drv.h 180, gem.c 1807, gem.h 294, ioctl.c 401,
submit.c 2258 (unchanged; no submit.c edit), ioctl.h 52, jm_hw.h 390,
job.c 1955, job.h 438, main.c 760, mmu.c 447, mmu.h 171, power.c 603,
power.h 211, syncobj.c 859, syncobj.h 70, panfrost_drm.h 155,
pf7580_devfreq.c 355, pf7580_devfreq.h 251 — the build-time
`+count` integrity check passes.

## 3. Build

Minimal builder (same apply order as `build-int45.sh` steps 1–6, no
gate suite; the integrator owns gates): shims 0001–0005, kms 0001,
dmabuf 0001, driver 0001+0002 → `make ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j4 Image`.

* `Image-int46fix` 18 209 976 B sha256 `a720b7cd…731d82`,
  `kernel-boot-int46fix.img` 27 348 992 B sha256 `258f3004…`
  (`READY` per `check-readiness.sh`), build log 0 `error:`, 499
  warnings (same ambient count as int41–int45; the only panfrost warning
  is the pre-existing submit.c frame size).
* Measurement variant `Image-int46measure` sha256 `9720dbfd…`,
  `kernel-boot-int46measure.img` sha256 `15bdf8ff…` = the same driver
  plus an uncommitted ktime instrumentation patch (teardown pieces and
  per-call-class timers); it is not shipped and not part of the commit.

Flash note for int46: this userland has **no `/dev/block/mmcblk0pN`
nodes** — the boot partition is `/dev/mmcblk0p10` (259:2, 32 MiB).
`dd` to `/dev/block/mmcblk0p10` (as in `gpu31-runbook.md` §2) creates a
regular file in the 10 MiB devtmpfs instead of touching the partition
(it fails ENOSPC and is harmless). Use `/dev/mmcblk0p10`.

## 4. Silicon: negative control on int45 vs the fixed image

**int45 (old core manager), churn `4 threads × 250 iterations × ≤6 BOs`,
exactly the CREATE_BO + MMAP_BO + mmap + GEM_CLOSE + client-exit mix:**

```
[drm:drm_gem_create_mmap_offset] *ERROR* failed to allocate offset for bo 0
(userspace SIGSEGV; then "pf7580: tiler did not idle on power-off,
aborting suspend"; the box wedged: load 5.8, ps and rootshell blocked,
only a forced sysrq reboot recovered it)
```

The ENOSPC comes from the corrupted `drm_mm` (ranges leaked / tree
relinked by the double insert), not from a real window exhaustion; it is
the object-level signature of the same bug that produced
`dead000000000130` under repeated 300-frame runs (2 of 3 fresh int45
runs died today with a userspace SIGSEGV cascade and `taint D`).

**int46measure (fixed), same churn:** the kernel log contains **no**
`dead0000`, `Unable to handle`, `list_del` or `drm_mm` line; only the
benign `drm_mmap_locked: Could not find map` from the client's
deliberate stale-offset probes (expected: the offset is gone and the
legacy fallback rejects it). **However the churn client process still
dies with SIGSEGV** (audit `sig=11`) part-way through the 1000
iterations — no kernel fault accompanies it. Until that is explained
(§6.1) this stress is **not** a pass, even though the corruption
signature itself is gone.

## 5. fps: per-piece costs and where the 3.5 ms/frame actually is

### 5.1 Per-free-piece cost (int46measure, 100-free windows, ns per BO)

| piece | ns/BO | note |
|---|---|---|
| `p7580_mmu_unmap` loop (leaf clear + per-page `dma_sync_single`) | **71 000** | 288 pages |
| TLB invalidate + PM get/put (`p7580_mmu_invalidate`) | **14 600** | one per BO |
| `p7580_gem_dma_unbind` (clean pass + 272 × `dma_unmap_page`) | **117 000** | dominant |
| `drm_mm_remove_node` (IOVA) | 450 | |
| offset free (hash + mm) | 480 | new manager, same order as core |
| shmem free: 272 × `put_page` + `fput` (inode evict) | 7 100 + 310 | |
| **free_object total** | **213 000** | ~2.7 calls/frame → **0.57 ms/frame** |

The old (int45) free path ran in the same workqueue but only for the
driver's own shadows; the newly executed tail is this 0.57 ms/frame —
real but an order of magnitude below the gap.

### 5.2 Frame-level numbers (int46measure, 256², 300 frames, NOREAD)

| run | fps | client user/sys CPU | GPU IRQs | ERR/TIMEOUT |
|---|---|---|---|---|
| 1 | 93.7 | — | — | 4 |
| 2 | 99.3 | — | — | 0 |
| 3 | 94.5 | — | — | 0 |
| timeline run | 102.0 (2.94 s) | 0.22 s / **1.61 s** | 1200 (4/frame) | 0 |
| 600 frames | 93.0 | — | — | 0 |
| 30 frames | 125.8 | — | — | 0 |

Resolution sweep (same image): 32² 161.5 fps, 128² 109.5, 256² 101.6,
512² 44.8. `serialize_fs=1` A/B: 112.3 / 112.9 / 112.1 (default 90–103
in the same session); documented as harmful in `gpu31-final.md`, so not
a candidate.

### 5.3 What this says

* The free-path teardown is **0.57 ms/frame** (27 % of the ~2.1 ms
  needed just to close the 3.5 ms gap, and it is in a kworker, not the
  render thread).
* The render thread itself burns **5.2 ms/frame in sys** at 256²
  (1.6 s of 2.9 s) and only 0.2 s in user; ~3.6 ms/frame of the wall is
  waiting (fence/GPU). Frame time grows with resolution, so GPU/pixel
  work is part of it, but the fixed per-frame kernel CPU is the larger
  term and is where int41's 155 fps (6.4 ms/frame) can no longer fit.
* Memory is not the limiter during a run (`nr_free_pages` flat, no
  `allocstall`/direct reclaim); `Shmem` grows ~1.3 MB/frame while the
  client runs because Mesa keeps its per-frame BOs until exit (they are
  all released at exit: `Shmem` returns to ~1.2 MB, `pgfree` +437
  pages/frame during the run).
* **Optimizations not yet justified by data:** batching the PTE clears
  is worth ~30–50 µs/BO (the per-page `dma_sync_single` is the cost, not
  the stores); dropping the free-time TLB invalidate (the next bind's
  invalidate covers a recycled range) would save ≤15 µs/BO and one
  PM get/put; deferring `dma_unmap_page` moves 117 µs/BO off the
  free_job path. Together they bound the teardown at ~0.2 ms/frame —
  they cannot reach 150 fps alone, so they are **not implemented yet**;
  the next step is instrumentation of the client's 5.2 ms sys/frame
  (§6.2), not more teardown micro-optimization.

### 5.4 The leading fps hypothesis is BO re-fetch, not teardown (G31_FpsSplit / int46 input)

The fpssplit slice reports that the 0002 dma-resv shim's
`dfence_wait_timeout()` returns `timeout` (0) for an already-signaled
fence, i.e. a 0-jiffy poll on an idle BO. Driver 98640d5 maps a 0-jiffy
return to `-ETIMEDOUT` (gpu31-storm58: “Mesa's BO-cache poll was always
told ready”). If the shim returns 0 for a signaled fence, that mapping
makes **every** Mesa BO-cache poll report "busy", so Mesa never re-fetches
a cached BO and allocates a fresh one every frame. That matches the
measurements above exactly: ~3 fresh CREATE_BO per frame, `Shmem`
+1.3 MB/frame while the client runs, `pgfree` +437 pages/frame, and
5.2 ms/frame of client sys time — the allocation half
(`p7580_shmem_alloc`: 288 × page-alloc + `clear_highpage` +
`__flush_dcache_area`, then offset + IOVA alloc), not the free half
(0.57 ms/frame, measured). The 32² (161 fps, small BOs) vs 256²
(101 fps, 1.15 MB/frame) scaling is consistent with per-byte allocation
cost. This is a hypothesis until the shim fix is measured; it does not
change §5.1–§5.3, and it makes the teardown micro-optimisations even
less relevant.

**Silicon check (int46measure, idle never-submitted 64 KiB BO, raw
UAPI):** `WAIT_BO` with an absolute `timeout_ns` of now (0 jiffies)
returns **-ETIMEDOUT (errno 110)**; the same BO with a +100 ms absolute
stamp returns **0**. So the 0-jiffy poll on an idle BO *does* report
"busy" on the current shims — exactly the call Mesa's BO cache makes
(`panfrost_bo_wait(bo, 0, RW)`), which means no BO is ever re-fetched
from the cache. Test binary: `/data/mmlock/waitbo` (created BO, no
submit, two WAIT_BO calls).

Attribution: **98640d5's mapping was right and the 0002 shim is the
liar.** `dma_resv_wait_timeout()`'s contract is "0 = budget expired, >0 =
signaled"; a signaled fence must return a positive remainder even for a
0-jiffy poll. The shim returns 0 there, so the driver's
0 → `-ETIMEDOUT` mapping faithfully reports what the shim said. The
mapping only became *expensive* once frees became real
(gpu31-boleak/int45): that is the first image where Mesa's BO cache can
actually hold and re-fetch freed BOs, so a poll that always says "busy"
turns the intended reuse into one fresh ~1.15 MB allocation per frame.
This is the int42–45 ambient change the ticket suspected; it is not a
teardown cost and no driver-0001 change can fix it.

## 5.5 int47 silicon (shim fix, driver 04db08d unchanged) — the poll lie is gone, fps is not

`kernel-boot-int47.img` sha256 `80ff0f9e…`, built by G31_Integrator41
(31/31 gates, 5950b7e hunk headers repaired in d2c82ab), flashed and
measured here on a fresh boot (`#37 19:33:43`):

| check | int47 result |
|---|---|
| 3 × 300 f, 256², NOREAD | **107.2 / 108.0 / 104.7 fps** (2.799 / 2.777 / 2.865 s) |
| ERR / TIMEOUT | 0 / 0 / 0 |
| `Shmem:` per run | +56 / +36 / +44 kB (flat) |
| 512² readback ×10 | 13.1 fps, 0 ERR/TIMEOUT |
| 512² readback ×30 | 6.1 fps, 2 ERR/TIMEOUT (the int41 “silent 0x08” storm, unrelated) |
| `WAIT_BO` 0-jiffy poll of an idle BO | **rc=0** (was `-ETIMEDOUT`), +100 ms still rc=0 |
| churn 4 × 250 | SIGSEGV, kernel log clean (`poison=0`) — same userspace item as §6.1 |

So the shim contract is fixed end to end (silicon-verified before/after
on the same raw-UAPI test), but it buys **~5 %** (101 → 105–108 fps), not
the 45 % gap. Qualification of §5.4: the poll lie was real and is now
gone, but either Mesa 20.3 re-fetches BOs rarely enough for it not to
dominate, or the fresh-BO cost is smaller than the `Shmem` growth
suggested. The remaining gap is still the ~5.2 ms/frame client sys time
plus the resolution-scaling GPU term; the per-class instrumentation
(§6.2) is what decides it. The 150 fps target is **not** reached by
either 04db08d or int47.

## 6. Open items

1. **Churn SIGSEGV on the fixed image** (a userspace-open item, kept
   outside the kernel race acceptance). Discriminator run on
   int46measure (same 04db08d driver):
   - deterministic sequential phase (create / mmap / write / munmap /
     GEM_CLOSE / stale mmap) passes for 4 KiB, 64 KiB and 1 MiB: the
     stale offset stops resolving, no fault;
   - **single thread, 250 iterations, stale probes disabled: PASS**
     (`CHURN ok iters=250 threads=1 errs=0`);
   - **4 threads, 250 iterations each: SIGSEGV** (with and without the
     stale probes). Handler-captured fault: a page-aligned address that
     is the base of a BO region the thread had just mmap'd, inside a
     hole between `rw-s … /dev/dri/renderD128` VMAs, i.e. the mapping is
     gone at the moment of the write; kernel log at that point contains
     no oops/poison line and no `MMAP_BO` returned `-ENOENT` (0
     `CHURN FAIL` lines).
   A missing `offset_hash` entry cannot produce this: `.mmap` would
   return `-EINVAL` and the client would skip the write (the fallback
   `drm_mmap()` also fails, it only knows the empty core hash).
   Remaining suspects, in order: the test client's own cross-thread
   mmap/munmap/GEM_CLOSE/fd-close pattern (it is the only thing that
   removes a VMA in the process) and, less likely, a VMA that the kernel
   tears down under a live GEM reference. Next step: same loop with two
   threads sharing one fd and no fd close/reopen, then a run with all
   BOs kept alive to the end (no early munmap).
2. **Where the 5.2 ms/frame sys goes.** Instrumentation patch #2 is
   written (per-class timers for CREATE_BO, `p7580_shmem_alloc`,
   `p7580_drv_map_bo`, SUBMIT, MMAP, WAIT, free_job, `job_done_sync`,
   printed every 100 submits) but not yet built. One measurement boot
   gives the split; only then can kernel-side per-frame work be judged
   avoidable. The 155 fps basis also needs re-checking against int41's
   exact run conditions (int42's report records `serialize_fs=1` for
   that number, which is now known to be harmful; the watchdog report
   records 154–160 fps for plain runs).
3. **fps acceptance (≥150 at 256²) is not reached** by this change
   (and int47's shim fix only reaches 105–108, §5.5):
   clean runs are 94–103 fps, `serialize_fs=1` 112. The teardown is not
   the cause (§5); per §5.4 the likely cause is the 0002 shim's
   0-jiffy signaled-fence return making Mesa re-fetch BOs every frame
   (fix owned by G31_FpsSplit; uncommitted shim edits were in the tree
   during the int46 build, so int46's fps datum must not be quoted
   until a rebuild). If §5.4 holds, the 155 basis is a BO-reuse frame
   and the acceptance target is reachable without any driver change.

## 7. Device state

The phone is running **`int46measure`** (`#37 18:44:21`), not the final
image: the acceptance boot (Integrator41's int46 rebuild of `04db08d`)
has not been done. Artifacts on the external disk:
`Image-int46fix`, `kernel-boot-int46fix.img`, `Image-int46measure`,
`kernel-boot-int46measure.img`, logs under `…/j7k/gpu31/logs/`. On the
phone: `/data/mmlock/{churn,measure.sh,timeline2.sh,res.sh}` and the
`measure`/`dmesg-*`/`mem3`/`tl2`/`res`/`churn` logs used above.
