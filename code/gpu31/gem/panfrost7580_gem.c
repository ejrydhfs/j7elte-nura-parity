/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_gem.c - BO alloc/import/export/mmap for the T720 port.
 *
 * v5.2 reference: panfrost_gem.c (96 LOC): CREATE via drm_gem_shmem_create,
 * MMAP via drm_gem_mmap + drm_gem_shmem_mmap, WAIT via dma_resv_wait_timeout,
 * GET_PARAM/GET_BO_OFFSET trivially. Adapted to 3.10:
 *
 *  - drm_gem_shmem_* does not exist: CMA path mirrors exynos/drm_gem_cma
 *    (drm_gem_object_init + dma_alloc_attrs WRITE_COMBINE +
 *    drm_gem_create_mmap_offset); SHMEM path mirrors drm_gem_object_init's
 *    own shmem_file_setup + shmem_read_mapping_page per page (mm/shmem.c).
 *    CMA uses plain WRITE_COMBINE only (arm64 dma-mapping honours exactly
 *    this attr; NO_KERNEL_MAPPING is ignored by the arm64 backend and
 *    FORCE_CONTIGUOUS is a display-side mistaken carry-over, so neither
 *    is set here).
 *  - CPU mmap is IMMEDIATE (exynos_drm_gem_mmap_buffer /
 *    drm_gem_cma_mmap shape): pgprot_writecombine + dma_mmap_attrs
 *    for CMA (plain WRITE_COMBINE only), remap_pfn_range per page
 *    for SHMEM/imported. Fault-driven vm_insert_mixed cannot work:
 *    drm_gem_mmap() sets VM_PFNMAP and installs driver->gem_vm_ops
 *    itself (EINVAL when that table is NULL); clearing PFNMAP for
 *    VM_MIXEDMAP + swapping in a second ops table breaks the core
 *    contract (mmap -EINVAL, never faults). The fault helper stays
 *    for reference only. WC/cached prot selection is exynos
 *    update_vm_cache_attr shape.
 *  - WAIT_BO mirrors v5.2 panfrost_wait_bo: dma_resv_wait_timeout on the
 *    embedded 0002 resv (locked inside the shim; no outer lock needed).
 *    Timeout arrives in jiffies (caller converts ns via nsecs_to_jiffies).
 *  - PRIME export mirrors 3.10 drm_gem_map_dma_buf (drm_prime.c:67-82):
 *    get_sg_table + dma_map_sg; the dma_buf_ops are the stock
 *    drm_gem_prime_dmabuf_ops via drm_gem_prime_export. Import mirrors
 *    drm_gem_prime_import (attach + map + import_sg_table) but keeps
 *    ATTACHMENT-BACKED sharing (review F1 on the display patch:
 *    never import-by-copy). import_sg_table builds a SHMEM-backed BO
 *    whose sgt IS the live exporter attachment mapping (drm_prime.c:500
 *    passes exactly this pointer in); free tears it down with
 *    drm_prime_gem_destroy (unmap + detach + put). vmap holds one GEM
 *    object reference per live CPU mapping (review F2), released in
 *    vunmap; the NULL check there rejects foreign pointers.
 *  - The driver-private IOVA allocator is a per-device drm_mm owned by
 *    struct p7580_dev (header): create/fini init/takedown it, and alloc
 *    inserts each BO's embedded drm_mm_node (free removes the node, so
 *    the range is really returned). Ranges are PAGE_SHIFT units like
 *    every 3.10 drm_mm caller (drm_gem.c, ttm_bo.c).
 *
 * SHIM-CALLSITES are marked 0001/0002 (fence/resv). 0003/0004 (syncobj/
 * sched) belong to the jobs slice; this file only exposes the embedded
 * resv for SUBMIT to lock. <drm/drm.h> (not "drm.h") resolves through
 * -I include/uapi like the rest of the tree.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dma-attrs.h>
#include <linux/dma-buf.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>

#include <drm/drmP.h>
#include <drm/drm.h>

#include <linux/dma-fence.h>		/* 0001 */
#include <linux/dma-resv.h>		/* 0002 */

#include "panfrost7580_gem.h"

struct p7580_bo *to_p7580_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct p7580_bo, base);
}

/* Per-device IOVA allocator (struct p7580_dev, header). Units: the
 * drm_mm window is addressed in PAGES (init with the byte window
 * shifted by PAGE_SHIFT; node .start is therefore a PFN and the byte
 * GPU address is node.start << PAGE_SHIFT). Raw byte init would place
 * every BO near 0x2000000000, past the 4 GiB window -- MMAP_BO via
 * the GEM offset manager kept working, so open/CREATE/GET_BO all
 * passed while only the GPU alias was wrong (gpu31-mmap.md).
 * ------------------------------------------------------------------ */

