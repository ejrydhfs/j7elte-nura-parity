# gpu31 driver review — assembled GEM / jobs / MMU + drmcore interfaces

Date: 2026-11-19. Reviewer: G31_DriverReview2.
Scope: `code/gpu31/driver/` **does not exist as a driver** (empty directory,
created 10:07, zero files); reviewed the assembled pieces that will compose
it: `drmcore/` + `gem/` + `jobs/` + `mmu/` + `iommu/` interfaces, plus the
KMS Mode-B core patch and the 0001–0005 shims as context. No device access,
no flash, no build (verification is Main's job). 3.10 API claims checked
read-only against the scratch tree
(`/run/media/.../j7k/gpu31/scratch`, 3.10.108 headers + exynos/drm sources).

Prior reviews incorporated: `gpu31-review.md` (0001 F1/F2/F3), `gpu31-drmcore.md`,
`gpu31-target.md` (v5.2 H1 contract). This report does not re-file those.

## Verdict per file

| File | Verdict | One-line reason |
|---|---|---|
| `code/gpu31/driver/` | **NOT PRESENT** (empty dir) | No `drm_driver`, probe/remove, fops, or ioctl table — lifetime/render-minor unverifiable |
| `drmcore/drm_compat_310.h` | CONDITIONAL PASS | Correct 3.10 shapes; missing `compat_ioctl` (F-DRM1) |
| `drmcore/panfrost_ioctls_310.h` | CONDITIONAL PASS | Numbers/flags match v5.2; duplicate UAPI structs must die at land (F-DRM2) |
| `gem/panfrost_drm.h` | PASS | 6-ioctl v5.2 mirror, spot-checked field-for-field |
| `gem/panfrost7580_gem.h` | NEEDS FIXES | Opaque `resv_lock_pad` coupling (F-GEM1); otherwise sound |
| `gem/panfrost7580_gem.c` | NEEDS FIXES | IOVA manager reuse + node leak (F-GEM2, blocker); `NO_KERNEL_MAPPING` vs retained `cma_vaddr` (F-GEM3); SHMEM double-pin leak (F-GEM4) |
| `gem/panfrost7580_ioctl.c` | NEEDS FIXES | Validate-only SUBMIT returns **0** (F-IO1, must be `-ENOSYS`); absolute `timeout_ns` treated as relative (F-IO2); per-fd state never constructed/torn down (F-IO3) |
| `gem/panfrost7580_ioctl.h` | CONDITIONAL PASS | Signature shape right; lifetime comment promises a `.postclose` that does not exist yet |
| `gem/panfrost7580_devfreq.c/.h` (+ `devfreq/pf7580_devfreq.*` duplicate) | NEEDS FIXES | Two copies will diverge (F-DV1); `dev_set_drvdata` collides with owning DRM driver (F-DV2); OPP leak on init failure (F-DV3) |
| `jobs/panfrost7580_job.c` | NEEDS FIXES | Inverted `jc` align test (F-JOB1); userspace pointers dereferenced in kernel (F-JOB2, crash/infoleak); fence double-put (F-JOB3, UAF); fault fence never signaled (F-JOB4, permanent hang) |
| `jobs/panfrost7580_job.h` | CONDITIONAL PASS | State shape OK; `void *` fence/entity fields hide the F-JOB3 aliasing |
| `jobs/panfrost7580_jm_hw.h` | PASS | Register/config/slot/fault tables cross-checked, no finding |
| `mmu/pf7580_as.h` | CONDITIONAL PASS | Sequences match kbase/panfrost; fault-class names disagree with iommu decoder (F-MMU4) |
| `iommu/panfrost7580_mmu.c/.h` | NEEDS FIXES | Invalidate writes raw addr, no width encoding (F-MMU1, stale TLB); missing `wmb` (F-MMU2); tight MMIO spin (F-MMU3); no teardown walker (F-MMU5) |
| `kms/0001-drm-3.10-render-node-mode-b.patch` | PASS | Alloc-before-load + unwind, put/unplug, always-auth render, MASTER-deny — correct; same `compat_ioctl` note as F-DRM1 |
| `series/0001..0005` shims | (context) | F1/F2/F3 from `gpu31-review.md` still open; job fence hand-init at `job.c:169-183` is pinned to them |

## Blockers (must fix before any `driver/` lands)

### F-GEM2 — IOVA allocator reuses the GEM mmap-offset manager and leaks nodes
`gem/panfrost7580_gem.c:331-347` (`p7580_gem_mm_init`) calls
`drm_mm_init(&mm->offset_manager, P7580_IOVA_START, P7580_IOVA_SIZE)` on
`dev->mm_private`, but `drm_gem_init` (scratch `drivers/gpu/drm/drm_gem.c:91-111`)
already initialised that same `offset_manager` for **mmap fake offsets**
(`DRM_FILE_PAGE_OFFSET_START`). Re-init destroys the offset allocator.
`p7580_gem_iova_alloc` (`:349-367`) then allocates IOVA and mmap offsets from
one pool (collision), and `p7580_gem_iova_free` (`:369-374`) only clears a flag —
the `drm_mm_node` from `drm_mm_get_block` is never stored and `drm_mm_put_block`
is never called. Every CREATE_BO leaks IOVA until ENOSPC; mmap offsets can
alias live IOVAs.
Fix: separate `struct drm_mm` for the IOVA window owned by the driver (not
`mm_private->offset_manager`); store the `struct drm_mm_node` (or at least the
block) in `struct p7580_bo` and `drm_mm_put_block` + `drm_mm_remove_node` in
free. While here, `p7580_gem_mm_init` dereferences `dev->mm_private` without a
NULL/ordering guarantee — call only after `drm_gem_init`.

### F-JOB1 — SUBMIT `jc` alignment test is inverted in the jobs slice
`jobs/panfrost7580_job.c:445`: `if ((args->jc & 7u) == 0u || args->jc == 0u)
return -EINVAL;` rejects **aligned** addresses and accepts unaligned ones.
The GEM-slice stub (`gem/panfrost7580_ioctl.c:226`) has it right
(`!jc || (jc & 7u)` → EINVAL). Fix job.c to `if (!args->jc || (args->jc & 7u))`.

### F-JOB2 — SUBMIT dereferences userspace pointers in kernel
`jobs/panfrost7580_job.c:483` (`in_syncs`) and `:521` (`bo_handles`) cast
`(uint32_t *)(uintptr_t)args->...` and read them directly. The ioctl struct is
kernel-copied but the pointed-to arrays are still userspace. Bad pointer =
oops; also breaks compat32. Fix: `copy_from_user` into the kcalloc'd arrays
(and validate `in_sync_count ≤ 16`, `bo_handle_count ≤ 64` first — already done
at `:450-453`).

### F-JOB3 — job fence double-put (refcount underflow → UAF)
`jobs/panfrost7580_job.c:348-359`: `p7580_job_run` takes **one** ref
(`dma_fence_get`) and aliases it into both `job->done_fence` and
`job->render_done_fence`. `p7580_job_free` (`:373-376`) puts **both**.
Second put drops the count below zero / frees while the other pointer is still
used by WAIT_BO/syncobj paths. Fix: pick one owner (e.g. `done_fence` owns;
`render_done_fence` is a non-owning alias, or take two refs).

### F-JOB4 — fault path never signals the job fence; waiters hang forever
`jobs/panfrost7580_job.c:587-651` (`p7580_job_recover`) ends with
`(void)status; return ret;` — the comment at `:644-645` admits the fence "must
not hang" but no signal happens. IRQ ERR path (`:313-318`) likewise only
clears the sticky bit. Any faulted job leaves `done_fence` pending until the
5 s sched timeout fires (which re-enters recover and still does not signal).
Fix: set an error status on the fence if the 0001 ops support it, else
`dma_fence_signal`, exactly once, under the same ownership as F-JOB3.

### F-MMU1 — invalidate programs a raw address, not a LOCK region
`iommu/panfrost7580_mmu.c:299-302` writes `vpfn << PAGE_SHIFT` straight into
LOCKADDR. The hardware field folds the range width into bits 5:0
(`pf7580_lock_region` in `mmu/pf7580_as.h:111-142` does this; kbase
`lock_region` likewise). Page-aligned addresses have low bits zero, so every
invalidate locks the minimum width regardless of `nr` → multi-page maps leave
stale TLB entries. Fix: reuse `pf7580_lock_region(iova, size)` (or duplicate
its encoding) instead of the raw shift.

## Required (fix before / at `driver/` land)

### F-IO1 — validate-only SUBMIT stub reports success
`gem/panfrost7580_ioctl.c:219-233` returns 0 after validation. If the
integrator wires `PANFROST_IOCTLS_310` as-is, userspace SUBMIT "succeeds" while
nothing executes. Return `-ENOSYS` until the jobs+sched path lands (mirrors
`p7580_job_submit` at `jobs/panfrost7580_job.c:562-569`, which correctly does).

### F-IO2 — WAIT_BO treats absolute `timeout_ns` as relative
`gem/panfrost7580_ioctl.c:203-208` feeds `timeout_ns` straight into
`nsecs_to_jiffies`. v5.2 UAPI (`panfrost_drm.h:81`, mirrored in-tree) says
**absolute** (`CLOCK_MONOTONIC`). An absolute stamp (~1e18 ns) converts to a
gigantic/overflowed budget — effectively infinite. Fix: convert with
`ktime_get` delta (e.g. `drm_timeout_abs_to_jiffies` equivalent) and clamp
negative to 0 (already done for `<= 0`).

### F-IO3 — per-fd state is allocated but never constructed or destroyed
`gem/panfrost7580_ioctl.c:69-90`: `p7580_file_priv_init` kzallocs and stores
`driver_priv` but never initialises `mgr`/`mgr_init`/`job_fctx`;
`p7580_file_priv_fini` kfrees without destroying the syncobj manager (per-handle
fences leak) or freeing sched entities. There is also no `.open`/`.postclose`
wiring anywhere (no driver struct exists). At land: construct the 0003 manager
+ per-slot entities in init (or lazily on first use), tear all of it down in
`postclose`, and never fail-open a half-initialised priv.

### F-GEM1 — `resv_lock_pad` opaque coupling is fragile
`gem/panfrost7580_gem.h:66-71` embeds `void *resv_lock_pad[16]` with layout
`BUILD_BUG_ON`s at `gem.c:57-58,234-238`. It only checks the first three
members; any 0002 growth (seqcount, extra lists) silently overlaps
`iova`/`backing`/etc. At integration (headers staged) include the real
`<linux/dma-resv.h>` and embed `struct dma_resv` directly.

### F-GEM3 — `NO_KERNEL_MAPPING` contradicts the retained `cma_vaddr`
`gem/panfrost7580_gem.c:86-88` sets `DMA_ATTR_NO_KERNEL_MAPPING`, then keeps
`bo->cma_vaddr` and later (a) `sg_set_buf(sgt, bo->cma_vaddr, ...)` at `:505`
and (b) returns it from `p7580_gem_prime_vmap` at `:602-603`. On ARM
(`arch/arm/mm/dma-mapping.c:1538`) the kernel mapping is skipped under this
attr, so the stored vaddr is NULL/garbage for export/vmap. Either drop
`NO_KERNEL_MAPPING` (simplest; CMA path wants the mapping) or stop using
`cma_vaddr` for sg/vmap and build those from the page/phys walk like the fault
path does.

### F-GEM4 — SHMEM pages pinned twice, released once
`gem/panfrost7580_gem.c:227-232` adds a `get_page` per page on top of the
reference `shmem_read_mapping_page` already holds (`:133-144`); `p7580_shmem_free`
(`:167-174`) drops only one. The extra ref survives `fput` of the unlinked
tmpfs file → one leaked page per SHMEM page per BO. Remove the extra pin (the
mapping lifetime already covers fault/`get_sg_table`) or pair it with a second
`put_page`.

### F-DRM1 — fops macros lack `compat_ioctl`
`drmcore/drm_compat_310.h:79-102`: both `DEFINE_DRM_GEM_FOPS` variants omit
`.compat_ioctl = drm_compat_ioctl` (cf. exynos
`drivers/gpu/drm/exynos/exynos_drm_drv.c:249-260`, which sets it under
`CONFIG_COMPAT`). 32-bit Mesa on this 64-bit kernel gets ENOTTY on all six
ioctls. Add the guarded entry to the `_COMPAT` variant (and use that variant
for the driver); keep the plain one only for documentation.

### F-DRM2 — two competing UAPI struct definitions
`drmcore/panfrost_ioctls_310.h:74-114` carries minimal `_310` struct mirrors
"to be dropped" when `gem/panfrost_drm.h` wins. Both headers define the same
ioctl numbers with different type names; including both in one TU (likely at
`driver/` land: ioctl table + handlers) collides conceptually if not
textually. Decide now: `gem/panfrost_drm.h` is canonical; `_310` structs become
`#ifdef`-guarded fallback or are deleted at land. Also note the SUBMIT flag row
(`:126`, `DRM_RENDER_ALLOW | DRM_AUTH`) is consistent with Mode-B (render always
authenticated, MASTER denied on render) — no change needed there.

### F-DV1/DV2/DV3 — devfreq duplication and collisions
Two copies exist: `devfreq/pf7580_devfreq.*` vs `gem/panfrost7580_devfreq.*`
(content differs — boot Hz, locking model). Land exactly one. Regardless of
which: (DV2) `gem/panfrost7580_devfreq.c:44,103,120,166` uses
`dev_{get,set}_drvdata(dev)` on the DRM device, whose drvdata belongs to the
owning driver — overwrites/corrupts the remove path. Use a file-static handle
(the `devfreq/` copy's kbase-style approach) or container-of from devfreq.
(DV3) `p7580_devfreq_init` (`:139-177`): any `opp_add`/`opp_enable` failure
after the first row returns without `opp_disable`/cleanup of earlier rows;
`devfreq_remove_device` is only reachable via fini on the success path.
Unwind on error. (Also: the profile at `:34-40` is `static` shared; a second
init corrupts `freq_table`/`max_state` — embed per-instance like kbase.)

### F-MMU2/3/4/5 — iommu follow-ups
- F-MMU2: `p7580_mmu_program_as` (`iommu/panfrost7580_mmu.c:251-270`) lacks the
  `wmb()` the `pf7580_as.h:191` twin has. Add it (table/memattr before UPDATE
  on COHERENCY_NONE).
- F-MMU3: `p7580_as_wait_ready` (`:275-283`) is a tight 100k `readl` loop with
  no `cpu_relax`/`udelay`; the `pf7580_as.h:143-159` twin polls 10 µs-stepped.
  Copy the stepped shape.
- F-MMU4: fault decoders disagree: `iommu/...:313-330` maps `0x2<<3`→
  TRANSTAB_BUS / `0x3<<3`→ACCESS_FLAG, while `mmu/pf7580_as.h:256-268` maps
  `0xD0-0xD7`→TRANSTAB_BUS / `0xD8`→ACCESS_FLAG (LPAE names) and `:270-282`
  different access-type words. One of them mislabels silicon faults. Reconcile
  against KT `mali_midg_regmap.h:341-353` and keep a single decoder.
- F-MMU5: `p7580_mmu_unmap` never frees child tables by design (header `:125-127`
  says caller must via `p7580_mmu_free_table`), but no context-teardown walker
  exists. `driver/` land must add the recursive unmap+free walk wired to
  file-close/remove; otherwise every process exit leaks page tables.

### F-JOB5 — slot programming has a check-then-act race
`jobs/panfrost7580_job.c:257` reads `JS_COMMAND_NEXT` before taking `hw_lock`;
two file contexts submitting to the same slot can both pass the `WARN_ON` and
then serialise with the second overwriting the first's HEAD/AFFINITY. Move the
busy/in-flight check inside `hw_lock` (and consult `in_flight[slot]`).

### F-JOB6 — recovery spins the workqueue
`jobs/panfrost7580_job.c:611-633`: multi-second `cpu_relax` polls for slot
drain and RESET_COMPLETED. In process context — add bounded `udelay`/`msleep`
and `cond_resched`, and escalate to HARD reset exactly as written (that part is
right).

## Correct as verified (no action)

- ioctl flags: SUBMIT keeps `DRM_AUTH`, all six carry `DRM_RENDER_ALLOW→DRM_UNLOCKED`
  (`panfrost_ioctls_310.h:125-131`), consistent with Mode-B (render fd is
  always-authenticated, `DRM_MASTER` denied on render in the KMS patch
  `drm_drv.c` hunk). No new RENDER bit needed.
- `MMAP_BO` offset encoding (`ioctl.c:162`) matches the 3.10 `hash.key <<
  PAGE_SHIFT` scheme (`drm_gem.c:354-380`); `drm_gem_mmap` lookup is by
  `vm_pgoff`, so byte-offset userspace works.
- `drm_gem_object_lookup` needs no `struct_mutex` (table_lock only,
  `drm_gem.c:391-411`); the handlers' habit of holding `struct_mutex` across
  lookup is harmless and matches exynos. `unreference` (not `_unlocked`) while
  holding `struct_mutex` in `mmap_bo`/`get_bo_offset` is the *correct* pairing
  for 3.10's `BUG_ON(!mutex_is_locked)` in `drm_gem_object_free` — keep it, and
  extend the same discipline to the prime `vmap`/`vunmap` put path (currently
  `vunmap` at `gem.c:613-620` can drop the last ref without the mutex held).
- PRIME import shape (`gem.c:628-676`) mirrors `drm_gem_prime_import`
  (`drm_prime.c:254-307`): attach → `get_dma_buf` → map → import_sg_table, and
  free (`:297-306`) mirrors the fail-path unwind. No import-by-copy. (The
  `imported_dmabuf` field is dead weight — set nowhere — remove or use.)
- `p7580_gem_prime_get_sg_table` for CMA (`:497-507`) plus `drm_gem_map_dma_buf`
  (`drm_prime.c:67-82`) double-map is the stock shape; fine.
- `p7580_gem_mmap` (`:437-462`) sets `VM_MIXEDMAP`, drops `VM_PFNMAP`, picks
  prot from `cache_attr`, and installs `vm_ops` after `drm_gem_mmap` — matches
  `exynos_drm_gem_mmap`/`drm_gem_mmap` (`drm_gem.c:658-…`: sets `VM_IO|
  VM_PFNMAP…`, requires driver `gem_vm_ops`, takes a mapping ref). Fault
  handler locking under `struct_mutex` matches `exynos_drm_gem_fault`.
- `DEFINE_DMA_ATTRS` + `WRITE_COMBINE` + `FORCE_CONTIGUOUS` exist in 3.10
  (`dma-attrs.h:16-20`); `shmem_file_setup`/`shmem_read_mapping_page`,
  `vm_insert_mixed`, `pgprot_writecombine/noncached`, `drm_prime_pages_to_sg` /
  `drm_prime_sg_to_page_addr_arrays` (returns 0/−1 — the `if (...)` check at
  `gem.c:539` is fine), `drm_gem_prime_export`, `get_dma_buf`,
  `DRM_IOCTL_DEF_DRV`/`DRM_COMMAND_BASE`, `gem_free_object`/`gem_vm_ops`/
  prime hook slots in `struct drm_driver` — all present as used.
- Missing-but-correctly-absent: `dma_fence_init` (shim 0001 gap, worked around
  with hand-init + comment at `job.c:169-183` — remove the workaround when
  0001 F1/F2 lands); `drm_dev_alloc/register` (legacy `drm_platform_init`
  path is right for 3.10); atomic helpers (render-only, unneeded); sysmmu/IOMMU
  attach (Mali MMU owns the walk — agreed).

## Architectural gap: nothing maps BOs into the GPU tables yet
GEM allocates an IOVA per BO at create; the MMU slice maps single pages by
`(iova, paddr)`; the SUBMIT slice validates `jc` via a `jc_validate` hook but
never pins/reserves/maps the `bo_handles` set into the address space before
`p7580_job_hw_submit`. First real submit will fault. `driver/` land must define
the missing link: SUBMIT pins each BO (resv lock discipline per the 0002
plain-mutex), `dma_map`/MMU-map its pages at the BO's IOVA, invalidates
(FLUSH_PT via the fixed F-MMU1 path), programs AS_TRANSTAB once per context,
and unmaps/unpins on free/teardown — with the HW-lock/reset-lock ordering from
`job.c:28-34` held.

## `driver/` landing checklist (integrator)
1. Fix blockers F-GEM2, F-JOB1–4, F-MMU1 first; required F-IO1–3, F-GEM1/3/4,
   F-DRM1/2, F-DV1–3, F-MMU2–5, F-JOB5/6 with them.
2. Single `struct drm_driver`: `.ioctls = PANFROST_IOCTLS_310` (+`num_ioctls`),
   `.fops` (GEM fops with `compat_ioctl`), `.gem_free_object =
   p7580_gem_free_object`, `.gem_vm_ops`, prime hooks, `.open`/`.postclose`
   (init/fini file priv), `.load`/`.unload` around `drm_platform_init/exit`,
   IRQ install/free around `p7580_job_dev_init/fini`, `dev_pm_ops` from the
   power slice, `DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER` (no MODESET).
3. Probe/unplug/remove must release in reverse: mask IRQs, idle-check, free
   jobs/sched, devfreq fini, MMU teardown walk (F-MMU5), GEM mm fini
   (post-F-GEM2 split), `drm_platform_exit`. Never leak `driver_priv`,
   syncobj managers, IOVA nodes, or page tables across close/remove.
4. Keep UAPI byte-identical (H1): 6 ioctls, `GPU_PROD_ID`-only GET_PARAM,
   zeroed CREATE_BO flags, SUBMIT ending at `requirements`. Re-run the
   client selftest (`code/gpu31/client/pf7580-selftest.c`) against the landed
   table.
