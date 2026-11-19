# GPU31 GEM — panfrost GEM + devfreq hooks for the T720 port

Date: 2026-11-19. Slice: G31_Gem. Target: `code/gpu31/gem/` (+ `code/gpu31/drmcore/`,
`code/gpu31/dmabuf/` context; those two dirs are NOT edited here).
Device at check time: phone UP per Main (kernel `3.10.108 #35`,
`/dev/mali0` + `/dev/fb0` present, `/dev/dri` absent — running kernel
predates any panfrost driver). No reboot, no flash, no kernel build,
shared tree never edited (read-only `-fsyntax-only` + `-c` checks only).

Related: `docs/reports/gpu31-target.md` (v5.2 H1 UAPI lock),
`docs/reports/gpu31-drmcore.md` (compat fops/ioctls), `docs/reports/gpu31-dmabuf.md`
(PRIME prerequisites), `docs/reports/gpu31-driver-review.md` (this report answers
every F-GEM finding + F-IO2 + F-DV1/2/3), `docs/reports/gpu31-arch.md` §3.1
(BO model contract).

Prior state: the slice existed as a partial draft (CMA/SHMEM alloc, ioctls,
devfreq sketch). The review found four GEM defects (one blocker) plus ioctl
and devfreq defects. All are fixed in this revision — see §6.

## Scope

Finish panfrost GEM for 3.10 in `code/gpu31/gem/` (7 files):

- CMA BO alloc/import/export/mmap (WRITE_COMBINE via dma_attrs) + SHMEM
  path (private page array, no shmem helper in 3.10) + attachment-backed
  PRIME (never import-by-copy) + vmap refcount + free hooks + devfreq hooks.
- Compile `-fsyntax-only` AND `-c` vs staged headers; host unit test for
  offset/size validation.

## Delivered files

| File | Purpose |
|---|---|
| `code/gpu31/gem/panfrost_drm.h` | v5.2 UAPI mirror, byte-identical structs/ioctls (0x00–0x05); single-line adaptation: `<drm/drm.h>` for the 3.10 uapi layout (abi-fuzz repair folded in: `*/` terminator present) |
| `code/gpu31/gem/panfrost7580_gem.h` | BO model + per-device IOVA owner + mapping-hook prototypes |
| `code/gpu31/gem/panfrost7580_gem.c` | alloc/import/export/mmap/wait/fault + mapping-hook bodies |
| `code/gpu31/gem/panfrost7580_ioctl.h` | six handler prototypes + file-priv init/fini |
| `code/gpu31/gem/panfrost7580_ioctl.c` | six ioctl bodies: validate + dispatch (SUBMIT), absolute WAIT_BO, offset/mmap |
| `code/gpu31/gem/panfrost7580_devfreq.{h,c}` | GEM-owned DVFS hooks (sole devfreq impl for the driver) |
| `code/gpu31/gem/Makefile` | `make check` (host test) + `stage-shims`/`kcheck`/`kobj` (kernel checks, read-only tree) |
| `code/gpu31/gem/gem_validate.c` | host unit test (offset/size validation, native + aarch64 PASS) |

`code/gpu31/drmcore/` and `code/gpu31/ddmabuf/` are unchanged (context only).

## 1. BO model (mirrors v5.2 `panfrost_gem_object`, adapted to 3.10)

- Embedded `struct drm_gem_object` (handle + mmap-offset machinery);
  backing selector `P7580_BO_CMA/SHMEM` (private, never on the UAPI wire;
  `CREATE_BO.flags` nonzero → `-EINVAL`).
- CMA: `drm_gem_object_init` + `dma_alloc_attrs` WRITE_COMBINE (plain WC
  only — the arm64 backend honours exactly this attr; `NO_KERNEL_MAPPING`
  is silently ignored and `FORCE_CONTIGUOUS` belongs to a later DMA API,
  so neither is set; F-GEM3).