int p7580_dev_init(struct p7580_dev *gd, struct device *dev)
{
	int ret;

	if (!gd || !dev)
		return -EINVAL;
	gd->dev = dev;
	mutex_init(&gd->iova_lock);
	ret = drm_mm_init(&gd->iova_mm, P7580_IOVA_START >> PAGE_SHIFT,
			  P7580_IOVA_SIZE >> PAGE_SHIFT);
	if (ret) {
		gd->dev = NULL;
		return ret;
	}
	return 0;
}

void p7580_dev_fini(struct p7580_dev *gd)
{
	if (!gd)
		return;
	drm_mm_takedown(&gd->iova_mm);
	gd->dev = NULL;
}

static int p7580_iova_alloc(struct p7580_dev *gd, struct p7580_bo *bo)
{
	unsigned long npages = bo->base.size >> PAGE_SHIFT;
	struct drm_mm_node *hole;

	mutex_lock(&gd->iova_lock);
	hole = drm_mm_search_free(&gd->iova_mm, npages, 0, false);
	if (!hole) {
		mutex_unlock(&gd->iova_lock);
		return -ENOSPC;
	}
	/* Canonical 3.10 insert: the node is preallocated in the BO
	 * (F-GEM2); drm_mm_insert_node links it and consumes the hole.
	 * Mirrors drm_gem_create_mmap_offset's search/insert pair. */
	memset(&bo->iova_node, 0, sizeof(bo->iova_node));
	if (drm_mm_insert_node(&gd->iova_mm, &bo->iova_node, npages, 0)) {
		mutex_unlock(&gd->iova_lock);
		return -ENOSPC;
	}
	bo->iova = bo->iova_node.start << PAGE_SHIFT;
	bo->iova_valid = 1;
	mutex_unlock(&gd->iova_lock);
	return 0;
}

static void p7580_iova_free(struct p7580_dev *gd, struct p7580_bo *bo)
{
	if (!bo->iova_valid)
		return;
	/* F-GEM2: remove the node so the range is really returned
	 * (drm_mm_remove_node re-links the hole; plain remove is the
	 * non-sleeping path, correct under the free hook's lock). */
	mutex_lock(&gd->iova_lock);
	drm_mm_remove_node(&bo->iova_node);
	mutex_unlock(&gd->iova_lock);
	bo->iova_valid = 0;
}

/* ------------------------------------------------------------------ */
/* CMA backing                                                        */
/* ------------------------------------------------------------------ */

static int p7580_cma_alloc(struct drm_device *dev, struct p7580_bo *bo,
			   size_t size)
{
	DEFINE_DMA_ATTRS(attrs);
	void *vaddr;
	dma_addr_t paddr;

	size = round_up(size, PAGE_SIZE);
	/* WRITE_COMBINE is the only attr the arm64 backend honours
	 * (arch/arm64/mm/dma-mapping.c -> MT_NORMAL_NC). NO_KERNEL_MAPPING
	 * would be silently ignored; FORCE_CONTIGUOUS belongs to a later
	 * DMA API. Plain WC matches the display CMA delta (0001). */
	dma_set_attr(DMA_ATTR_WRITE_COMBINE, &attrs);
	vaddr = dma_alloc_attrs(dev->dev, size, &paddr, GFP_KERNEL, &attrs);
	if (!vaddr)
		return -ENOMEM;
	bo->cma_vaddr = vaddr;
	bo->cma_paddr = paddr;
	return 0;
}

static void p7580_cma_free(struct drm_device *dev, struct p7580_bo *bo)
{
	DEFINE_DMA_ATTRS(attrs);

	if (!bo->cma_vaddr)
		return;
	dma_set_attr(DMA_ATTR_WRITE_COMBINE, &attrs);
	dma_free_attrs(dev->dev, bo->base.size, bo->cma_vaddr,
		       bo->cma_paddr, &attrs);
	bo->cma_vaddr = NULL;
}

/* ------------------------------------------------------------------ */
/* SHMEM backing (private page array, no shmem helper in 3.10)        */
/* ------------------------------------------------------------------ */

