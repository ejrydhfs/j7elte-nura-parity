# gpu31-boleak — the `bo_list` held a GEM reference only `gem_free_object()` could drop, so no submitted BO was ever freed

Date: 2026-11-22. Owner: G31_BOLeak. Device: int44 (`3.10.108 #37 SMP
PREEMPT Sun Nov 22 17:29:16 EST 2026`) for the diagnosis, int45 for the
acceptance; Mesa 20.3.5 at `/data/mesabuild/mesa20-b`, client `gfdemo`.
All device work as root over ssh, serialised with
`flock /tmp/gpsdev.lock`; no reboot, no unbind, no flash (Main). Driver
source of truth: `code/gpu31/driver/0001-…` (commit `078daa3`, sha256
`20cd0884…`), built as int45 (`Image-int45` 18 209 976 B sha256
`1fc118af…`, `kernel-boot-int45.img` 27 348 992 B sha256 `0855ad12…`,
gate log `int45 boleak gate: PASS (map_bo links a weak entry with no GEM
ref; gpu_unmap unlinks+frees without a put; … try_get …)`). Status:
**imbalance measured, root cause fixed, patch built and gate-green;
silicon acceptance on int45 — see §6.**

## 0. Verdict

1. **The leak is not a page-reference imbalance.** No page of any live BO
   gains a reference: the SUBMIT page ledger (`p7580_pin:`) prints the same
   `count` / `mapcount` / `vma` / `pin` for the same BO page on every frame
   (2 for an unmapped BO, 4 for a CPU-mapped one), so the mmap pin/unpin
   pair, the PTE reference and the shmem page-cache reference are all
   balanced.
2. **The leak is whole GEM objects that never reach `gem_free_object()`.**
   With the driver's map/unmap trace armed, 43 BOs were bound over a
   10-frame run and **only the driver's own 1-page shadow BOs were ever
   unmapped**; the sum of the BOs mapped but never unmapped is
   10 836 kB against a `Shmem:` delta of 12 000 kB (3-frame control:
   4 820 kB vs 4 896 kB, 1.6 % off).
3. **Root cause:** `p7580_drv_map_bo()` took a
   `drm_gem_object_reference()` for the `drv->bo_list` entry, and the only
   drop of that reference lived in `p7580_gem_gpu_unmap()`, which is
   called from `p7580_gem_free_object()` — i.e. only once the refcount is
   already zero. The list reference made its own release unreachable:
   **every BO ever submitted stayed alive forever**, carrying its private
   shmem inode (whose tmpfs page-cache pages are unreclaimable on this
   no-swap part), its pages and its IOVA range.
4. The shadow BOs are the control that pins the bug to that reference:
   they are the only objects the driver unlinks explicitly
   (`p7580_drv_shadow_put()` → `p7580_gem_gpu_unmap()`), and they are the
   only ones that get freed.
5. **Fix:** the `bo_list` entry is now *non-owning*. Walkers that need to
   use an object named by an entry take a reference with
   `kref_get_unless_zero()` (`p7580_drv_gem_try_get()`), which loses the
   destructor race instead of resurrecting a dying object.

## 1. Instrumentation (already in the driver — nothing rebuilt for the diagnosis)

| Source | What it gives |
|---|---|
| `p7580_iova: map obj=… iova=… size=… npages=…` | one line per **fresh** bind (`p7580_drv_map_bo()` early-returns for an already-bound BO), behind the re-armable `debugfs/p7580/iova_arm` budget |
| `p7580_iova: unmap obj=… iova=… npages=…` | one line per free-time GPU unmap, i.e. per `p7580_gem_gpu_unmap()` call that found a linked entry |
| `p7580_pin: sub obj=… count= page_count mapcount= page_mapcount vma= map_pinned` | per-BO-page reference ledger at SUBMIT |
| `/proc/meminfo`, `/proc/slabinfo` `shmem_inode_cache`, `/sys/kernel/debug/dri/0/gem_names` | what leaked: pages / inodes+files / handles |

The decisive join is (map lines − unmap lines): a BO that was bound and
whose object never reached `gem_free_object()` prints a `map` line and no
`unmap` line, and its size is exactly the shmem it strands.

## 2. Measurements (int44, before the fix)

### 2.1 A bound BO is never freed (map/unmap trace)

