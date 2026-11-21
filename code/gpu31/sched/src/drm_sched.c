// SPDX-License-Identifier: GPL-2.0
/*
 * drm_sched panfrost-job-only shim implementation (Nura 3.10.108).
 *
 * Dispatch model: per-entity FIFO on a shared ordered workqueue.
 * Dependencies gate dispatch: a job whose dep fence has not signaled is
 * left at the head while the dep callback re-queues the pump when the
 * fence fires; only signaled (or dep-less) jobs reach run_job. Timeout
 * is a per-job delayed_work armed at dispatch from entity->timeout
 * (copied from sched->timeout at entity_init); the fence completion
 * cancels it on the retirement path (cancel_delayed_work_sync from
 * finish_work -- never from IRQ context).
 *
 * Review fixes (gpu31-review §4): sched_link vs queue_list are separate
 * lists (F1); deps are enforced at dispatch, not trusted (F2); dep and
 * completion callbacks are separate cbs with a one-in-fence contract
 * (F3); the run_job ERR_PTR path frees the job (F4); the timeout uses
 * entity->timeout (F5); push_job is defined (minor).
 *
 * Close fix (gpu31-close-fix): entity_fini is idempotent and bounded.
 * Queued jobs are cancelled synchronously (dep callbacks detached before
 * freeing, so a late fence signal cannot queue work into a dead entity);
 * dispatched-but-unretired jobs are waited on with a bounded budget
 * (drm_sched_entity_wait_idle: entity->timeout + 8 s recovery margin)
 * instead of an unbounded flush. Push re-arms entity_idle on the 0->1
 * transition so the fini wait cannot observe a stale completion.
 */

#include "drm_sched.h"
#include <stddef.h>
#ifdef __KERNEL__
#include <linux/bug.h>
#define DRM_SCHED_BUILD_BUG_ON(e) BUILD_BUG_ON(e)
#else
/* Host emu has BUG_ON (kernel-emu.h, abort on fire); no bug.h needed. */
#define DRM_SCHED_BUILD_BUG_ON(e) BUG_ON(e)
#endif
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/printk.h>

static long drm_sched_job_err(const char *stage, long err);
static void drm_sched_pump(struct work_struct *work);
static void drm_sched_job_timeout(struct work_struct *work);
static void drm_sched_job_finish(struct work_struct *work);
/* Bounded per-stage error attribution (multidep ticket): every early
 * return in this file routes its errno through drm_sched_job_err so
 * the next silicon assert carries the exact stage (alloc/arm/dep/
 * submit/pump/finish) and errno instead of a bare ioctl rc. Budget:
 * only fires while the ST trace budget lasts (same 8-line discipline
 * as the ST-PUSH/ST-PUMP prints), so steady-state dmesg is quiet. */
static unsigned int p7580_dep_trace = 8;

static long drm_sched_job_err(const char *stage, long err)
{
	if (p7580_dep_trace && err) {
		p7580_dep_trace--;
		pr_info("p7580_sched: err stage=%s rc=%ld\n", stage, err);
	}
	return err;
}

/* Bounded dispatch trace (schedtrace): ST-PUSH/ST-PUMP/ST-FENCE see
 * the 0004 FIFO handoff (queue, dequeue, retire). Shared budget:
 * first 8 jobs print (0 = off). Plain static, same rationale as
 * the p7580_sched_trace budget in panfrost7580_submit.c (NOT a
 * module param; standalone -fsyntax-only compiles this file).
 * Racy dec under concurrent entities overshoots trivially.
 * Numeric probes (gpu31-probenumeric): kptr_restrict=4 censors %p to
 * zero here, so addresses print as (unsigned long) with %lu (no %px
 * on 3.10) -- readable censored or not. */
static unsigned int p7580_sched_trace = 8;

/* Probenumeric weak default (see drm_sched.h): NULL when the backend TU
 * is not linked (host emu); the driver's strong definition wins in the
 * Image link. __weak (not __weak_alias): plain C, same as module.h. */
drm_sched_run_fn __attribute__((weak)) p7580_drv_run_job_addr(void)
{
	return NULL;
}

static void drm_sched_dep_cb(struct dma_fence *fence,
			     struct dma_fence_cb *cb)
{
	struct drm_sched_job *job =
		container_of(cb, struct drm_sched_job, dep_cb);

	queue_work(job->entity->wq, &job->entity->sched_work);
}

int drm_sched_init(struct drm_sched *sched,
		   const struct drm_sched_backend_ops *ops,
		   long timeout, const char *name)
{
	if (!sched || !ops || !ops->run_job)
		return -EINVAL;

	sched->wq = alloc_ordered_workqueue("%s", 0, name);
	if (!sched->wq)
		return -ENOMEM;

	INIT_LIST_HEAD(&sched->entities);
	mutex_init(&sched->lock);
	sched->ops = ops;
	sched->timeout = timeout;
	sched->name = name;
	sched->ready = true;

	return 0;
}
EXPORT_SYMBOL(drm_sched_init);

