// SPDX-License-Identifier: GPL-2.0
/*
 * drm_sched panfrost-job-only scheduler shim for Nura 3.10.108.
 *
 * What this provides: the exact drm_sched surface panfrost_job.c needs --
 * entity (one runqueue per JM slot context), job (fence-backed unit of
 * work with .run_job/.timedout_job callbacks), FIFO dispatch on an
 * ordered workqueue, dependency tracking via dma-fence callbacks, job
 * timeout via delayed work, and entity flush on release. Panfrost mapping:
 * one sched per device, one entity per open-file submit context, one job
 * per SUBMIT ioctl batch; run_job programs the JM slot registers and
 * installs the returned job fence; hw completion (JOB_INT) signals it.
 *
 * What this deliberately OMITs vs mainline drm_sched (2684 LOC: scheduler
 * + fence + entity): priority runqueues (single FIFO), credit-based load
 * balancing (one queue = T720 JM slot set), GPU reset/recovery workers,
 * guilty-job tracking, dynamic job-flow control, and free-job workqueue
 * batching (synchronous put). This is a correctness shim, not a
 * performance scheduler: jobs retire in submit order.
 *
 * Depends on patches 01+02 (dma-fence). Verified 3.10 primitives:
 * alloc_ordered_workqueue (workqueue.h:434), INIT_DELAYED_WORK (260),
 * queue_delayed_work / cancel_delayed_work_sync, complete_all
 * (completion.h:95), mutex_lock_interruptible (mutex.h:145),
 * msecs_to_jiffies (jiffies.h). No kthread usage (mainline uses a
 * dedicated thread; this shim uses an ordered wq -- same FIFO order,
 * no RT-priority inversion risk on this single-GPU part).
 *
 * Review fixes (gpu31-review §4): sched->entities is the membership
 * list with its own link (F1); the pump skips jobs whose dep fence
 * is still pending and re-queues itself behind the dep callback (F2);
 * the dep waiter is a separate cb so -EBUSY completion races and
 * dual-role overlap are gone -- one in-fence per job, rejected with
 * -EOPNOTSUPP otherwise (F3, matches the one-in-fence Mesa contract);
 * the run_job ERR_PTR path frees the job instead of dropping it (F4);
 * the timeout comes from entity->timeout, seeded from sched->timeout
 * at entity_init (F5); push_job is implemented (minor).
 *
 * Close path (gpu31-close-fix): entity_fini cancels queued jobs, detaches
 * from the scheduler, then waits BOUNDED for dispatched-but-unretired
 * jobs (budget = entity->timeout + recovery margin). Mainline waits
 * unbounded here; on this SoC an unbounded close wedges the release
 * (watchdog reset ~14 s after CLOSE on the int7 node). entity_fini is
 * idempotent (second call is a no-op). drm_sched_fini stops workers
 * (ready=false, evict entities) before draining the workqueue.
 */
#ifndef _DRM_SCHED_BACKPORT_H
#define _DRM_SCHED_BACKPORT_H

#include <linux/dma-fence.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/jiffies.h>
#include <linux/errno.h>
/* Static offset asserts below need offsetof + a 3.10-style
 * BUILD_BUG_ON_ZERO. In-tree the header is <linux/bug.h>; the host
 * emu (emu/include, used by sched-host-test) has no bug.h, so use a
 * local negative-bitfield equivalent with identical semantics. */
#ifdef __KERNEL__
#include <linux/bug.h>
#define DRM_SCHED_BUILD_BUG_ON_ZERO(e) BUILD_BUG_ON_ZERO(e)
#else
#define DRM_SCHED_BUILD_BUG_ON_ZERO(e) (sizeof(struct { int:-!!(e); }))
#endif

struct drm_sched_entity;
struct drm_sched_job;

enum drm_sched_priority {
	DRM_SCHED_PRIORITY_MIN,
	DRM_SCHED_PRIORITY_NORMAL = DRM_SCHED_PRIORITY_MIN,
	DRM_SCHED_PRIORITY_HIGH_HW,
	DRM_SCHED_PRIORITY_HIGH_SW,
	DRM_SCHED_PRIORITY_KERNEL,
	DRM_SCHED_PRIORITY_COUNT
};

