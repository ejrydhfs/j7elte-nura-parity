/* SPDX-License-Identifier: GPL-2.0
 *
 * gpu31-kms: contract between the DECON KMS slices (M1/M2).
 *
 * This header is the ONLY shared interface. Each slice implements the
 * functions declared for it in its own source file and MUST NOT define or
 * reference anything else from a sibling's file. Assembly is done by the
 * integration seat.
 *
 * Ownership
 *   exynos_drm7580_main.c     KmsBoot? -> see slice map (probe/bind/card0)
 *   exynos_drm7580_crtc.c     DeconCrtc  : crtc, mode_set, link enable, PM
 *   exynos_drm7580_plane.c    DeconPlane : primary/overlay planes, formats
 *   exynos_drm7580_dsi.c      DsiEncoder : encoder + connector
 *   exynos_drm7580_dmabuf.c   KmsDmabuf  : dma-buf import -> GEM/framebuffer
 *   exynos_drm7580_fence.c    KmsFence   : vblank -> fence, flip events
 *   exynos_drm7580_fbdev.c    KmsFbdev   : drm_fb_helper emulation on KMS
 *
 * Binding rules (from KmsCoexist, non-negotiable)
 *   - match only "samsung,exynos5-decon_driver"; never the dsim node;
 *     never add DT compatibles (the DT payload stays stock).
 *   - late_initcall, not module_platform_driver.
 *   - register card0 via drm_platform_init; on any failure return an error so
 *     the vendor fbdev driver (late_initcall_sync) binds instead.
 */
#ifndef __EXYNOS_DRM7580_H__
#define __EXYNOS_DRM7580_H__

#include <drm/drmP.h>
#include <drm/drm_crtc.h>	/* 3.10 keeps connector/encoder decls here */

/* Bit flags for exynos7580_connector_state()/debug. */
#define EXYNOS7580_MAX_PLANES		3

/* Opaque handle to the vendor DECON window engine (DeconApi contract). */
struct decon_kms_dev;

/* 3.10 has no embedded drm_device (drm_dev_alloc arrived in 3.13):
 * drm_platform_init() allocates the drm_device itself, so our state hangs off
 * drm->dev_private and we must NEVER container_of() a drm_device. */
struct exynos7580_device {
	struct drm_device	*drm;	/* owned by drm_platform_init(), not us */
	struct device		*dev;
	struct decon_kms_dev	*decon;

	struct drm_crtc		crtc;
	struct drm_plane	planes[EXYNOS7580_MAX_PLANES];
	unsigned int		num_planes;

	struct drm_encoder	encoder;
	struct drm_connector	connector;
};

static inline struct exynos7580_device *to_exynos7580(struct drm_device *drm)
{
	return drm->dev_private;
}

/* ---- DeconCrtc ---------------------------------------------------------- */
/* Registers the single CRTC (atomic helper funcs). Returns 0 or -errno. */
int exynos7580_crtc_init(struct exynos7580_device *ex);
/* Called from the CRTC's mode_set/commit path; owns the DSI link enable and
 * the DECON pm_runtime reference (DsiPanel/DispPower contracts). */
int exynos7580_crtc_enable_link(struct exynos7580_device *ex);
void exynos7580_crtc_disable_link(struct exynos7580_device *ex);
/* mode_config funcs (KmsAssemble: folded in from 0200's local declarations).
 * atomic_check validates the whole state; atomic_commit arms the flip and
 * runs the CRTC/plane tail. Both are implemented by the CRTC slice. */
int exynos7580_atomic_check(struct drm_device *drm,
			    struct drm_atomic_state *state);
int exynos7580_atomic_commit(struct drm_device *drm,
			     struct drm_atomic_state *state, bool async);

/* ---- DeconPlane --------------------------------------------------------- */
/* Registers primary + overlay planes bound to the vendor window API. */
int exynos7580_planes_init(struct exynos7580_device *ex);
/* Atomic plane update path; returns 0, -EINVAL or -ENOSPC (window shortage). */
int exynos7580_plane_atomic_update(struct drm_plane *plane,
				   struct drm_plane_state *state);
void exynos7580_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *state);

/* ---- DsiEncoder --------------------------------------------------------- */
/* Registers the DSI encoder + connector (DSI type = 16, see M0 UAPI). */
int exynos7580_dsi_init(struct exynos7580_device *ex);
/* Connector helper: fills the single supported mode from the panel timings. */
int exynos7580_dsi_get_modes(struct drm_connector *connector);

/* ---- KmsDmabuf ---------------------------------------------------------- */
/* Imports a dma-buf fd as a GEM object usable as a scanout fb. */
struct drm_gem_object *exynos7580_gem_prime_import(struct drm_device *drm,
						   struct dma_buf *dma_buf);
/* Creates a DRM framebuffer from a GEM handle (ADDFB2 path). */
struct drm_framebuffer *exynos7580_fb_create(struct drm_device *drm,
					     struct drm_file *file,
					     struct drm_mode_fb_cmd2 *mode_cmd);
/* Scanout DMA address of `plane` in `fb` (imported or CMA). Returns 0 and
 * writes *addr, or -EINVAL/-ENODEV. Implemented by KmsDmabuf; used by
 * DeconPlane (windows) and KmsFbdev (fb0). */
int exynos7580_fb_scanout(struct drm_framebuffer *fb, unsigned int plane,
			  dma_addr_t *addr);

/* ---- KmsFence ----------------------------------------------------------- */
/* Arms flip events for the pending commit (vblank-armed, from M0 helper). */
void exynos7580_commit_tail(struct drm_atomic_state *state);
/* Page-flip/cursor event completion on vblank; called from the vblank IRQ. */
void exynos7580_vblank_irq(struct drm_device *drm, unsigned int pipe);

/* ---- KmsFbdev ----------------------------------------------------------- */
/* drm_fb_helper emulation on KMS; returns 0 or -errno. */
int exynos7580_fbdev_init(struct exynos7580_device *ex);

#endif /* __EXYNOS_DRM7580_H__ */