static int p7580_shmem_alloc(struct drm_device *dev, struct p7580_bo *bo,
			     size_t size)
{
	unsigned int nr = size >> PAGE_SHIFT;
	struct file *filp;
	struct page **pages;
	unsigned int i;

	(void)dev;
	filp = shmem_file_setup("drm mm object", size, VM_NORESERVE);
	if (IS_ERR(filp))
		return PTR_ERR(filp);

	pages = kcalloc(nr, sizeof(*pages), GFP_KERNEL);
	if (!pages) {
		fput(filp);
		return -ENOMEM;
	}

	for (i = 0; i < nr; i++) {
		struct page *page = shmem_read_mapping_page(
				filp->f_mapping, i);
		if (IS_ERR(page)) {
			int err = PTR_ERR(page);
			while (i--)
				put_page(pages[i]);
			kfree(pages);
			fput(filp);
			return err;
		}
		pages[i] = page;
	}

	bo->shmem_filp = filp;
	bo->pages = pages;
	bo->nr_pages = nr;
	return 0;
}

static void p7580_shmem_free(struct p7580_bo *bo)
{
	unsigned int i;

	if (!bo->pages && !bo->shmem_filp)
		return;
	/* NOTE: sgt/import teardown is NOT here: imported BOs store the
	 * live exporter mapping in bo->sgt and free handles it via
	 * drm_prime_gem_destroy. Local SHMEM BOs never set bo->sgt. */
	if (bo->pages) {
		for (i = 0; i < bo->nr_pages; i++)
			if (bo->pages[i])
				put_page(bo->pages[i]);
		kfree(bo->pages);
		bo->pages = NULL;
		bo->nr_pages = 0;
	}
	if (bo->shmem_filp) {
		fput(bo->shmem_filp);
		bo->shmem_filp = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Create / free                                                      */
/* ------------------------------------------------------------------ */

int p7580_gem_create(struct p7580_dev *gd, struct drm_device *dev,
		     size_t size, unsigned int backing,
		     unsigned int cache_attr, struct drm_gem_object **out)
{
	struct p7580_bo *bo;
	int ret;

	if (!gd || !dev || !size || !out)
		return -EINVAL;
	size = round_up(size, PAGE_SIZE);
	if (backing != P7580_BO_CMA && backing != P7580_BO_SHMEM)
		return -EINVAL;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;

	bo->backing = backing;
	bo->cache_attr = cache_attr;

	if (backing == P7580_BO_CMA) {
		ret = drm_gem_object_init(dev, &bo->base, size);
		if (ret)
			goto err_bo;
		ret = p7580_cma_alloc(dev, bo, size);
		if (ret)
			goto err_obj;
	} else {
		ret = drm_gem_private_object_init(dev, &bo->base, size);
		if (ret)
			goto err_bo;
		ret = p7580_shmem_alloc(dev, bo, size);
		if (ret)
			goto err_obj;
		/* F-GEM4: no extra get_page() here. shmem_read_mapping_page
		 * already returns each page with a reference held; that ref
		 * IS the lifetime pin (paired with the single put_page()
		 * per page in p7580_shmem_free). The private filp is never
		 * exposed to userspace so ftruncate cannot drop pages
		 * under us, and i915 (i915_gem.c:4322/4383) relies on the
		 * same single-ref ownership. The old second pin leaked one
		 * page per SHMEM page per BO after fput. */
	}
	dma_resv_init(&bo->resv);			/* 0002 */

	ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret)
		goto err_resv;

	ret = p7580_iova_alloc(gd, bo);
	if (ret)
		goto err_offset;

	*out = &bo->base;
	return 0;

err_offset:
	drm_gem_free_mmap_offset(&bo->base);
err_resv:
	dma_resv_fini(&bo->resv);			/* 0002 */
err_obj:
	drm_gem_object_release(&bo->base);
	if (backing == P7580_BO_CMA)
		p7580_cma_free(dev, bo);
	else
		p7580_shmem_free(bo);
err_bo:
	kfree(bo);
	return ret;
}

/* ------------------------------------------------------------------ */
/* GPU page-table mapping hook (for DriverAssemble / jobs SUBMIT).    */
/*                                                                    */
/* GEM allocates an IOVA per BO at create/import but never programs   */
/* the Mali page tables itself. The owning driver maps BOs into the   */
/* per-context tables with this hook, then lets jobs SUBMIT pin each  */
/* referenced BO (dma_resv lock discipline per the 0002 plain-mutex)  */
/* before p7580_job_hw_submit reads the job descriptor. Unmap runs    */
/* before free; the context teardown walker is p7580_mmu_teardown()   */
/* (iommu/panfrost7580_mmu.h, F-MMU5), wired to file-close/remove.     */
/*                                                                    */
/* Contract (mirrors the review's `driver/` landing checklist):       */
/*  - map: for each page index 0..npages-1, p7580_mmu_map(mmu,        */
/*    bo->iova + idx*PAGE_SIZE, phys, P7580_PROT_RD|WR, memattr),     */
/*    where phys is cma_paddr + idx*PAGE_SIZE (CMA), pages[idx]       */
/*    (SHMEM, via page_to_phys after dma_map_page), or the sg         */
/*    walk (imported, sg_phys + offset over bo->sgt); then            */
/*    p7580_mmu_invalidate(regs, as_nr, iova>>12, npages, sync=0)     */
/*    (FLUSH_PT via the fixed F-MMU1 LOCK-region encoding) and        */
/*    p7580_mmu_program_as(regs, as_nr, pgd_dma, memattr_lo/hi)       */
/*    once per context (code/gpu31/iommu/panfrost7580_mmu.h, pf7580   */
/*    lock/encode helpers in code/gpu31/mmu/pf7580_as.h).             */
/*  - unmap: p7580_mmu_unmap(mmu, bo->iova + idx*PAGE_SIZE) per page,  */
/*    then invalidate the same range; the caller frees child tables   */
/*    with p7580_mmu_free_table after the parent entry is cleared.    */
/*  - locking: resv lock (0002) around pin/map, MMU-HW lock around    */
/*    program/invalidate (pf7580_as_* takes it), reset_lock ordering  */
/*    per job.c:28-34. Fault recovery signals done_fence (F-JOB4).    */
/* Caller-visible helpers:                                            */

/* Mapping-hook bodies: declared in the header, defined here (see the
 * header contract for the DriverAssemble map/unmap sequence). */
unsigned int p7580_bo_npages(struct drm_gem_object *obj)
{
	return to_p7580_bo(obj)->base.size >> PAGE_SHIFT;
}

phys_addr_t p7580_bo_page_phys(struct drm_gem_object *obj,
			       unsigned int idx, int *ok);

int p7580_gem_create_with_handle(struct p7580_dev *gd,
				 struct drm_file *file_priv,
				 struct drm_device *dev, size_t size,
				 unsigned int backing,
				 unsigned int cache_attr,
				 uint32_t *handle)
{
	struct drm_gem_object *obj;
	int ret;

	ret = p7580_gem_create(gd, dev, size, backing, cache_attr, &obj);
	if (ret)
		return ret;

	ret = drm_gem_handle_create(file_priv, obj, handle);
	if (ret) {
		drm_gem_object_unreference_unlocked(obj);
		return ret;
	}

	/* Handle holds the reference now. */
	drm_gem_object_unreference_unlocked(obj);
	return 0;
}

/* Driver .gem_free_object hook (runs with dev->struct_mutex held,
 * from drm_gem_object_free). Never sleeps, never fails. */
void p7580_gem_free_object(struct drm_gem_object *obj)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	struct p7580_dev *gd;
	struct drm_device *dev = obj->dev;

	gd = dev->dev_private;
	if (gd)
		p7580_iova_free(gd, bo);

	if (obj->import_attach) {
		/* Imported: sgt is the live exporter mapping; the core
		 * helper unmaps + detaches + drops our dma_buf ref
		 * (drm_prime.c:467-479). */
		drm_prime_gem_destroy(obj, bo->sgt);
		bo->sgt = NULL;
	}
	/* Importer-held dma_buf refs die inside drm_prime_gem_destroy
	 * (dma_buf_put); bo->imported_dmabuf only exists to keep the
	 * pointer visible to debug paths. */
	bo->imported_dmabuf = NULL;
	if (WARN_ON(bo->vmap_count != 0))
		bo->vmap_count = 0;

	if (obj->map_list.map)
		drm_gem_free_mmap_offset(obj);
	drm_gem_object_release(obj);

	dma_resv_fini(&bo->resv);			/* 0002 */

	if (bo->backing == P7580_BO_CMA)
		p7580_cma_free(dev, bo);
	else
		p7580_shmem_free(bo);
	kfree(bo);
}

