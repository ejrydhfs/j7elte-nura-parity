// SPDX-License-Identifier: GPL-2.0
/*
 * exynos_drm7580_crtc.c - DECON KMS CRTC, link enable and modeset plumbing
 *
 * Part of the exynos7580 DECON DRM/KMS subdriver (slice DeconCrtc of
 * docs/reports/decon-kms-plan.md).  One CRTC, no cursor plane, no scaler.
 *
 * OWNERSHIP CONTRACT (cited by the integration seat and by DeconPlane)
 * ---------------------------------------------------------------
 * 1. DeconPlane owns the single decon_kms_commit() of an atomic commit: it
 *    builds wins[] from the committed plane states in its last-plane hook,
 *    which runs inside commit_planes, i.e. between atomic_begin() and
 *    atomic_flush() and before exynos7580_commit_tail() arms the flip event.
 *    This file NEVER commits a frame; atomic_flush() only re-asserts the link.
 * 2. This file owns the fallible display bring-up.  It runs in
 *    mode_config.funcs.atomic_commit() BEFORE drm_atomic_helper_swap_state(),
 *    because the M0 helper ignores crtc_funcs->mode_set()'s return value
 *    (drm_atomic_helper.c, crtc_set_mode()): a DSI/panel failure there could
 *    not abort the commit.  crtc_funcs->mode_set() then just re-asserts the
 *    (idempotent) link, which keeps the documented ordering "link before
 *    decon_reg_init()" (docs/reports/decon-kms-dsi.md 2.3).
 * 3. atomic_check() requires the primary plane to be part of any commit that
 *    activates the CRTC or needs a modeset, so a plane-driven commit cannot
 *    miss the frame that starts scanout.
 *
 * ATOMIC COMMIT ORDER (agreed with KmsFence, see docs/reports/decon-kms-crtc.md)
 *   a. this file: enable_link() for every CRTC that becomes active
 *      (pm_runtime_get + DSIM s_stream(1) + decon_reg_init/reg_start)
 *   b. drm_atomic_helper_prepare_planes()
 *   c. drm_atomic_helper_swap_state()
 *   d. exynos7580_commit_tail() (KmsFence): pre/commit/post planes, event
 *      arming at the next vblank, wait, cleanup, free(state)
 * Non-blocking and async requests run the same tail (M1 is synchronous).
 *
 * LOCKING (why the order below cannot deadlock)
 * ---------------------------------------------
 * 3.10 has no ww_mutex; M0 serialises atomic commits with the single
 * drm_device.mode_config.mutex (drm_modeset_lock_all(), taken by the ioctl
 * layer before atomic_check/atomic_commit and held across them).
 *
 *   mode_config.mutex  ->  crtc_lock  ->  decon->output_lock -> decon->mutex
 *
 * - mode_config.mutex is only ever taken by DRM core callers *above* this
 *   file; nothing here takes it, so it can never be acquired while crtc_lock
 *   or a DECON lock is held: the outer edge of the graph is acyclic.
 * - crtc_lock (file scope) protects the link/modeset bookkeeping.  It is
 *   taken in enable_link()/disable_link()/atomic_check() only, and the only
 *   locks taken inside it are the vendor ones via decon_kms_enable/disable()
 *   (output_lock); no DRM lock and no callback into DRM is made under it.
 * - decon->output_lock serialises the window API (taken by decon_enable(),
 *   decon_disable() and decon_kms_commit()), decon->mutex is pm_runtime's.
 *   The vendor code never calls back into DRM, so both are leaves.
 * The order is therefore a strict hierarchy with no cycle, and no path takes
 * crtc_lock while holding a DECON lock (the vendor callbacks under
 * output_lock never reach this file).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

#include <linux/exynos_decon_kms.h>

#include "exynos_drm7580.h"

/*
 * Link/power bookkeeping of the single CRTC.
 *
 * DECON is a single-instance driver (decon_api.h layer B resolves every
 * register call through one global context) and this header gives the CRTC no
 * private pointer of its own, so the state of the one CRTC lives here.
 *
 * @on:                  DSI link + DECON are enabled and programmed.
 * @needs_full_modeset:  set whenever the DECON lost its register state (link
 *                       off, i.e. after any pm_runtime cycle).  DispPower 5.1:
 *                       nothing saves DECON state across a power cycle, so the
 *                       next commit must re-run decon_reg_init()/reg_start().
 */
