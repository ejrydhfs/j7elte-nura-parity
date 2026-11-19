// SPDX-License-Identifier: GPL-2.0
/*
 * dma-resv minimal shim implementation (Nura 3.10.108).
 * Plain-mutex locking replaces mainline's ww_mutex; fence replacement is
 * done under resv->lock so readers holding the lock see a stable set.
 *
 * Review fixes (gpu31-review §2): every entry point takes resv->lock,
 * so concurrent SUBMIT vs BO_WAIT can no longer tear the pointer set
 * (F1); wait paths dma_fence_get() each waited fence under the lock
 * and sleep only on the pinned copy, so add_excl/add_shared can no
 * longer free a fence out from under a waiter (F2).
 */

#include <linux/dma-resv.h>
#include <linux/module.h>

int dma_resv_reserve_shared(struct dma_resv *obj, unsigned int num_fences)
{
	struct dma_resv_list *new_list, *old_list;
	unsigned max;
	int ret = 0;

	if (!obj)
		return -EINVAL;

	mutex_lock(&obj->lock);
	old_list = obj->fence_shared;
	max = old_list ? old_list->shared_max : 0;
	if (old_list && old_list->shared_count + num_fences <= max)
		goto out;

	max += max(num_fences, 4u);
	new_list = kmalloc(sizeof(*new_list) + max * sizeof(*new_list->shared),
			   GFP_KERNEL);
	if (!new_list) {
		ret = -ENOMEM;
		goto out;
	}

	new_list->shared_count = old_list ? old_list->shared_count : 0;
	new_list->shared_max = max;
	if (old_list)
		memcpy(new_list->shared, old_list->shared,
		       old_list->shared_count * sizeof(*old_list->shared));

	kfree(old_list);
	obj->fence_shared = new_list;
out:
	mutex_unlock(&obj->lock);
	return ret;
}
EXPORT_SYMBOL(dma_resv_reserve_shared);

void dma_resv_add_shared_fence(struct dma_resv *obj, struct dma_fence *fence)
{
	struct dma_resv_list *fobj;

	BUG_ON(!obj);

	mutex_lock(&obj->lock);
	fobj = obj->fence_shared;
	BUG_ON(!fobj || fobj->shared_count >= fobj->shared_max);

	dma_fence_get(fence);
	fobj->shared[fobj->shared_count++] = fence;
	mutex_unlock(&obj->lock);
}
EXPORT_SYMBOL(dma_resv_add_shared_fence);

void dma_resv_add_excl_fence(struct dma_resv *obj, struct dma_fence *fence)
{
	struct dma_fence *old;
	unsigned i;

	BUG_ON(!obj);

	mutex_lock(&obj->lock);
	old = obj->fence_excl;

	dma_fence_get(fence);

	/* An exclusive fence retires all older shared fences. */
	if (obj->fence_shared) {
		for (i = 0; i < obj->fence_shared->shared_count; ++i)
			dma_fence_put(obj->fence_shared->shared[i]);
		obj->fence_shared->shared_count = 0;
	}

	obj->fence_excl = fence;
	mutex_unlock(&obj->lock);

	dma_fence_put(old);
}
EXPORT_SYMBOL(dma_resv_add_excl_fence);

struct dma_fence *dma_resv_get_excl(struct dma_resv *obj)
{
	struct dma_fence *fence;

	mutex_lock(&obj->lock);
	fence = obj->fence_excl;
	dma_fence_get(fence);
	mutex_unlock(&obj->lock);

	return fence;
}
EXPORT_SYMBOL(dma_resv_get_excl);

bool dma_resv_test_signaled(struct dma_resv *obj, bool test_all)
{
	struct dma_resv_list *fobj;
	struct dma_fence **shared = NULL;
	struct dma_fence *excl;
	unsigned i, count = 0;
	bool sig_excl, ret = true;

	mutex_lock(&obj->lock);
	excl = obj->fence_excl;
	dma_fence_get(excl);
	fobj = obj->fence_shared;
	if (fobj && fobj->shared_count) {
		count = fobj->shared_count;
		shared = kmalloc(count * sizeof(*shared), GFP_KERNEL);
		if (shared) {
			for (i = 0; i < count; ++i)
				dma_fence_get(fobj->shared[i]);
			memcpy(shared, fobj->shared,
			       count * sizeof(*shared));
		}
	}
	mutex_unlock(&obj->lock);

	sig_excl = !excl || dma_fence_is_signaled(excl);
	dma_fence_put(excl);
	if (!sig_excl || !test_all) {
		ret = sig_excl;
		goto out;
	}

	if (!count)
		goto out;

	if (!shared) {
		ret = false;
		goto out;
	}

	for (i = 0; i < count; ++i) {
		if (!dma_fence_is_signaled(shared[i])) {
			ret = false;
			break;
		}
	}
out:
	if (shared) {
		for (i = 0; i < count; ++i)
			dma_fence_put(shared[i]);
		kfree(shared);
	}
	return ret;
}
EXPORT_SYMBOL(dma_resv_test_signaled);