/* ------------------------------------------------------------------ */
/* CPU mmap (fault-driven, mirrors exynos_drm_gem_fault/map_buf)      */
/* ------------------------------------------------------------------ */

/* exynos convert_to_vm_err_msg, verbatim incl. the -EINTR -> NOPAGE arm
 * (-EAGAIN comes from shmem_getpage while the page is under reclaim and
 * is equally transient, so it joins the retry arm). */
static unsigned int p7580_fault_err(int ret)
{
	switch (ret) {
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EAGAIN:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
}

static int p7580_map_page(struct p7580_bo *bo, struct vm_area_struct *vma,
			  unsigned long f_vaddr, pgoff_t page_offset)
{
	struct scatterlist *sgl;
	unsigned long pfn;
	unsigned int i;

	if (page_offset >= (bo->base.size >> PAGE_SHIFT))
		return -EINVAL;

	if (bo->backing == P7580_BO_CMA) {
		pfn = __phys_to_pfn(bo->cma_paddr) + page_offset;
	} else if (bo->sgt) {
		/* Imported: walk the live attachment mapping exactly like
		 * exynos_drm_gem_map_buf (sg lengths, not page index). */
		sgl = bo->sgt->sgl;
		for_each_sg(bo->sgt->sgl, sgl, bo->sgt->nents, i) {
			if (page_offset < (sgl->length >> PAGE_SHIFT))
				break;
			page_offset -= (sgl->length >> PAGE_SHIFT);
		}
		if (!sgl)
			return -EINVAL;
		pfn = __phys_to_pfn(sg_phys(sgl)) + page_offset;
	} else {
		struct page *page = bo->pages[page_offset];

		pfn = page_to_pfn(page);
	}
	return vm_insert_mixed(vma, f_vaddr, pfn);
}

int p7580_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vma->vm_private_data;
	struct p7580_bo *bo = to_p7580_bo(obj);
	struct drm_device *dev = obj->dev;
	unsigned long f_vaddr;
	pgoff_t page_offset;
	int ret;

	page_offset = ((unsigned long)vmf->virtual_address -
		       vma->vm_start) >> PAGE_SHIFT;
	f_vaddr = (unsigned long)vmf->virtual_address;

	mutex_lock(&dev->struct_mutex);
	ret = p7580_map_page(bo, vma, f_vaddr, page_offset);
	mutex_unlock(&dev->struct_mutex);
	return p7580_fault_err(ret);
}