struct exynos7580_crtc_link {
	bool	on;
	bool	needs_full_modeset;
};

static struct exynos7580_crtc_link crtc_link;
static DEFINE_MUTEX(crtc_lock);

static struct exynos7580_device *crtc_to_ex(struct drm_crtc *crtc)
{
	return to_exynos7580(crtc->dev);
}

/* ------------------------------------------------------------------------ */
/* link / power                                                             */
/* ------------------------------------------------------------------------ */

int exynos7580_crtc_enable_link(struct exynos7580_device *ex)
{
	int ret;

	if (!ex || !ex->decon)
		return -ENODEV;

	mutex_lock(&crtc_lock);

	if (crtc_link.on) {
		/* The link is up, so whatever asked for a full modeset has been
		 * satisfied already; a stale flag must not leak into the next
		 * atomic_check(). */
		if (crtc_link.needs_full_modeset) {
			pr_info("exynos7580_crtc_enable_link: link already on, clearing stale needs_full_modeset\n");
			crtc_link.needs_full_modeset = false;
		}
		mutex_unlock(&crtc_lock);
		return 0;
	}

	/*
	 * DispPower 6.4 order: take the DECON runtime reference first (pd_disp
	 * + CMU_DISP are one resource), then the DSIM link and the panel, then
	 * the DECON controller.  decon_kms_enable() is the vendor
	 * decon_enable() sequence and its only fallible step is the DSIM
	 * s_stream(1) call, so its error is what a failed modeset reports.
	 */
	ret = decon_kms_power_get(ex->decon);
	if (ret < 0) {
		DRM_ERROR("DECON power get failed: %d\n", ret);
		mutex_unlock(&crtc_lock);
		return ret;
	}

	ret = decon_kms_enable(ex->decon);
	if (ret) {
		DRM_ERROR("DECON enable (DSI link) failed: %d\n", ret);
		decon_kms_power_put(ex->decon);
		mutex_unlock(&crtc_lock);
		return ret;
	}

	crtc_link.on = true;
	/* DispPower 5.1 is satisfied: DECON was re-initialised by
	 * decon_kms_enable() above, so the next commit no longer needs a full
	 * modeset.  Clearing here (and not only in crtc->mode_set) also covers
	 * the atomic_commit() enable path, which never calls mode_set(). */
	crtc_link.needs_full_modeset = false;

	mutex_unlock(&crtc_lock);

	pr_info("exynos7580_crtc_enable_link: link on, needs_full_modeset cleared\n");

	return 0;
}