void drm_sched_fini(struct drm_sched *sched)
{
	struct drm_sched_entity *entity, *tmp;
	struct workqueue_struct *wq;

	if (!sched)
		return;

	/* Stop workers first: no new dispatch after this point. Evict
	 * every entity under sched->lock so a concurrent pump observes
	 * ready=false (or a NULL sched) and returns without dispatch. */
	mutex_lock(&sched->lock);
	sched->ready = false;
	list_for_each_entry_safe(entity, tmp, &sched->entities,
				 sched_link) {
		list_del_init(&entity->sched_link);
		entity->sched = NULL;
		entity->stopped = true;
	}
	wq = sched->wq;
	sched->wq = NULL;
	mutex_unlock(&sched->lock);

	/* Drain only what was queued on entry; destroy is then safe.
	 * NULL wq (double fini) is a no-op. */
	if (wq) {
		flush_workqueue(wq);
		destroy_workqueue(wq);
	}
}
EXPORT_SYMBOL(drm_sched_fini);

int drm_sched_entity_init(struct drm_sched_entity *entity,
			  struct drm_sched *sched)
{
	if (!entity || !sched)
		return -EINVAL;

	INIT_LIST_HEAD(&entity->sched_link);
	INIT_LIST_HEAD(&entity->queue_list);
	spin_lock_init(&entity->queue_lock);
	entity->in_flight = NULL;
	entity->sched = sched;
	entity->timeout = sched->timeout;
	entity->wq = sched->wq;
	INIT_WORK(&entity->sched_work, drm_sched_pump);
	mutex_init(&entity->lock);
	init_completion(&entity->entity_idle);
	complete_all(&entity->entity_idle);
	atomic_set(&entity->job_count, 0);
	entity->stopped = false;

	mutex_lock(&sched->lock);
	list_add_tail(&entity->sched_link, &sched->entities);
	mutex_unlock(&sched->lock);

	return 0;
}
EXPORT_SYMBOL(drm_sched_entity_init);

/* Default close budget: one job timeout plus the recovery margin.
 * A zero entity->timeout (disarmed timeout net) still gets the margin,
 * so fini can never spin forever on a wedged worker. */
long drm_sched_close_budget(struct drm_sched_entity *entity)
{
	long t;

	if (!entity)
		return DRM_SCHED_CLOSE_MARGIN_JIFFIES;
	t = entity->timeout;
	if (t < 0)
		t = 0;
	return t + DRM_SCHED_CLOSE_MARGIN_JIFFIES;
}
EXPORT_SYMBOL(drm_sched_close_budget);

/* Bounded idle wait for close. Returns 0 when job_count reaches zero,
 * -ETIME when the budget expires with jobs still outstanding, -EINVAL
 * on a NULL entity. The wait terminates: every dispatched job retires
 * through finish_work (IRQ signal) or the timeout work (which the
 * backend must keep fence-signalling even when it skips dark MMIO). */
long drm_sched_entity_wait_idle(struct drm_sched_entity *entity,
				long budget)
{
	unsigned long ret;

	if (!entity)
		return -EINVAL;
	if (budget <= 0)
		budget = drm_sched_close_budget(entity);
	if (atomic_read(&entity->job_count) == 0)
		return 0;
	ret = wait_for_completion_timeout(&entity->entity_idle,
					  (unsigned long)budget);
	return ret ? 0 : -ETIME;
}
EXPORT_SYMBOL(drm_sched_entity_wait_idle);

void drm_sched_entity_fini(struct drm_sched *sched,
			   struct drm_sched_entity *entity)
{
	struct drm_sched_job *job, *tmp;
	struct workqueue_struct *wq = NULL;
	long budget;
	LIST_HEAD(retire);

	if (!entity)
		return;

	/* Idempotent: a second fini (double close, error unwind after
	 * a partial init) is a no-op. stopped is set under entity->lock
	 * on the first pass; sched_link is empty after unlink. */
	mutex_lock(&entity->lock);
	if (entity->stopped && list_empty(&entity->queue_list) &&
	    list_empty(&entity->sched_link) && !entity->sched &&
	    atomic_read(&entity->job_count) == 0) {
		mutex_unlock(&entity->lock);
		return;
	}
	entity->stopped = true;
	spin_lock(&entity->queue_lock);
	list_splice_init(&entity->queue_list, &retire);
	spin_unlock(&entity->queue_lock);
	mutex_unlock(&entity->lock);

	/* Leave sched->entities before cancelling: no new dispatch.
	 * Snapshot the wq under sched->lock; fini must not chase
	 * sched->wq after fini cleared it (double-fini use-after-NULL). */
	if (sched && entity->sched == sched) {
		mutex_lock(&sched->lock);
		if (!list_empty(&entity->sched_link))
			list_del_init(&entity->sched_link);
		entity->sched = NULL;
		wq = sched->wq;
		mutex_unlock(&sched->lock);
	} else if (sched) {
		mutex_lock(&sched->lock);
		wq = sched->wq;
		mutex_unlock(&sched->lock);
	}

	/* Cancel the pump. A running pump finishes its current dispatch
	 * (bounded: one run_job + one callback arm), then observes
	 * stopped and exits. */
	cancel_work_sync(&entity->sched_work);