- SHMEM: `drm_gem_private_object_init` + private `shmem_file_setup` +
  `shmem_read_mapping_page` per page (same pattern `drm_gem_object_init`
  uses). Single-ref ownership: NO extra `get_page()` (F-GEM4 — the old
  second pin leaked one page per SHMEM page per BO).
- IOVA: per-device `struct p7580_dev` (`dev_private`) owns a `struct drm_mm`
  over `[32 MiB, 4 GiB)` + `iova_lock`; each BO carries an embedded
  `struct drm_mm_node` inserted at create/import (`drm_mm_insert_node`,
  the canonical 3.10 search+insert pair) and removed in free
  (`drm_mm_remove_node` returns the range — no leak, no aliasing with
  the GEM mmap-offset manager, which lives in `dev->mm_private` and is
  never touched here; F-GEM2 blocker).
- Reservation: embedded `struct dma_resv` (real 0002 type via
  `<linux/dma-resv.h>`, NOT an opaque pad; F-GEM1). The 0001/0002 series
  is a build prerequisite (Makefile KINC stages them).

## 2. Mmap / fault / wait

- `p7580_gem_mmap`: `drm_gem_mmap` (takes the fault-grab ref) → drop
  `VM_PFNMAP`, set `VM_MIXEDMAP`, WC/cached prot from `cache_attr`
  (exynos `update_vm_cache_attr` shape), install `p7580_gem_vm_ops`.
  KMS dumb mmap keeps its own entry point in the KMS slice.
- Fault: exynos `map_buf` walk (sg lengths for imports, `__phys_to_pfn`
  + page offset) via `vm_insert_mixed` under `struct_mutex`; error map
  is exynos `convert_to_vm_err_msg` verbatim incl. `-EAGAIN → NOPAGE`
  (shmem reclaim transient).
- WAIT_BO: `dma_resv_wait_timeout` on the embedded resv (0002 shim locks
  internally). Timeout arrives in jiffies; the ioctl converts the v5.2
  ABSOLUTE `timeout_ns` (CLOCK_MONOTONIC) via `ktime_get_ts` delta, past/zero
  stays non-blocking, overflow saturates at LONG_MAX (F-IO2). Negative
  `wait_all` semantics unchanged (1 = all).

## 3. PRIME (attachment-backed, never import-by-copy)

- Export: stock `drm_gem_prime_export` (dma_buf ops + get_sg_table →
  `dma_map_sg` in the core's map path).
- `get_sg_table`: fresh table per call (core frees after map; caching
  would double-free). CMA single entry from `cma_vaddr`; local SHMEM via
  `drm_prime_pages_to_sg`; re-export of an imported BO hands out the
  live attachment mapping (exynos dmabuf shape).
- `import_sg_table`: stores the exporter's LIVE mapping pointer (core
  `drm_prime.c:500` passes exactly the `dma_buf_map_attachment` return;
  free hands it to `drm_prime_gem_destroy` — unmap + detach + put). No
  page allocation, no copy (review F1). IOVA via the same per-device
  allocator (F-GEM2 unified).
- `import` (PRIME_FD_TO_HANDLE): attach → `get_dma_buf` → map →
  `import_sg_table`; self-import (own export re-imported on the same
  dev) bumps the GEM ref instead of wrapping (core fast-path shape).
- `vmap`/`vunmap`: one GEM ref per live CPU mapping (review F2),
  `P7580_VMAP_MAX` slots; foreign pointers rejected (`WARN_ON`).

## 4. Ioctls (six v5.2 handlers)

- GET_PARAM: `GPU_PROD_ID`-only (`0x720`), nonzero pad → `-EINVAL`.
- CREATE_BO: `P7580_BO_CMA` + WC, handle create (handle owns the ref),
  `args->offset = bo->iova` (nonzero guaranteed).
- MMAP_BO: fake offset `map_list.hash.key << PAGE_SHIFT` (cma_dumb_map
  encoding); lookup takes its own ref (no outer struct_mutex needed).