void exynos7580_crtc_disable_link(struct exynos7580_device *ex)
{
	if (!ex || !ex->decon)
		return;

	/*
	 * Called twice per modeset-off: once from the encoder's ->disable
	 * (drm_crtc_helper.c:288-294) and once from the CRTC's ->disable
	 * (:296-306).  Both decon_kms_disable() and this guard are idempotent.
	 */
	mutex_lock(&crtc_lock);

	if (!crtc_link.on) {
		mutex_unlock(&crtc_lock);
		return;
	}

	pr_info("exynos7580_crtc_disable_link: disabling link state (by %s)\n",
		current->comm);
	decon_kms_disable(ex->decon);	/* s_stream(0) then reg_stop, vendor order */
	decon_kms_power_put(ex->decon);

	crtc_link.on = false;
	crtc_link.needs_full_modeset = true;

	mutex_unlock(&crtc_lock);

	DRM_DEBUG_KMS("DECON link disabled\n");
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Does this commit have to bring the DECON up (and therefore has to be a full
 * modeset)?  True when the CRTC becomes active or its mode changes.
 */
static bool exynos7580_crtc_enabling(struct drm_crtc_state *state,
				     struct drm_crtc_state *old)
{
	if (!state->active || !state->enable)
		return false;
	if (!old || !old->active)
		return true;

	return state->mode_changed;
}

/* Is @primary part of this atomic commit? */
static bool exynos7580_crtc_state_has_plane(struct drm_atomic_state *astate,
					    struct drm_plane *plane)
{
	int i, n = astate->dev->mode_config.num_total_plane;

	for (i = 0; i < n; i++)
		if (astate->planes[i] == plane)
			return true;

	return false;
}

/* ------------------------------------------------------------------------ */
/* crtc helper funcs                                                        */
/* ------------------------------------------------------------------------ */

static bool exynos7580_crtc_mode_fixup(struct drm_crtc *crtc,
				       const struct drm_display_mode *mode,
				       struct drm_display_mode *adjusted_mode)
{
	/*
	 * There is no scaler: DECON scans out exactly the timing the DSIM/panel
	 * was programmed with (decon->lcd_info, filled by the vendor DSIM
	 * subdev).  The DRM mode is informational for the DECON side, so any
	 * mode the connector accepted is passed through unchanged.
	 */
	drm_mode_copy(adjusted_mode, mode);

	return true;
}

static int exynos7580_crtc_mode_set(struct drm_crtc *crtc,
				    struct drm_display_mode *mode,
				    struct drm_display_mode *adjusted_mode,
				    int x, int y,
				    struct drm_framebuffer *old_fb)
{
	struct exynos7580_device *ex = crtc_to_ex(crtc);
	int idx = drm_crtc_index(crtc);
	int ret;

	DRM_DEBUG_KMS("[CRTC:%d] modeset %ux%u@%u\n", crtc->base.id,
		      adjusted_mode->hdisplay, adjusted_mode->vdisplay,
		      drm_mode_vrefresh(adjusted_mode));

	drm_vblank_pre_modeset(crtc->dev, idx);

	/*
	 * The fallible step, before any window register is touched:
	 * pm_runtime_get_sync -> DSIM s_stream(1) (rails, D-PHY, panel reset
	 * and init) -> iovmm_activate -> decon_reg_init -> decon_reg_start.
	 * Normal commits already did this in atomic_commit(); this call is the
	 * idempotent re-assert that keeps the documented link-before-reg_init
	 * ordering for every legacy/helper path too.
	 */
	ret = exynos7580_crtc_enable_link(ex);
	if (ret) {
		DRM_ERROR("[CRTC:%d] DSI link enable failed: %d\n",
			  crtc->base.id, ret);
		drm_vblank_post_modeset(crtc->dev, idx);
		return ret;
	}

	mutex_lock(&crtc_lock);
	crtc_link.needs_full_modeset = false;
	mutex_unlock(&crtc_lock);

	drm_vblank_post_modeset(crtc->dev, idx);

	return 0;
}

static void exynos7580_crtc_enable(struct drm_crtc *crtc)
{
	struct exynos7580_device *ex = crtc_to_ex(crtc);

	/* commit_post_planes(): the modeset already ran in mode_set(). */
	if (exynos7580_crtc_enable_link(ex))
		DRM_ERROR("[CRTC:%d] link enable failed\n", crtc->base.id);
}

static void exynos7580_crtc_disable(struct drm_crtc *crtc)
{
	struct exynos7580_device *ex = crtc_to_ex(crtc);

	/*
	 * Drop the vblank reference/armed events before the pipe goes away
	 * (3.10 API: there is no drm_crtc_vblank_off()).  KmsFence armed any
	 * pending flip event with drm_vblank_get(); drm_vblank_off() makes the
	 * core drop those references so a disable cannot leave the vblank
	 * machinery on.
	 */
	pr_info("[CRTC:%d] crtc_disable by %s\n", crtc->base.id, current->comm);
	drm_vblank_off(crtc->dev, drm_crtc_index(crtc));

	/* Encoder ->disable already ran (s_stream(0)); this is idempotent. */
	exynos7580_crtc_disable_link(ex);
}

static void exynos7580_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	/* Legacy/helper fallback only; the atomic path uses enable/disable. */
	pr_info("[CRTC:%d] dpms mode=%d by %s\n", crtc->base.id, mode,
		current->comm);
	if (mode == DRM_MODE_DPMS_ON)
		exynos7580_crtc_enable(crtc);
	else
		exynos7580_crtc_disable(crtc);
}

static int exynos7580_crtc_atomic_check(struct drm_crtc *crtc,
					struct drm_crtc_state *state)
{
	struct drm_atomic_state *astate = state->state;
	bool modeset;