	/* Free queued (never-dispatched) jobs. Detach the dep callback
	 * BEFORE freeing: a late dep-fence signal must not re-queue the
	 * pump into this dead entity. NULL dep_fence (no dependency
	 * attached) detaches nothing. */
	list_for_each_entry_safe(job, tmp, &retire, node) {
		list_del_init(&job->node);
		if (job->dep_fence) {
			dma_fence_remove_callback(job->dep_fence,
						  &job->dep_cb);
			dma_fence_put(job->dep_fence);
			job->dep_fence = NULL;
		}
		if (atomic_dec_and_test(&entity->job_count))
			complete_all(&entity->entity_idle);
		if (job->ops->free_job)
			job->ops->free_job(job);
		else
			kfree(job);
	}

	/* Bounded wait for dispatched-but-unretired jobs (IRQ or timeout
	 * path owns them; they are not on any list). The backend timeout
	 * always retires the fence, so this terminates. On expiry the
	 * jobs still retire late through their own callbacks (entity
	 * memory stays alive: it is borrowed from the file state, freed
	 * only after fini returns -- the driver must honour this). */
	budget = drm_sched_close_budget(entity);
	if (atomic_read(&entity->job_count) != 0 && wq) {
		flush_workqueue(wq);
		if (atomic_read(&entity->job_count) != 0)
			drm_sched_entity_wait_idle(entity, budget);
	}

	/* Final drain of retirements the bounded wait just released. */
	if (wq)
		flush_workqueue(wq);
}
EXPORT_SYMBOL(drm_sched_entity_fini);

struct drm_sched_job *drm_sched_job_alloc(struct drm_sched_entity *entity,
					  const struct drm_sched_backend_ops *ops,
					  void *owner)
{
	struct drm_sched_job *job;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return NULL;

	INIT_LIST_HEAD(&job->node);
	INIT_LIST_HEAD(&job->dep_cb.node);
	INIT_LIST_HEAD(&job->finish_cb.node);
	job->entity = entity;
	job->ops = ops;
	/* Opsfix: fail LOUD if the store did not land where the pump
	 * reads it (silicon ST-OPS showed ops=NULL despite a source
	 * line that stores it). Header asserts offsetof==24 in both
	 * TUs (enum-constant BUILD_BUG_ON_ZERO at file scope); a
	 * mismatch means a duplicate struct definition or a stale .o,
	 * never a silent "NULL ops" retire. */
	DRM_SCHED_BUILD_BUG_ON(offsetof(struct drm_sched_job, ops) != 24);
	BUG_ON(job->ops != ops);
	job->owner = owner;
	job->fence = NULL;
	INIT_DELAYED_WORK(&job->timeout_work, drm_sched_job_timeout);
	INIT_WORK(&job->finish_work, drm_sched_job_finish);

	return job;
}
EXPORT_SYMBOL(drm_sched_job_alloc);

void drm_sched_job_cleanup(struct drm_sched_job *job)
{
	if (!job)
		return;
	dma_fence_remove_callback(job->fence, &job->finish_cb);
	dma_fence_put(job->fence);
	job->fence = NULL;
	dma_fence_remove_callback(job->dep_fence, &job->dep_cb);
	dma_fence_put(job->dep_fence);
	job->dep_fence = NULL;
}
EXPORT_SYMBOL(drm_sched_job_cleanup);

void drm_sched_job_arm(struct drm_sched_job *job)
{
	struct dma_fence *fence;

	INIT_LIST_HEAD(&job->dep_cb.node);
	INIT_LIST_HEAD(&job->finish_cb.node);
	/* Arm-time fence (mainline semantics): exactly one allocation per
	 * job, before queueing. Idempotent: a set fence is never replaced.
	 * Fails closed: NULL ops/alloc_fence or a NULL/ERR_PTR return
	 * leaves fence == NULL and the SUBMIT-time out_sync install
	 * becomes a no-op (job still runs; waiters see no fence). */
	if (!job || job->fence)
		return;
	if (!job->ops || !job->ops->alloc_fence)
		return;
	fence = job->ops->alloc_fence(job);
	if (IS_ERR_OR_NULL(fence))
		return;
	job->fence = fence;
}
EXPORT_SYMBOL(drm_sched_job_arm);

/**
 * struct drm_sched_dep_and - compound wait-all dependency node.
 *
 * Built when a second DISTINCT unsignaled fence arrives for a job that
 * already holds one: dep_fence is re-pointed at this node and the job's
 * single dep_cb waits on the node instead of on either leaf. The node
 * fires (signals itself) only after every leaf has signaled -- plain
 * AND semantics, so the pump's single-fence gate is unchanged and FIFO
 * ordering is preserved. Fast paths stay allocation-free: 0 deps (no
 * node), 1 dep (leaf stored directly, no node).
 *
 * Lifetime: owned by the job (dep_fence pointer). The merges below
 * hold one ref per leaf plus the node's init ref; release drops the
 * leaf refs, then the node ref frees the node. Both release sites in
 * this file (queued-cancel in entity_fini, pump error legs) go through
 * drm_sched_job_cleanup AFTER detaching dep_cb, so the node's cb_list
 * is empty at free time (release_base only WARNS on a live waiter).
 *
 * Cap: DRM_SCHED_DEP_MAX_N leaves per job. Mesa's chain submit carries
 * a bounded handful of in_syncs + a few BO excl fences (P7580_MAX_
 * IN_SYNCS=16 caps the ioctl array); 32 leaves is headroom, not a
 * hot path. Beyond the cap add_dependency fails -ENOSPC through the
 * bounded error print (stage=dep) so the silicon assert names it.
 */
