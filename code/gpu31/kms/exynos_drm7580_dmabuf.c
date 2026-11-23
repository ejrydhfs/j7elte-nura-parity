// SPDX-License-Identifier: GPL-2.0
/*
 * gpu31-kms: dma-buf import and framebuffer creation for the DECON KMS device.
 *
 * KmsDmabuf slice of docs/reports/decon-kms-plan.md (M2: "dma-buf import into
 * KMS ... GBM allocation from panfrost exported to KMS").  This file implements
 * the three entry points the shared contract header assigns to it:
 *
 *   exynos7580_gem_prime_import()  dma-buf fd -> GEM object (DRM_PRIME)
 *   exynos7580_fb_create()         ADDFB2 -> framebuffer wrapping that GEM
 *   exynos7580_fb_scanout()        framebuffer -> DECON IOVA, for the plane
 *
 * The driver's GEM backend is the CMA helper (exynos_drm7580_main.c:
 * .gem_free_object = drm_gem_cma_free_object, .dumb_create =
 * drm_gem_cma_dumb_create), so every object this file produces is a
 * struct drm_gem_cma_object:
 *
 *   - imported dma-buf: a wrapper with base.import_attach set and the
 *     exporter's sg table in base.driver_private.  The helper's free path is
 *     made import-aware by patches/drm-kms/0005-core-drm-gem-cma-import-free.patch
 *     (this tree's drm_gem_cma_free_object predates PRIME import and would
 *     otherwise dma_free_attrs() the exporter's pages).
 *   - dumb buffer: the genuine CMA object drm_gem_cma_dumb_create() made.
 *
 * drm_gem_prime_import() itself is not called: it requires
 * dev->driver->gem_prime_import_sg_table, which is not part of this driver's
 * contract.  The attach/map/object sequence below is the same code with the
 * wrapper allocation inlined.
 *
 * Why iovmm_map() and not ion_iovmm_map(): ion_iovmm_map() BUG_ON()s on a
 * dma-buf whose ops are not ION's (drivers/staging/android/ion/ion.c:2643),
 * so a panfrost/GEM buffer imported through it panics the kernel.  iovmm_map()
 * is the generic exynos primitive ion_iovmm_map() itself calls
 * (include/linux/exynos_iovmm.h:47) and is exporter-agnostic.
 *
 * IOVA vs DMA address: a DECON window register takes an address in the DECON
 * device's SYSMMU/IOVMM domain (the vendor path maps through ion_iovmm_map;
 * exynos-iovmm-v6.c uses IOVA_START_V6 == 0x10000000).  A dma-buf's
 * sg_dma_address() and a CMA object's paddr are DMA-direct/physical addresses
 * and are NOT usable as window addresses, so exynos7580_fb_scanout() returns
 * only properly iovmm_map()ed addresses.
 *
 * Modifiers: this tree's UAPI is 3.10 ADDFB2 (struct drm_mode_fb_cmd2 has no
 * modifiers[]/DRM_MODE_FB_MODIFIERS); the driver advertises no IN_FORMATS, so
 * every non-zero mode_cmd->flags value is rejected here.
 */

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/exynos_iovmm.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>

#include "exynos_drm7580.h"

/* DECON windows are programmed with SYSMMU/IOVMM addresses; iovmm_map() only
 * exists with one of these, and the display does not work without it. */
#if !IS_ENABLED(CONFIG_EXYNOS_IOVMM) && !IS_ENABLED(CONFIG_EXYNOS_IOVMM_V6)
#error "exynos_drm7580_dmabuf.c requires CONFIG_EXYNOS_IOVMM_V6"
#endif


/*
 * How many fbdev (foreign CMA) framebuffers may have a cached DECON mapping.
 * fbdev registers exactly one (fb0); the other slots only exist so an fb0 that
 * is re-created after a failed probe/recovery does not exhaust the cache.
 */
#define EXYNOS7580_FOREIGN_MAPS		4

/*
 * ---------- framebuffer mode validation (pure, host-unit-tested) ----------
 *
 * The block between the BEGIN/END markers is compiled verbatim on the build
 * host by code/gpu31/kms/tools/dmabuf/check.c (extracted with awk), so keep it
 * free of kernel types, helpers and side effects.  The fourcc values are the
 * DRM_FORMAT_* constants from include/uapi/drm/drm_fourcc.h, written out so
 * this block needs no include.
 */