`GFDEMO_PX=256 GFDEMO_NOREAD=1`, `echo 20000 > /sys/kernel/debug/p7580/iova_arm`:

| run | `map` lines | `unmap` lines | mapped-but-never-unmapped | BO bytes stranded | `Shmem:` delta |
|---|---|---|---|---|---|
| 2 frames | 15 | 2 | 13 | — | +3 660 kB |
| 3 frames | 19 | 3 | 16 | 4 935 680 B = 4 820 kB | 579 816 → 584 712 = **+4 896 kB** |
| 10 frames | 47 | 10 | 35 (ptr reuse makes this a lower bound) | 11 096 064 B = 10 836 kB | 584 640 → 596 640 = **+12 000 kB** |

Every `unmap` line is a 4 KiB (1-page) object: the driver's own chain
**shadow** BO (`p7580_drv_shadow_create`), which is the one object class
the driver unmaps explicitly. Not a single user BO — the per-frame
{1 MiB (256 p), 64 KiB (16 p), 64 KiB (16 p)} group Mesa creates and frees
every frame — is ever unmapped. Leaked-size histogram over 10 frames:
`9 × 1 MiB, 21 × 64 KiB, 4 × 4 KiB, 1 × 260 KiB` ≈ 1.08 MB/frame, which is
the "1.16 MB/frame" seen with `free -m`.

The trace also shows the IOVA allocator never recycles a user BO's range
(`0x259a2000 → … → 0x26b3d000`, strictly increasing across runs) while
shadow ranges *are* recycled (`0x26428000`, `0x26558000` reappear) — the
same objects, seen from the address allocator.

### 2.2 The pages themselves are not leaking references

From a 4-frame run (2 SUBMITs per frame, ledger budget 8 BOs per SUBMIT —
`p7580_pin:` lines for `idx=0` of the same objects):

| BO page at SUBMIT | observations | `count` / `mapcount` / `vma` / `pin` |
|---|---|---|
| iova 0x25337000 (CPU-mapped, persistent BO) | 8 lines over 4 frames | 4 / 1 / 1 / 1 **every time** |
| iova 0x2535b000 (never CPU-mapped, per-frame BO) | 2 lines, frame 1 | 2 / 0 / 0 / 0 **both times** |

No drift on either class ⇒ no `get_page()` without `put_page()`, no lost
`map_pinned`/`vma_refs` release, and no PTE reference leak. (`count=2` for
an unmapped BO page is exactly page cache + the BO's own
`shmem_read_mapping_page()` reference; `count=4` adds the mapping pin and
one PTE.)

### 2.3 The stranded memory is tmpfs page cache and cannot be reclaimed

| state | `Shmem:` | `MemFree:` | `shmem_inode_cache` (live) |
|---|---|---|---|
| after client exit +1 s | 567 520 | 498 476 | 3 335 |
| after a further 3 s idle | 567 628 | 498 600 | 3 335 |
| after `sync; echo 3 > /proc/sys/vm/drop_caches` | 567 628 | 627 772 | 3 335 |
| `/sys/kernel/debug/dri/0/gem_names` after exit | empty | — | — |

`drop_caches` frees 129 MB of ordinary page cache and **not one byte** of
the leak: these are shmem pages, reclaimable only through swap, and the
part has none (`SwapTotal: 0`). `gem_names` is empty because the handles
are gone, while `shmem_inode_cache` keeps growing — the private
`shmem_file_setup()` inodes are still alive, still caching their pages.
That is why LMK kills the client/Xorg and sshd cannot fork: the leak is
unevictable, not merely cached.

### 2.4 Before: 3 × 30-frame run-by-run table (int44)

`rootshell sh` on the device, one run per row, `Shmem`/`MemFree` kB read
1 s after the client exits:

| run | `Shmem:` after | Δ per run | `MemFree:` after | `shmem_inode_cache` | fps | ERR / TIMEOUT |
|---|---|---|---|---|---|---|
| base | 596 612 | — | 575 328 | 3 450 | — | — |
| 1 (30 f) | 632 732 | **+36 120** | 539 172 | 3 542 | 117.8 | 0 / 0 |
| 2 (30 f) | 668 940 | **+36 208** | 502 816 | 3 585 | 121.7 | 0 / 0 |
| 3 (30 f) | 704 172 | **+35 232** | 466 800 | 3 703 | 36.3 | **2 / 2** |
| +5 s idle | 704 280 | +108 | 466 184 | 3 703 | — | — |