#define DRM_SCHED_DEP_MAX_N 32
struct drm_sched_dep_and {
	struct dma_fence base;
	unsigned int n;
	struct dma_fence_cb leaf_cb[DRM_SCHED_DEP_MAX_N];
	struct dma_fence *leaf[DRM_SCHED_DEP_MAX_N];
};

static const char *drm_sched_dep_and_drv(struct dma_fence *f)
{
	(void)f;
	return "drm_sched_dep_and";
}

static const char *drm_sched_dep_and_tl(struct dma_fence *f)
{
	(void)f;
	return "dep-and";
}

static bool drm_sched_dep_and_signaled(struct dma_fence *f)
{
	return !!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags);
}

static void drm_sched_dep_and_release(struct dma_fence *f)
{
	struct drm_sched_dep_and *a =
		container_of(f, struct drm_sched_dep_and, base);
	unsigned int i;

	dma_fence_release_base(f);
	for (i = 0; i < a->n; i++)
		dma_fence_put(a->leaf[i]);
	kfree(a);
}

static const struct dma_fence_ops drm_sched_dep_and_ops = {
	.get_driver_name = drm_sched_dep_and_drv,
	.get_timeline_name = drm_sched_dep_and_tl,
	.enable_signaling = NULL,
	.signaled = drm_sched_dep_and_signaled,
	.wait = NULL,
	.release = drm_sched_dep_and_release,
};

/* AND drain: signal the node once every leaf is signaled. The signal
 * runs the job's dep_cb (queued on the node by the merge below),
 * which re-queues the pump. Idempotent: late leaf signals after the
 * node fired hit the -EINVAL already-fired leg and are harmless. */
static void drm_sched_dep_and_leaf_fired(struct drm_sched_dep_and *a)
{
	unsigned int i;

	for (i = 0; i < a->n; i++) {
		if (a->leaf[i] && !dma_fence_is_signaled(a->leaf[i]))
			return;
	}
	dma_fence_signal(&a->base);
}

/* Per-leaf trampolines: cb == &a->leaf_cb[k], so cb - k is
 * &a->leaf_cb[0] and container_of recovers the node. A single
 * shared callback cannot recover k (the cb struct carries no
 * back-pointer by mainline design), hence one tiny function per
 * slot. N <= 32 keeps this bounded. */
#define DRM_SCHED_DEP_LEAF_CB(k) \
static void drm_sched_dep_and_leaf_cb_##k(struct dma_fence *f, \
					  struct dma_fence_cb *cb) \
{ \
	struct drm_sched_dep_and *a = \
		container_of((cb) - (k), struct drm_sched_dep_and, \
			     leaf_cb[0]); \
	(void)f; \
	drm_sched_dep_and_leaf_fired(a); \
}
DRM_SCHED_DEP_LEAF_CB(0)
DRM_SCHED_DEP_LEAF_CB(1)
DRM_SCHED_DEP_LEAF_CB(2)
DRM_SCHED_DEP_LEAF_CB(3)
DRM_SCHED_DEP_LEAF_CB(4)
DRM_SCHED_DEP_LEAF_CB(5)
DRM_SCHED_DEP_LEAF_CB(6)
DRM_SCHED_DEP_LEAF_CB(7)
DRM_SCHED_DEP_LEAF_CB(8)
DRM_SCHED_DEP_LEAF_CB(9)
DRM_SCHED_DEP_LEAF_CB(10)
DRM_SCHED_DEP_LEAF_CB(11)
DRM_SCHED_DEP_LEAF_CB(12)
DRM_SCHED_DEP_LEAF_CB(13)
DRM_SCHED_DEP_LEAF_CB(14)
DRM_SCHED_DEP_LEAF_CB(15)
DRM_SCHED_DEP_LEAF_CB(16)
DRM_SCHED_DEP_LEAF_CB(17)
DRM_SCHED_DEP_LEAF_CB(18)
DRM_SCHED_DEP_LEAF_CB(19)
DRM_SCHED_DEP_LEAF_CB(20)
DRM_SCHED_DEP_LEAF_CB(21)
DRM_SCHED_DEP_LEAF_CB(22)
DRM_SCHED_DEP_LEAF_CB(23)
DRM_SCHED_DEP_LEAF_CB(24)
DRM_SCHED_DEP_LEAF_CB(25)
DRM_SCHED_DEP_LEAF_CB(26)
DRM_SCHED_DEP_LEAF_CB(27)
DRM_SCHED_DEP_LEAF_CB(28)
DRM_SCHED_DEP_LEAF_CB(29)
DRM_SCHED_DEP_LEAF_CB(30)
DRM_SCHED_DEP_LEAF_CB(31)