/* gem_vm_ops for this driver (exynos shape: fault/open/close from the
 * core). Published to the core as driver->gem_vm_ops in main.c
 * (non-static: referenced from another TU). */
const struct vm_operations_struct p7580_gem_vm_ops = {
	.fault = p7580_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/* Driver .fops mmap (GEM path): resolve the fake offset to the BO via
 * drm_gem_mmap, then map the pages IMMEDIATELY (no fault round trip).
 * Fault-driven vm_insert_mixed cannot work here: drm_gem_mmap() on
 * 3.10 sets VM_PFNMAP|VM_DONTEXPAND, installs driver->gem_vm_ops
 * itself, and rejects VM_MIXEDMAP vmas/-EINVALs when that table is
 * NULL -- clearing PFNMAP/adding MIXEDMAP and swapping in a second
 * ops table after the fact breaks the core's contract (mmap() returns
 * -EINVAL without ever faulting; dmesg silent). The working shape on
 * this tree is exynos_drm_gem_mmap_buffer/drm_gem_cma_mmap:
 * pgprot_writecombine (WC, exynos update_vm_cache_attr shape), then
 * one dma_mmap_attrs() for the CMA page (arm64 backend honours plain
 * WRITE_COMBINE; NO_KERNEL_MAPPING/FORCE_CONTIGUOUS must NOT be set,
 * F-GEM3) or one remap_pfn_range() per SHMEM/imported page with the
 * same prot + size check. KMS dumb mmap keeps its own entry point. */
int p7580_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj;
	struct p7580_bo *bo;
	struct scatterlist *sgl;
	unsigned long vm_size, pfn, off;
	unsigned int i, npages;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	obj = vma->vm_private_data;
	bo = to_p7580_bo(obj);
	npages = obj->size >> PAGE_SHIFT;
	vm_size = vma->vm_end - vma->vm_start;

	if (vm_size > obj->size) {
		ret = -EINVAL;
		goto err_close;
	}

	/* WC default, cached opt-in (CMA BOs are created WC). */
	if (bo->cache_attr & P7580_GEM_CACHABLE)
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	else if ((bo->cache_attr & P7580_GEM_WC) ||
		 !(bo->cache_attr & P7580_GEM_CACHABLE))
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	if (bo->backing == P7580_BO_CMA && !bo->sgt) {
		DEFINE_DMA_ATTRS(attrs);
		dma_set_attr(DMA_ATTR_WRITE_COMBINE, &attrs);
		ret = dma_mmap_attrs(obj->dev->dev, vma, bo->cma_vaddr,
				     bo->cma_paddr, obj->size, &attrs);
		if (ret)
			goto err_close;
		return 0;
	}

	if (bo->sgt) {
		off = 0;
		for_each_sg(bo->sgt->sgl, sgl, bo->sgt->nents, i) {
			unsigned long chunk = (unsigned long)sgl->length;
			if (chunk > vm_size - off)
				chunk = vm_size - off;
			pfn = __phys_to_pfn(sg_phys(sgl));
			ret = remap_pfn_range(vma, vma->vm_start + off, pfn,
					chunk, vma->vm_page_prot);
			if (ret)
				goto err_close;
			off += sgl->length;
			if (off >= vm_size)
				break;
		}
		return 0;
	}

	if (!bo->pages) {
		ret = -ENODEV;
		goto err_close;
	}
	for (i = 0; i < npages; i++) {
		if ((unsigned long)(i + 1) * PAGE_SIZE > vm_size)
			break;
		pfn = page_to_pfn(bo->pages[i]);
		ret = remap_pfn_range(vma, vma->vm_start + (i << PAGE_SHIFT),
				      pfn, PAGE_SIZE, vma->vm_page_prot);
		if (ret)
			goto err_close;
	}
	return 0;

err_close:
	drm_gem_vm_close(vma);
	return ret;
}

