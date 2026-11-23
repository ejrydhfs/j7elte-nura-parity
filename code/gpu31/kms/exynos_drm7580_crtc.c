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

	mutex_unlock(&crtc_lock);

	DRM_DEBUG_KMS("DECON link enabled\n");

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
	drm_vblank_off(crtc->dev, drm_crtc_index(crtc));

	/* Encoder ->disable already ran (s_stream(0)); this is idempotent. */
	exynos7580_crtc_disable_link(ex);
}

static void exynos7580_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	/* Legacy/helper fallback only; the atomic path uses enable/disable. */
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

	modeset = state->mode_changed || state->active_changed;

	/*
	 * DispPower 5.1: nothing saves DECON register state across a
	 * pm_runtime cycle, so after a link-off every commit must be a full
	 * modeset.  Forcing mode_changed here makes the core reject a
	 * plane-only commit that does not carry DRM_MODE_ATOMIC_ALLOW_MODESET
	 * (drm_atomic_check_only()) and makes crtc_set_mode() re-run
	 * mode_set() when it does, which re-programs the whole pipe.
	 */
	mutex_lock(&crtc_lock);
	if (crtc_link.needs_full_modeset && !state->mode_changed)
		state->mode_changed = true;
	mutex_unlock(&crtc_lock);

	if (state->mode_changed)
		modeset = true;

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

static const struct drm_crtc_helper_funcs exynos7580_crtc_helper_funcs = {
	.dpms		= exynos7580_crtc_dpms,
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
	/* 3.10's vtable has no flags argument; async flips are not supported. */
	return drm_atomic_helper_page_flip(crtc, fb, event, 0);
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

int exynos7580_atomic_check(struct drm_device *drm,
			    struct drm_atomic_state *state)
{
	return drm_atomic_helper_check(drm, state);
}
EXPORT_SYMBOL_GPL(exynos7580_atomic_check);

int exynos7580_atomic_commit(struct drm_device *drm,
			     struct drm_atomic_state *state, bool async)
{
	struct exynos7580_device *ex = to_exynos7580(drm);
	int i, ret;

	if (WARN_ON(!ex || !ex->decon))
		return -ENODEV;

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
		if (ret) {
			DRM_ERROR("commit aborted: DECON/DSI bring-up failed: %d\n",
				  ret);
			return ret;
		}
	}

	/* Step (b) */
	ret = drm_atomic_helper_prepare_planes(drm, state);
	if (ret)
		return ret;

	/* Step (c) */
	drm_atomic_helper_swap_state(drm, state);

	/*
	 * Step (d): KmsFence's complete tail (pre/commit/post planes, flip event
	 * arming at the next vblank, wait, cleanup and drm_atomic_state_free).
	 * M1 is synchronous: a NONBLOCK/PAGE_FLIP_EVENT request gets exactly the
	 * same tail and is completed by the event, which is what KWin requires.
	 */
	exynos7580_commit_tail(state);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos7580_atomic_commit);