static const dma_fence_func_t drm_sched_dep_leaf_cbs[DRM_SCHED_DEP_MAX_N] = {
	drm_sched_dep_and_leaf_cb_0, drm_sched_dep_and_leaf_cb_1,
	drm_sched_dep_and_leaf_cb_2, drm_sched_dep_and_leaf_cb_3,
	drm_sched_dep_and_leaf_cb_4, drm_sched_dep_and_leaf_cb_5,
	drm_sched_dep_and_leaf_cb_6, drm_sched_dep_and_leaf_cb_7,
	drm_sched_dep_and_leaf_cb_8, drm_sched_dep_and_leaf_cb_9,
	drm_sched_dep_and_leaf_cb_10, drm_sched_dep_and_leaf_cb_11,
	drm_sched_dep_and_leaf_cb_12, drm_sched_dep_and_leaf_cb_13,
	drm_sched_dep_and_leaf_cb_14, drm_sched_dep_and_leaf_cb_15,
	drm_sched_dep_and_leaf_cb_16, drm_sched_dep_and_leaf_cb_17,
	drm_sched_dep_and_leaf_cb_18, drm_sched_dep_and_leaf_cb_19,
	drm_sched_dep_and_leaf_cb_20, drm_sched_dep_and_leaf_cb_21,
	drm_sched_dep_and_leaf_cb_22, drm_sched_dep_and_leaf_cb_23,
	drm_sched_dep_and_leaf_cb_24, drm_sched_dep_and_leaf_cb_25,
	drm_sched_dep_and_leaf_cb_26, drm_sched_dep_and_leaf_cb_27,
	drm_sched_dep_and_leaf_cb_28, drm_sched_dep_and_leaf_cb_29,
	drm_sched_dep_and_leaf_cb_30, drm_sched_dep_and_leaf_cb_31,
};


/* Compound-node helpers (multidep): alloc before the docstring's
 * add_dependency so the merge code below reads top-down. */
static struct dma_fence *drm_sched_dep_and_alloc(void)
{
	struct drm_sched_dep_and *a = kzalloc(sizeof(*a), GFP_KERNEL);
	unsigned int i;

	if (!a)
		return NULL;
	dma_fence_init(&a->base, &drm_sched_dep_and_ops, 0, 0);
	for (i = 0; i < DRM_SCHED_DEP_MAX_N; i++) {
		INIT_LIST_HEAD(&a->leaf_cb[i].node);
		a->leaf[i] = NULL;
	}
	return &a->base;
}

static void drm_sched_dep_and_maybe_signal(struct dma_fence *node)
{
	struct drm_sched_dep_and *a =
		container_of(node, struct drm_sched_dep_and, base);
	unsigned int i;

	for (i = 0; i < a->n; i++) {
		if (a->leaf[i] && !dma_fence_is_signaled(a->leaf[i]))
			return;
	}
	dma_fence_signal(node);
}

/* Merge one more unsignaled leaf into the job's dependency (stage=dep).
 * Caller holds a fresh add_dependency call with an unsignaled, distinct
 * fence. Returns 0 on success (job owns the dep), -ENOMEM / -ENOSPC /
 * callback errors otherwise with dep_fence left valid. The single-fence
 * fast path never reaches here (handled inline in add_dependency). */
