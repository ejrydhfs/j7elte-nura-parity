# GPU31 DmaBuf — DMA-BUF + CMA + dma_attrs prerequisites for panfrost GEM

Date: 2026-11-19 (UTC). Device: phone **UP** (kernel `3.10.108 #35`, SSH/RNDIS
ok; `/dev/mali0` present, `/dev/dri` absent — running kernel predates M1).
No reboot, no flash, no kernel build performed (Integrator/M1Verify own
those). Shared tree never edited (read-only `-fsyntax-only` checks only).

## Verdict

DMA-BUF core, CMA, and dma_attrs on 3.10.108 are **sufficient for panfrost
GEM import/export with no new core code**: one arm64 adaptation hunk
(0001), one PRIME wiring patch with a review-required real-sharing fix
(0002), one comment-only board-facts patch (0003), plus a 30-check host
contract test (native + aarch64/qemu PASS). Explicit cache/coherency
statement in §5. Review findings (gpu31-review.md §10) are **fixed in this
revision** — see §6.

## 1. What panfrost GEM needs vs what 3.10 has

| Need | Tree state | Patch |
|---|---|---|
| `dma_buf_{attach,detach,get,put,fd}` + `map/unmap_attachment` + `begin/end_cpu_access` + `kmap/kmap_atomic/mmap/vmap/vunmap` | Present (`include/linux/dma-buf.h:72-107`, `drivers/base/dma-buf.c`; no `resv`/`fence` members — those arrive via the G31 drm-backport fence/resv series) | none |
| CMA allocation for GEM/dumb buffers | `CONFIG_DMA_CMA=y` but `CONFIG_CMA_SIZE_MBYTES=0` (shipping defconfig:984) → no default area; `__dma_alloc_coherent` falls through to swiotlb | 0003 documents; 32 MB comes from the display config fragment (not this slice) |
| arm64 `dma_alloc_attrs` + `DMA_ATTR_WRITE_COMBINE` | Present (`arch/arm64/include/asm/dma-mapping.h:97-123`, `include/linux/dma-attrs.h:16`, honoured at `arch/arm64/mm/dma-mapping.c:40-41` → `MT_NORMAL_NC`) | 0001 (in-tree CMA helper references 32-bit-only `dma_alloc_writecombine`, which does not exist on arm64) |
| `DEFINE_DMA_ATTRS` / `dma_set_attr` | Present (`dma-attrs.h:34-56`, `HAVE_DMA_ATTRS` via `arch/arm64/Kconfig:43`) | 0001 uses them |
| PRIME `DRIVER_PRIME` + `prime_handle/fd_to` + `gem_prime_export/import` + 5 low-level hooks | Present in 3.10 DRM core (`include/drm/drmP.h:158,922-941`, `drivers/gpu/drm/drm_prime.c:64-362`); absent on the new `exynos7580_drm_driver` (features = `MODESET\|GEM`, no prime hooks) | 0002 |
| Generic `drm_gem_map_dma_buf` (`get_sg_table` → `dma_map_sg(dev)`) | Present (`drm_prime.c:67-82`); kbase UMM consumes its output via `sg_dma_address/len` (`mali_kbase_mem.c:2204-2244`) | 0002 matches this contract |
| `drm_prime_gem_destroy` (unmap+detach+put) | Present (`drm_prime.c:467-479`); legacy exynos pattern keeps `buf->sgt` + `import_attach` (`exynos_drm_gem.c:651-665`) | 0002 mirrors it (`driver_private` stash) |

## 2. Patches (under `code/gpu31/dmabuf/`, integrator applies)

All verified `git apply --check` PASS in a private clone (display patch as
base for 0002/0003, pristine tree file for 0001). Order: 0001 with any
kernel, 0002 then 0003 on top of the display series (`patches/drm/0001` /
`code/gpu31/series` mirror — same file, same context).