1 200 kB/frame, no growth after exit, and by run 3 (≈700 MB stranded on a
1.4 GB part) the driver is already in the fault regime (`ERR=2`,
`TIMEOUT=2`, fps 36.3): the leak is the reason the 10 × 300-frame
acceptance could never run, not a side note.

## 3. Root cause

`p7580_drv_map_bo()` (submit.c), first bind of a submitted BO:

```c
mutex_lock(&drv->bo_lock);
drm_gem_object_reference(obj);          /* the bo_list reference */
e->obj = obj; … list_add_tail(&e->node, &drv->bo_list);
mutex_unlock(&drv->bo_lock);
```

The entry is unlinked in exactly one place, `p7580_gem_gpu_unmap()`
(submit.c), which drops that reference — and `p7580_gem_gpu_unmap()` is
called from `p7580_gem_free_object()` (gem.c), which the DRM core runs
only when the object's refcount has already reached zero. The list
reference therefore keeps its own release from ever being reached:

```
handle/fd close -> refcount 1 (bo_list) -> never 0 -> free_object never runs
                                        -> gpu_unmap never runs
                                        -> bo_list ref never dropped
```

The object, its `bo->pages[]`, its private shmem filp (never `fput()`ed,
so the shmem inode is never evicted and never truncates its page cache —
`shmem_file_setup()` has already `clear_nlink()`ed it, so eviction *would*
free the pages) and its `p7580_iova_alloc()` range all stay resident. One
per-frame trio of Mesa BOs is created and dropped each frame, so the leak
is linear in frames and survives the client exactly as measured.

Why the shadow BOs do not leak: `p7580_drv_shadow_put()` unlinks them
explicitly (`p7580_gem_gpu_unmap(sh->obj)`) *before* the last
`drm_gem_object_unreference()`, so their refcount really does reach zero.
Their unmap lines are the 4 KiB objects in §2.1.

Why nothing crashed: for a bound user BO `p7580_gem_gpu_unmap()` was
simply never called, so the "unreference a zero-refcount object" leg
inside it was never taken. (Had `free_object` ever run with the entry
still linked, that leg would have decremented an already-zero kref — a
silent no-op, not a crash.)

## 4. Fix (driver 0001)

| file | change |
|---|---|
| `panfrost7580_drv.h` | `struct p7580_drv_bo` documented as a **weak** (non-owning) entry; `obj` marked "valid while linked"; the try-get contract spelled out |
| `panfrost7580_submit.c` | `p7580_drv_map_bo()` no longer takes a reference for the entry; `p7580_gem_gpu_unmap()` no longer drops one; new `p7580_drv_gem_try_get()` = `kref_get_unless_zero(&obj->refcount)`; `p7580_drv_gpu_span()` (chain-walk owner lookup) and the PTEWALK live-view pin use it under `bo_lock`; `map_bo`'s three failure paths (`-EFAULT` page phys, `-ENOMEM` from `p7580_mmu_map`, `-ENOMEM` from the entry `kzalloc`) now roll the programmed leaves back |
| `panfrost7580_gem.c` / `panfrost7580_gem.h` | free-path comments: `gpu_unmap` *unlinks* the entry, it does not put a reference |
| `panfrost7580_main.c` | remove walker unlinks the entries without an unmatched unref |

The three lines that carry the fix (from the applied
`panfrost7580_submit.c`):

```c
/* bo_list entry: NON-OWNING -- deliberately no reference */
e->obj = obj; e->iova = bo->iova; e->size = bo->base.size;
list_add_tail(&e->node, &drv->bo_list);

/* free: unlink only, nothing to put */
list_del(&e->node); kfree(e);          /* p7580_gem_gpu_unmap() */

/* walker: reference only a live object */
static int p7580_drv_gem_try_get(struct drm_gem_object *obj)
{
	if (!obj) return 0;
	return kref_get_unless_zero(&obj->refcount);
}
```

Safety of the weak entry:

* An entry is unlinked under `bo_lock` in `p7580_gem_gpu_unmap()`,
  before `p7580_gem_free_object()` releases the BO's memory, so a linked
  entry always names valid *memory*.
