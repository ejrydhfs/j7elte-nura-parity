/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost_gem.h - GEM object model for the Mali-T720 panfrost port
 * (Exynos 7580 / Nura 3.10.108, j7elte).
 *
 * v5.2 reference: drivers/gpu/drm/panfrost/panfrost_gem.{c,h} (96/30 LOC)
 * against drm_gem_shmem_helper. This tree has NO shmem helper
 * (gpu31-target.md D5: reuse the present 272-line drm_gem_cma_helper),
 * so the port below is a clean-room CMA implementation with a private
 * page-array shmem path. DrmCore's drm_compat_310.h only wires fops;
 * it does NOT own BO structs or ioctl bodies (agreed split).
 *
 * BO model (mirrors v5.2 panfrost_gem_object layout adapted to 3.10):
 *  - embedded struct drm_gem_object (handle + mmap-offset machinery);
 *  - backing selector: CMA (dma_alloc_attrs, contiguous, scanout-safe)
 *    or SHMEM (shmem file + per-page array, import/vmap friendly).
 *    The selector is P7580_BO_* (private, never on the UAPI wire;
 *    v5.2 UAPI has no BO flags and CREATE_BO.flags must stay zero).
 *  - embedded struct dma_resv (0002 shim type; see p7580_bo.resv):
 *    SUBMIT reserves it, WAIT_BO polls it; BO free path releases it.
 *    The field is transparent here because the integrator applies the
 *    0001/0002 backport series BEFORE this driver (series/0002), so
 *    <linux/dma-resv.h> exists at driver build time. The KINC check in
 *    the Makefile stages exactly those headers and fails loudly if
 *    the layout ever drifts.
 *  - IOVA node: per-BO struct drm_mm_node linked into the per-device
 *    drm_mm (P7580_IOVA_START/SIZE window); GET_BO_OFFSET and the MMU
 *    slice read bo->iova (nonzero guaranteed: v5.2 CREATE_BO UAPI
 *    requires a nonzero offset and the T720 treats IOVA 0 as
 *    unmapped). Carrying the node (not a bare start/size pair) lets
 *    free return the range via drm_mm_remove_node; the mutex in
 *    struct p7580_dev owns the allocator (3.10 drm_mm has no
 *    per-mm lock of its own).
 *
 * Lifetime: CMA path uses drm_gem_object_init + dma_alloc_attrs
 * (WRITE_COMBINE via dma_attrs, same as the display CMA delta);
 * SHMEM path uses drm_gem_private_object_init + its own shmem file
 * (same shmem_file_setup("drm mm object") pattern drm_gem_object_init
 * uses) + shmem_read_mapping_page per page. PRIME import holds one
 * dma_buf_get() ref (released in free). vmap pins with the core's own
 * fault-grab reference (see vunmap): while any vmap is live the GEM
 * refcount is held, so free cannot race the mapping (review F2 on the
 * display PRIME patch); the vaddr counting lives in the core, sized by
 * P7580_VMAP_MAX. No import-by-copy anywhere (review F1).
 */
#ifndef PANFROST7580_GEM_H
#define PANFROST7580_GEM_H

/* F-GEM1 (gpu31-driver-review): the BO embeds the real struct dma_resv
 * from the 0002 backport header instead of an opaque pad + layout
 * BUILD_BUG_ONs. The 0001/0002 series is a build prerequisite of this
 * driver (series/0001-0002 applied first; Makefile KINC stages them),
 * so the field tracks shim growth automatically. */
#include <linux/dma-resv.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>

struct device;

#define P7580_BO_CMA		0u
#define P7580_BO_SHMEM		1u

/* IOVA window in BYTES: 32 MiB .. 4 GiB, T720 legacy 32-bit window
 * (panfrost v5.2: PFN 0x8000..0x100000, see panfrost_mmu_map_io_region;
 *  T720 VA bits unread until M0 runs on silicon, gpu-panfrost.md R3).
 * Units: the drm_mm allocator counts PAGES, so p7580_dev_init passes
 * these shifted by PAGE_SHIFT; node.start is a PFN and IOVA bytes are
 * node.start << PAGE_SHIFT. Do NOT pass raw bytes to drm_mm_init. */
#define P7580_IOVA_START	(32u << 20)
#define P7580_IOVA_SIZE		((4ull << 30) - P7580_IOVA_START)

/* CPU-cache attribute for the fault handler (matches exynos update flags). */
#define P7580_GEM_WC		(1u << 0)
#define P7580_GEM_CACHABLE	(1u << 1)