/**
 * struct drm_sched_backend_ops - device callbacks, mirrors mainline subset.
 * @run_job: execute @sched_job; must return the in-flight job fence, or
 *           ERR_PTR on immediate failure. Called in FIFO submit order.
 * @timedout_job: called from timeout work when the job fence has not
 *           signaled in @timeout msecs; backend should reset the JM slot.
 * @free_job: release backend resources for a retired job (may be NULL).
 */
struct drm_sched_backend_ops {
	struct dma_fence *(*run_job)(struct drm_sched_job *sched_job);
	void (*timedout_job)(struct drm_sched_job *sched_job);
	void (*free_job)(struct drm_sched_job *sched_job);
};

struct drm_sched_job {
	struct list_head node;
	struct drm_sched_entity *entity;
	const struct drm_sched_backend_ops *ops;
	struct dma_fence *fence;	/* completion fence for this job */
	struct dma_fence *dep_fence;	/* in-fence: must signal before dispatch */
	struct dma_fence_cb dep_cb;	/* waits on dep_fence (dependency role) */
	struct dma_fence_cb finish_cb;	/* waits on fence (completion role) */
	struct delayed_work timeout_work;
	struct work_struct finish_work;	/* retirement sweep */
	int slot;			/* dispatch slot chosen at submit; the
					 * pump preserves it, run_job must not
					 * re-derive it (entity-pointer math
					 * breaks under container layouts) */
	int ran;			/* pumpfix stamp: set to 1 when run_job
					 * returns; finish prints it so dmesg
					 * distinguishes "never ran" (0) from
					 * "ran but probe skipped" (1) */
	void *owner;			/* backend private (panfrost_job) */
};
/* Opsfix: single authoritative definition (drivers/gpu/drm via 0004 and
 * drivers/gpu/drm/panfrost via ccflags -Idrivers/gpu/drm read THIS file).
 * Silicon ST-OPS showed ops=NULL despite a correct store + correct call
 * site, so both translation units must agree on where ->ops lives: the
 * compile breaks if either TU drifts. True in-tree (arm64 3.10) values,
 * measured against the real headers (see docs/reports/gpu31-opsfix):
 * node=0 entity=16 ops=24 fence=32 dep_fence=40 dep_cb=48 finish_cb=72
 * timeout_work=96 finish_work=200 slot=232 ran=236 owner=240
 * sizeof=248 (work_struct=32 with lockdep off, delayed_work=104).
 * Host emu values differ (work_struct=16, delayed_work=24+8, no lockdep:
 * slot=144 ran=148 owner=152 sizeof=160) but ops/entity offsets match,
 * so per-TU asserts below pin the fields both sides must agree on for
 * dispatch: ops (the failing word) plus the identity words. */
/* The _ZERO macro is an expression, not a statement, so each assert
 * initialises a private enum constant. */
enum { DRM_SCHED_ASSERT_OPS_IS_24 =
	DRM_SCHED_BUILD_BUG_ON_ZERO(offsetof(struct drm_sched_job, ops) != 24) };
enum { DRM_SCHED_ASSERT_SLOT_IS_232 =
	DRM_SCHED_BUILD_BUG_ON_ZERO(
#ifdef __KERNEL__
		offsetof(struct drm_sched_job, slot) != 232
#else
	/* Host emu shrinks work structs; slot lives at 144 there. The
	 * cross-TU contract is ops==24 in both worlds (the failing word),
	 * and each world pins its own full layout below. */
		offsetof(struct drm_sched_job, slot) != 144
#endif
	) };
enum { DRM_SCHED_ASSERT_RAN_IS_236 =
	DRM_SCHED_BUILD_BUG_ON_ZERO(
#ifdef __KERNEL__
		offsetof(struct drm_sched_job, ran) != 236
#else
		offsetof(struct drm_sched_job, ran) != 148
#endif
	) };