/* ---- BEGIN exynos7580_fb_check ---- */
struct exy7580_fb_desc {
	unsigned int	pixel_format;	/* DRM_FORMAT_* */
	unsigned int	width;
	unsigned int	height;
	unsigned int	flags;		/* drm_mode_fb_cmd2.flags */
	unsigned int	handles[4];
	unsigned int	pitches[4];
	unsigned int	offsets[4];
	unsigned long long size;	/* bytes of the GEM object */
	unsigned int	max_width;
	unsigned int	max_height;
};

/* Bytes per pixel of an RGB format the DECON shim can program; 0 = no. */
static unsigned int exy7580_format_bpp(unsigned int fourcc)
{
	switch (fourcc) {
	case 0x34325258:	/* DRM_FORMAT_XRGB8888 */
	case 0x34324258:	/* DRM_FORMAT_XBGR8888 */
	case 0x34325852:	/* DRM_FORMAT_RGBX8888 */
	case 0x34325842:	/* DRM_FORMAT_BGRX8888 */
	case 0x34325241:	/* DRM_FORMAT_ARGB8888 */
	case 0x34324241:	/* DRM_FORMAT_ABGR8888 */
	case 0x34324152:	/* DRM_FORMAT_RGBA8888 */
	case 0x34324142:	/* DRM_FORMAT_BGRA8888 */
		return 32;
	case 0x36314752:	/* DRM_FORMAT_RGB565 */
	case 0x35314152:	/* DRM_FORMAT_RGBA5551 */
		return 16;
	default:
		return 0;
	}
}

/*
 * exy7580_fb_check() - validate an ADDFB2 request for DECON scanout
 *
 * Only single-plane RGB formats are accepted: the primary/overlay windows
 * scan out one buffer and the M2 userspace (KWin/kmscube) uses XRGB8888 or
 * ARGB8888.  Planar YUV is left to a later change that also teaches the
 * plane about the second IOVA plane (the plane slice already tolerates
 * addr[1] == 0 for NV12/NV21 interleaved buffers).
 *
 * DECON hardware limits enforced here (see decon_core.c:1500-1600 and
 * decon_validate_x_alignment()): a window must be at least 128 bytes wide,
 * the stride must be whole pixels, and the buffer must hold the whole
 * (stride, height) extent starting at the requested offset.
 */
static int exy7580_fb_check(const struct exy7580_fb_desc *d)
{
	unsigned int bpp, cpp, i;
	unsigned long long stride, need;

	if (!d)
		return -EINVAL;

	/* No modifiers, no interlace: this driver advertises neither. */
	if (d->flags)
		return -EINVAL;

	if (!d->handles[0])
		return -EINVAL;
	for (i = 1; i < 4; i++)
		if (d->handles[i] || d->pitches[i] || d->offsets[i])
			return -EINVAL;

	bpp = exy7580_format_bpp(d->pixel_format);
	if (!bpp)
		return -EINVAL;

	if (!d->width || !d->height || !d->max_width || !d->max_height)
		return -EINVAL;
	if (d->width > d->max_width || d->height > d->max_height)
		return -EINVAL;

	cpp = bpp / 8;
	stride = (unsigned long long)d->width * cpp;
	if (stride < 128)		/* DECON: window < 128 bytes is -EINVAL */
		return -EINVAL;

	if (!d->pitches[0] || d->pitches[0] < stride)
		return -EINVAL;
	if (d->pitches[0] % cpp || d->offsets[0] % cpp)
		return -EINVAL;

	if (d->offsets[0] >= d->size)
		return -EINVAL;
	need = (unsigned long long)d->pitches[0] * (d->height - 1) +
	       stride + d->offsets[0];
	if (need > d->size)
		return -EINVAL;

	return 0;
}
/* ---- END exynos7580_fb_check ---- */

/* ---------- our framebuffer object ---------- */

/*
 * Private framebuffer.  The GEM reference is held from fb_create() until
 * exynos7580_fb_destroy(), so an imported dma-buf's attachment (and the sg
 * table our IOVA mappings are built from) outlives every cached mapping.
 */
