// SPDX-License-Identifier: GPL-2.0
/*
 * drm_syncobj minimal subset for Nura 3.10.108 panfrost backport.
 *
 * What this provides: the syncobj handle layer panfrost SUBMIT consumes --
 * per-file handle table (CREATE/DESTROY), point lookup by handle,
 * fence replace/query (binary payload only), WAIT (block on the current
 * fence), and FD export/import plumbing stubs wired to dma-fence
 * (syncobj->fence is a plain dma_fence pointer + refcount, no
 * struct file backing -- 3.10 has no sync_file.h).
 *
 * Panfrost ioctl mapping: DRM_PANFROST_SUBMIT takes in-syncobj handles
 * (resolved to fences via drm_syncobj_find_fence) and an out-syncobj
 * handle (completed via drm_syncobj_replace_fence). Mesa panfrost
 *winsys passes one syncobj per batch; WAIT_SEM-style chained payloads
 * are NOT needed and are omitted.
 *
 * What this deliberately OMITs vs mainline (~800 LOC): timeline
 * (TIMELINE_SIGNAL/WAIT/QUERY) payloads, drm_syncobj timeline points,
 * eventfd/SYNCOBJ_WAIT_FOR_SUBMIT support, and the syncobj->file fd
 * lifecycle (sync_file.c does not exist in 3.10 -- FD import/export
 * here resolves the fence synchronously instead of passing struct file).
 *
 * Depends on patch 01 (dma-fence). Uses idr_alloc/idr_find/idr_remove
 * (idr.h, verified present) and mutex (mutex.h). DRM_SYNCOBJ ioctls
 * reuse the 3.10 driver-private range via struct drm_driver ioctls --
 * see the .rst section below; no core drm.h changes are made.
 */
#ifndef _DRM_SYNCOBJ_BACKPORT_H
#define _DRM_SYNCOBJ_BACKPORT_H

#include <linux/dma-fence.h>
#include <linux/idr.h>
#include <linux/mutex.h>

struct drm_syncobj {
	struct kref refcount;
	struct dma_fence *fence;
	struct mutex lock;		/* guards fence replace/query */
};

/**
 * struct drm_syncobj_manager - per-drm_file handle table for syncobjs.
 * One instance per open file; handles are small ints local to the fd.
 */
struct drm_syncobj_manager {
	struct idr idr;
	struct mutex lock;
};

void drm_syncobj_manager_init(struct drm_syncobj_manager *mgr);
void drm_syncobj_manager_fini(struct drm_syncobj_manager *mgr);

int drm_syncobj_create(struct drm_syncobj_manager *mgr, u32 *handle);
int drm_syncobj_destroy(struct drm_syncobj_manager *mgr, u32 handle);
struct drm_syncobj *drm_syncobj_find(struct drm_syncobj_manager *mgr,
				     u32 handle);

static inline void drm_syncobj_get(struct drm_syncobj *syncobj)
{
	if (syncobj)
		kref_get(&syncobj->refcount);
}

void drm_syncobj_free(struct kref *kref);

static inline void drm_syncobj_put(struct drm_syncobj *syncobj)
{
	if (syncobj)
		kref_put(&syncobj->refcount, drm_syncobj_free);
}

/* fence payload ops (binary syncobj only) */
int drm_syncobj_replace_fence(struct drm_syncobj *syncobj,
			      struct dma_fence *fence);
struct dma_fence *drm_syncobj_find_fence(struct drm_syncobj *syncobj);
int drm_syncobj_wait(struct drm_syncobj *syncobj, bool intr, long timeout);

#endif /* _DRM_SYNCOBJ_BACKPORT_H */
