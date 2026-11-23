// SPDX-License-Identifier: GPL-2.0
/*
 * exynos_drm7580_plane.c - DECON KMS primary/overlay planes
 *
 * Part of the exynos7580 DECON DRM/KMS subdriver.  The planes are thin
 * translators: they validate one atomic plane state against the limits of the
 * vendor window engine and hand a frame built of struct decon_kms_win
 * descriptors to decon_kms_commit().  No DECON register is touched here.
 *
 * Window/plane assignment is fixed by the board DT and the vendor driver:
 *
 *   plane 0  primary   window 0  IDMA G0   (decon_fb default_win = 0)
 *   plane 1  overlay   window 1  IDMA G1
 *   plane 2  overlay   window 2  IDMA VG0  (only window with a YUV channel)
 *
 * The vendor exposes max_win = 3 on j7elte (exynos7580.dtsi:1145) although the
 * DECON block itself has MAX_DECON_WIN = 7 (decon.h:45); planes_init() only
 * registers what decon_kms_caps()->max_win allows and atomic_check() reports
 * -ENOSPC for anything beyond it, which is the signal KWin uses to drop an
 * overlay plane in its TEST_ONLY commit.
 *
 * zpos: M0's struct drm_plane_state (include/drm/drm_crtc.h) has no zpos or
 * alpha field and the backport does not create the matching properties, so
 * neither can be exposed to userspace.  Stacking order is nevertheless fixed
 * and correct: window index == plane index, primary at the bottom.  Per-plane
 * alpha is hardwired to 0xff ("no plane alpha"); per-pixel alpha is honoured
 * for the ARGB formats through the DECON blender.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>

#include <linux/exynos_decon_kms.h>

#include "exynos_drm7580.h"

/*
 * Format description as the vendor DECON sees it.
 *
 * @bpp:    bits per pixel the vendor driver uses to pick WINCON_BPPMODE and
 *          to size the "window width >= 128 bytes" check; 12 is the vendor
 *          value for NV12/NV21 (decon_core.c:1568-1578, wincon():175-208).
 * @planes: memory planes in the framebuffer (drm_format_num_planes()).
 * @alpha:  the format carries an alpha channel the DECON blender can use
 *          (decon_transp_length():425-451).
 * @yuv:    YUV420 semi-planar, only reachable through the VG channel
 *          (regs-decon.h:90-91, WINCON_BPPMODE_NV21/NV12).
 *
 * Only formats that decon_get_memory_plane_cnt() accepts and wincon() can
 * encode are listed (decon_core.c:1369-1409, 175-208).
 */
struct exynos7580_format {
	u32		fourcc;
	unsigned int	bpp;
	unsigned int	planes;
	bool		alpha;
	bool		yuv;
};

static const struct exynos7580_format exynos7580_formats[] = {
	{ DRM_FORMAT_ARGB8888, 32, 1, true,  false },
	{ DRM_FORMAT_XRGB8888, 32, 1, false, false },
	{ DRM_FORMAT_RGB565,   16, 1, false, false },
	{ DRM_FORMAT_NV12,     12, 2, false, true  },
	{ DRM_FORMAT_NV21,     12, 2, false, true  },
};

/* Direct-colour planes (G0/G1): RGB only. */
static const u32 exynos7580_rgb_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

/* The VG channel additionally has the YUV420 converters. */
static const u32 exynos7580_vg_formats[] = {
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
};

/*
 * One DMA channel per window.  Window 0 must keep the channel the bootloader
 * and the vendor driver left on it (G0): decon_kms_api.md recommends it and
 * decon_api.h documents the same rule.  The three channels are distinct, which
 * is what decon_reg_chmap_validate() requires (decon_core.c:1886-1900);
 * decon_reg_config_win_channel() encodes G0/G1 as WINCHMAP 1/2 and the VG
 * family as 0 (decon_reg_7580.c:320-346).
 */
static const enum decon_kms_idma exynos7580_plane_idma[] = {
	DECON_KMS_IDMA_G0,
	DECON_KMS_IDMA_G1,
	DECON_KMS_IDMA_VG0,
};

static unsigned int exynos7580_plane_index(struct exynos7580_device *ex,
					   struct drm_plane *plane)
{
	return plane - ex->planes;
}

static const struct exynos7580_format *
exynos7580_format_lookup(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(exynos7580_formats); i++)
		if (exynos7580_formats[i].fourcc == fourcc)
			return &exynos7580_formats[i];

	return NULL;
}