/* ------------------------------------------------------------------ */
/* WAIT_BO (mirrors v5.2 panfrost_wait_bo on the 0002 resv shim)      */
/* ------------------------------------------------------------------ */

/* Driver wait helper behind the WAIT_BO ioctl. timeout is jiffies;
 * the 0002 shim locks resv->lock internally and handles the excl +
 * shared fence set (shared only when wait_all). */
long p7580_gem_wait(struct drm_device *dev, struct drm_file *file_priv,
		    uint32_t handle, long timeout_jiffies, int wait_all)
{
	struct drm_gem_object *obj;
	struct p7580_bo *bo;
	long ret;

	obj = drm_gem_object_lookup(dev, file_priv, handle);
	if (!obj)
		return -ENOENT;
	bo = to_p7580_bo(obj);
	ret = dma_resv_wait_timeout(&bo->resv, wait_all, true,
				    timeout_jiffies);
	drm_gem_object_unreference_unlocked(obj);
	return ret;
}

/* ------------------------------------------------------------------ */
/* PRIME (real attachment-backed sharing; never import-by-copy)       */
/* ------------------------------------------------------------------ */

/* GPU page-table mapping hook bodies (declared above create so the
 * contract comment lives next to the create path; DriverAssemble
 * includes this header for the prototypes). */

phys_addr_t p7580_bo_page_phys(struct drm_gem_object *obj,
			       unsigned int idx, int *ok)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	unsigned int npages = bo->base.size >> PAGE_SHIFT;
	struct scatterlist *sgl;
	unsigned int off = idx;
	unsigned int i;

	if (idx >= npages) {
		if (ok)
			*ok = 0;
		return 0;
	}
	if (bo->backing == P7580_BO_CMA) {
		if (ok)
			*ok = 1;
		return (phys_addr_t)bo->cma_paddr + ((phys_addr_t)idx <<
						     PAGE_SHIFT);
	}
	if (bo->sgt && !bo->pages) {
		for_each_sg(bo->sgt->sgl, sgl, bo->sgt->nents, i) {
			if (off < (sgl->length >> PAGE_SHIFT)) {
				if (ok)
					*ok = 1;
				return (phys_addr_t)sg_phys(sgl) +
				       ((phys_addr_t)off << PAGE_SHIFT);
			}
			off -= (sgl->length >> PAGE_SHIFT);
		}
		if (ok)
			*ok = 0;
		return 0;
	}
	if (bo->pages && bo->pages[idx]) {
		if (ok)
			*ok = 1;
		return (phys_addr_t)page_to_phys(bo->pages[idx]);
	}
	if (ok)
		*ok = 0;
	return 0;
}

/* Driver .gem_prime_get_sg_table: fresh table per call (the core's
 * map path frees it after dma_map_sg; caching it here would
 * double-free). CMA is a single entry; SHMEM/imported walk the page
 * array via the helper (which uses sg_alloc_table_from_pages). */