long dma_resv_wait_timeout(struct dma_resv *obj, bool wait_all, bool intr,
			   long timeout)
{
	struct dma_fence *pinned[8];
	struct dma_fence **extra = NULL;
	struct dma_fence *excl;
	unsigned i, nshared = 0, nwait;
	long ret = timeout;

	mutex_lock(&obj->lock);
	excl = obj->fence_excl;
	dma_fence_get(excl);
	if (obj->fence_shared)
		nshared = obj->fence_shared->shared_count;
	/* Wait at most 8 shared fences directly; heap-pin any overflow. */
	nwait = nshared > ARRAY_SIZE(pinned) ? ARRAY_SIZE(pinned) : nshared;
	for (i = 0; i < nwait; ++i) {
		pinned[i] = obj->fence_shared->shared[i];
		dma_fence_get(pinned[i]);
	}
	if (nshared > nwait) {
		extra = kmalloc((nshared - nwait) * sizeof(*extra),
				GFP_KERNEL);
		if (!extra) {
			/* OOM under lock: drop pins, report, no sleep. */
			for (i = 0; i < nwait; ++i)
				dma_fence_put(pinned[i]);
			dma_fence_put(excl);
			mutex_unlock(&obj->lock);
			return -ENOMEM;
		}
		for (i = nwait; i < nshared; ++i)
			dma_fence_get(obj->fence_shared->shared[i]);
		memcpy(extra, &obj->fence_shared->shared[nwait],
		       (nshared - nwait) * sizeof(*extra));
	}
	mutex_unlock(&obj->lock);

	if (excl) {
		ret = dma_fence_wait_timeout(excl, intr, timeout);
		dma_fence_put(excl);
		if (ret <= 0 || !wait_all)
			goto out;
		timeout = ret;
	}

	for (i = 0; i < nwait && timeout > 0; ++i)
		timeout = dma_fence_wait_timeout(pinned[i], intr, timeout);
	for (i = 0; extra && i < nshared - nwait && timeout > 0; ++i)
		timeout = dma_fence_wait_timeout(extra[i], intr, timeout);
	ret = timeout;
out:
	for (i = 0; i < nwait; ++i)
		dma_fence_put(pinned[i]);
	if (extra) {
		for (i = 0; i < nshared - nwait; ++i)
			dma_fence_put(extra[i]);
		kfree(extra);
	}
	return ret;
}
EXPORT_SYMBOL(dma_resv_wait_timeout);

/**
 * dma_resv_copy_fences - copy the fence set from @src to @dst.
 *
 * Takes both reservation locks in increasing address order so two
 * concurrent copies in opposite directions serialize instead of
 * ABBA-deadlocking. @dst's old fences are dropped; every copied
 * pointer carries a fresh reference.
 */
int dma_resv_copy_fences(struct dma_resv *dst, struct dma_resv *src)
{
	struct dma_resv_list *new_list = NULL;
	struct dma_resv *first, *second;
	struct dma_fence *new_excl = NULL;
	unsigned count = 0;
	int ret = 0;

	if (!dst || !src)
		return -EINVAL;
	if (dst == src)
		return 0;

	first = dst < src ? dst : src;
	second = dst < src ? src : dst;

	mutex_lock(&first->lock);
	if (second != first)
		mutex_lock(&second->lock);

	if (src->fence_shared)
		count = src->fence_shared->shared_count;
	if (count) {
		new_list = kmalloc(sizeof(*new_list) +
				   count * sizeof(*new_list->shared),
				   GFP_KERNEL);
		if (!new_list) {
			ret = -ENOMEM;
			goto out;
		}
		new_list->shared_count = count;
		new_list->shared_max = count;
		memcpy(new_list->shared, src->fence_shared->shared,
		       count * sizeof(*new_list->shared));
		for (count = 0; count < new_list->shared_count; ++count)
			dma_fence_get(new_list->shared[count]);
	}
	new_excl = src->fence_excl;
	dma_fence_get(new_excl);

	dma_fence_put(dst->fence_excl);
	if (dst->fence_shared) {
		for (count = 0; count < dst->fence_shared->shared_count;
		     ++count)
			dma_fence_put(dst->fence_shared->shared[count]);
		kfree(dst->fence_shared);
	}
	dst->fence_shared = new_list;
	dst->fence_excl = new_excl;
	new_list = NULL;
	new_excl = NULL;
out:
	if (second != first)
		mutex_unlock(&second->lock);
	mutex_unlock(&first->lock);
	if (new_list) {
		unsigned i;

		for (i = 0; i < new_list->shared_count; ++i)
			dma_fence_put(new_list->shared[i]);
		kfree(new_list);
	}
	dma_fence_put(new_excl);
	return ret;
}
EXPORT_SYMBOL(dma_resv_copy_fences);