/*
 * Geometry validation shared by atomic_check() and the commit gatherer.
 * Every limit below is enforced by the vendor window code as well:
 *
 *  - no scaling: does_layer_need_scale() (decon_core.c:610-613) disables the
 *    window when dst.w/h != src.w/h;
 *  - dst.x/y must be non-negative and dst.w/h non-zero
 *    (decon_set_win_buffer(), decon_core.c:1545-1552);
 *  - a direct-colour window must be at least 128 bytes wide
 *    (decon_core.c:1580-1586);
 *  - src.f_w >= dst.w (decon_core.c:1588-1594);
 *  - direct-colour x coordinates must be aligned to the 32-bit word
 *    (decon_validate_x_alignment(), decon_core.c:551-568).
 */
static int exynos7580_check_rect(const struct exynos7580_format *fmt,
				 const struct decon_kms_frame *src,
				 const struct decon_kms_frame *dst)
{
	unsigned int align, width;

	if (src->w == 0 || src->h == 0 || dst->w == 0 || dst->h == 0)
		return -EINVAL;
	if (src->x < 0 || src->y < 0 || dst->x < 0 || dst->y < 0)
		return -EINVAL;

	/* The window engine cannot scale. */
	if (src->w != dst->w || src->h != dst->h)
		return -EINVAL;

	if (src->f_w == 0 || src->f_h == 0)
		return -EINVAL;
	if (src->f_w < (u32)src->x + src->w)
		return -EINVAL;
	if (src->f_h < (u32)src->y + src->h)
		return -EINVAL;
	if (src->f_w < dst->w)
		return -EINVAL;

	if (fmt->bpp > 12) {
		width = dst->w * fmt->bpp / 8;
		if (width < 128)
			return -EINVAL;

		align = 32 / fmt->bpp;
		if (align > 1 &&
		    ((dst->x % align) || ((dst->x + dst->w) % align)))
			return -EINVAL;
	}

	return 0;
}

/*
 * Translate one atomic plane state into a DECON window descriptor.
 *
 * @with_addr: resolve scanout addresses through exynos7580_fb_scanout().
 *             atomic_check() passes false so that a TEST_ONLY commit never
 *             touches the buffer mapping path; the commit gatherer passes
 *             true.
 */
static int exynos7580_plane_state_to_win(struct exynos7580_device *ex,
					 unsigned int win_idx,
					 struct drm_plane_state *state,
					 struct decon_kms_win *win,
					 bool with_addr)
{
	const struct exynos7580_format *fmt;
	struct drm_framebuffer *fb;
	unsigned int i, nplanes;
	int ret;

	memset(win, 0, sizeof(*win));
	win->state = DECON_KMS_WIN_DISABLED;

	if (!state || !state->crtc || !state->fb)
		return 0;

	if (state->crtc != &ex->crtc) {
		DRM_ERROR("[PLANE:%d] crtc %d is not the DECON crtc\n",
			  state->plane->base.id, state->crtc->base.id);
		return -EINVAL;
	}

	fb = state->fb;
	fmt = exynos7580_format_lookup(fb->pixel_format);
	if (!fmt) {
		DRM_ERROR("[PLANE:%d] unsupported pixel format 0x%08x\n",
			  state->plane->base.id, fb->pixel_format);
		return -EINVAL;
	}

	if (fmt->yuv && win_idx != 2) {
		DRM_ERROR("[PLANE:%d] YUV format on a non-VG window\n",
			  state->plane->base.id);
		return -EINVAL;
	}

	/* 16.16 fixed point: no fractional source position or size. */
	if ((state->src_x | state->src_y | state->src_w | state->src_h) & 0xffff)
		return -EINVAL;

	win->state = DECON_KMS_WIN_BUFFER;
	win->fourcc = fb->pixel_format;
	win->idma_type = exynos7580_plane_idma[win_idx];
	win->blending = win_idx == 0 ? DECON_KMS_BLENDING_NONE :
			(fmt->alpha ? DECON_KMS_BLENDING_PREMULT :
				      DECON_KMS_BLENDING_NONE);
	win->plane_alpha = 0xff;
	win->protection = false;

	win->src.x = state->src_x >> 16;
	win->src.y = state->src_y >> 16;
	win->src.w = state->src_w >> 16;
	win->src.h = state->src_h >> 16;
	/*
	 * The DECON works in pixels: src.f_w is the line stride of the buffer.
	 * For NV12/NV21 the vendor bpp is 12, i.e. bytes == luma pixels, which
	 * makes pitch[0] the Y stride in pixels exactly like the fbdev driver
	 * does (decon_core.c:1670-1678).
	 */
	if (fb->pitches[0] % (fmt->bpp / 8)) {
		DRM_ERROR("[PLANE:%d] pitch %u is not a whole number of pixels\n",
			  state->plane->base.id, fb->pitches[0]);
		return -EINVAL;
	}
	win->src.f_w = fb->pitches[0] / (fmt->bpp / 8);
	win->src.f_h = fb->height;

	win->dst.x = state->crtc_x;
	win->dst.y = state->crtc_y;
	win->dst.w = state->crtc_w;
	win->dst.h = state->crtc_h;
	win->dst.f_w = win->src.f_w;
	win->dst.f_h = win->src.f_h;

	ret = exynos7580_check_rect(fmt, &win->src, &win->dst);
	if (ret) {
		DRM_ERROR("[PLANE:%d] invalid %ux%u+%d+%d -> %ux%u+%d+%d\n",
			  state->plane->base.id, win->src.w, win->src.h,
			  win->src.x, win->src.y, win->dst.w, win->dst.h,
			  win->dst.x, win->dst.y);
		return ret;
	}

	if (!with_addr)
		return 0;

	nplanes = drm_format_num_planes(fb->pixel_format);
	if (nplanes > ARRAY_SIZE(win->addr))
		nplanes = ARRAY_SIZE(win->addr);

	for (i = 0; i < nplanes; i++) {
		dma_addr_t addr;

		ret = exynos7580_fb_scanout(fb, i, &addr);
		if (ret) {
			if (i == 0) {
				DRM_ERROR("[PLANE:%d] no scanout address: %d\n",
					  state->plane->base.id, ret);
				return ret;
			}
			/*
			 * Semi-planar NV12/NV21 exported as one dma-buf: leave
			 * addr[1] zero, the shim derives it from the luma
			 * stride like decon_calc_plane_offset() does
			 * (decon_core.c:1411-1431).
			 */
			win->addr[i] = 0;
			continue;
		}
		win->addr[i] = addr;
	}

	return 0;
}