- `0001-drm-gem-cma-use-dma-attrs-writecombine.patch` — in-tree
  `drm_gem_cma_helper.c`: `dma_{alloc,free}_writecombine` →
  `dma_{alloc,free}_attrs` + `DMA_ATTR_WRITE_COMBINE`. Same hunk as the
  proven display-patch adaptation (kept standalone so it applies once
  with or without the display series; applying both is a no-op — apply
  only once). `-fsyntax-only` PASS; only remaining warnings are
  pre-existing `%Zx/%d` debugfs formats also present unpatched, while
  the unpatched file additionally warns implicit-declaration on both
  writecombine calls (i.e. the patch removes real warnings).
- `0002-drm-exynos7580-gem-prime-export.patch` — `DRIVER_PRIME` +
  `prime_handle_to_fd/fd_to_handle` + `gem_prime_export/import` +
  get/import_sg_table + vmap/vunmap + import-aware `gem_free_object`.
  **Review fix applied** (§6).
- `0003-drm-exynos7580-cma-coherency-note.patch` — comment-only: CMA
  32 MB rationale + COHERENCY_NONE statement + memattr R4 pointer.

Out of scope (correctly): `dma_fence/dma_resv/drm_syncobj/drm_sched`
(G31 drm-backport series), GEM ioctl surface (G31 Gem), sysmmu (display
path only — GPU programs its own MMU; `code/gpu31/iommu/` owns that).

## 3. Host tests (`code/gpu31/dmabuf/`, `make check`)

`dmabuf-check.c` (30 checks, native + `qemu-aarch64` PASS) pins every tree
fact the patches rely on: dma-buf ops inventory, attach→
`DMA_BIDIRECTIONAL` map discipline + PAGE_SIZE alignment contract, attr
IDs, WRITE_COMBINE→`MT_NORMAL_NC`, global noncoherent-swiotlb ops,
CMA 32 MB sizing vs 720×1280×4 double scanout, COHERENCY_NONE (31),
memattr `0x8D884D4F48` vs `0x888D88` divergence, TRANSTAB encode incl.
never-`SHARE_OUTER`, and the attachment-backed PRIME contract (no copy,
pinned vmap).

## 4. kbase consumer contract (why 0002 is shaped this way)

kbase UMM import: `dma_buf_get` + `dma_buf_attach(kbdev->dev)` at open
(`mali_kbase_mem_linux.c:940-951`, `*va_pages` from `dma_buf->size`,
`sgt = NULL` until first use), then at first job use
`dma_buf_map_attachment(..., DMA_BIDIRECTIONAL)` (`mali_kbase_mem.c:2189`)
and per-page `sg_dma_address + (j << PAGE_SHIFT)` with
PAGE-aligned/length `WARN_ONCE` guards (`:2214-2235`). The exporter must
therefore return pinned pages through `get_sg_table` and honour
`DMA_BIDIRECTIONAL` — exactly what 0002 provides. kbase ION error path
additionally calls `exynos_ion_sync_dmabuf_for_device` (cached ION only),
which is a no-op for our WC buffers.

## 5. Explicit cache/coherency statement (acceptance requirement)

**This SoC is COHERENCY_NONE as far as the GPU is concerned.**

- `mali_kbase_core_linux.c:3577` forces `kbdev->system_coherency =
  COHERENCY_NONE`; the DT override path (`:3579-3601`) never fires
  because `mali@0x11400000` has no `system-coherency` property
  (`exynos7580.dtsi:1066-1076`). Verified live: coherency mode `0x1f`
  (31 = NONE) in `gpu-verify.md` §4.
- **Page tables**: `kbase_mmu_sync_pgd` (`mali_kbase_mmu.c:78-87`) runs
  `dma_sync_single_for_device` on every update unless ACE. A panfrost
  port MUST use `coherent_walk = false` + explicit `dma_sync` after
  every table write (risk R5); the global arm64 DMA ops are the
  noncoherent swiotlb set (`dma-mapping.c:527`), so map == maintenance.