/* Max concurrent vmaps per BO. The core pins each vmap with one GEM
 * reference; the count lets vunmap know when the last mapping went
 * away. 8 is plenty (importer + CPU writer + fault path + scanout). */
#define P7580_VMAP_MAX		8u

struct p7580_bo {
	struct drm_gem_object base;
	/* 0002 reservation object: SUBMIT locks it, WAIT_BO waits on
	 * it, prime import shares it with the exporter. Real type
	 * (linux/dma-resv.h) at build time; see file header. */
	struct dma_resv resv;
	/* IOVA allocation state: node linked into the per-device
	 * drm_mm while valid (iova_valid), start cached for the
	 * GET_BO_OFFSET fast path. */
	struct drm_mm_node iova_node;
	unsigned long iova;
	int iova_valid;
	unsigned int backing;
	unsigned int cache_attr;
	/* CMA backing. sg table is built on demand by
	 * p7580_gem_prime_get_sg_table (never cached here). */
	void *cma_vaddr;
	dma_addr_t cma_paddr;
	/* SHMEM backing. */
	struct file *shmem_filp;
	struct page **pages;
	unsigned int nr_pages;
	/* PRIME import state: set only on the importer. sgt is the live
	 * exporter attachment mapping (unmapped + detached in free via
	 * drm_prime_gem_destroy); the dma_buf ref is the get_dma_buf()
	 * taken at import. Non-imported BOs leave both NULL. */
	struct sg_table *sgt;
	struct dma_buf *imported_dmabuf;
	/* vmap pin: GEM references held while CPU mappings are live;
	 * count mirrors them (p7580_gem_prime_vmap/vunmap). */
	void *vaddrs[P7580_VMAP_MAX];
	unsigned int vmap_count;
	/* gpu31-mmaprefs: userspace-mapping page pins. While at least
	 * one VMA maps this BO every page carries one extra reference
	 * (map_pinned) so the page cannot be recycled under a live
	 * mapping, even for pages no fault has reached yet. vma_refs
	 * counts the VMAs that will each be closed once (mmap success +
	 * vm_ops->open, the split/mremap copies); the 1->0 close drops
	 * the pin. Serialised by dev->struct_mutex in both directions.
	 * Local SHMEM BOs only: CMA memory is dma_alloc-owned and
	 * imported pages belong to the exporter (bo->sgt). */
	unsigned int vma_refs;
	unsigned int map_pinned;
	/* COHERENCY_NONE DMA sync state (gpu31-coherency): bind-time
	 * dma_map of the SHMEM pages for GPU access. dma_addrs[i] is
	 * the bus address programmed into the GPU PTE (identity on
	 * this tree, carried verbatim otherwise); sync_dev is the
	 * platform device the mapping belongs to. Both set by
	 * p7580_gem_dma_bind, cleared by p7580_gem_dma_unbind/free.
	 * NULL/NULL on never-bound and CMA/imported BOs. */
	dma_addr_t *dma_addrs;
	struct device *sync_dev;
	/* gpu31-mmlock: driver-owned fake mmap offset. The 3.10 core
	 * manager (dev->mm_private: drm_mm + drm_hashtab) has no internal
	 * locking and is written from the sched free_job workqueue while
	 * CREATE_BO/import run in ioctl context, so base.map_list is
	 * never used (no drm_gem_create/free_mmap_offset call exists).
	 * offset_node is a page index inside gd->offset_mm; offset_hash
	 * maps offset_node.start back to this BO. Both, and offset_valid,
	 * are only touched with gd->mmap_offset_lock held. */
	struct drm_mm_node offset_node;
	struct drm_hash_item offset_hash;
	int offset_valid;
};
/* Per-device GEM state: IOVA allocator + fake-mmap-offset manager and
 * the locks that serialise them (the driver allocates this in probe
 * and stores it in dev_private; the GEM helpers below take it by
 * pointer so they stay testable). */
struct p7580_dev {
	struct device *dev;
	struct drm_mm iova_mm;
	struct mutex iova_lock;
	/* gpu31-mmlock: driver-owned fake-mmap-offset manager. The 3.10
	 * core manager is unlocked in every direction and every user of
	 * it here runs in a different context: create/import in the
	 * ioctl, free from the sched free_job workqueue, lookup in
	 * .mmap. Core drm_gem_mmap() additionally takes its object
	 * reference unreservedly, so it can resurrect an object already
	 * at refcount 0 (dying in free_object). Own the space instead:
	 * offset_mm carves page-index offsets out of the core window
	 * (DRM_FILE_PAGE_OFFSET_*), offset_hash resolves them back, and
	 * one mutex serialises alloc/free/err-undo/lookup, so no caller
	 * can ever see a partially linked drm_mm node or hash chain. */
	struct drm_mm offset_mm;
	struct drm_open_hash offset_hash;
	struct mutex mmap_offset_lock;
};