struct exynos7580_fb {
	struct drm_framebuffer		fb;
	struct drm_gem_cma_object	*cma;
	dma_addr_t			scanout[4];	/* per plane, 0 = unmapped */
};

static inline struct exynos7580_fb *to_exynos7580_fb(struct drm_framebuffer *fb)
{
	return container_of(fb, struct exynos7580_fb, fb);
}

/*
 * Serialises IOVA map creation against fb destruction and the foreign-fb
 * cache.  iovmm_map()/iovmm_unmap() may sleep (iommu_map), so this is a mutex.
 */
static DEFINE_MUTEX(exynos7580_scanout_lock);

#if IS_ENABLED(CONFIG_DRM_KMS_CMA_HELPER)
struct exynos7580_foreign_map {
	struct drm_framebuffer	*fb;
	unsigned int		plane;
	dma_addr_t		addr;
};

static struct exynos7580_foreign_map
			exynos7580_foreign_maps[EXYNOS7580_FOREIGN_MAPS];
#endif

static void exynos7580_fb_destroy(struct drm_framebuffer *fb)
{
	struct exynos7580_fb *efb = to_exynos7580_fb(fb);
	unsigned int i;

	mutex_lock(&exynos7580_scanout_lock);
	for (i = 0; i < ARRAY_SIZE(efb->scanout); i++) {
		if (!efb->scanout[i])
			continue;
		iovmm_unmap(fb->dev->dev, efb->scanout[i]);
		efb->scanout[i] = 0;
	}
	mutex_unlock(&exynos7580_scanout_lock);

	drm_framebuffer_cleanup(fb);
	drm_gem_object_unreference_unlocked(&efb->cma->base);
	kfree(efb);
}

static int exynos7580_fb_create_handle(struct drm_framebuffer *fb,
				       struct drm_file *file,
				       unsigned int *handle)
{
	struct exynos7580_fb *efb = to_exynos7580_fb(fb);

	return drm_gem_handle_create(file, &efb->cma->base, handle);
}

/*
 * The DECON is not a rendering engine and there is no cache to flush: the
 * dma-buf exporter's map_dma_buf() already performed the cache maintenance
 * when the attachment was mapped DMA_BIDIRECTIONAL in the import.
 */
static int exynos7580_fb_dirty(struct drm_framebuffer *fb,
			       struct drm_file *file, unsigned int flags,
			       unsigned int color, struct drm_clip_rect *clips,
			       unsigned int num_clips)
{
	return 0;
}

static const struct drm_framebuffer_funcs exynos7580_fb_funcs = {
	.destroy	= exynos7580_fb_destroy,
	.create_handle	= exynos7580_fb_create_handle,
	.dirty		= exynos7580_fb_dirty,
};

/* ---------- scanout address resolution ---------- */

/* Has a mapping for @plane been made already?  Returns 1 and the address. */
static int exynos7580_scanout_cached(struct exynos7580_fb *efb,
				     unsigned int plane, dma_addr_t *addr)
{
	int cached;

	mutex_lock(&exynos7580_scanout_lock);
	cached = efb->scanout[plane] != 0;
	if (cached)
		*addr = efb->scanout[plane];
	mutex_unlock(&exynos7580_scanout_lock);

	return cached;
}

/*
 * exynos7580_map_plane() - map an sg list into the DECON IOVMM domain
 *
 * The mapping is cached in the framebuffer and released in
 * exynos7580_fb_destroy(): KWin creates one fb per buffer, and the fb
 * reference held by the atomic state keeps the mapping alive until the commit
 * that stops scanning it has completed.
 */
static int exynos7580_map_plane(struct drm_framebuffer *fb, unsigned int plane,
				struct sg_table *sgt, dma_addr_t *addr)
{
	struct exynos7580_fb *efb = to_exynos7580_fb(fb);
	struct exynos7580_device *ex = to_exynos7580(fb->dev);
	dma_addr_t iova;
	size_t size;
	int ret = 0;

	if (!ex || !ex->dev || !sgt || !sgt->sgl)
		return -ENODEV;

	size = (size_t)fb->pitches[plane] * fb->height;

