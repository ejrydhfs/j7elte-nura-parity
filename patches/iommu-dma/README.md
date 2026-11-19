# IOMMU / DMA / CMA analysis for j7elte (Exynos 7580, Nura 3.10.108)

Scope: read-only analysis of the kernel tree, the `patches/drm/` display stack,
and the shipping Mali kbase driver. No shared files were modified; the only new
file besides this README is the optional PRIME-export patch below.

The three questions from the ticket (a/b/c), decisions first, evidence after.

---

## Decisions

### (a) Can a panfrost-class GEM layer use `drm_gem_cma_helper` + `dma_alloc_attrs(WRITE_COMBINE)` on this arm64 tree?

**Yes — with no new adaptation needed. The 0001 display patch already did the
adaptation, and it is sufficient for both the display driver and any future
render-only GEM user on the same DRM device.**

- The stock `drivers/gpu/drm/drm_gem_cma_helper.c:61-62,38-39` calls
  `dma_alloc_writecombine()` / `dma_free_writecombine()`, which do not exist on
  arm64 in this tree (arm32-only wrappers; `grep -rn dma_alloc_writecombine
  arch/arm64/ include/` returns nothing outside a stray comment in
  `arch/arm64/mach-exynos/memory_compressor.c:491`).
- The vendor arm64 DMA API is the attribute-based one:
  `arch/arm64/include/asm/dma-mapping.h:97-123` defines `dma_alloc_attrs()` /
  `dma_free_attrs()` on top of `get_dma_ops(dev)->alloc/free`, with
  `dma_alloc_coherent(d,s,h,f) = dma_alloc_attrs(d,s,h,f,NULL)` as a macro
  (`:94-95`). `DMA_ATTR_WRITE_COMBINE` is declared in
  `include/linux/dma-attrs.h:16`, and arm64 honours it in `__get_dma_pgprot`
  (`arch/arm64/mm/dma-mapping.c:40-41` → `pgprot_writecombine`, which arm64
  defines at `arch/arm64/include/asm/pgtable.h:303`).
- `patches/drm/0001-...patch:31-66` already adapts the CMA helper to
  `DEFINE_DMA_ATTRS` + `dma_set_attr(DMA_ATTR_WRITE_COMBINE)` +
  `dma_alloc_attrs(GFP_KERNEL | __GFP_NOWARN)` / `dma_free_attrs`, with a comment
  recording exactly the arm64 rationale. Any panfrost-class GEM code that
  allocates through `drm_gem_cma_create()` therefore gets write-combined CMA
  memory in exactly the same way the display path does. No further patch to
  `drm_gem_cma_helper.c` is required — and one is prohibited anyway (the 0001
  patch is owned by the integrator).
- The CPU mmap path is also write-combined without extra work: the 3.10 GEM core
  sets `vma->vm_page_prot = pgprot_writecombine(...)` in `drm_gem_mmap()`
  (`drivers/gpu/drm/drm_gem.c:700`), and `drivers/gpu/drm/drm_gem_cma_helper.c:234`
  does `remap_pfn_range(... cma_obj->paddr ...)` under that prot.

**Checked on this tree** (compile-level reasoning only, no build was run per the
no-full-build rule): the adapted symbols (`dma_alloc_attrs`, `dma_free_attrs`,
`DEFINE_DMA_ATTRS`, `DMA_ATTR_WRITE_COMBINE`) all resolve in-tree as cited
above; the adapted helper is the same file the new `exynos7580_drm.c` binds to
(`.mmap = drm_gem_cma_mmap`, `.gem_vm_ops = &drm_gem_cma_vm_ops`,
`.dumb_create = drm_gem_cma_dumb_create` at 0001 patch lines 1652/1663-1666).

### (b) Does a GPU-side driver need the Exynos system MMU (sysmmu) at all?

**No — for the GPU. The display path needs it; the GPU has its own MMU and
must program it itself. Owner of `AS_TRANSTAB` is whichever GPU driver owns the
block: kbase today, panfrost after a cutover; never both, never the sysmmu.**

- DT: `arch/arm64/boot/dts/exynos7580.dtsi:1066-1075` (`mali@0x11400000`) has
  `reg/interrupts/interrupt-names/clocks/samsumg,power-domain` and **no**
  `iommus`/`iommu` property. The four `iommu-domain_*` buses declared in the
  same file cover `fimc-is` (`:1455`), `decon_0` (`:1535`), `scaler/jpeg`
  (`:1560`) and `mfc` (`:1587`) — there is no GPU/IOMMU-domain node and no
  `sysmmu@114xxxxx` node. This matches the panfrost report's finding
  (`docs/reports/gpu-panfrost.md:22`): "The GPU has no system MMU node and needs
  none (it uses its own MMU)."