- **Data (UMM/dma-buf)**: `kbase_sync_single_for_{device,cpu}` skip
  ONLY under ACE (`mali_kbase_cache_policy.c:48-63`); under NONE every
  share goes through `dma_sync`. `DMA_ATTR_SKIP_CPU_SYNC` is honoured
  only in `__swiotlb_map_sg_attrs` (`dma-mapping.c:261`) — the GPU GEM
  path must NOT set it for its own tables. `NO_KERNEL_MAPPING` /
  `NON_CONSISTENT` / `FORCE_CONTIGUOUS` are ignored by arm64's
  `dma-mapping.c` (accepted by the container, honoured nowhere): do
  not copy the exynos `NO_KERNEL_MAPPING`-yet-dereference pattern for
  memory the CPU touches.
- **CPU mapping**: `DMA_ATTR_WRITE_COMBINE` → `pgprot_writecombine` →
  `MT_NORMAL_NC` (non-cacheable, `pgtable.h:303-306`, `memory.h:97`).
  WC scanout buffers therefore owe **no CPU-cache flush** on either
  side; cached imported-ION buffers are the opposite and NEED
  `begin/end_cpu_access` sync.
- **TRANSTAB**: `pgd | TABLE(3) | READ_INNER(4)`; `SHARE_OUTER(0x10)`
  only under ACE (`mali_kbase_mmu_hw_direct.c:260-276) — never set here.
- **MEMATTR (risk R4)**: kbase LPAE `0x8D884D4F48` (indices 0..4 =
  `0x48,0x4F,0x4D,0x88,0x8D`; `mali_midg_regmap.h:506-514`) vs upstream
  `0x888D88`. Default to the kbase table on this silicon; validate with
  a GPU-read test at M1+ before Mesa.

## 6. Review response (gpu31-review.md §10 — NEEDS FIXES → fixed)

- **F1 (import-by-copy fakes sharing) — FIXED.** `import_sg_table` no
  longer allocates a private buffer. It wraps the exporter's
  already-mapped sg (valid `sg_dma_address/len` for the display sysmmu
  IOVA path), stashes the sgt on `driver_private`, and the import-aware
  free hook releases via `drm_prime_gem_destroy(obj, sgt)` + mmap-offset
  + object release + `kfree` (mirrors legacy exynos
  `exynos_drm_gem.c:651-665`). Zero-copy scanout is preserved.
- **F2 (unpinned vmap UAF) — FIXED.** `vmap` takes a GEM ref
  (`drm_gem_object_reference`) and `vunmap` drops it
  (`drm_gem_object_unreference_unlocked`); imported objects (no CPU
  mapping) return `-ENODEV` instead of an unpinned pointer.
  `get_sg_table` additionally refuses imported objects (`-EINVAL`).
- Quoted-Unchecked status is retired for 0002: `git apply --check`
  PASS (both hunks, on the display-series base) + host contract test
  updated (§3 ¶3) + `-fsyntax-only` on the touched CMA helper. Full
  Image build + on-device PRIME smoke (`/dev/dri` + kbase UMM import)
  remain integrator/M1Verify steps (no flash without Main's go).

## 7. Integrator commands (NOT run — owner: G31_Integrator)

```sh
# 0001 (any tree state):
git apply --check code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch
# 0002 + 0003 (on top of the display series):
git apply --check code/gpu31/dmabuf/0002-drm-exynos7580-gem-prime-export.patch
git apply --check code/gpu31/dmabuf/0003-drm-exynos7580-cma-coherency-note.patch
# host contract:
make -C code/gpu31/dmabuf check   # 30 checks native + qemu-aarch64
# on device (after M2 flash gate, Main's go only): /dev/dri/card0 present,
# PRIME fd from a dumb buffer, kbase UMM import + no-op job; expect GPU
# virtual mapping via AS_TRANSTAB, no sysmmu involvement.
```