	if (WARN_ON(!astate))
		return -EINVAL;

	/* "active without enabled" is a core error, checked before us, but a
	 * CRTC cannot be enabled without being active either. */
	if (state->enable && !state->active)
		return -EINVAL;

	if (!state->active)
		return 0;

	/*
	 * int91: the link flags are only a hint for THIS commit.  The old code
	 * forced mode_changed whenever needs_full_modeset was set and never
	 * cleared it outside crtc->mode_set(), so a stale flag turned every
	 * later plane-only commit into a full modeset and drm_atomic_check_only()
	 * rejected it with -EINVAL when the client (KWin) did not pass
	 * DRM_MODE_ATOMIC_ALLOW_MODESET.
	 *
	 * Decide from the real transition:
	 *  - activating (inactive -> active): the commit already calls
	 *    enable_link() (exynos7580_crtc_enabling()), so no forcing;
	 *  - mode/active change: already a full modeset;
	 *  - link really off while staying active (disable/pm cycle): DECON
	 *    registers are gone, so this commit must re-init the pipe.  Only
	 *    legal with ALLOW_MODESET; without it the commit cannot be served
	 *    safely and is rejected (the log names link_off);
	 *  - link on: a stale needs_full_modeset cannot force anything; the
	 *    next enable_link()/mode_set() clears it.
	 */
	{
		bool link_on, needs, activating, forced = false;

		mutex_lock(&crtc_lock);
		link_on = crtc_link.on;
		needs = crtc_link.needs_full_modeset;
		mutex_unlock(&crtc_lock);

		activating = state->active && state->enable &&
			     (!crtc->state || !crtc->state->active);
		modeset = state->mode_changed || state->active_changed ||
			  activating;

		if (!modeset && needs && !link_on) {
			if (!astate->allow_modeset) {
				pr_info("[CRTC:%d] atomic_check: link_off plane-only commit without ALLOW_MODESET -> -EINVAL (needs_full_modeset=%d link_on=%d)\n",
					crtc->base.id, needs, link_on);
				return -EINVAL;
			}
			state->mode_changed = true;
			modeset = true;
			forced = true;
		}
		pr_info("[CRTC:%d] atomic_check: needs_full_modeset=%d link_on=%d activating=%d mode_changed=%d active_changed=%d allow_modeset=%d modeset=%d%s\n",
			crtc->base.id, needs, link_on, activating,
			state->mode_changed, state->active_changed,
			astate->allow_modeset, modeset,
			forced ? " (forced: link_off)" : "");
	}

	/*
	 * DeconPlane commits the frame from its last plane hook, so a commit
	 * that activates the CRTC but carries no plane would leave the pipe
	 * running with no window and no frame ever programmed.  Every real
	 * client (modetest -s, kmscube, KWin) sends the primary plane with the
	 * modeset; reject the pathological case instead of scanning out black.
	 */
	if (modeset && !exynos7580_crtc_state_has_plane(astate, crtc->primary)) {
		DRM_DEBUG_KMS("[CRTC:%d] modeset without the primary plane\n",
			      crtc->base.id);
		return -EINVAL;
	}

	return 0;
}

static void exynos7580_crtc_atomic_begin(struct drm_crtc *crtc)
{
	/*
	 * The DECON commit engine opens and closes the window shadow protection
	 * itself (decon_export_update_regs() -> __decon_update_regs()), so there
	 * is nothing to protect here.
	 */
	DRM_DEBUG_KMS("[CRTC:%d] frame begin\n", crtc->base.id);
}

static void exynos7580_crtc_atomic_flush(struct drm_crtc *crtc)
{
	struct exynos7580_device *ex = crtc_to_ex(crtc);

	/*
	 * No decon_kms_commit() here - DeconPlane issued it from
	 * commit_planes(), which runs between atomic_begin() and this hook.
	 * Only re-assert the link so a commit that was not a modeset cannot
	 * leave the pipe disabled (e.g. after a failed earlier enable) and log
	 * the frame for bring-up.
	 */
	if (crtc->state && crtc->state->active && exynos7580_crtc_enable_link(ex))
		DRM_ERROR("[CRTC:%d] link re-assert failed\n", crtc->base.id);

	DRM_DEBUG_KMS("[CRTC:%d] frame flush\n", crtc->base.id);
}