static int drm_sched_dep_merge(struct drm_sched_job *job, struct dma_fence *f)
{
	struct dma_fence *old = job->dep_fence;
	struct drm_sched_dep_and *a;
	struct dma_fence *node;
	unsigned int n;
	int ret;

	if (!old)
		return (int)drm_sched_job_err("dep", -EINVAL);
	if (old->ops == &drm_sched_dep_and_ops) {
		/* Third+ distinct fence: append a leaf to the node. */
		a = container_of(old, struct drm_sched_dep_and, base);
		if (a->n >= DRM_SCHED_DEP_MAX_N)
			return (int)drm_sched_job_err("dep", -ENOSPC);
		dma_fence_get(f);
		n = a->n;
		INIT_LIST_HEAD(&a->leaf_cb[n].node);
		a->leaf[n] = f;
		a->n = n + 1;
		ret = dma_fence_add_callback(f, &a->leaf_cb[n],
					     drm_sched_dep_leaf_cbs[n]);
		if (ret == -ENOENT) {
			/* Raced signaled between the is_signaled check and
			 * the callback arm: drop our ref and re-check the
			 * AND -- the last leaf may complete the node. */
			dma_fence_put(f);
			a->leaf[n] = NULL;
			a->n = n;
			drm_sched_dep_and_maybe_signal(old);
			return 0;
		}
		if (ret) {
			dma_fence_put(f);
			a->leaf[n] = NULL;
			a->n = n;
			return (int)drm_sched_job_err("dep", ret);
		}
		drm_sched_dep_and_maybe_signal(old);
		return 0;
	}
	/* Second distinct fence: allocate the node, move both leaves in. */
	node = drm_sched_dep_and_alloc();
	if (!node)
		return (int)drm_sched_job_err("dep", -ENOMEM);
	a = container_of(node, struct drm_sched_dep_and, base);
	/* Steal the job's ref on the old leaf (no get/put churn); take
	 * one ref on the new leaf. */
	a->leaf[0] = old;
	a->leaf[1] = f;
	dma_fence_get(f);
	a->n = 2;
	/* Detach the job waiter from the old leaf BEFORE re-pointing:
	 * dep_cb is currently queued on old -- detach is safe. */
	dma_fence_remove_callback(old, &job->dep_cb);
	ret = dma_fence_add_callback(old, &a->leaf_cb[0],
				     drm_sched_dep_leaf_cbs[0]);
	if (ret == -ENOENT) {
		/* Old leaf signaled under us: node unneeded. Retry the
		 * still-pending fence as the single fast path. */
		a->leaf[0] = NULL;
		a->leaf[1] = NULL;
		a->n = 0;
		dma_fence_put(f);
		dma_fence_put(old);
		dma_fence_put(node);
		if (!dma_fence_is_signaled(f)) {
			dma_fence_get(f);
			job->dep_fence = f;
			ret = dma_fence_add_callback(f, &job->dep_cb,
						     drm_sched_dep_cb);
			if (ret == -ENOENT) {
				dma_fence_put(f);
				job->dep_fence = NULL;
				return 0;
			}
			if (ret) {
				dma_fence_put(f);
				job->dep_fence = NULL;
				return (int)drm_sched_job_err("dep", ret);
			}
			return 0;
		}
		job->dep_fence = NULL;
		return 0;
	}
	if (ret) {
		dma_fence_remove_callback(old, &a->leaf_cb[0]);
		a->leaf[0] = NULL;
		a->leaf[1] = NULL;
		a->n = 0;
		dma_fence_put(f);
		dma_fence_put(node);
		/* Old leaf ref stays with the job (dep_fence untouched). */
		job->dep_fence = old;
		return (int)drm_sched_job_err("dep", ret);
	}
	ret = dma_fence_add_callback(f, &a->leaf_cb[1],
				     drm_sched_dep_leaf_cbs[1]);
	if (ret == -ENOENT) {
		/* New leaf signaled under us: node unneeded. The old leaf
		 * is still valid -- re-park the job waiter on it. */
		dma_fence_remove_callback(old, &a->leaf_cb[0]);
		a->leaf[0] = NULL;
		a->leaf[1] = NULL;
		a->n = 0;
		dma_fence_put(f);
		dma_fence_put(node);
		job->dep_fence = old;
		if (dma_fence_is_signaled(old)) {
			dma_fence_remove_callback(old, &job->dep_cb);
			dma_fence_put(old);
			job->dep_fence = NULL;
		}
		return 0;
	}
	if (ret) {
		dma_fence_remove_callback(old, &a->leaf_cb[0]);
		a->leaf[0] = NULL;
		a->leaf[1] = NULL;
		a->n = 0;
		dma_fence_put(f);
		dma_fence_put(node);
		job->dep_fence = old;
		return (int)drm_sched_job_err("dep", ret);
	}
	/* Both leaves parked: point the job at the node. */
	job->dep_fence = node;
	ret = dma_fence_add_callback(node, &job->dep_cb,
				     drm_sched_dep_cb);
	if (ret == -ENOENT) {
		/* Both leaves were already signaled (or raced): no gate. */
		dma_fence_remove_callback(node, &job->dep_cb);
		drm_sched_dep_and_release(node);
		job->dep_fence = NULL;
		return 0;
	}
	if (ret) {
		dma_fence_remove_callback(node, &job->dep_cb);
		drm_sched_dep_and_release(node);
		job->dep_fence = old;
		return (int)drm_sched_job_err("dep", ret);
	}
	drm_sched_dep_and_maybe_signal(node);
	if (dma_fence_is_signaled(node)) {
		dma_fence_remove_callback(node, &job->dep_cb);
		drm_sched_dep_and_release(node);
		job->dep_fence = NULL;
	}
	return 0;
}

/**
 * drm_sched_job_add_dependency - attach one more in-fence to the job.
 *
 * N in-fences per job (the Mesa chain submit passes an array of
 * in-syncobj handles plus implicit BO excl fences -- gpu31-sched F4).
 * The first unsignaled fence stores directly (no allocation); the
 * second and later DISTINCT fences merge into a compound wait-all AND
 * node (see struct drm_sched_dep_and): the job's single dep_cb waits
 * on the node, which fires only after every leaf signals. Same fence
 * twice is a no-op 0; already-signaled fences are skipped by the
 * caller contract (checked again here). Ordering is unchanged: the
 * pump still gates only the FIFO head on dep_fence.
 */
int drm_sched_job_add_dependency(struct drm_sched_job *job,
				 struct dma_fence *fence)
{
	struct drm_sched_dep_and *a;
	unsigned int i;
	int ret;

	if (!fence)
		return 0;
	if (dma_fence_is_signaled(fence))
		return 0;

