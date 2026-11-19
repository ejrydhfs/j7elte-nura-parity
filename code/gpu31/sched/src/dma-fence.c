// SPDX-License-Identifier: GPL-2.0
/*
 * dma-fence minimal subset implementation for Nura 3.10.108.
 *
 * Mirrors drivers/dma-buf/dma-fence.c (mainline) minus array/chain/debugfs.
 * Locking: cb_lock protects cb_list and flags. signal_locked()
 * drains the waiter list with the lock held (callbacks run under the lock,
 * same as mainline fence callbacks) then completes.
 *
 * Review fixes (gpu31-review §1): dma_fence_init() centralises setup (F1);
 * the vestigial lock pointer / status word / reserved default_work are
 * gone from the struct (F2); default_wait returns the remaining budget
 * so chained waits do not get overlong budgets (F3).
 */

#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/module.h>

/* Global fence-context allocator: backport of dma_fence_context_alloc(). */
static unsigned dma_fence_context_counter = 1;
static DEFINE_SPINLOCK(dma_fence_context_lock);

unsigned dma_fence_context_alloc(unsigned num)
{
	unsigned context;

	spin_lock(&dma_fence_context_lock);
	context = dma_fence_context_counter;
	dma_fence_context_counter += num;
	spin_unlock(&dma_fence_context_lock);

	return context;
}
EXPORT_SYMBOL(dma_fence_context_alloc);

/**
 * dma_fence_init - prepare an embedded fence before first use.
 *
 * Fills in refcount/completion/cb_list/cb_lock/flags plus the ops,
 * context and seqno, so backends can no longer ship a half-initialised
 * fence. BUGs on a NULL fence, NULL ops or a NULL release hook: a fence
 * without release() cannot be freed by dma_fence_put().
 */
void dma_fence_init(struct dma_fence *fence,
		    const struct dma_fence_ops *ops,
		    unsigned context, unsigned seqno)
{
	BUG_ON(!fence || !ops || !ops->release);

	kref_init(&fence->refcount);
	init_completion(&fence->completion);
	INIT_LIST_HEAD(&fence->cb_list);
	spin_lock_init(&fence->cb_lock);
	fence->flags = 0;
	fence->ops = ops;
	fence->context = context;
	fence->seqno = seqno;
	fence->timestamp = ktime_set(0, 0);
}
EXPORT_SYMBOL(dma_fence_init);

/**
 * dma_fence_release_base - default teardown used by backend release().
 *
 * Only the refcount owner (fence->ops->release) calls this; it runs
 * exactly once when the last reference drops. Callers must not hold
 * cb_lock. A non-empty cb_list here means a waiter leaked (fired
 * callbacks always unlink before running), hence the WARN.
 */
void dma_fence_release_base(struct dma_fence *fence)
{
	WARN_ON(!list_empty(&fence->cb_list));
}
EXPORT_SYMBOL(dma_fence_release_base);

int dma_fence_signal_locked(struct dma_fence *fence)
{
	struct dma_fence_cb *cur, *tmp;

	lockdep_assert_held(&fence->cb_lock);

	if (test_and_set_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return -EINVAL;

	fence->timestamp = ktime_get();

	list_for_each_entry_safe(cur, tmp, &fence->cb_list, node) {
		list_del_init(&cur->node);
		cur->func(fence, cur);
	}
	INIT_LIST_HEAD(&fence->cb_list);

	complete_all(&fence->completion);

	return 0;
}
EXPORT_SYMBOL(dma_fence_signal_locked);

int dma_fence_signal(struct dma_fence *fence)
{
	unsigned long flags;
	int ret;

	if (!fence)
		return -EINVAL;

	spin_lock_irqsave(&fence->cb_lock, flags);
	ret = dma_fence_signal_locked(fence);
	spin_unlock_irqrestore(&fence->cb_lock, flags);

	return ret;
}
EXPORT_SYMBOL(dma_fence_signal);

signed long dma_fence_default_wait(struct dma_fence *fence, bool intr,
				   signed long timeout)
{
	signed long ret;

	if (intr) {
		ret = wait_for_completion_interruptible_timeout(
				&fence->completion, timeout);
		if (ret <= 0)
			return timeout ? -ETIME : 0;
	} else {
		ret = wait_for_completion_timeout(&fence->completion,
						  timeout);
		if (ret <= 0)
			return timeout ? -ETIME : 0;
	}

	return ret;
}
EXPORT_SYMBOL(dma_fence_default_wait);

signed long dma_fence_wait_timeout(struct dma_fence *fence, bool intr,
				   signed long timeout)
{
	signed long ret;

	if (WARN_ON(!fence))
		return -EINVAL;

	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return timeout;

	if (fence->ops->wait)
		ret = fence->ops->wait(fence, intr, timeout);
	else
		ret = dma_fence_default_wait(fence, intr, timeout);

	return ret;
}
EXPORT_SYMBOL(dma_fence_wait_timeout);

/**
 * dma_fence_add_callback - register @func on @fence; runs immediately if
 * already signaled. Mirrors the mainline return contract: 0 queued,
 * -ENOENT already signaled (func NOT called), -EINVAL on bad args.
 */
int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
			   dma_fence_func_t func)
{
	unsigned long flags;
	int ret = 0;

	if (WARN_ON(!fence || !func))
		return -EINVAL;

	spin_lock_irqsave(&fence->cb_lock, flags);

	if (dma_fence_is_signaled_locked(fence)) {
		ret = -ENOENT;
		goto out;
	}

	if (!list_empty(&cb->node)) {
		ret = -EBUSY;
		goto out;
	}

	cb->func = func;
	if (!fence->ops->enable_signaling ||
	    !fence->ops->enable_signaling(fence)) {
		list_add_tail(&cb->node, &fence->cb_list);
	} else {
		/* Backend already signaled during arming; re-check. */
		if (dma_fence_is_signaled_locked(fence))
			ret = -ENOENT;
		else
			list_add_tail(&cb->node, &fence->cb_list);
	}
out:
	spin_unlock_irqrestore(&fence->cb_lock, flags);
	return ret;
}
EXPORT_SYMBOL(dma_fence_add_callback);

bool dma_fence_remove_callback(struct dma_fence *fence,
			       struct dma_fence_cb *cb)
{
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&fence->cb_lock, flags);
	if (!list_empty(&cb->node)) {
		list_del_init(&cb->node);
		ret = true;
	}
	spin_unlock_irqrestore(&fence->cb_lock, flags);

	return ret;
}
EXPORT_SYMBOL(dma_fence_remove_callback);

void dma_fence_enable_sw_signaling(struct dma_fence *fence)
{
	unsigned long flags;

	spin_lock_irqsave(&fence->cb_lock, flags);
	if (!test_and_set_bit(DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT,
			       &fence->flags) && fence->ops->enable_signaling)
		fence->ops->enable_signaling(fence);
	spin_unlock_irqrestore(&fence->cb_lock, flags);
}
EXPORT_SYMBOL(dma_fence_enable_sw_signaling);