/*
 * int69: the 3.10 fbdev path calls these unconditionally:
 *   - drm_fb_helper_setcmap() ends with crtc_funcs->load_lut(crtc);
 *   - drm_fb_helper_restore_lut_atomic()/drm_mode_gamma_set_ioctl() use
 *     crtc->funcs->gamma_set (the ioctl rejects a NULL one, the restore path
 *     checks it, but a no-op makes both harmless).
 * The fbdev palette is a pseudo-palette in truecolor mode, so there is no
 * hardware LUT to program: both are logging no-ops.
 */
static void exynos7580_crtc_load_lut(struct drm_crtc *crtc)
{
	DRM_DEBUG_KMS("load_lut (no HW LUT, pseudo-palette only)\n");
}

static void exynos7580_crtc_gamma_set(struct drm_crtc *crtc, u16 *r,
				      u16 *g, u16 *b, uint32_t start,
				      uint32_t size)
{
	DRM_DEBUG_KMS("gamma_set start=%u size=%u (no HW LUT)\n", start, size);
}

static const struct drm_crtc_helper_funcs exynos7580_crtc_helper_funcs = {
	.dpms		= exynos7580_crtc_dpms,
	.load_lut	= exynos7580_crtc_load_lut,
	.mode_fixup	= exynos7580_crtc_mode_fixup,
	.mode_set	= exynos7580_crtc_mode_set,
	.disable	= exynos7580_crtc_disable,
	.enable		= exynos7580_crtc_enable,
	.atomic_check	= exynos7580_crtc_atomic_check,
	.atomic_begin	= exynos7580_crtc_atomic_begin,
	.atomic_flush	= exynos7580_crtc_atomic_flush,
};

/* ------------------------------------------------------------------------ */
/* crtc funcs                                                               */
/* ------------------------------------------------------------------------ */

static int exynos7580_crtc_page_flip(struct drm_crtc *crtc,
				     struct drm_framebuffer *fb,
				     struct drm_pending_vblank_event *event)
{
	struct drm_device *dev = crtc->dev;
	struct drm_plane *plane = crtc->primary;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	struct drm_plane_state *plane_state;
	int ret;

	/*
	 * A local acquire context, not the legacy global one: the global
	 * context never drops the per-object modeset locks after a successful
	 * commit, so the first flip worked and the second one blocked forever
	 * in drm_atomic_get_crtc_state().  This mirrors the atomic ioctl and
	 * releases every lock it took when it returns.
	 */
	drm_modeset_acquire_init(&ctx, 0);

retry:
	state = drm_atomic_state_alloc(dev);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto fail;
	}
	crtc_state->event = event;

	plane_state = drm_atomic_get_plane_state(state, plane);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto fail;
	}
	ret = drm_atomic_set_crtc_for_plane(plane_state, crtc);
	if (ret)
		goto fail;
	drm_atomic_set_fb_for_plane(plane_state, fb);

	/* Full-frame source/destination from the current mode. */
	{
		struct drm_display_mode *mode = &crtc->state->mode;

		plane_state->src_x = 0;
		plane_state->src_y = 0;
		plane_state->src_w = mode->hdisplay << 16;
		plane_state->src_h = mode->vdisplay << 16;
		plane_state->crtc_x = 0;
		plane_state->crtc_y = 0;
		plane_state->crtc_w = mode->hdisplay;
		plane_state->crtc_h = mode->vdisplay;
	}

	pr_info("exynos7580_page_flip: state=%p crtc event=%p fb=%p\n",
		state, crtc_state->event, fb);

	crtc->fb = fb;	/* legacy pre-check in drm_mode_page_flip_ioctl */

	ret = drm_atomic_async_commit(state);
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	if (ret)
		goto fail;

	plane->fb = plane->state->fb;
	crtc->fb = plane->state->fb;
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return 0;

fail:
	drm_atomic_state_free(state);
out:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return ret;
}

static void exynos7580_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static const struct drm_crtc_funcs exynos7580_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= exynos7580_crtc_page_flip,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.gamma_set		= exynos7580_crtc_gamma_set,
	.destroy		= exynos7580_crtc_destroy,
};