	mutex_lock(&exynos7580_scanout_lock);
	if (efb->scanout[plane]) {
		*addr = efb->scanout[plane];
		goto out;
	}

	iova = iovmm_map(ex->dev, sgt->sgl, fb->offsets[plane], size,
			 DMA_TO_DEVICE, 0);
	if (IS_ERR_VALUE(iova)) {
		DRM_ERROR("iovmm_map() of %ux%u fb failed: %d\n",
			  fb->width, fb->height, (int)iova);
		ret = -ENODEV;
		goto out;
	}

	efb->scanout[plane] = iova;
	*addr = iova;
out:
	mutex_unlock(&exynos7580_scanout_lock);
	return ret;
}

/*
 * exynos7580_own_scanout() - address of a plane of one of our framebuffers
 *
 * Two backing kinds share the framebuffer type:
 *   - an imported dma-buf: the exporter's sg table is in base.driver_private
 *     (see exynos7580_gem_prime_import()), already mapped on the DECON device
 *     with DMA_BIDIRECTIONAL;
 *   - a dumb buffer (drm_gem_cma_dumb_create): physically contiguous CMA
 *     memory, described by a one-entry sg table built on the fly.
 */
static int exynos7580_own_scanout(struct drm_framebuffer *fb,
				  unsigned int plane, dma_addr_t *addr)
{
	struct exynos7580_fb *efb = to_exynos7580_fb(fb);
	struct drm_gem_object *obj = &efb->cma->base;
	struct sg_table sgt;
	int ret;

	if (plane != 0)		/* only single-plane formats are accepted */
		return -EINVAL;

	if (exynos7580_scanout_cached(efb, plane, addr))
		return 0;

	if (obj->import_attach)
		return exynos7580_map_plane(fb, plane, obj->driver_private,
					    addr);

	if (!efb->cma->vaddr)
		return -ENODEV;

	ret = sg_alloc_table(&sgt, 1, GFP_KERNEL);
	if (ret)
		return -ENODEV;
	sg_set_buf(sgt.sgl, efb->cma->vaddr, obj->size);
	ret = exynos7580_map_plane(fb, plane, &sgt, addr);
	sg_free_table(&sgt);

	return ret;
}

/*
 * exynos7580_foreign_scanout() - address of a plane of an fbdev/CMA fb
 *
 * fb0 is a drm_fb_cma framebuffer over a drm_gem_cma_create() object, whose
 * paddr is a DMA-direct address and must not be programmed into a window.
 * Build a one-entry sg table over the CMA object (CMA memory is physically
 * contiguous) and map it into the DECON IOVMM domain like any other buffer.
 *
 * The mapping is cached per (fb, plane) and pins the framebuffer with a
 * reference: the CMA helper's fb has no destroy hook this file can extend, and
 * fb0 lives for the whole driver lifetime.  Cache exhaustion is -ENODEV.
 */