	if (!job->dep_fence) {
		dma_fence_get(fence);
		job->dep_fence = fence;
		/* Park the pump on the dep; the cb re-queues dispatch. */
		ret = dma_fence_add_callback(fence, &job->dep_cb,
					     drm_sched_dep_cb);
		if (ret == -ENOENT) {
			dma_fence_put(fence);
			job->dep_fence = NULL;
			return 0;
		}
		if (ret) {
			dma_fence_put(fence);
			job->dep_fence = NULL;
			return (int)drm_sched_job_err("dep", ret);
		}
		return 0;
	}
	/* Same fence (or same leaf inside the node): no-op success. */
	if (job->dep_fence == fence)
		return 0;
	if (job->dep_fence->ops == &drm_sched_dep_and_ops) {
		a = container_of(job->dep_fence, struct drm_sched_dep_and,
				 base);
		for (i = 0; i < a->n; i++) {
			if (a->leaf[i] == fence)
				return 0;
		}
	}
	/* Second+ distinct fence: merge into the AND node. */
	return drm_sched_dep_merge(job, fence);
}
EXPORT_SYMBOL(drm_sched_job_add_dependency);

/**
 * drm_sched_entity_push_job - submit helper for backends that keep
 * the alloc/submit split: arm the job, queue it on the entity FIFO,
 * and kick the dispatch pump. Returns 0 or -EINVAL on a NULL job.
 */
int drm_sched_entity_push_job(struct drm_sched_job *sched_job)
{
	struct drm_sched_entity *entity;
	unsigned long flags;

	if (!sched_job || !sched_job->entity)
		return (int)drm_sched_job_err("submit", -EINVAL);

	entity = sched_job->entity;

	spin_lock_irqsave(&entity->queue_lock, flags);
	list_add_tail(&sched_job->node, &entity->queue_list);
	if (atomic_inc_return(&entity->job_count) == 1)
		INIT_COMPLETION(entity->entity_idle);
	spin_unlock_irqrestore(&entity->queue_lock, flags);

	/* ST-PUSH: job queued on the entity FIFO, pump kicked. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: push job_count=%d\n",
			atomic_read(&entity->job_count));
	}

	queue_work(entity->wq, &entity->sched_work);
	return 0;
}
EXPORT_SYMBOL(drm_sched_entity_push_job);

int drm_sched_job_submit(struct drm_sched_job *job)
{
	return drm_sched_entity_push_job(job);
}
EXPORT_SYMBOL(drm_sched_job_submit);

static void drm_sched_job_finish(struct work_struct *work)
{
	struct drm_sched_job *job =
		container_of(work, struct drm_sched_job, finish_work);
	struct drm_sched_entity *entity = job->entity;

	cancel_delayed_work_sync(&job->timeout_work);
	dma_fence_put(job->fence);
	job->fence = NULL;

	if (atomic_dec_and_test(&entity->job_count))
		complete_all(&entity->entity_idle);

	/* ST-FENCE: fence signalled (finish queued by fence_cb),
	 * retirement handed to the backend free_job. ST-FINISH adds
	 * the ran stamp + slot + owner so dmesg tells "retire without
	 * ever running" (ran=0) from "run_job returned but its probe
	 * was skipped" (ran=1); owner links back to the ST-RUN line. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: fence job_count=%d\n",
			atomic_read(&entity->job_count));
	}
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: finish ran=%d slot=%d owner=%lu\n",
			job->ran, job->slot, (unsigned long)job->owner);
	}

	if (job->ops->free_job)
		job->ops->free_job(job);
	else
		kfree(job);

	/* Keep the FIFO moving after each retirement. */
	queue_work(entity->wq, &entity->sched_work);
}

static void drm_sched_fence_cb(struct dma_fence *fence,
			       struct dma_fence_cb *cb)
{
	struct drm_sched_job *job = container_of(cb, struct drm_sched_job,
						 finish_cb);

	queue_work(job->entity->wq, &job->finish_work);
}

static void drm_sched_job_timeout(struct work_struct *work)
{
	struct drm_sched_job *job = container_of(work, struct drm_sched_job,
						 timeout_work.work);

	if (job->fence && !dma_fence_is_signaled(job->fence) &&
	    job->ops->timedout_job)
		job->ops->timedout_job(job);
}