- The display sysmmu (`sysmmu_disp_mmu@14860000`, dtsi `:1543`) belongs to the
  DECON scanout path only. The new display driver handles it explicitly:
  `iovmm_map()`/`iovmm_unmap()` in `exynos7580_iova_get/put`
  (0001 patch lines ~1126-1215) with `use_iommu = !iovmm_activate(dev)`
  (patch lines ~1754-1762), falling back to `cma_obj->paddr` in bypass.
  That code path is display-only and irrelevant to GPU virtual addresses.
- GPU MMU ownership in the shipping driver
  (`drivers/gpu/arm/t7xx/r15p0/`, selected by `CONFIG_MALI_MIDGARD=y` +
  `CONFIG_MALI_T7XX=y` + `CONFIG_MALI_R15P0=y`):
  - Address-space programming lives in
    `backend/gpu/mali_kbase_mmu_hw_direct.c`, `kbase_mmu_hw_configure()`
    (`:242-288`): writes `AS_TRANSTAB_LO/HI`, `AS_MEMATTR_LO/HI`
    (`AS_TRANSCFG_LO/HI` only under `CONFIG_MALI_GPU_MMU_AARCH64`, which is
    `# CONFIG_... is not set` here), then `AS_COMMAND_UPDATE`.
    Setup values come from `mali_kbase_mmu_mode_lpae.c:76-98`
    (`transtab = pgd & mask | TABLE | READ_INNER`, `transcfg = 0` non-AArch64).
  - Page-table inserts go through `mali_kbase_mmu.c` with per-update cache
    maintenance via `kbase_mmu_sync_pgd()` (`mali_kbase_mmu.c:74-83`:
    `dma_sync_single_for_device(..., DMA_TO_DEVICE)` whenever
    `system_coherency != COHERENCY_ACE`).
  - Import path: `kbase_mem_from_umm()` at
    `t7xx/r15p0/mali_kbase_mem_linux.c:940-...` does `dma_buf_get(fd)` (`:947`)
    + `dma_buf_attach(dma_buf, kbdev->dev)` (`:951`), then on first job use the
    UMM pages are resolved through the dma-buf attachment into GPU PTEs and
    `kbase_mmu_insert_pages()`. There is no sysmmu/exynos-iommu call anywhere
    in this chain — the GPU MMU *is* the IOMMU for GPU virtual addresses.
- Consequence for panfrost: a panfrost port must implement its own
  `AS_TRANSTAB/MEMATTR/COMMAND/TRANSCFG` programming (the tested encoder in
  `code/userspace/gpu/panfrost/` already models exactly these registers) and
  the two drivers **cannot share the block** — cutover is `CONFIG_MALI_MIDGARD=n`
  (`docs/reports/gpu-panfrost.md:139-142`). No exynos-sysmmu code, DT node, or
  `drivers/iommu/exynos-*.c` involvement is needed for the GPU path; do not add any.

### (c) CMA sizing and coherency

- **Sizing:** stock config has
  `CONFIG_CMA_SIZE_MBYTES=0` (`code/pmaports/.../config-samsung-j7elte.aarch64:988`,
  same in every `arch/arm64/configs/*7580*: CMA_SIZE_MBYTES=0`), i.e. no default
  CMA area — `__dma_alloc_coherent()` (`arch/arm64/mm/dma-mapping.c:103-117`)
  falls straight through to `swiotlb_alloc_coherent()`, and
  `drm_gem_cma_create()` fails for anything beyond the atomic pool. The
  `patches/drm/config-drm-exynos7580.fragment` already raises this to
  `CONFIG_CMA_SIZE_MBYTES=32`, which is the minimum for display dumb buffers +
  fbdev. **Decision: keep 32 MB for M1; revisit (64 MB) only if the M2
  render-node/win-sys import path shows CMA pressure** — raising it further now
  would shrink the movable zone for no measured reason, and CMA is a boot-time
  reservation (no DT `reserved-memory`/`linux,cma` node exists in this dtsi to
  carve per-device pools from; the global `CONFIG_CMA_SIZE_MBYTES` is the knob).
