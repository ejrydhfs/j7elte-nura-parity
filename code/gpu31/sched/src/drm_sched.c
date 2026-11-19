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
 */

#include "drm_sched.h"
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

	if (!sched)
		return;

	mutex_lock(&sched->lock);
	sched->ready = false;
	list_for_each_entry_safe(entity, tmp, &sched->entities,
				 sched_link) {
		list_del_init(&entity->sched_link);
		entity->sched = NULL;
		entity->stopped = true;
	}
	mutex_unlock(&sched->lock);

	flush_workqueue(sched->wq);
	destroy_workqueue(sched->wq);
	sched->wq = NULL;
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

void drm_sched_entity_fini(struct drm_sched *sched,
			   struct drm_sched_entity *entity)
{
	struct drm_sched_job *job, *tmp;
	LIST_HEAD(retire);

	if (!entity)
		return;

	mutex_lock(&entity->lock);
	entity->stopped = true;
	spin_lock(&entity->queue_lock);
	list_splice_init(&entity->queue_list, &retire);
	spin_unlock(&entity->queue_lock);
	mutex_unlock(&entity->lock);

	/* Leave sched->entities before flushing: no new dispatch. */
	if (sched && entity->sched == sched) {
		mutex_lock(&sched->lock);
		if (!list_empty(&entity->sched_link))
			list_del_init(&entity->sched_link);
		entity->sched = NULL;
		mutex_unlock(&sched->lock);
	}

	cancel_work_sync(&entity->sched_work);

	list_for_each_entry_safe(job, tmp, &retire, node) {
		list_del_init(&job->node);
		dma_fence_remove_callback(job->dep_fence, &job->dep_cb);
		dma_fence_put(job->dep_fence);
		job->dep_fence = NULL;
		if (job->ops->free_job)
			job->ops->free_job(job);
		else
			kfree(job);
	}

	if (sched && sched->wq)
		flush_workqueue(sched->wq);
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
	atomic_inc(&entity->job_count);
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
