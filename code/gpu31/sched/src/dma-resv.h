// SPDX-License-Identifier: GPL-2.0
/*
 * dma-resv minimal shim for Nura 3.10.108 panfrost backport.
 *
 * What this provides: the reservation object panfrost GEM needs --
 * one shared fence slot (latest writer) + an exclusive-fence pointer +
 * a shared-fence list, with the mainline add-shared/add-excl/test-signaled
 * contract. Panfrost ioctl mapping: CREATE_BO creates an empty resv;
 * SUBMIT reserves (lock), attaches in-fences via add_shared/add_excl,
 * installs the job fence as exclusive on completion; BO_WAIT calls
 * dma_resv_test_signaled() / dma_resv_wait_timeout().
 *
 * What this deliberately OMITs vs mainline dma-resv.c (818 LOC): the
 * ww_mutex deadlock-avoidance dance (this tree has no ww_mutex.h --
 * reservation uses resv->lock, a plain mutex, plus an init-once
 * seqcount-free fast path), RCU-protected fence replacement, the
 * fence-context/excl-fence iterator helpers, dma_resv_get_fences()
 * full-copy API (only is_empty/copy-fences-needed-by-prime are kept),
 * and the reservation-object slab cache (plain kzalloc/kfree).
 *
 * Builds on patch 01 (include/linux/dma-fence.h). Verified primitive
 * usage: mutex_lock_interruptible (mutex.h:145), idr helpers (idr.h).
 *
 * Review fixes (gpu31-review §2): resv->lock is now a REAL lock, not
 * documentary -- mutators and readers lock it (F1); wait paths pin
 * each waited fence under the lock before sleeping (F2); shared[]
 * is C99 flexible (F-minor). copy_fences takes both locks in
 * address order -- documented at the declaration.
 */
#ifndef _LINUX_DMA_RESV_BACKPORT_H
#define _LINUX_DMA_RESV_BACKPORT_H

#include <linux/dma-fence.h>
#include <linux/mutex.h>
#include <linux/slab.h>

struct dma_resv_list {
	unsigned shared_count;
	unsigned shared_max;
	struct dma_fence *shared[];
};

/**
 * struct dma_resv - reservation object: one writer slot + shared readers.
 * @lock: serializes writers; replaces mainline's ww_mutex (absent in 3.10).
 * @fence_excl: latest exclusive (write) fence, or NULL if idle.
 * @fence_shared: list of shared (read) fences, may be NULL.
 *
 * Locking: every helper below takes @lock internally unless the "_locked"
 * name says the caller already holds it. Fence ownership: resv holds one
 * reference per stored pointer; replaced pointers are put after the
 * swap, same as mainline.
 *
 * copy_fences locking: takes both locks in increasing address order
 * (lower address first, re-taken after drop) so two concurrent copies
 * in opposite directions cannot ABBA-deadlock.
 */
struct dma_resv {
	struct mutex lock;
	struct dma_fence *fence_excl;
	struct dma_resv_list *fence_shared;
};

static inline void dma_resv_init(struct dma_resv *obj)
{
	mutex_init(&obj->lock);
	obj->fence_excl = NULL;
	obj->fence_shared = NULL;
}

static inline void dma_resv_fini(struct dma_resv *obj)
{
	struct dma_resv_list *fobj = obj->fence_shared;
	unsigned i;

	dma_fence_put(obj->fence_excl);
	if (fobj) {
		for (i = 0; i < fobj->shared_count; ++i)
			dma_fence_put(fobj->shared[i]);
		kfree(fobj);
	}
	obj->fence_excl = NULL;
	obj->fence_shared = NULL;
}

int dma_resv_reserve_shared(struct dma_resv *obj, unsigned int num_fences);
void dma_resv_add_shared_fence(struct dma_resv *obj, struct dma_fence *fence);
void dma_resv_add_excl_fence(struct dma_resv *obj, struct dma_fence *fence);
struct dma_fence *dma_resv_get_excl(struct dma_resv *obj);

static inline bool dma_resv_is_empty(struct dma_resv *obj)
{
	return obj->fence_excl == NULL && obj->fence_shared == NULL;
}

bool dma_resv_test_signaled(struct dma_resv *obj, bool test_all);
long dma_resv_wait_timeout(struct dma_resv *obj, bool wait_all, bool intr,
			   long timeout);
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src);

#endif /* _LINUX_DMA_RESV_BACKPORT_H */