- **Coherency:** the SoC is non-coherent as far as the GPU is concerned.
  `t7xx/r15p0/mali_kbase_core_linux.c:3577` forces
  `kbdev->system_coherency = COHERENCY_NONE`; the only override is a DT
  `system-coherency` property (`:3579-3601`), which `mali@0x11400000` does not
  have — so `COHERENCY_NONE` holds on this board (same conclusion as
  `docs/reports/gpu-panfrost.md:121-124`). Practical consequences, all already
  handled by the code a panfrost port must mirror:
  - page-table writes need explicit maintenance: `kbase_mmu_sync_pgd()`
    (`mali_kbase_mmu.c:74-83`) and the non-ACE path in `mali_kbase_mmu_hw_direct.c`;
    the panfrost backport must use `coherent_walk = false` + explicit
    `dma_sync_*` (report risk R5).
  - UMM/dma-buf sharing uses the DMA-sync discipline (`DMA_BIDIRECTIONAL`
    attach in the older `midgard/mali_kbase_jd.c:185`; explicit
    `kbase_sync_single_for_device/cpu` wrappers in
    `t7xx/r15p0/mali_kbase_cache_policy.c:51-77` that skip only under
    `COHERENCY_ACE`); CMA scanout buffers are write-combined (`DMA_ATTR_WRITE_COMBINE`)
    rather than cached, so no CPU-cache flush is owed on the display path.
  - `AS_MEMATTR` LPAE table (`mali_kbase_mmu_mode_lpae.c:76-87`) and the known
    kbase-vs-panfrost divergence (`0x8D884D4F48` vs `0x888D88`,
    `docs/reports/gpu-panfrost.md:126-131`) remain a correctness item for M1+.

---

## Optional patch (NEW file, unchecked)

`0001-drm-exynos7580-wire-gem-prime-export-for-kbase-umm.patch` — wires the
five-line PRIME exporter the new display driver is missing so that
`BASE_MEM_IMPORT_TYPE_UMM` (`dma_buf_get` + `dma_buf_attach` at
`t7xx/r15p0/mali_kbase_mem_linux.c:947,951`) can import scanout GEM buffers:

- `DRIVER_PRIME` in `driver_features` (`include/drm/drmP.h:158` defines the
  flag; the legacy exynos driver sets `DRIVER_GEM | DRIVER_PRIME` at
  `drivers/gpu/drm/exynos/exynos_drm_drv.c:264`).
- `.prime_handle_to_fd = drm_gem_prime_handle_to_fd`,
  `.prime_fd_to_handle = drm_gem_prime_fd_to_handle` (both exported from
  `drivers/gpu/drm/drm_prime.c:252,362`).
- `.gem_prime_export = drm_gem_prime_export`,
  `.gem_prime_import = drm_gem_prime_import` (both exported from
  `drivers/gpu/drm/drm_prime.c:195,307`; on 3.10 these go through the driver's
  `gem_prime_get_sg_table`/`gem_prime_import_sg_table` low-level hooks — the
  stock `drm_gem_prime_export` in this tree still takes that route, so this
  patch also adds two thin low-level callbacks (`sg_alloc_table(1)` +
  `sg_set_buf(cma_obj->vaddr, size)` for get, fresh `drm_gem_cma_create()`
  for import, plus vmap/vunmap borrowing `cma_obj->vaddr`); without them the
  generic `drm_gem_map_dma_buf` at `drm_prime.c:67-82` would dereference NULL
  hooks).

Status: **unchecked** — written against the 0001 patch text and the 3.10
`drm_prime.c` API by inspection; it has NOT been applied, compiled, or booted
(`git apply --check` + build + `/dev/dri` + kbase-import smoke test are
integrator steps; see below). If the integrator prefers kbase to keep importing
ION buffers only (no display-GEM sharing in M1), skip this patch entirely —
display + fbdev work without it.

## Integrator commands (build/flash/verify — NOT run by this slice)

```sh
# 1. check the optional patch applies (from repo root):
git apply --check patches/iommu-dma/0001-drm-exynos7580-wire-gem-prime-export-for-kbase-umm.patch

# 2. build (integrator-owned; ~7 min Image build — do not run in slices):
#    apply patches/drm/0001,0002 + patches/iommu-dma/0001(optional) + config fragment,
#    then the single serialised Image build.

# 3. on device: /dev/dri/card0 exists, fbdev X still works; with the optional
#    patch, PRIME export path present:
ls -l /dev/dri
#    kbase UMM import smoke (needs kbase winsys): import a PRIME fd from a
#    dumb buffer and submit a no-op job; expect GPU virtual mapping via
#    AS_TRANSTAB, no sysmmu involvement (trace AS_COMMAND_UPDATE, not iovmm).
```

## Smallest next experiment (if this is questioned)

Boot the M1 display image, dump one CMA dumb-buffer's `paddr` vs the DECON
`VIDW*_ADD0` value with `use_iommu=true/false`: with the sysmmu bound the two
must differ (iovmm mapping active); in bypass they must be equal — that single
comparison separates "CMA/DMA working" from "display-IOMMU working" without
touching the GPU path.