/* ------------------------------------------------------------------------ */
/* init                                                                     */
/* ------------------------------------------------------------------------ */

int exynos7580_crtc_init(struct exynos7580_device *ex)
{
	struct drm_device *drm;
	struct drm_crtc *crtc;
	int ret;

	if (!ex || !ex->drm)
		return -EINVAL;
	if (!ex->decon)
		return -ENODEV;

	/*
	 * The primary plane must exist before the CRTC: drm_crtc_init_with_planes()
	 * links them and derives possible_crtcs.  exynos7580_planes_init() is
	 * therefore required to run first (integration order, documented).
	 */
	if (!ex->num_planes)
		return -EINVAL;

	drm = ex->drm;
	crtc = &ex->crtc;

	crtc_link.on = false;
	crtc_link.needs_full_modeset = true;

	ret = drm_crtc_init_with_planes(drm, crtc, &ex->planes[0], NULL,
					&exynos7580_crtc_funcs);
	if (ret) {
		DRM_ERROR("drm_crtc_init_with_planes failed: %d\n", ret);
		return ret;
	}

	drm_crtc_helper_add(crtc, &exynos7580_crtc_helper_funcs);

	/*
	 * int80: the legacy gamma ioctls require a non-zero gamma_size;
	 * without it drm_mode_gamma_set_ioctl returns -EINVAL ("failed to set
	 * gamma: Invalid argument").  The CRTC has no hardware LUT (truecolor),
	 * so a 256-entry software store is enough for the ioctl to succeed and
	 * .gamma_set/.load_lut stay no-ops.
	 */
	if (!crtc->gamma_size) {
		crtc->gamma_size = 256;
		crtc->gamma_store = kzalloc(crtc->gamma_size * 3 * sizeof(u16),
					    GFP_KERNEL);
	}

	/*
	 * drm_mode_config_reset() normally allocates the initial CRTC state; do
	 * it here as well so a driver that never calls it cannot crash the
	 * atomic helpers on a NULL crtc->state.
	 */
	if (!crtc->state)
		drm_atomic_helper_crtc_reset(crtc);
	if (!crtc->state)
		return -ENOMEM;

	DRM_INFO("DECON CRTC registered (index %d, atomic)\n",
		 drm_crtc_index(crtc));

	return 0;
}

/* ------------------------------------------------------------------------ */
/* drm_device mode_config ops                                               */
/* ------------------------------------------------------------------------ */

/*
 * These two are the drivers' mode_config.funcs.atomic_check/atomic_commit.
 * They are the only symbols this file exports beyond the header contract:
 * the header assigns the CRTC to this file but has no accessor for the
 * mode_config function table, which the main slice has to fill (it also owns
 * fb_create).  Reported to the integration seat; no header edit was made.
 */

/* int90 diagnostic: one line per plane/crtc in a commit state. */
static void exynos7580_dump_atomic_state(const char *where,
					 struct drm_device *drm,
					 struct drm_atomic_state *state)
{
	int i;

	pr_info("exynos7580_atomic[%s]: state=%p allow_modeset=%d nplanes=%d ncrtcs=%d by %s\n",
		where, state, state->allow_modeset,
		drm->mode_config.num_total_plane, drm->mode_config.num_crtc,
		current->comm);
	for (i = 0; i < drm->mode_config.num_total_plane; i++) {
		struct drm_plane *p = state->planes[i];
		struct drm_plane_state *ps = state->plane_states[i];

		if (!p)
			continue;
		pr_info("  plane[%d] id=%d crtc=%p fb=%p fmt=0x%08x src=(0x%x,0x%x,0x%x,0x%x) dst=%ux%u%+d%+d\n",
			i, p->base.id, ps->crtc, ps->fb,
			ps->fb ? ps->fb->pixel_format : 0,
			ps->src_x, ps->src_y, ps->src_w, ps->src_h,
			ps->crtc_w, ps->crtc_h, ps->crtc_x, ps->crtc_y);
	}
	for (i = 0; i < drm->mode_config.num_crtc; i++) {
		struct drm_crtc *c = state->crtcs[i];
		struct drm_crtc_state *cs = state->crtc_states[i];

		if (!c)
			continue;
		pr_info("  crtc[%d] id=%d active=%d enable=%d mode_changed=%d active_changed=%d planes_changed=%d mode_blob=%p\n",
			i, c->base.id, cs->active, cs->enable,
			cs->mode_changed, cs->active_changed,
			cs->planes_changed, cs->mode_blob);
	}
}

