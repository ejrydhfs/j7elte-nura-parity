// SPDX-License-Identifier: GPL-2.0
/*
 * dma-fence minimal subset for the j7elte (Nura 3.10.108) panfrost backport.
 *
 * What this provides: a fence object with the exact mainline semantics the
 * M3 scheduler needs -- ordered seqno retirement per fence context via an
 * atomic counter, wait/event/callback hooks, and a global waiter list so
 * drm_sched-style dependency tracking works. Panfrost SUBMIT/BO_WAIT need
 * exactly: dma_fence_context_alloc(), dma_fence_init(), dma_fence_get/put(),
 * dma_fence_wait(), dma_fence_is_signaled(), dma_fence_add_callback(),
 * dma_fence_signal(), DMA_FENCE_FLAG_SIGNALED_BIT, and the dma_fence_ops
 * (.enable_signaling, .signaled, .wait, .release) contract.
 *
 * What this deliberately OMITs vs mainline (1221+332+288 LOC): the
 * dma-fence-array and dma-fence-chain container types (panfrost only needs
 * single fences + an explicit merged-wait helper in patch 03), stub/legacy
 * hooks (no set_deadline, no timeline-test helpers, no debugfs), and the
 * seqno-fence timeline wrapper (3.10 has no drivers/base fence -- the
 * per-timeline ordering comes from the seqno counter below, same as
 * mainline's dma_fence_begin/end_signalling contract).
 *
 * 3.10 notes: no ww_mutex in this tree (reservation locking uses a plain
 * mutex + seqcount-free fast path in patch 02 instead); RCU list ops used
 * for the waiter list are stock 3.10. Depends only on completion.h, kref.h,
 * list.h, spinlock.h, workqueue.h -- all verified present in 3.10.108.
 *
 * Review fixes (gpu31-review §1): every fence is prepared with
 * dma_fence_init() (F1 -- no more hand-assembled init); the vestigial
 * timeline-lock pointer, status word and reserved default_work are GONE
 * (F2 -- they were never assigned/read); default_wait returns the
 * remaining budget (F3).
 */
#ifndef _LINUX_DMA_FENCE_BACKPORT_H
#define _LINUX_DMA_FENCE_BACKPORT_H

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct dma_fence;
struct dma_fence_cb;

/*
 * Mainline-compatible callback entry. queued == 1 while linked on the
 * fence's waiter list; removed (== 0) once fired/cancelled. The func is
 * invoked with the fence lock held, same contract as mainline.
 */
typedef void (*dma_fence_func_t)(struct dma_fence *fence,
				 struct dma_fence_cb *cb);

struct dma_fence_cb {
	struct list_head node;
	dma_fence_func_t func;
};

/**
 * struct dma_fence_ops - fence backend, mirrors mainline.
 * @get_driver_name: timeline owner, e.g. "panfrost7580".
 * @get_timeline_name: queue name, used for debugging only.
 * @enable_signaling: arm the fence's completion path; true if already done.
 * @signaled: fast non-blocking check; NULL falls back to status bit.
 * @wait: block until signaled or timeout/error; NULL falls back to polling
 *        the completion. Return values match mainline: 0 signaled,
 *        -ENOENT cancelled, -ETIME timed out, -ERESTARTSYS on signal.
 * @release: free the container; must call dma_fence_release_base().
 */
struct dma_fence_ops {
	const char *(*get_driver_name)(struct dma_fence *fence);
	const char *(*get_timeline_name)(struct dma_fence *fence);
	bool (*enable_signaling)(struct dma_fence *fence);
	bool (*signaled)(struct dma_fence *fence);
	signed long (*wait)(struct dma_fence *fence, bool intr, signed long t);
	void (*release)(struct dma_fence *fence);
};

enum dma_fence_flag_bits {
	DMA_FENCE_FLAG_SIGNALED_BIT,
	DMA_FENCE_FLAG_TIMESTAMP_BIT,
	DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT,
	DMA_FENCE_FLAG_USER_BITS,
};

struct dma_fence {
	struct kref refcount;
	const struct dma_fence_ops *ops;
	unsigned context;
	unsigned seqno;
	unsigned long flags;
	struct completion completion;	/* 3.10 equivalent of mainline's cb_list drain */
	ktime_t timestamp;
	struct list_head cb_list;	/* dma_fence_cb waiter list */
	spinlock_t cb_lock;		/* guards cb_list + flags */
};

extern unsigned dma_fence_context_alloc(unsigned num);

/**
 * dma_fence_init - prepare an embedded fence before first use.
 * @fence: fence to initialise (usually inside a backend container).
 * @ops: backend ops; must be non-NULL with a non-NULL release().
 * @context: fence-timeline id from dma_fence_context_alloc().
 * @seqno: per-timeline sequence number of this fence.
 *
 * Does the kref/completion/cb_list/cb_lock/flags setup every backend
 * needs, so a missed field can no longer cause use-after-uninit.
 * Differs from mainline only in dropping the timeline spinlock_t *
 * parameter (3.10 subset keeps no such pointer).
 */
void dma_fence_init(struct dma_fence *fence,
		    const struct dma_fence_ops *ops,
		    unsigned context, unsigned seqno);

void dma_fence_release_base(struct dma_fence *fence);

static inline void dma_fence_get(struct dma_fence *fence)
{
	if (fence)
		kref_get(&fence->refcount);
}

static inline void dma_fence_put(struct dma_fence *fence)
{
	if (fence)
		kref_put(&fence->refcount,
			 (void (*)(struct kref *))fence->ops->release);
}

int dma_fence_signal_locked(struct dma_fence *fence);

static inline bool dma_fence_is_signaled_locked(struct dma_fence *fence)
{
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return true;
	if (fence->ops->signaled && fence->ops->signaled(fence)) {
		dma_fence_signal_locked(fence);
		return true;
	}
	return false;
}

static inline bool dma_fence_is_signaled(struct dma_fence *fence)
{
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->flags))
		return true;
	if (fence->ops->signaled && fence->ops->signaled(fence))
		return true;
	return false;
}

int dma_fence_signal(struct dma_fence *fence);

signed long dma_fence_default_wait(struct dma_fence *fence, bool intr,
				   signed long timeout);
signed long dma_fence_wait_timeout(struct dma_fence *fence, bool intr,
				   signed long timeout);

static inline signed long dma_fence_wait(struct dma_fence *fence, bool intr)
{
	return dma_fence_wait_timeout(fence, intr, MAX_SCHEDULE_TIMEOUT);
}

int dma_fence_add_callback(struct dma_fence *fence, struct dma_fence_cb *cb,
			   dma_fence_func_t func);
bool dma_fence_remove_callback(struct dma_fence *fence,
			       struct dma_fence_cb *cb);
void dma_fence_enable_sw_signaling(struct dma_fence *fence);

#endif /* _LINUX_DMA_FENCE_BACKPORT_H */