struct sg_table *p7580_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	struct sg_table *sgt;

	if (bo->backing == P7580_BO_CMA) {
		sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
		if (!sgt)
			return ERR_PTR(-ENOMEM);
		if (sg_alloc_table(sgt, 1, GFP_KERNEL)) {
			kfree(sgt);
			return ERR_PTR(-ENOMEM);
		}
		sg_set_buf(sgt->sgl, bo->cma_vaddr, obj->size);
		return sgt;
	}

	if (bo->sgt && obj->import_attach)
		/* Re-export of an imported BO: hand out the live
		 * attachment mapping (exynos dmabuf.c shape: the exporter's
		 * map_dma_buf owns the table). */
		return bo->sgt;

	if (!bo->pages)
		return ERR_PTR(-ENODEV);
	/* drm_prime_pages_to_sg builds the table with
	 * sg_alloc_table_from_pages (present, scatterlist.h:237). */
	return drm_prime_pages_to_sg(bo->pages, bo->nr_pages);
}

/* Driver .gem_prime_import_sg_table: wrap the exporter's live mapping.
 * The core (drm_prime.c:500) passes exactly the pointer its
 * dma_buf_map_attachment returned, and our free path hands that same
 * pointer to drm_prime_gem_destroy -- so store it, do NOT copy it.
 * No page allocation happens here (review F1). */
struct drm_gem_object *p7580_gem_prime_import_sg_table(
		struct drm_device *dev, size_t size, struct sg_table *sgt)
{
	struct p7580_bo *bo;
	int ret;

	size = round_up(size, PAGE_SIZE);
	if (!sgt || !size)
		return ERR_PTR(-EINVAL);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_private_object_init(dev, &bo->base, size);
	if (ret) {
		kfree(bo);
		return ERR_PTR(ret);
	}

	bo->backing = P7580_BO_SHMEM;
	bo->cache_attr = P7580_GEM_WC;
	bo->sgt = sgt;

	dma_resv_init(&bo->resv);			/* 0002 */

	ret = drm_gem_create_mmap_offset(&bo->base);
	if (ret) {
		dma_resv_fini(&bo->resv);
		drm_gem_object_release(&bo->base);
		kfree(bo);
		return ERR_PTR(ret);
	}

	/* F-GEM2: same per-device IOVA allocator as create (never the
	 * GEM mmap-offset manager). Imported BOs carry the exporter's
	 * attachment mapping in bo->sgt; the node owns the new range
	 * and free removes it. */
	if (dev->dev_private) {
		struct p7580_dev *gd = dev->dev_private;
		unsigned long npages = size >> PAGE_SHIFT;
		struct drm_mm_node *hole;
		int alloc_ret = -ENOSPC;

		mutex_lock(&gd->iova_lock);
		hole = drm_mm_search_free(&gd->iova_mm, npages, 0, false);
		if (hole) {
			memset(&bo->iova_node, 0, sizeof(bo->iova_node));
			if (!drm_mm_insert_node(&gd->iova_mm, &bo->iova_node,
						npages, 0)) {
				bo->iova = bo->iova_node.start << PAGE_SHIFT;
				bo->iova_valid = 1;
				alloc_ret = 0;
			}
		}
		mutex_unlock(&gd->iova_lock);
		if (alloc_ret) {
			drm_gem_free_mmap_offset(&bo->base);
			dma_resv_fini(&bo->resv);
			drm_gem_object_release(&bo->base);
			kfree(bo);
			return ERR_PTR(alloc_ret);
		}
	}

	return &bo->base;
}

/* Driver .gem_prime_vmap: one GEM reference per live CPU mapping
 * (review F2). The vaddrs slots record them so vunmap can tell a
 * foreign pointer from ours. CMA returns the coherent kernel address
 * (still pinned by the ref); SHMEM/imported vmaps the page array. */