int exynos7580_atomic_check(struct drm_device *drm,
			    struct drm_atomic_state *state)
{
	int ret;

	exynos7580_dump_atomic_state("check-enter", drm, state);
	ret = drm_atomic_helper_check(drm, state);
	pr_info("exynos7580_atomic_check: drm_atomic_helper_check rc=%d\n", ret);
	if (ret)
		exynos7580_dump_atomic_state("check-failed", drm, state);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos7580_atomic_check);

int exynos7580_atomic_commit(struct drm_device *drm,
			     struct drm_atomic_state *state, bool async)
{
	struct exynos7580_device *ex = to_exynos7580(drm);
	int i, ret;

	if (WARN_ON(!ex || !ex->decon))
		return -ENODEV;

	exynos7580_dump_atomic_state("commit-enter", drm, state);

	/*
	 * Step (a): bring the display up before the software state is swapped,
	 * so a failing DSI link / panel init aborts the commit cleanly and
	 * userspace can report it.  Every later step can no longer fail.
	 */
	for (i = 0; i < drm->mode_config.num_crtc; i++) {
		struct drm_crtc *crtc = state->crtcs[i];
		struct drm_crtc_state *cs = state->crtc_states[i];

		if (!crtc || !cs)
			continue;
		if (!exynos7580_crtc_enabling(cs, crtc->state))
			continue;

		ret = exynos7580_crtc_enable_link(ex);
		pr_info("exynos7580_atomic_commit: enable_link rc=%d\n", ret);
		if (ret) {
			DRM_ERROR("commit aborted: DECON/DSI bring-up failed: %d\n",
				  ret);
			return ret;
		}
	}

	/* Step (b) */
	ret = drm_atomic_helper_prepare_planes(drm, state);
	pr_info("exynos7580_atomic_commit: prepare_planes rc=%d\n", ret);
	if (ret) {
		exynos7580_dump_atomic_state("prepare-failed", drm, state);
		return ret;
	}

	/* Step (c) */
	drm_atomic_helper_swap_state(drm, state);
	pr_info("exynos7580_atomic_commit: swap_state done\n");

	/*
	 * Step (d): KmsFence's complete tail (pre/commit/post planes, flip event
	 * arming at the next vblank, wait, cleanup and drm_atomic_state_free).
	 * M1 is synchronous: a NONBLOCK/PAGE_FLIP_EVENT request gets exactly the
	 * same tail and is completed by the event, which is what KWin requires.
	 */
	pr_info("exynos7580_atomic_commit: async=%d crtc->fb=%p primary->state->fb=%p by %s\n",
		async, drm->mode_config.crtc_list.next ?
			container_of(drm->mode_config.crtc_list.next,
				     struct drm_crtc, head)->fb : NULL,
		drm->mode_config.crtc_list.next ?
			container_of(drm->mode_config.crtc_list.next,
				     struct drm_crtc, head)->primary->state ?
			container_of(drm->mode_config.crtc_list.next,
				     struct drm_crtc, head)->primary->state->fb : NULL : NULL,
		current->comm);
	exynos7580_commit_tail(state);
	pr_info("exynos7580_atomic_commit: commit_tail done\n");

	/*
	 * int82: the 3.10 legacy fields are still consulted by the core:
	 * drm_mode_page_flip_ioctl() returns -EBUSY when crtc->fb is NULL, and
	 * drm_crtc_helper paths read crtc->fb.  The atomic commit path does not
	 * maintain them, so sync them here (plane->fb is handled by the
	 * helper's page_flip TODO; do it for all cases).
	 */
	{
		struct drm_crtc *c;

		list_for_each_entry(c, &drm->mode_config.crtc_list, head) {
			struct drm_plane *p = c->primary;

			if (p && p->state) {
				p->fb = p->state->fb;
				c->fb = p->state->fb;
			}
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(exynos7580_atomic_commit);