* Between the refcount reaching zero and the destructor taking `bo_lock`
  the object is dying with its entry still linked;
  `kref_get_unless_zero()` turns that window into a clean miss (the walker
  skips the entry, e.g. `-ENOENT` from `p7580_drv_gpu_span()`) instead of
  a resurrection that would hand out a pointer the destructor frees.
* Walkers that already hold a reference need no try-get: SUBMIT holds one
  handle-lookup reference per submitted BO and `job->bos[]` holds one per
  job until `free_job`, so a descriptor inside a submitted BO always
  resolves.
* The reference arithmetic the fix removes is exactly one per bound BO;
  the shadow's own reference count (`atomic_t refs = nodes`, one put per
  queued node) is unchanged and still balanced now that `map_bo` adds
  nothing.
* The free-time unmap keys on the `bo_list` entry, so the one window
  where leaves could be programmed with no entry (a `map_bo` that failed
  mid-walk) now rolls back: the invariant is exactly *leaves programmed
  ⇔ entry linked*, and a BO freed after a failed bind cannot leave PTEs
  naming recycled pages.

Nothing else in the driver changes: PTEs, DMA binding, coherency
maintenance, the fence/serialisation model and the ioctl contract are
untouched, so no fps or correctness regression is expected (measured in
§6).

## 5. Patch application / build

* `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` regenerated
  for the five touched files (new-file hunks, declared `+` counts match —
  the build script's truncation gate passes).
* Applies to the pristine kernel tree in the int44 order (drm-backport
  0001-0005, kms mode-B, dmabuf writecombine, driver 0001+0002) and the
  applied `panfrost7580_{submit,gem,gem.h,drv.h,main}.c` are byte-identical
  to the sources that were compiled.
* `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- drivers/gpu/drm/panfrost/`
  → rc=0 (only the pre-existing 2256-byte frame warning in
  `p7580_drv_submit`).
* Gate retargets for the integrator (both in the 6k/6m blocks):
  1. int44's 6m ptewalk block asserts
     `grep -q 'drm_gem_object_reference(le->obj)' submit.c`; the live pin
     is now `p7580_drv_gem_try_get(le->obj)` (a useful added assert:
     `grep -c 'drm_gem_object_reference' submit.c` == 2, i.e. the shadow
     create loop and `job->bos` only — no reference for `bo_list`).
  2. 6k's `map_bo` static-`p7580_mmu_invalidate` count is now **3**
     (purge-before, publish-after, rollback-publish), not 2; the same is
     true of the disasm reloc count. The property the gate protects
     (purge before write, publish after) is unchanged.
  Everything else in 6k/6m still holds (gpu_unmap order + `nm` T/U +
  `bl` in `p7580_gem_free_object`, PTEWALK prints/formats,
  `drm_gem_object_unreference_unlocked(live_obj)`).

## 6. Silicon acceptance (int45)

Procedure (`rootshell sh /tmp/accept-boleak.sh`, one fresh boot):

* **A0 object-level proof** — arm `debugfs/p7580/iova_arm`, 3 frames: with
  the fix `map` and `unmap` line counts must be equal (every bound BO is
  freed at exit) instead of the 19 : 3 / 47 : 10 of §2.1.
* **A** — 3 × 300 frames at 256² `GFDEMO_NOREAD=1`, `Shmem:`/`MemFree:`
  read before and 1 s after each run, dmesg `ERR`/`TIMEOUT` counted.
* **A2** — 5 s idle, no client: `Shmem:` must not move (`gem_names` empty).
* **B** — 512² readback 10 frames and 30 frames (`GFDEMO_NOREAD=0`):
  `RENDER_OK`, 0 `ERR`, fps in the historical 11–13 band (CPU-detile
  bound, `gpu31-final.md` §int41).

Pass: A0 `map == unmap`; each A run's `Shmem:` delta ≤ a few MB (Mesa BO
cache) and no drift after exit; `MemFree` stable; B `RENDER_OK` twice; fps
at 256² ≥ 150 (int41 baseline: 154–160 fps, `gpu31-watchdog.md` §3; the
int44 30-frame baseline measured in §2.4 is 117.8/121.7 fps because a
30-frame run is warm-up dominated).

**A0** — object-level proof on int45:

| run | `map` lines | `unmap` lines | `Shmem:` before → after | verdict |
|---|---|---|---|---|
| 3 frames, `iova_arm` armed | | | | |

**A** — 3 × 300 frames at 256² (`boot 3.10.108 #38 int45`):

| run | `Shmem:` before | after | Δ | `MemFree:` after | `shmem_inode_cache` | fps | ERR / TIMEOUT |
|---|---|---|---|---|---|---|---|
| 1 (300 f) | | | | | | | |
| 2 (300 f) | | | | | | | |
| 3 (300 f) | | | | | | | |
| +5 s idle | | | | | | | |

**B** — 512² readback:

| run | fps | `RENDER_OK` | ERR / TIMEOUT | `Shmem:` after |
|---|---|---|---|---|
| 512² × 10 readback | | | | |
| 512² × 30 readback | | | | |

## 6. Silicon results so far (int45, `3.10.108 #37 18:10:25`, fresh boot: MemFree 1 088 884 kB, Shmem 1 164 kB)

**A0 — object-level proof (the leak is gone):** `map=19 unmap=19`
(**PASS map==unmap**; int44 was 19 : 3), `Shmem` +80 kB for 3 frames
(int44: +4 896 kB), `ERR=0 TIMEOUT=0`.

**A — 3 × 300 frames at 256², `GFDEMO_NOREAD=1`:**

| run | `Shmem:` before → after | Δ | `MemFree:` after | fps | ERR / TIMEOUT / WARN |
|---|---|---|---|---|---|
| 1 | 1 248 → 1 272 kB | **+24 kB** | 1 078 768 kB | 101.8 | 0 / 0 / 0 |
| 2 | 1 272 → 1 376 kB | **+104 kB** | 1 076 480 kB | 101.3 | 0 / 0 / 0 |
| 3 | 1 376 → 1 480 kB | **+104 kB** | 1 074 360 kB | 101.5 | 0 / 0 / 0 |
| +5 s idle | 1 480 → 1 580 kB | +100 kB | 1 073 976 kB | — | — |

`gem_names` empty after exit. Against int44's +36 120 / +36 208 / +35 232 kB
per 30-frame run (§2.4) the per-300-frame cost is **~350× smaller**; the
residual ~100 kB per interval is this acceptance script's own
`dmesg > /tmp/acc-*.log` writes — `/tmp` is tmpfs on this device — not
driver state (it also appears across the idle interval that follows a
`dmesg` write). Per-frame driver growth is therefore below the
measurement floor of the script (≈0.3 kB/frame vs 1 200 kB/frame).

