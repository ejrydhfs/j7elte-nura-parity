// SPDX-License-Identifier: GPL-2.0
/*
 * drm_syncobj minimal subset implementation (Nura 3.10.108).
 * Binary-payload only. No timeline points, no sync_file fd passing.
 */

#include "drm_syncobj.h"
#include <linux/slab.h>
#include <linux/module.h>

void drm_syncobj_manager_init(struct drm_syncobj_manager *mgr)
{
	idr_init(&mgr->idr);
	mutex_init(&mgr->lock);
}
EXPORT_SYMBOL(drm_syncobj_manager_init);

void drm_syncobj_manager_fini(struct drm_syncobj_manager *mgr)
{
	struct drm_syncobj *syncobj;
	int id;

	/*
	 * Drain via idr_get_next (review F1): idr_for_each_entry is not
	 * safe against removal of the current entry, so look up the
	 * lowest remaining id, remove it, and repeat until empty.
	 * Safe under mgr->lock; each removed entry drops its owner ref.
	 */
	for (;;) {
		mutex_lock(&mgr->lock);
		id = 0;
		syncobj = idr_get_next(&mgr->idr, &id);
		if (syncobj)
			idr_remove(&mgr->idr, id);
		mutex_unlock(&mgr->lock);
		if (!syncobj)
			break;
		drm_syncobj_put(syncobj);
	}
	idr_destroy(&mgr->idr);
}
EXPORT_SYMBOL(drm_syncobj_manager_fini);

int drm_syncobj_create(struct drm_syncobj_manager *mgr, u32 *handle)
{
	struct drm_syncobj *syncobj;
	int ret;

	syncobj = kzalloc(sizeof(*syncobj), GFP_KERNEL);
	if (!syncobj)
		return -ENOMEM;

	kref_init(&syncobj->refcount);
	mutex_init(&syncobj->lock);
	syncobj->fence = NULL;

	idr_preload(GFP_KERNEL);
	mutex_lock(&mgr->lock);
	ret = idr_alloc(&mgr->idr, syncobj, 1, 0, GFP_NOWAIT);
	mutex_unlock(&mgr->lock);
	idr_preload_end();

	if (ret < 0) {
		kfree(syncobj);
		return ret;
	}

	*handle = (u32)ret;
	return 0;
}
EXPORT_SYMBOL(drm_syncobj_create);

int drm_syncobj_destroy(struct drm_syncobj_manager *mgr, u32 handle)
{
	struct drm_syncobj *syncobj;

	mutex_lock(&mgr->lock);
	syncobj = idr_find(&mgr->idr, (int)handle);
	if (syncobj)
		idr_remove(&mgr->idr, (int)handle);
	mutex_unlock(&mgr->lock);

	if (!syncobj)
		return -ENOENT;

	drm_syncobj_put(syncobj);
	return 0;
}
EXPORT_SYMBOL(drm_syncobj_destroy);

struct drm_syncobj *drm_syncobj_find(struct drm_syncobj_manager *mgr,
				     u32 handle)
{
	struct drm_syncobj *syncobj;

	mutex_lock(&mgr->lock);
	syncobj = idr_find(&mgr->idr, (int)handle);
	drm_syncobj_get(syncobj);
	mutex_unlock(&mgr->lock);

	return syncobj;
}
EXPORT_SYMBOL(drm_syncobj_find);

void drm_syncobj_free(struct kref *kref)
{
	struct drm_syncobj *syncobj =
		container_of(kref, struct drm_syncobj, refcount);

	dma_fence_put(syncobj->fence);
	kfree(syncobj);
}
EXPORT_SYMBOL(drm_syncobj_free);

int drm_syncobj_replace_fence(struct drm_syncobj *syncobj,
			      struct dma_fence *fence)
{
	struct dma_fence *old;

	if (!syncobj)
		return -EINVAL;

	dma_fence_get(fence);

	mutex_lock(&syncobj->lock);
	old = syncobj->fence;
	syncobj->fence = fence;
	mutex_unlock(&syncobj->lock);

	dma_fence_put(old);
	return 0;
}
EXPORT_SYMBOL(drm_syncobj_replace_fence);

struct dma_fence *drm_syncobj_find_fence(struct drm_syncobj *syncobj)
{
	struct dma_fence *fence;

	if (!syncobj)
		return NULL;

	mutex_lock(&syncobj->lock);
	fence = syncobj->fence;
	dma_fence_get(fence);
	mutex_unlock(&syncobj->lock);

	return fence;
}
EXPORT_SYMBOL(drm_syncobj_find_fence);

int drm_syncobj_wait(struct drm_syncobj *syncobj, bool intr, long timeout)
{
	struct dma_fence *fence;
	long ret;

	fence = drm_syncobj_find_fence(syncobj);
	if (!fence)
		return 0;	/* NULL fence == already signaled (mainline) */

	ret = dma_fence_wait_timeout(fence, intr, timeout);
	dma_fence_put(fence);

	return ret < 0 ? (int)ret : 0;
}
EXPORT_SYMBOL(drm_syncobj_wait);