- GET_BO_OFFSET: same IOVA value as CREATE returned on this fd.
- WAIT_BO: F-IO2 absolute conversion (§2).
- SUBMIT: validates (`jc` nonzero + 8-byte aligned, requirements ⊆ FS,
  counts ≤ 64/16, file priv + job dev/ctx present) then stages the u32
  user arrays with `copy_from_user` into stack buffers and dispatches
  to the jobs core `p7580_job_submit` (canonical `struct p7580_submit`
  layout mirrored locally so the file `-c` compiles without the jobs
  tree). No direct user dereference. Per-fd state: 0003 syncobj manager
  constructed in `file_priv_init`, destroyed in `file_priv_fini` (jobs
  entities owned/freed by the jobs slice's own postclose leg; F-IO3
  wired at the struct level — the `.open`/`.postclose` driver hooks
  land with `driver/`).
- The `validate-only returns 0` stub is gone: SUBMIT either dispatches
  or fails (`-EINVAL`/`-EFAULT`/`-ENODEV`).

## 5. Devfreq hooks (sole impl for the driver, F-DV1/2/3)

- OPP ladder: 8 stock rows ascending (160–800 MHz, 900 mV–1.1 V, from
  `gpu_exynos7580.c`), boot 350 MHz @ 1.00 V, `simple_ondemand` + 100 ms
  poll (kbase parity), per-instance profile + freq table (no shared static).
- Lookup via file-static single-G3D handle + `dev == dvfs->dev` check
  (never `dev_get/set_drvdata` — the owning DRM driver's drvdata belongs
  to the driver; F-DV2).
- `target()` forwards devfreq flags to `devfreq_recommended_opp` (kbase
  shape: plain polls ceil up, max_freq clamps resolve down), then steps
  voltage+clock in GPU_SET_CLK_VOL order (up: V→f, down: f→V) via
  `pf7580_set_voltage/pf7580_set_rate`; `-EAGAIN` propagates.
- Init unwinds OPPs in reverse on failure (`opp_disable`, F-DV3); fini
  disables all rows + removes devfreq + clears the handle.
- Suspend/resume legs wrap `devfreq_suspend/resume_device` (NULL-safe 0).
- The jobs slice feeds load via `p7580_devfreq_set_busy` per retired job.
- Dedup note: the standalone `devfreq/` slice's pure-C ladder/ceil helpers
  + host test stay the canonical host-test copy (arch §4 agreement); the
  ladder VALUES are shared by design, no code is duplicated into this tree.

## 6. Review disposition (`gpu31-driver-review.md`)

| Finding | Verdict | Fix in this revision |
|---|---|---|
| F-GEM1 (resv pad coupling) | FIXED | `p7580_bo.resv` is `struct dma_resv` via `<linux/dma-resv.h>`; BUILD_BUG_ON pad checks deleted; Makefile stages 0001–0003 first |
| F-GEM2 (IOVA/mmap-manager collision + node leak, BLOCKER) | FIXED | driver-private `struct p7580_dev` (`iova_mm` + `iova_lock`, `dev_private`); `drm_mm_insert_node`/`drm_mm_remove_node` on the embedded node; create + import share the path; `p7580_gem_mm_init/fini` deleted |
| F-GEM3 (`NO_KERNEL_MAPPING` vs `cma_vaddr`) | FIXED | attrs are WRITE_COMBINE only; `cma_vaddr` retained for sg/vmap (the attr that keeps it is the one set) |
| F-GEM4 (SHMEM double-pin) | FIXED | extra `get_page()` loop deleted in create; single-ref ownership documented at alloc + free |
| F-IO2 (absolute timeout) | FIXED | `ktime_get_ts` delta, ≤0 → 0, saturate LONG_MAX |
| F-DV1 (two copies) | FIXED | this tree is the sole driver impl; standalone slice keeps only the pure-C host-test helpers (no kernel .c duplication) |
| F-DV2 (drvdata collision) | FIXED | file-static `p7580_dvfs_g` + dev check; no drvdata touch |
| F-DV3 (OPP leak) | FIXED | reverse `opp_disable` unwind on init failure; fini disables all rows |
| F-IO1 (SUBMIT returns 0) | FIXED (sibling) | SUBMIT now dispatches to `p7580_job_submit` or fails; no validate-only success path |
| F-IO3 (per-fd construct/teardown) | FIXED (struct level) | manager init/fini in file_priv init/fini; jobs ctx owned by jobs slice; `.open`/`.postclose` land with `driver/` |