static int exynos7580_foreign_scanout(struct drm_framebuffer *fb,
				      unsigned int plane, dma_addr_t *addr)
{
#if IS_ENABLED(CONFIG_DRM_KMS_CMA_HELPER)
	struct drm_gem_cma_object *cma;
	struct exynos7580_device *ex;
	struct sg_table sgt;
	dma_addr_t iova;
	size_t size;
	unsigned int i, slot = EXYNOS7580_FOREIGN_MAPS;
	int ret = 0;

	if (plane >= 4 || !fb->pitches[plane])
		return -EINVAL;

	ex = to_exynos7580(fb->dev);
	if (!ex || !ex->dev)
		return -ENODEV;

	cma = drm_fb_cma_get_gem_obj(fb, plane);
	if (!cma || !cma->vaddr)
		return -ENODEV;

	size = (size_t)fb->pitches[plane] * fb->height;
	if ((unsigned long long)fb->offsets[plane] + size > cma->base.size)
		return -EINVAL;

	mutex_lock(&exynos7580_scanout_lock);
	for (i = 0; i < EXYNOS7580_FOREIGN_MAPS; i++) {
		if (exynos7580_foreign_maps[i].fb == fb &&
		    exynos7580_foreign_maps[i].plane == plane) {
			*addr = exynos7580_foreign_maps[i].addr;
			goto out;
		}
		if (!exynos7580_foreign_maps[i].fb &&
		    slot == EXYNOS7580_FOREIGN_MAPS)
			slot = i;
	}
	if (slot == EXYNOS7580_FOREIGN_MAPS) {
		DRM_ERROR("no slot left to map CMA fb %d\n", fb->base.id);
		ret = -ENODEV;
		goto out;
	}

	ret = sg_alloc_table(&sgt, 1, GFP_KERNEL);
	if (ret) {
		ret = -ENODEV;
		goto out;
	}
	sg_set_buf(sgt.sgl, cma->vaddr, cma->base.size);

	iova = iovmm_map(ex->dev, sgt.sgl, fb->offsets[plane], size,
			 DMA_TO_DEVICE, 0);
	sg_free_table(&sgt);
	if (IS_ERR_VALUE(iova)) {
		DRM_ERROR("iovmm_map() of CMA fb %d failed: %d\n",
			  fb->base.id, (int)iova);
		ret = -ENODEV;
		goto out;
	}

	exynos7580_foreign_maps[slot].fb = fb;
	exynos7580_foreign_maps[slot].plane = plane;
	exynos7580_foreign_maps[slot].addr = iova;
	drm_framebuffer_reference(fb);
	*addr = iova;
out:
	mutex_unlock(&exynos7580_scanout_lock);
	return ret;
#else
	return -ENODEV;
#endif
}

/**
 * exynos7580_fb_scanout - DMA address of a plane of @fb in the DECON IOVA space
 * @fb: framebuffer (ours, or the fbdev CMA framebuffer)
 * @plane: plane index inside @fb; only 0 is supported (single-plane formats)
 * @addr: output, valid until @fb is destroyed
 *
 * Returns 0, -EINVAL for a bad fb/plane/geometry, or -ENODEV when no scanout
 * address can be produced (not our fb and not a CMA fb, missing DECON device
 * or IOVMM domain, iovmm_map() failure).
 *
 * Process context only: the first call may sleep in iovmm_map().
 */
int exynos7580_fb_scanout(struct drm_framebuffer *fb, unsigned int plane,
			  dma_addr_t *addr)
{
	if (!fb || !addr)
		return -EINVAL;
	if (plane >= 4 || !fb->pitches[plane])
		return -EINVAL;

	if (fb->funcs == &exynos7580_fb_funcs)
		return exynos7580_own_scanout(fb, plane, addr);

	return exynos7580_foreign_scanout(fb, plane, addr);
}

/* ---------- dma-buf import ---------- */

/**
 * exynos7580_gem_prime_import - import a dma-buf as a scanout-capable GEM
 * @drm: DECON KMS device
 * @dma_buf: buffer to import (one reference held by the caller)
 *
 * Called only through dev->driver->gem_prime_import (drm_gem_prime_fd_to_handle)
 * for a dma-buf that is not already cached in the file's PRIME table.
 *
 * Object shape: struct drm_gem_cma_object with base.import_attach set and the
 * exporter's mapped sg table in base.driver_private.  The driver's
 * gem_free_object (drm_gem_cma_free_object(), made import-aware by the core
 * patch in this slice) then calls drm_prime_gem_destroy(obj,
 * obj->driver_private), which unmaps the attachment, detaches it and drops the
 * dma-buf reference taken below - no unmap/detach happens in this file after a
 * successful import.
 *
 * References: the returned object carries one GEM reference (from
 * drm_gem_object_init); drm_gem_prime_fd_to_handle() turns that into a handle
 * and unreferences it.  The attachment owns one dma_buf reference (get_dma_buf)
 * until drm_prime_gem_destroy().
 */
struct drm_gem_object *exynos7580_gem_prime_import(struct drm_device *drm,
						   struct dma_buf *dma_buf)
{
	struct drm_gem_cma_object *cma;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	size_t size;
	int ret;

	if (!drm || !drm->dev || !dma_buf || !dma_buf->size)
		return ERR_PTR(-EINVAL);

	attach = dma_buf_attach(dma_buf, drm->dev);
	if (IS_ERR(attach))
		return ERR_PTR(PTR_ERR(attach));