/*
 * Program exactly one frame once the whole changeset has been translated.
 *
 * The atomic helper calls ->atomic_update()/->atomic_disable() for every plane
 * in the changeset in ascending plane index (drm_atomic_helper_commit_planes(),
 * drm_atomic_helper.c:1128-1190) and the new states are already installed in
 * plane->state (swap_state() ran before commit_planes).  The plane with the
 * highest index in the changeset therefore commits the complete frame, which
 * keeps the "exactly one decon_kms_commit() per atomic commit" rule.
 */
static int exynos7580_plane_commit_frame(struct exynos7580_device *ex,
					 struct drm_plane *plane,
					 struct drm_plane_state *old_state)
{
	struct drm_atomic_state *astate = old_state ? old_state->state : NULL;
	struct drm_plane_state *pstate = plane->state;
	struct decon_kms_win wins[EXYNOS7580_MAX_PLANES];
	unsigned int i, nplanes = ex->num_planes;
	int ret;

	/*
	 * The CRTC is going inactive: its disable path stops the DECON, so
	 * programming a frame here would touch a stopped pipe.
	 */
	if (!pstate || !pstate->crtc || !pstate->crtc->state)
		return 0;
	if (!pstate->crtc->state->active)
		return 0;

	/*
	 * A later plane of this changeset commits the frame.  A NULL atomic
	 * state means the transitional plane ioctl path (a single-plane
	 * update), which commits right away.
	 */
	if (astate) {
		for (i = drm_plane_index(plane) + 1;
		     i < plane->dev->mode_config.num_total_plane; i++)
			if (astate->planes[i])
				return 0;
	}

	if (nplanes == 0 || nplanes > EXYNOS7580_MAX_PLANES)
		return -ENOSPC;

	for (i = 0; i < nplanes; i++) {
		ret = exynos7580_plane_state_to_win(ex, i, ex->planes[i].state,
						    &wins[i], true);
		if (ret) {
			/*
			 * Legitimate states were accepted by atomic_check();
			 * a failure here is a driver bug, so blank that window
			 * instead of programming garbage.
			 */
			DRM_ERROR("[PLANE:%d] commit translation failed: %d\n",
				  ex->planes[i].base.id, ret);
			memset(&wins[i], 0, sizeof(wins[i]));
			wins[i].state = DECON_KMS_WIN_DISABLED;
		}
	}

	ret = decon_kms_commit(ex->decon, wins, nplanes);
	if (ret)
		DRM_ERROR("DECON commit failed: %d\n", ret);

	return ret;
}

static int exynos7580_plane_atomic_check(struct drm_plane *plane,
					 struct drm_plane_state *state)
{
	struct exynos7580_device *ex = to_exynos7580(plane->dev);
	const struct decon_kms_caps *caps =
		ex->decon ? decon_kms_caps(ex->decon) : NULL;
	struct decon_kms_win win;
	unsigned int idx = exynos7580_plane_index(ex, plane);

	if (!state->crtc && !state->fb)
		return 0;
	if (!state->crtc || !state->fb)
		return -EINVAL;