Out of scope here (owned elsewhere, stated for the assembler): F-JOB1–6
(jobs slice), F-MMU1–5 (iommu/mm slices), F-DRM1/2 (drmcore slice).

## 7. Mapping hook for DriverAssemble (architectural gap in the review)

GEM allocates an IOVA per BO but never programs the Mali page tables.
The owning driver maps BOs with these header-declared helpers
(`panfrost7580_gem.h`, bodies in the `.c`):

- `p7580_bo_npages(obj)` — GPU pages covered by a BO (`size >> 12`).
- `p7580_bo_page_phys(obj, idx, &ok)` — backing phys per page index:
  CMA `cma_paddr + idx*4K`; SHMEM `page_to_phys(pages[idx])`; imported
  sg-walk (`sg_phys + offset` over `bo->sgt`); `ok=false` when out of range.

Driver sequence per SUBMIT BO (review checklist): resv-lock + pin each
`bo_handles` entry → per page `p7580_mmu_map(mmu, iova + idx*4K, phys,
P7580_PROT_RD|WR, memattr)` (`code/gpu31/iommu/panfrost7580_mmu.h`) →
`p7580_mmu_invalidate(regs, as_nr, iova>>12, npages, sync=0)` (FLUSH_PT via
the fixed F-MMU1 LOCK-region encoding, `pf7580_lock_region` in
`code/gpu31/mmu/pf7580_as.h`) → `p7580_mmu_program_as(regs, as_nr, pgd_dma,
memattr_lo/hi)` once per context. Unmap per page + invalidate before free
(F-MMU5 teardown walk); HW-lock/reset-lock ordering per `job.c:28-34`;
fault recovery signals `done_fence` (F-JOB4). Until the driver lands this
link, first real submit would fault — stated, not hidden.

## 8. Compile evidence (acceptance)

Read-only checks against the real 3.10.108 headers (`-nostdinc`,
`arch/arm64/include` + `include`, `__KERNEL__`, kconfig include),
`aarch64-linux-gnu-gcc 16.2.0`, 0001–0003 headers staged privately:

- `-fsyntax-only -Wall -Werror` (pagemap `fault_in_multipages` host-gcc
  `unused-but-set-variable` warning filtered — pre-existing tree header,
  not this code): **all 3 PASS, zero warnings**.
- `-c` objects: `panfrost7580_gem.o` 24752 B, `panfrost7580_ioctl.o`
  7072 B, `panfrost7580_devfreq.o` 8208 B — **all PASS, zero warnings**.
- Host test `gem_validate` (offset/size validation): **native + aarch64
  (qemu) PASS** (“all checks passed”; exit 0 both).
- `make check` runs the host test; `make kcheck`/`make kobj` run the
  kernel checks (needs `make stage-shims` first for the private 0001–0003
  staging; the shared tree is never touched).

## 9. Honest gaps (NOT done)

- No `driver/` struct yet (no `drm_driver`, probe/remove, fops table,
  `.open`/`.postclose` wiring, IRQ install, `dev_pm_ops`): Integrator /
  DriverAssemble own the landing checklist (review §`driver/` items 1–4).
- No GPU map/unmap callsites (hook provided, §7 — the driver wires it).
- No full kernel `Image` link, no boot test, no GPU-read test.