struct p7580_bo *to_p7580_bo(struct drm_gem_object *obj);

int p7580_dev_init(struct p7580_dev *gd, struct device *dev);
void p7580_dev_fini(struct p7580_dev *gd);

int p7580_gem_create(struct p7580_dev *gd, struct drm_device *dev,
		     size_t size, unsigned int backing,
		     unsigned int cache_attr, struct drm_gem_object **out);
int p7580_gem_create_with_handle(struct p7580_dev *gd,
				 struct drm_file *file_priv,
				 struct drm_device *dev, size_t size,
				 unsigned int backing,
				 unsigned int cache_attr,
				 uint32_t *handle);
void p7580_gem_free_object(struct drm_gem_object *obj);
int p7580_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf);
/* Published to the core as driver->gem_vm_ops (main.c) and installed
 * by the driver's own .mmap lookup (p7580_gem_mmap); defined in
 * panfrost7580_gem.c. Must stay non-NULL: the mmap handler rejects a
 * NULL table. */
extern const struct vm_operations_struct p7580_gem_vm_ops;
int p7580_gem_mmap(struct file *filp, struct vm_area_struct *vma);
long p7580_gem_wait(struct drm_device *dev, struct drm_file *file_priv,
		    uint32_t handle, long timeout_jiffies, int wait_all);

/* GPU page-table mapping hook (for DriverAssemble / jobs SUBMIT).
 * GEM allocates an IOVA per BO at create/import but never programs the
 * Mali page tables itself. The owning driver maps BOs into the
 * per-context tables with these helpers, then lets jobs SUBMIT pin each
 * referenced BO (dma_resv lock discipline per the 0002 plain-mutex)
 * before p7580_job_hw_submit reads the job descriptor. Unmap runs
 * before free; the context teardown walker is p7580_mmu_teardown()
 * (iommu/panfrost7580_mmu.h, F-MMU5), wired to file-close/remove.
 *  - bind (new, gpu31-coherency): p7580_gem_dma_bind(obj, dev) once
 *    before the first map; then per page 0..npages-1, p7580_mmu_map(mmu,
 *    bo->iova + idx*PAGE_SIZE, p7580_bo_dma_addr(obj, idx),
 *    P7580_PROT_RD|WR, memattr); then p7580_mmu_invalidate(regs,
 *    as_nr, iova>>12, npages, sync=0) and p7580_mmu_program_as once
 *    per context (iommu/panfrost7580_mmu.h, lock/encode in
 *    mmu/pf7580_as.h). PTEs MUST come from p7580_bo_dma_addr (the
 *    bind-time bus address), never page_to_phys/sg_phys directly.
 *  - submit: p7580_gem_submit_sync(obj) per BO after the CPU fills
 *    descriptors/payloads, before queueing. This is a CLEAN
 *    (sync_for_device, DMA_TO_DEVICE): writeback only, never an
 *    invalidate (gpu31-cachedir).
 *  - completion: p7580_gem_job_done_sync(obj) per BO in free_job and
 *    in the pre-signal IRQ path: CLEAN-THEN-INVALIDATE, so GPU bytes
 *    become visible to the next CPU read and a CPU write that raced
 *    the job is written back instead of dropped.
 *  - snapshot/dump read: p7580_gem_invalidate_for_cpu(obj) = the same
 *    clean-then-invalidate pair (an invalidate alone would drop dirty
 *    lines; it is needed to drop the alias's stale clean lines).
 *  - unbind/free: p7580_mmu_unmap per page + invalidate, then
 *    p7580_gem_dma_unbind(obj) (clean, then unmap+invalidate) before
 *    pages are released. */
unsigned int p7580_bo_npages(struct drm_gem_object *obj);
phys_addr_t p7580_bo_page_phys(struct drm_gem_object *obj,
			       unsigned int idx, int *ok);
