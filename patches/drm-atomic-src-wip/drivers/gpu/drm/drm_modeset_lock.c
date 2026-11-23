/*
 * Copyright (C) 2014 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_modeset_lock.h>

/**
 * DOC: kms locking
 *
 * This is a 3.10 backport of the 3.19 modeset locking helpers.  3.10 has no
 * ww_mutex, so instead of the wound/wait algorithm the per-object locks are
 * plain mutexes and callers are required to serialize themselves with
 * &drm_mode_config.mutex (all legacy KMS entry points already do this through
 * drm_modeset_lock_all()).  The acquire-context bookkeeping is kept so that
 * the atomic core and drivers can be written against the upstream API.
 *
 * The basic usage pattern is to:
 *
 *     drm_modeset_acquire_init(&ctx)
 *   retry:
 *     foreach (lock in random_ordered_set_of_locks) {
 *       ret = drm_modeset_lock(lock, &ctx)
 *       if (ret == -EDEADLK) {
 *          drm_modeset_backoff(&ctx);
 *          goto retry;
 *       }
 *     }
 *
 *     ... do stuff ...
 *
 *     drm_modeset_drop_locks(&ctx);
 *     drm_modeset_acquire_fini(&ctx);
 */

static bool modeset_lock_is_held(struct drm_modeset_lock *lock,
		struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_modeset_lock *l;

	list_for_each_entry(l, &ctx->locked, head)
		if (l == lock)
			return true;

	return false;
}

/**
 * drm_modeset_acquire_init - initialize acquire context
 * @ctx: the acquire context
 * @flags: for future
 */
void drm_modeset_acquire_init(struct drm_modeset_acquire_ctx *ctx,
		uint32_t flags)
{
	memset(ctx, 0, sizeof(*ctx));
	INIT_LIST_HEAD(&ctx->locked);
}
EXPORT_SYMBOL(drm_modeset_acquire_init);

/**
 * drm_modeset_acquire_fini - cleanup acquire context
 * @ctx: the acquire context
 */
void drm_modeset_acquire_fini(struct drm_modeset_acquire_ctx *ctx)
{
	WARN_ON(!list_empty(&ctx->locked));
}
EXPORT_SYMBOL(drm_modeset_acquire_fini);

/**
 * drm_modeset_drop_locks - drop all locks
 * @ctx: the acquire context
 *
 * Drop all locks currently held against this acquire context.
 */
void drm_modeset_drop_locks(struct drm_modeset_acquire_ctx *ctx)
{
	WARN_ON(ctx->contended);
	while (!list_empty(&ctx->locked)) {
		struct drm_modeset_lock *lock;

		lock = list_first_entry(&ctx->locked,
				struct drm_modeset_lock, head);

		drm_modeset_unlock(lock);
	}
}
EXPORT_SYMBOL(drm_modeset_drop_locks);

static inline int modeset_lock(struct drm_modeset_lock *lock,
		struct drm_modeset_acquire_ctx *ctx,
		bool interruptible, bool slow)
{
	int ret;

	WARN_ON(ctx->contended);

	/* Same semantics as ww_mutex_lock's -EALREADY. */
	if (modeset_lock_is_held(lock, ctx))
		return 0;

	if (ctx->trylock_only) {
		if (!mutex_trylock(&lock->mutex))
			return -EBUSY;
	} else if (interruptible) {
		ret = mutex_lock_interruptible(&lock->mutex);
		if (ret)
			return ret;
	} else {
		mutex_lock(&lock->mutex);
	}

	WARN_ON(!list_empty(&lock->head));
	list_add(&lock->head, &ctx->locked);

	return 0;
}

static int modeset_backoff(struct drm_modeset_acquire_ctx *ctx,
		bool interruptible)
{
	struct drm_modeset_lock *contended = ctx->contended;

	ctx->contended = NULL;

	if (WARN_ON(!contended))
		return 0;

	drm_modeset_drop_locks(ctx);

	return modeset_lock(contended, ctx, interruptible, true);
}

/**
 * drm_modeset_backoff - deadlock avoidance backoff
 * @ctx: the acquire context
 *
 * If deadlock is detected (ie. drm_modeset_lock() returns -EDEADLK),
 * you must call this function to drop all currently held locks and
 * block until the contended lock becomes available.
 */
void drm_modeset_backoff(struct drm_modeset_acquire_ctx *ctx)
{
	modeset_backoff(ctx, false);
}
EXPORT_SYMBOL(drm_modeset_backoff);

/**
 * drm_modeset_backoff_interruptible - deadlock avoidance backoff
 * @ctx: the acquire context
 *
 * Interruptible version of drm_modeset_backoff()
 */
int drm_modeset_backoff_interruptible(struct drm_modeset_acquire_ctx *ctx)
{
	return modeset_backoff(ctx, true);
}
EXPORT_SYMBOL(drm_modeset_backoff_interruptible);

/**
 * drm_modeset_lock - take modeset lock
 * @lock: lock to take
 * @ctx: acquire ctx
 *
 * If ctx is not NULL, then the lock will be tracked by the context and can be
 * released by calling drm_modeset_drop_locks().  If -EDEADLK is returned, this
 * means a deadlock scenario has been detected and it is an error to attempt
 * to take any more locks without first calling drm_modeset_backoff().
 */