	/*
	 * Window shortage: the plane exists but the vendor instance has fewer
	 * windows than the DT advertised.  -ENOSPC makes KWin drop the plane
	 * when it tests the atomic commit.
	 */
	if (!caps || idx >= caps->max_win)
		return -ENOSPC;

	return exynos7580_plane_state_to_win(ex, idx, state, &win, false);
}

int exynos7580_plane_atomic_update(struct drm_plane *plane,
				   struct drm_plane_state *state)
{
	struct exynos7580_device *ex = to_exynos7580(plane->dev);
	unsigned int idx = exynos7580_plane_index(ex, plane);
	struct decon_kms_win win;
	int ret;

	/* @state is the old plane state; the new one is in plane->state. */
	ret = exynos7580_plane_state_to_win(ex, idx, plane->state, &win, true);
	if (ret) {
		/*
		 * -ENODEV/-EINVAL from the address resolver is final for this
		 * fb (KmsDmabuf's exynos7580_fb_scanout()), not transient.  We
		 * are past the point of no return in the atomic commit, so
		 * report it and still latch the rest of the frame; the
		 * gatherer below blanks this window.
		 */
		DRM_ERROR("[PLANE:%d] cannot build window: %d\n",
			  plane->base.id, ret);
	}

	return exynos7580_plane_commit_frame(ex, plane, state);
}

void exynos7580_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *state)
{
	struct exynos7580_device *ex = to_exynos7580(plane->dev);

	exynos7580_plane_commit_frame(ex, plane, state);
}

static void exynos7580_plane_atomic_update_cb(struct drm_plane *plane,
					      struct drm_plane_state *old_state)
{
	int ret;

	ret = exynos7580_plane_atomic_update(plane, old_state);
	if (ret)
		DRM_ERROR("[PLANE:%d] atomic update failed: %d\n",
			  plane->base.id, ret);
}

static void exynos7580_plane_atomic_disable_cb(struct drm_plane *plane,
					       struct drm_plane_state *old_state)
{
	exynos7580_plane_atomic_disable(plane, old_state);
}

static void exynos7580_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
}

static const struct drm_plane_funcs exynos7580_plane_funcs = {
	.update_plane		= drm_plane_helper_update,
	.disable_plane		= drm_plane_helper_disable,
	.destroy		= exynos7580_plane_destroy,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs exynos7580_plane_helper_funcs = {
	.atomic_check		= exynos7580_plane_atomic_check,
	.atomic_update		= exynos7580_plane_atomic_update_cb,
	.atomic_disable		= exynos7580_plane_atomic_disable_cb,
};

int exynos7580_planes_init(struct exynos7580_device *ex)
{
	const struct decon_kms_caps *caps =
		ex->decon ? decon_kms_caps(ex->decon) : NULL;
	unsigned int i, nplanes;
	int ret;

	/*
	 * One window is the minimum (the primary plane).  More planes than the
	 * vendor instance has windows are still registered: atomic_check()
	 * rejects them with -ENOSPC, which is the documented TEST_ONLY
	 * fallback that makes a compositor drop the overlay instead of
	 * failing the whole commit.
	 */
	if (!caps || caps->max_win == 0) {
		DRM_ERROR("DECON exposes no windows\n");
		return -ENOSPC;
	}

	if (!ex->drm || !ex->dev)
		return -EINVAL;

	nplanes = EXYNOS7580_MAX_PLANES;
	if (nplanes > ARRAY_SIZE(exynos7580_plane_idma))
		nplanes = ARRAY_SIZE(exynos7580_plane_idma);

	for (i = 0; i < nplanes; i++) {
		const u32 *formats;
		unsigned int count;
		enum drm_plane_type type;

		if (i == 0) {
			type = DRM_PLANE_TYPE_PRIMARY;
			formats = exynos7580_rgb_formats;
			count = ARRAY_SIZE(exynos7580_rgb_formats);
		} else {
			type = DRM_PLANE_TYPE_OVERLAY;
			/*
			 * Only the last window is the VG channel; the ones in
			 * between are G channels and take RGB only.
			 */
			if (i == 2) {
				formats = exynos7580_vg_formats;
				count = ARRAY_SIZE(exynos7580_vg_formats);
			} else {
				formats = exynos7580_rgb_formats;
				count = ARRAY_SIZE(exynos7580_rgb_formats);
			}
		}

		ret = drm_universal_plane_init(ex->drm, &ex->planes[i], BIT(0),
					       &exynos7580_plane_funcs,
					       formats, count, type);
		if (ret) {
			DRM_ERROR("failed to init plane %u: %d\n", i, ret);
			return ret;
		}

		drm_plane_helper_add(&ex->planes[i],
				     &exynos7580_plane_helper_funcs);
	}

	ex->num_planes = nplanes;

	return 0;
}