/* COHERENCY_NONE data sync (gpu31-coherency + gpu31-cachedir): per-page
 * DMA-API order with direction-correct cache ops. Bind maps the SHMEM
 * pages (dma_map_page, BIDIRECTIONAL = clean) and caches the bus
 * addresses; SUBMIT cleans CPU writes (sync_for_device, TO_DEVICE);
 * completion and every snapshot/dump read run clean-then-invalidate
 * (invalidate_for_cpu) so the next CPU read sees DRAM bytes; unbind
 * cleans, then unmaps (which invalidates). CMA/imported BOs are no-ops
 * everywhere. job_done_sync runs in free_job context (no MMIO, no
 * sleeps, no locks); the pre-signal wrapper below runs the same
 * maintenance in IRQ / fence-callback context before the fence
 * signals. PTEs must come from p7580_bo_dma_addr. */
/* Direction-correct helpers (gpu31-cachedir). clean_for_device =
 * dma_sync_single_for_device(DMA_TO_DEVICE) per bound page (writeback).
 * invalidate_for_cpu = the same clean, THEN
 * dma_sync_single_for_cpu(DMA_FROM_DEVICE) (discard); the order is the
 * contract, not an implementation detail. */
void p7580_gem_clean_for_device(struct drm_gem_object *obj);
void p7580_gem_invalidate_for_cpu(struct drm_gem_object *obj);
/* Pre-signal variant of the completion invalidate (gpu31-coherency-race):
 * runs job_done_sync on each owned BO WITHOUT dropping refs. Called with
 * the job's bos/bo_count (opaque void so this header stays drm-free)
 * BEFORE the done-fence is signalled, so a userspace wait returning
 * implies visible data. Same IRQ-off-spinlock / fence-callback safety
 * as job_done_sync (cache maintenance only, no MMIO/sleeps/locks).
 * free_job keeps its post-signal job_done_sync + unref walk as the
 * ref-drop leg. */
void p7580_gem_job_bos_pre_signal_sync(void **bos, unsigned int bo_count);
void p7580_gem_job_done_sync(struct drm_gem_object *obj);
int p7580_gem_dma_bind(struct drm_gem_object *obj, struct device *dev);
void p7580_gem_submit_sync(struct drm_gem_object *obj);
void p7580_gem_dma_unbind(struct drm_gem_object *obj);
phys_addr_t p7580_bo_dma_addr(struct drm_gem_object *obj, unsigned int idx);
/* gpu31-iova: free-time GPU unmap (driver snapshot, submit.c).
 * Called from p7580_gem_free_object before iova_free; clears the
 * BO's leaf PTEs + PM-held TLB invalidate + bo_list unlink (weak
 * entry, no ref drop) + DMA unbind. Never-bound BOs no-op. Register
 * hook is link-compat. */
void p7580_gem_gpu_unmap(struct drm_gem_object *obj);
void p7580_gem_mmu_register_notify(void (*fn)(void *, struct drm_gem_object *,
					      unsigned long, unsigned int),
				   void *priv);
/* DEBUG-ONLY (gpu31-gpuwrite, strip before cutover): kernel-side word
 * readback of a BO page after completion. Reuses p7580_gem_job_done_sync
 * (same invalidate the pre-signal IRQ path runs) then reads one u32 via
 * kmap. No new maintenance implementation; CMA/imported-never-bound BOs
 * follow the same no-op rules as job_done_sync. offset is a byte offset. */
int p7580_gem_debug_read_word(struct drm_gem_object *obj, uint32_t offset,
			      uint32_t *out);
/* DEBUG-ONLY (gpu31-mmaprefs, strip before cutover): the page ledger
 * from the gpu31-shaderbo report §5.1 -- page_count, page_mapcount, the
 * mapping pin state (vma_refs/map_pinned), PageUptodate/LRU/
 * SwapBacked/Dirty and the first four words of page idx. Called by the
 * 0xCF read path ("0xcf") and by SUBMIT per BO after submit-sync
 * ("sub", caller-budgeted). No MMIO, no sleeps. */
void p7580_gem_debug_page_ledger(struct drm_gem_object *obj, const char *tag,
				 unsigned int idx);
struct sg_table *p7580_gem_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *p7580_gem_prime_import_sg_table(
		struct drm_device *dev, size_t size, struct sg_table *sgt);
void *p7580_gem_prime_vmap(struct drm_gem_object *obj);
void p7580_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr);
struct dma_buf *p7580_gem_prime_export(struct drm_device *dev,
				       struct drm_gem_object *obj, int flags);
struct drm_gem_object *p7580_gem_prime_import(struct drm_device *dev,
					      struct dma_buf *dma_buf);

#endif /* PANFROST7580_GEM_H */