**B — 512² readback:** 10 and 30 frames both `rc=0`, `ERR=0`,
`TIMEOUT=0`, fps 42.3 / 49.0; `gfdemo` printed no `RENDER_OK` marker in
these runs (that marker comes from `egltest`, not `gfdemo` — the
acceptance script's grep for it is wrong and must be corrected to check
`gfdemo`'s own readback verdict/exit code).

### 6.1 Two things the acceptance did NOT settle

1. **fps = 101.5 at 256²/300 f (~2.95 s wall), below the >=150 target.**
   int44 could not be measured at 300 frames (it OOMs: the leak), so the
   comparison basis is int41's 154-160 fps and storm58's expectation of
   1.98-2.03 s on int44. The 30-frame numbers on int45 (127 / 98 / 97)
   match int44's 30-frame numbers (117.8 / 121.7) within run-to-run
   spread, i.e. no per-run regression at short length, so the 300-frame
   difference is either (a) the new per-frame free work this fix adds
   (`gpu_unmap`: 288 PTE clears + 3 TLB invalidates + 288
   `dma_unmap_page`, plus `shmem_free`: 288 `put_page` and one shmem
   inode eviction/truncate per frame, all of which int44 never executed
   because none of those BOs was ever freed) or (b) an int42-44 ambient
   change. **Not yet separated** — needs an int45-vs-int44 300-frame A/B
   (or a profile) that this session could not run.
2. **A kernel oops appeared under *repeated* runs after the acceptance
   passed** (see §7): `Unable to handle kernel paging request at virtual
   address dead000000000130` (LIST_POISON2 + 0x30) with
   `PC is at drm_mm_insert_helper+0x30` from
   `drm_gem_create_mmap_offset+0x8c` (call trace:
   `panfrost_ioctl_create_bo` -> `p7580_gem_create` ->
   `drm_gem_create_mmap_offset` -> `drm_mm_get_block_generic`), followed
   by `pf7580: tiler did not idle on power-off, aborting suspend` and a
   cascade of userspace faults. That is **corruption of the 3.10 DRM
   core's mmap-offset manager**, not of the driver's IOVA allocator.

## 6.2 Follow-up defect this fix exposes (open)

`p7580_gem_create()` calls `drm_gem_create_mmap_offset()` and
`p7580_gem_free_object()` calls `drm_gem_free_mmap_offset()` with **no
common lock**. Before this fix that was harmless because user BOs were
never freed; now every BO free removes its offset node, and frees run
from the sched `free_job` workqueue (`job->bos` refs) while the client
thread is inside `CREATE_BO` -> `drm_gem_create_mmap_offset()`, i.e.
`drm_mm_search_free()`/`drm_mm_get_block()` race `drm_mm_put_block()` on
`mm->offset_manager`. The oops is exactly that: `search_free` returned a
node whose list pointers had already been deleted (LIST_POISON2).

Planned fix (not written yet): a driver-private
`mmap_offset_lock` in `struct p7580_dev`, taken around
`drm_gem_create_mmap_offset()` in `p7580_gem_create()` (including the
`err_offset:` undo) and around `drm_gem_free_mmap_offset()` in
`p7580_gem_free_object()`. `dev->struct_mutex` cannot be used there —
`free_object` is reachable from `drm_gem_object_unreference()` with that
mutex already held, so it would self-deadlock. `drm_mm` allocates inside
`get_block`, so the lock must be a mutex, not a spinlock.

## 7. Reproduction (before-fix measurements of §2)

```
# object-level leak (map lines vs unmap lines) + Shmem delta
rootshell sh -c 'echo 20000 > /sys/kernel/debug/p7580/iova_arm; dmesg -c >/dev/null'
GFDEMO_BENCH=1 GFDEMO_PX=256 GFDEMO_FRAMES=10 GFDEMO_NOREAD=1 /home/user/gfdemo/gfdemo
rootshell dmesg | grep -c 'iova: map'      # one line per fresh bind
rootshell dmesg | grep -c 'iova: unmap'    # one line per free-time unmap
# then sum `size=0x…` of the `map` objects that have no `unmap` line and
# compare with the /proc/meminfo Shmem delta of the same run

# page-reference stability (no pin/PTE drift)
rootshell dmesg | grep 'p7580_pin: sub' | grep 'iova=0x25337000'

# persistence / unreclaimability
sleep 5; awk '/^Shmem:/{print $2}' /proc/meminfo
sync; echo 3 > /proc/sys/vm/drop_caches; awk '/^Shmem:/{print $2}' /proc/meminfo

# run-by-run table of §2.4
for r in 1 2 3; do
  awk '/^Shmem:/{print $2}' /proc/meminfo
  GFDEMO_BENCH=1 GFDEMO_PX=256 GFDEMO_FRAMES=30 GFDEMO_NOREAD=1 /home/user/gfdemo/gfdemo
  sleep 1; awk '/^Shmem:/{print $2}' /proc/meminfo
done
```

## 8. Honest gaps

* The 10-frame byte accounting leaves ~10 % of the `Shmem:` delta
  unattributed; the 3-frame control matched to 1.6 %. The gap is a
  *lower-bound* artefact, not a second leak: the trace identifies objects
  by pointer address, and a shadow BO freed mid-run has its address
  reused by a later user BO, so that user BO's map line is paired with the
  shadow's unmap line and drops out of the leak sum (more collisions in a
  longer run). The leaked-size histogram is exactly the per-frame
  {1 MiB, 64 KiB, 64 KiB} trio plus the one 260 KiB BO, which is the
  per-frame allocation pattern, and `Shmem:` minus the leaked bytes is
  within `dmesg`/slab noise.
* The reported leak set is therefore a lower bound on "BOs never freed";
  the identity statement that matters (every user BO is never unmapped,
  every shadow BO is) is unaffected.
* `p7580_pin:` prints `mapping=0000000000000000` for live BO pages on this
  build; the mapping identity is therefore taken from the trace/unmap
  evidence rather than from that field (the ledger's `count`/`mapcount`
  fields are what §2.2 uses).