/*
 * struct drm_sched_entity - one submit context's FIFO against a scheduler.
 * @sched_link: membership of sched->entities (never a job queue).
 * @queue_list: pending jobs in submit order (never in sched->entities).
 * @timeout: per-job dispatch budget (jiffies), copied from sched->timeout
 *           at entity_init; 0 disables the timeout arm.
 * @sched: back-pointer so the pump can honour sched->ready without
 *         reaching through globals.
 * @entity_idle: signaled while job_count == 0 (see job_count).
 * @job_count: queued + dispatched-but-unretired jobs. Push re-arms
 *           (INIT_COMPLETION on the 0->1 transition); retirement
 *           completes (complete_all on dec-to-zero). fini waits on
 *           this with a bounded budget; the wait terminates because
 *           the timeout path always retires the job (even dark: the
 *           backend skips MMIO but the fence still signals).
 * @stopped: set by fini under entity->lock; the pump exits early.
 */
struct drm_sched_entity {
	struct list_head sched_link;	/* sched->entities membership */
	struct list_head queue_list;	/* pending jobs, FIFO */
	spinlock_t queue_lock;
	struct drm_sched_job *in_flight;
	struct drm_sched *sched;	/* owning scheduler */
	long timeout;			/* per-job budget, jiffies */
	struct workqueue_struct *wq;	/* shared ordered wq (borrowed) */
	struct work_struct sched_work;	/* dispatch pump */
	struct mutex lock;
	struct completion entity_idle;
	atomic_t job_count;
	bool stopped;
};

struct drm_sched {
	struct workqueue_struct *wq;	/* ordered dispatch queue */
	struct list_head entities;
	struct mutex lock;
	const struct drm_sched_backend_ops *ops;
	long timeout;			/* job timeout, jiffies */
	const char *name;
	bool ready;
};

int drm_sched_init(struct drm_sched *sched,
		   const struct drm_sched_backend_ops *ops,
		   long timeout, const char *name);
void drm_sched_fini(struct drm_sched *sched);
/* Close budget margin past one job timeout + one recovery (jiffies). */
#define DRM_SCHED_CLOSE_MARGIN_JIFFIES	(HZ * 8)

int drm_sched_entity_init(struct drm_sched_entity *entity,
			  struct drm_sched *sched);
void drm_sched_entity_fini(struct drm_sched *sched,
			   struct drm_sched_entity *entity);
/* Bounded idle wait for close (0 idle, -ETIME on budget expiry, -EINVAL
 * on NULL entity). Budget <= 0 selects the default close budget. */
long drm_sched_entity_wait_idle(struct drm_sched_entity *entity,
				long budget);
/* Default close budget for an entity (jiffies). */
long drm_sched_close_budget(struct drm_sched_entity *entity);
int drm_sched_entity_push_job(struct drm_sched_job *sched_job);

struct drm_sched_job *drm_sched_job_alloc(struct drm_sched_entity *entity,
					  const struct drm_sched_backend_ops *ops,
					  void *owner);
/* Probenumeric hook (gpu31-probenumeric): the backend run_job address
 * for the numeric ST-OPS cross-check (ST-OPS run= must equal runexp=;
 * both printed as (unsigned long)+%lu, readable under kptr_restrict).
 * The backend (panfrost7580_submit.c) defines the strong version
 * returning its run_job; drm_sched.c carries a weak default returning
 * NULL so shim-only builds (host emu) still link. Plain declaration
 * here: the weak attribute lives only on the shim default definition,
 * so the backend definition stays strong and always wins the link. */
typedef struct dma_fence *(*drm_sched_run_fn)(struct drm_sched_job *);
drm_sched_run_fn p7580_drv_run_job_addr(void);
void drm_sched_job_cleanup(struct drm_sched_job *job);
void drm_sched_job_arm(struct drm_sched_job *job);
int drm_sched_job_add_dependency(struct drm_sched_job *job,
				 struct dma_fence *fence);
int drm_sched_job_submit(struct drm_sched_job *job);

#endif /* _DRM_SCHED_BACKPORT_H */
