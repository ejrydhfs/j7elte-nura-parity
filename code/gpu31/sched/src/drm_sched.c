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
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/module.h>

static void drm_sched_pump(struct work_struct *work);
static void drm_sched_job_timeout(struct work_struct *work);
static void drm_sched_job_finish(struct work_struct *work);

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
	job->owner = owner;
	job->dep_fence = NULL;
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
	INIT_LIST_HEAD(&job->dep_cb.node);
	INIT_LIST_HEAD(&job->finish_cb.node);
}
EXPORT_SYMBOL(drm_sched_job_arm);

/**
 * drm_sched_job_add_dependency - attach the job's single in-fence.
 *
 * One in-fence per job (the Mesa kbase backend sends exactly one
 * in-syncobj per batch; see gpu31-review §12 non-blocking note).
 * A second distinct fence is rejected with -EOPNOTSUPP: silently
 * dropping a real dependency would corrupt ordering, and queuing
 * two callbacks on one cb struct would corrupt the waiter list.
 */
int drm_sched_job_add_dependency(struct drm_sched_job *job,
				 struct dma_fence *fence)
{
	int ret;

	if (!fence)
		return 0;
	if (dma_fence_is_signaled(fence))
		return 0;

	if (job->dep_fence && job->dep_fence != fence)
		return -EOPNOTSUPP;
	if (job->dep_fence == fence)
		return 0;

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
		return ret;
	}

	return 0;
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
		return -EINVAL;

	entity = sched_job->entity;

	spin_lock_irqsave(&entity->queue_lock, flags);
	list_add_tail(&sched_job->node, &entity->queue_list);
	if (atomic_inc_return(&entity->job_count) == 1)
		INIT_COMPLETION(entity->entity_idle);
	spin_unlock_irqrestore(&entity->queue_lock, flags);

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

	run_fence = job->ops->run_job(job);
	if (IS_ERR(run_fence)) {
		spin_lock_irqsave(&entity->queue_lock, flags);
		entity->in_flight = NULL;
		spin_unlock_irqrestore(&entity->queue_lock, flags);
		/* Failed dispatch owns the job: free it (F4), then the
		 * -EBUSY/dependency waiters below run on freed memory
		 * unless removed first -- detach both cbs now. */
		dma_fence_remove_callback(job->dep_fence, &job->dep_cb);
		dma_fence_put(job->dep_fence);
		job->dep_fence = NULL;
		if (job->ops->free_job)
			job->ops->free_job(job);
		else
			kfree(job);
		queue_work(entity->wq, &entity->sched_work);
		return;
	}

	job->fence = run_fence;

	ret = dma_fence_add_callback(run_fence, &job->finish_cb,
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
