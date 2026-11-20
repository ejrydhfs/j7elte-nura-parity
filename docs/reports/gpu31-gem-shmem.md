# GPU31 GEM shmem — 16 MiB tiler heap via page-array backing

Date: 2026-11-20. Slice: G31_GemShmem. Ticket: panfrost7580 GEM
`panfrost7580_gem.c` (+ioctl, host test), driver snapshot GEM hunks.

Prior state (silicon): CREATE_BO 4 KiB–4 MiB OK, 8 MiB/16 MiB fail ENOMEM
(`dma_alloc_attrs -> swiotlb`, `swiotlb_alloc_coherent` warning in dmesg).
Mesa 20.3.5's panfrost asks for a 16 MiB tiler heap at screen creation, so
no renderer can start (Mesa20 report, this repo state). GEM CREATE_BO
selected `P7580_BO_CMA` unconditionally.

Fix: CREATE_BO now selects `P7580_BO_SHMEM`. CMA stays for explicit WC
scanout only (via `p7580_gem_create_with_handle`, never the ioctl).

## 1. Design

Upstream panfrost uses `drm_gem_shmem` for general BOs; CMA is only for
WC/scanout. The 3.10 tree has no `drm_gem_shmem_*` helper, so the port
keeps its private SHMEM path, unchanged in shape by this ticket:

- alloc: `drm_gem_private_object_init` + `shmem_file_setup("drm mm
  object", size, VM_NORESERVE)` + one `shmem_read_mapping_page` per
  page into `bo->pages[]` (`p7580_shmem_alloc`). Each returned page
  holds exactly one reference and that reference IS the pin — paired
  with one `put_page` per page in `p7580_shmem_free`. The private filp
  is never exposed to userspace so `ftruncate` cannot drop pages under
  us; no `get_page`, no sg, no `dma_map` in GEM.
- GPU mapping: GEM never programs the Mali tables. The owning driver
  maps BOs at SUBMIT (`p7580_drv_map_bo` in the driver snapshot) via
  `p7580_bo_page_phys` (CMA `cma_paddr + idx*4K`; SHMEM
  `page_to_phys(pages[idx])`; imported sg-walk over `bo->sgt`) into
  per-page LPAE entries, then `p7580_mmu_invalidate` + `dma_sync`
  maintenance for COHERENCY_NONE (kbase `mali_kbase_mmu.c` discipline).
- CPU mmap (`p7580_gem_mmap`): `drm_gem_mmap` lookup, then immediate
  mapping — `dma_mmap_attrs` (plain WRITE_COMBINE) for CMA,
  `remap_pfn_range` per page for SHMEM/imported, `vm_size > obj->size`
  rejected, errors via `drm_gem_vm_close`. The fault helper stays for
  reference only (3.10 core contract, gpu31-mmap.md).
- WAIT_BO: `dma_resv_wait_timeout` on the embedded 0002 resv; absolute
  `timeout_ns` converted via `ktime_get_ts` delta in the ioctl.
- PRIME: export via stock `drm_gem_prime_export` (fresh sg per
  `get_sg_table`: CMA single entry, SHMEM `drm_prime_pages_to_sg`,
  re-export of imports hands out the live mapping); import wraps the
  exporter's live mapping (`bo->sgt`, never import-by-copy) and free
  tears it down with `drm_prime_gem_destroy`; vmap holds one GEM ref
  per live CPU mapping (`P7580_VMAP_MAX` slots, foreign pointers
  rejected).
- IOVA: per-device `struct p7580_dev` drm_mm over `[32 MiB, 4 GiB)`,
  `drm_mm_insert_node` at create/import, `drm_mm_remove_node` in free.