	/* Reference owned by the attachment, dropped by the GEM free path. */
	get_dma_buf(dma_buf);

	/*
	 * DMA_BIDIRECTIONAL: the exporter performs the cache maintenance for
	 * the DECON device here; the same sg table is later handed to
	 * iovmm_map(), which only builds SYSMMU page tables from it.
	 */
	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(sgt)) {
		ret = sgt ? PTR_ERR(sgt) : -ENOMEM;
		goto err_detach;
	}

	cma = kzalloc(sizeof(*cma), GFP_KERNEL);
	if (!cma) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	/* drm_gem_object_init() BUG_ON()s on a non-page-aligned size. */
	size = PAGE_ALIGN(dma_buf->size);
	ret = drm_gem_object_init(drm, &cma->base, size);
	if (ret) {
		kfree(cma);
		goto err_unmap;
	}

	cma->vaddr = NULL;		/* exporter-owned pages, no CPU mapping */
	cma->paddr = sg_dma_address(sgt->sgl);
	cma->base.import_attach = attach;
	cma->base.driver_private = sgt;

	DRM_DEBUG_PRIME("imported %zu bytes, %d sg entries\n",
			dma_buf->size, sgt->nents);

	return &cma->base;

err_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);

	return ERR_PTR(ret);
}

/* ---------- framebuffer creation (ADDFB2) ---------- */

/**
 * exynos7580_fb_create - create a framebuffer from a GEM handle
 * @drm: DECON KMS device
 * @file: client that owns @mode_cmd->handles[]
 * @mode_cmd: validated by the core for flags/width/height/pitch/handles;
 *            re-checked here against the DECON's format and layout limits
 *
 * The handle names either an imported dma-buf (exynos7580_gem_prime_import())
 * or one of the driver's CMA dumb buffers.  Both are struct drm_gem_cma_object,
 * which is the only object type this driver's GEM backend creates.
 *
 * Returns the framebuffer (one client-visible reference, plus one held by the
 * object table) or an ERR_PTR.
 */
struct drm_framebuffer *exynos7580_fb_create(struct drm_device *drm,
					     struct drm_file *file,
					     struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *obj;
	struct drm_gem_cma_object *cma;
	struct exynos7580_fb *efb;
	struct exy7580_fb_desc desc;
	int i, ret;

	if (!drm || !file || !mode_cmd)
		return ERR_PTR(-EINVAL);

	obj = drm_gem_object_lookup(drm, file, mode_cmd->handles[0]);
	if (!obj)
		return ERR_PTR(-ENOENT);

	if (obj->dev != drm) {
		ret = -EINVAL;
		goto err_obj;
	}
	cma = to_drm_gem_cma_obj(obj);

	memset(&desc, 0, sizeof(desc));
	desc.pixel_format = mode_cmd->pixel_format;
	desc.width = mode_cmd->width;
	desc.height = mode_cmd->height;
	desc.flags = mode_cmd->flags;
	for (i = 0; i < 4; i++) {
		desc.handles[i] = mode_cmd->handles[i];
		desc.pitches[i] = mode_cmd->pitches[i];
		desc.offsets[i] = mode_cmd->offsets[i];
	}
	desc.size = obj->size;
	desc.max_width = drm->mode_config.max_width ?: 4096;
	desc.max_height = drm->mode_config.max_height ?: 4096;

	ret = exy7580_fb_check(&desc);
	if (ret)
		goto err_obj;

	if (obj->import_attach && !obj->driver_private) {
		/* Wrapper without its sg table: never produced by our import. */
		ret = -EINVAL;
		goto err_obj;
	}

	efb = kzalloc(sizeof(*efb), GFP_KERNEL);
	if (!efb) {
		ret = -ENOMEM;
		goto err_obj;
	}

	efb->cma = cma;
	drm_helper_mode_fill_fb_struct(&efb->fb, mode_cmd);

	ret = drm_framebuffer_init(drm, &efb->fb, &exynos7580_fb_funcs);
	if (ret) {
		kfree(efb);
		goto err_obj;
	}

	return &efb->fb;

err_obj:
	drm_gem_object_unreference_unlocked(obj);
	return ERR_PTR(ret);
}