static void drm_sched_pump(struct work_struct *work)
{
	struct drm_sched_entity *entity =
		container_of(work, struct drm_sched_entity, sched_work);
	struct drm_sched_job *job;
	struct dma_fence *run_fence;
	struct drm_sched *sched;
	unsigned long flags;
	int ret;

	if (entity->stopped)
		return;

	sched = entity->sched;
	if (sched) {
		mutex_lock(&sched->lock);
		ret = sched->ready ? 0 : -ENODEV;
		mutex_unlock(&sched->lock);
		if (ret)
			return;
	}

	spin_lock_irqsave(&entity->queue_lock, flags);
	if (entity->in_flight) {
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		return;
	}
	if (list_empty(&entity->queue_list)) {
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		return;
	}
	job = list_first_entry(&entity->queue_list, struct drm_sched_job,
			       node);
	/* Dependency not met: leave the job queued; the dep callback
	 * re-queues the pump when the fence fires. */
	if (job->dep_fence && !dma_fence_is_signaled(job->dep_fence)) {
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		return;
	}
	list_del_init(&job->node);
	entity->in_flight = job;
	spin_unlock_irqrestore(&entity->queue_lock, flags);

	/* ST-PUMP: worker dequeued (dep met or dep-less); run_job next.
	 * Numeric ST-OPS prints the dequeued job address, the address of
	 * its ->ops field, and the ops/run_job pointers plus owner and the
	 * carried slot, so a header/ABI skew (stale .o, duplicate struct,
	 * link picking two shim copies) shows up in dmesg: compare job->ops
	 * against the driver's &p7580_drv_sched_ops (from /proc/kallsyms)
	 * and offsetof(ops)==24 in both TUs. runexp= is the backend's own
	 * run_job address (no symbol lookup needed): run= must equal it.
	 * job->ops is trusted only after the NULL check below. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: pump job_count=%d\n",
			atomic_read(&entity->job_count));
	}
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d\n",
			(unsigned long)job, (unsigned long)&job->ops,
			(unsigned long)job->ops,
			(unsigned long)(job->ops ? job->ops->run_job : NULL),
			(unsigned long)p7580_drv_run_job_addr(),
			(unsigned long)job->owner, job->slot);
	}

	if (!job->ops || !job->ops->run_job) {
		drm_sched_job_err("pump", job->ops ? -ENOSYS : -EFAULT);
		spin_lock_irqsave(&entity->queue_lock, flags);
		entity->in_flight = NULL;
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		dma_fence_remove_callback(job->dep_fence, &job->dep_cb);
		dma_fence_put(job->dep_fence);
		job->dep_fence = NULL;
		if (job->ops && job->ops->free_job)
			job->ops->free_job(job);
		else
			kfree(job);
		queue_work(entity->wq, &entity->sched_work);
		return;
	}

	/* Preserve the submit-chosen slot: the backend must not re-derive
	 * it from the entity pointer (container layouts shift the index;
	 * int12 retired slot 0 for a slot-1 submit). */
	if (job->slot < 0)
		job->slot = 0;
	/* Arm-time fence contract (gpu31-armfence): job->fence was
	 * allocated at arm time and installed on out_sync during the
	 * ioctl. run_job must return that SAME fence (never a second
	 * one): completion (IRQ/timeout dma_fence_signal) retires the
	 * exact fence Mesa already holds. A mismatched return fails
	 * the job (F4 path) instead of stranding a live out_sync. */
	run_fence = job->ops->run_job(job);
	job->ran = 1;
	if (IS_ERR_OR_NULL(run_fence)) {
		if (!run_fence)
			run_fence = ERR_PTR(-ENODATA);
		drm_sched_job_err("run", PTR_ERR(run_fence));
		spin_lock_irqsave(&entity->queue_lock, flags);
		entity->in_flight = NULL;
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		/* Failed dispatch owns the job: free it (F4), then the
		 * -EBUSY/dependency waiters below run on freed memory
		 * unless removed first -- detach both cbs now. */
		dma_fence_remove_callback(job->dep_fence, &job->dep_cb);
		dma_fence_put(job->dep_fence);
		job->dep_fence = NULL;
		if (job->fence) {
			dma_fence_remove_callback(job->fence,
						  &job->finish_cb);
			dma_fence_put(job->fence);
			job->fence = NULL;
		}
		if (job->ops->free_job)
			job->ops->free_job(job);
		else
			kfree(job);
		queue_work(entity->wq, &entity->sched_work);
		return;
	}
	if (!job->fence || run_fence != job->fence) {
		drm_sched_job_err("run", job->fence ? -EEXIST : -ENODATA);
		/* Backend returned a second fence (or none was armed):
		 * never publish it. Drop its reference when it is a real
		 * fence, drop the armed fence too (never queued, never
		 * signaled), and free the job on the F4 path so out_sync
		 * never points at a fence the completion path will not
		 * signal. */
		if (!IS_ERR_OR_NULL(run_fence) && run_fence != job->fence)
			dma_fence_put(run_fence);
		spin_lock_irqsave(&entity->queue_lock, flags);
		entity->in_flight = NULL;
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		dma_fence_remove_callback(job->dep_fence, &job->dep_cb);
		dma_fence_put(job->dep_fence);
		job->dep_fence = NULL;
		if (job->fence) {
			dma_fence_remove_callback(job->fence,
						  &job->finish_cb);
			dma_fence_put(job->fence);
			job->fence = NULL;
		}
		if (job->ops && job->ops->free_job)
			job->ops->free_job(job);
		else
			kfree(job);
		queue_work(entity->wq, &entity->sched_work);
		return;
	}

	ret = dma_fence_add_callback(job->fence, &job->finish_cb,
				     drm_sched_fence_cb);
	if (ret == -ENOENT) {
		/* Already done (empty batch): retire synchronously. */
		queue_work(entity->wq, &job->finish_work);
	} else if (!ret) {
		if (entity->timeout)
			queue_delayed_work(entity->wq, &job->timeout_work,
					   entity->timeout);
	} else {
		/* -EBUSY (stale cb) must not wedge the FIFO: retire now
		 * and let free_job report the backend bug. */
		queue_work(entity->wq, &job->finish_work);
	}

	spin_lock_irqsave(&entity->queue_lock, flags);
	entity->in_flight = NULL;
	spin_unlock_irqrestore(&entity->queue_lock, flags);
}