int drm_modeset_lock(struct drm_modeset_lock *lock,
		struct drm_modeset_acquire_ctx *ctx)
{
	if (ctx)
		return modeset_lock(lock, ctx, false, false);

	mutex_lock(&lock->mutex);
	return 0;
}
EXPORT_SYMBOL(drm_modeset_lock);

/**
 * drm_modeset_lock_interruptible - take modeset lock
 * @lock: lock to take
 * @ctx: acquire ctx
 *
 * Interruptible version of drm_modeset_lock()
 */
int drm_modeset_lock_interruptible(struct drm_modeset_lock *lock,
		struct drm_modeset_acquire_ctx *ctx)
{
	if (ctx)
		return modeset_lock(lock, ctx, true, false);

	return mutex_lock_interruptible(&lock->mutex);
}
EXPORT_SYMBOL(drm_modeset_lock_interruptible);

/**
 * drm_modeset_unlock - drop modeset lock
 * @lock: lock to release
 */
void drm_modeset_unlock(struct drm_modeset_lock *lock)
{
	list_del_init(&lock->head);
	mutex_unlock(&lock->mutex);
}
EXPORT_SYMBOL(drm_modeset_unlock);

/* In some legacy codepaths it's convenient to just grab all the crtc and plane
 * related locks. */
int drm_modeset_lock_all_crtcs(struct drm_device *dev,
		struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	int ret = 0;

	list_for_each_entry(crtc, &config->crtc_list, head) {
		ret = drm_modeset_lock(&crtc->modeset_lock, ctx);
		if (ret)
			return ret;
	}

	list_for_each_entry(plane, &config->plane_list, head) {
		ret = drm_modeset_lock(&plane->modeset_lock, ctx);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL(drm_modeset_lock_all_crtcs);

/**
 * drm_modeset_lock_crtc - lock crtc with hidden acquire ctx for a plane update
 * @crtc: DRM CRTC
 * @plane: DRM plane to be updated on @crtc
 *
 * This function locks the given crtc and plane (which should be either the
 * primary or cursor plane) using a hidden acquire context. This is necessary so
 * that drivers internally using the atomic interfaces can grab further locks
 * with the lock acquire context.
 *
 * Note that @plane can be NULL, e.g. when the cursor support hasn't yet been
 * converted to universal planes yet.
 */
void drm_modeset_lock_crtc(struct drm_crtc *crtc,
			   struct drm_plane *plane)
{
	struct drm_modeset_acquire_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (WARN_ON(!ctx))
		return;

	drm_modeset_acquire_init(ctx, 0);

retry:
	ret = drm_modeset_lock(&crtc->modeset_lock, ctx);
	if (ret)
		goto fail;

	if (plane) {
		ret = drm_modeset_lock(&plane->modeset_lock, ctx);
		if (ret)
			goto fail;

		if (plane->crtc) {
			ret = drm_modeset_lock(&plane->crtc->modeset_lock, ctx);
			if (ret)
				goto fail;
		}
	}

	WARN_ON(crtc->acquire_ctx);

	/* now we hold the locks, so now that it is safe, stash the
	 * ctx for drm_modeset_unlock_crtc():
	 */
	crtc->acquire_ctx = ctx;

	return;

fail:
	if (ret == -EDEADLK) {
		drm_modeset_backoff(ctx);
		goto retry;
	}
}
EXPORT_SYMBOL(drm_modeset_lock_crtc);

/**
 * drm_modeset_legacy_acquire_ctx - find acquire ctx for legacy ioctls
 * @crtc: drm crtc
 *
 * Legacy ioctl operations like cursor updates or page flips only have per-crtc
 * locking, and store the acquire ctx in the corresponding crtc. All other
 * legacy operations take all locks and use a global acquire context. This
 * function grabs the right one.
 */
struct drm_modeset_acquire_ctx *
drm_modeset_legacy_acquire_ctx(struct drm_crtc *crtc)
{
	if (crtc->acquire_ctx)
		return crtc->acquire_ctx;

	WARN_ON(!crtc->dev->mode_config.acquire_ctx);

	return crtc->dev->mode_config.acquire_ctx;
}
EXPORT_SYMBOL(drm_modeset_legacy_acquire_ctx);

/**
 * drm_modeset_unlock_crtc - drop crtc lock
 * @crtc: drm crtc
 *
 * This drops the crtc lock acquire with drm_modeset_lock_crtc() and all other
 * locks acquired through the hidden context.
 */
void drm_modeset_unlock_crtc(struct drm_crtc *crtc)
{
	struct drm_modeset_acquire_ctx *ctx = crtc->acquire_ctx;

	if (WARN_ON(!ctx))
		return;

	crtc->acquire_ctx = NULL;
	drm_modeset_drop_locks(ctx);
	drm_modeset_acquire_fini(ctx);

	kfree(ctx);
}
EXPORT_SYMBOL(drm_modeset_unlock_crtc);