Files changed (live + mirrored hunks in
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`):

- `panfrost7580_ioctl.c` — `panfrost_ioctl_create_bo` passes
  `P7580_BO_SHMEM` (was `P7580_BO_CMA`) with the rationale comment.
- `panfrost7580_gem.c` — header comment (SHMEM default/CMA-for-scanout),
  SHMEM pin-discipline note, create size-limit rationale, mmap WC note.
- `gem_validate.c` — 8/16 MiB SHMEM regression checks.

## 2. Size-limit rationale

The board's shipping defconfig sets `CONFIG_CMA_SIZE_MBYTES=0` (no
default CMA area; `__dma_alloc_coherent` falls through to swiotlb), and
the display config fragment raises 32 MB only for scanout plus slack
(dmabuf 0003 note). One contiguous `dma_alloc_attrs` for 8+ MiB cannot
succeed there — ENOMEM by construction, not fragmentation. There is
therefore no CMA size at which Mesa's 16 MiB tiler heap could pass, and
no driver-side numeric cap fixes it: the cap is the allocator shape.

SHMEM has no such shape limit: a 16 MiB BO is 4096 ordinary page-cache
pages, each `shmem_read_mapping_page`'d independently. The only
driver-side bound is the IOVA window (`P7580_IOVA_SIZE`, ~4 GiB minus
32 MiB), enforced by the drm_mm allocator (`-ENOSPC`), plus physical
memory. CREATE_BO keeps no numeric `size` cap for this reason; zero
size is still `-EINVAL` (v5.2 UAPI).

## 3. Cache coherency (COHERENCY_NONE)

The SoC is non-coherent as far as the GPU is concerned (kbase forces
`COHERENCY_NONE`; no `system-coherency` on `mali@0x11400000`). Rules
the code follows:

- CPU mappings are WC (`pgprot_writecombine`, `MT_NORMAL_NC`) by
  default on both SHMEM and CMA paths — no CPU cache lines exist, so
  no CPU-side flush is owed. `P7580_GEM_CACHABLE` opt-in exists but
  CREATE_BO never selects it.
- Page-table writes are followed by
  `dma_sync_single_for_device(..., DMA_TO_DEVICE)` (kbase
  `mali_kbase_mmu.c:78-87` discipline); the GPU path never sets
  `SKIP_CPU_SYNC` for its own tables and never sets SHARE_OUTER in
  TRANSTAB.
- DMA-sync on UMM/PRIME sharing follows the exynos attachment shape
  (importer runs its normal discipline on the live sgt).

## 4. Verification

Host + kernel checks (this ticket, all green):

- `make check` — `gem_validate` PASS incl. new 8/16 MiB SHMEM checks.
- `make kcheck` — 3 files `-fsyntax-only`, zero warnings.
- `make kobj` — `panfrost7580_gem.o` 27904 B,
  `panfrost7580_ioctl.o` 7192 B, `panfrost7580_devfreq.o` 8208 B.
- `git apply --check` on the regenerated driver 0001: clean.

## 5. Test plan (after flash; no submits here)

1. Flash the Main-owned image built from this commit (PM-ordering fix
   pending — do NOT submit GPU work until that lands).
2. `--probe-only` selftest: GET_PARAM `0x720`, CREATE_BO 4K (handle
   nonzero, `gpu_offset` in `[0x2000000, 0x100000000)`), MMAP_BO,
   GET_BO_OFFSET match.
3. CREATE_BO sweep on the render node: 4 KiB, 1 MiB, 4 MiB (old OK),
   then 8 MiB and 16 MiB (old ENOMEM) — all must return handles with
   nonzero IOVAs; MMAP_BO + CPU map R/W on the 16 MiB BO; `wait_bo(0)`
   immediate.
4. Mesa 20.3.5 screen creation (the original blocker): tiler-heap
   CREATE succeeds; renderer init proceeds past heap alloc.
5. No SUBMIT in this ticket (PM-ordering fix pending). Full GPU-read
   and triangle stay with Integrator/DriverAssemble.

## 6. Honest gaps (NOT done)

- Driver `main.c` MMU-bind call sites unchanged; SHMEM phys walk
  reuses the existing `p7580_bo_page_phys` contract — no new hook.
- No kernel `Image` link, no boot, no device test here (Main owns
  flash; this ticket never flashes, never submits).
- `panfrost7580_gem.h` design comment still cites CMA-first history;
  updated only where the SHMEM default needed stating.