void *p7580_gem_prime_vmap(struct drm_gem_object *obj)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	void *vaddr = NULL;
	unsigned int i;

	if (bo->vmap_count >= P7580_VMAP_MAX)
		return NULL;
	for (i = 0; i < P7580_VMAP_MAX; i++)
		if (!bo->vaddrs[i])
			break;
	if (i >= P7580_VMAP_MAX)
		return NULL;

	drm_gem_object_reference(obj);
	if (bo->backing == P7580_BO_CMA) {
		vaddr = bo->cma_vaddr;
	} else if (bo->pages) {
		vaddr = vmap(bo->pages, bo->nr_pages, VM_MAP,
			     pgprot_writecombine(PAGE_KERNEL));
	} else if (bo->sgt) {
		/* Imported local re-export path: no local page array;
		 * map the attachment pages transiently. */
		struct page **pages;
		unsigned int np = obj->size >> PAGE_SHIFT;
		struct scatterlist *sgl;
		unsigned int pg = 0, j;

		pages = kmalloc_array(np, sizeof(*pages), GFP_KERNEL);
		if (pages) {
			for_each_sg(bo->sgt->sgl, sgl, bo->sgt->nents, j) {
				struct page *p = sg_page(sgl);
				unsigned int n = sgl->length >> PAGE_SHIFT;

				while (n-- && pg < np)
					pages[pg++] = p++;
			}
			if (pg == np)
				vaddr = vmap(pages, np, VM_MAP,
					     pgprot_writecombine(PAGE_KERNEL));
			kfree(pages);
		}
	}
	if (!vaddr) {
		drm_gem_object_unreference(obj);
		return NULL;
	}
	bo->vaddrs[i] = vaddr;
	bo->vmap_count++;
	return vaddr;
}

/* Driver .gem_prime_vunmap: drop the pin taken in vmap. Unknown
 * pointers are rejected (caller bug) instead of unmapping blindly. */
void p7580_gem_prime_vunmap(struct drm_gem_object *obj, void *vaddr)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	unsigned int i;

	if (!vaddr)
		return;
	for (i = 0; i < P7580_VMAP_MAX; i++) {
		if (bo->vaddrs[i] == vaddr)
			break;
	}
	if (i >= P7580_VMAP_MAX) {
		WARN_ON(1);
		return;
	}
	if (bo->backing != P7580_BO_CMA || bo->pages || bo->sgt)
		if (!(bo->backing == P7580_BO_CMA && !bo->pages &&
		      !bo->sgt))
			vunmap(vaddr);
	bo->vaddrs[i] = NULL;
	bo->vmap_count--;
	drm_gem_object_unreference(obj);
}

/* Driver .gem_prime_export: stock core helper (dma_buf ops +
 * get_sg_table -> dma_map_sg). */
struct dma_buf *p7580_gem_prime_export(struct drm_device *dev,
				       struct drm_gem_object *obj, int flags)
{
	return drm_gem_prime_export(dev, obj, flags);
}

/* Driver .gem_prime_import: entry point the core calls for
 * PRIME_FD_TO_HANDLE. Self-imports (dma_buf->priv is a GEM object from
 * this dev, i.e. our own export) bump the GEM ref instead of wrapping
 * a new object (core drm_gem_prime_import fast path, drm_prime.c:
 * 273-288). The core helper is static in drm_prime.c, so the check
 * below is by construction: only GEM exports of THIS driver carry a
 * (dev, gem_prime_import) pair that routes back here. Cross-device
 * GEM dma_bufs land in the attach path like any foreign exporter. */
struct drm_gem_object *p7580_gem_prime_import(struct drm_device *dev,
					     struct dma_buf *dma_buf)
{
	struct drm_gem_object *obj;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	int ret;

	if (!dev->driver->gem_prime_import_sg_table)
		return ERR_PTR(-EINVAL);

	if (!dma_buf || !dma_buf->ops)
		return ERR_PTR(-EINVAL);

	if (dma_buf->priv) {
		obj = dma_buf->priv;
		if (obj->dev == dev &&
		    dev->driver->gem_prime_import == p7580_gem_prime_import) {
			drm_gem_object_reference(obj);
			return obj;
		}
	}

	attach = dma_buf_attach(dma_buf, dev->dev);
	if (IS_ERR(attach))
		return ERR_PTR(PTR_ERR(attach));

	/* Attachment-backed import: the core's fd lookup already holds
	 * the dma_buf (dma_buf_get in PRIME_FD_TO_HANDLE before this
	 * runs). Take the explicit import ref so detach/put in free
	 * cannot race the exporter's release (cf. drm_gem_prime_import's
	 * get_dma_buf). */
	get_dma_buf(dma_buf);

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(sgt)) {
		ret = PTR_ERR(sgt);
		goto fail_detach;
	}

	obj = dev->driver->gem_prime_import_sg_table(dev, dma_buf->size, sgt);
	if (IS_ERR(obj)) {
		ret = PTR_ERR(obj);
		goto fail_unmap;
	}

	to_p7580_bo(obj)->sgt = sgt;
	to_p7580_bo(obj)->imported_dmabuf = dma_buf;
	obj->import_attach = attach;
	return obj;

fail_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
fail_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);
	return ERR_PTR(ret);
}
