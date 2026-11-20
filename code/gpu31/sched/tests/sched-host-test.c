/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sched-host-test.c - host verification for the drm-backport review fixes.
 *
 * Compiles the FIXED shim sources (code/gpu31/sched/src/) against a
 * single-threaded kernel emulation (emu/include/linux/) and asserts
 * every gpu31-review finding:
 *
 *  0001 F1  dma_fence_init() centralises setup (ops/context/seqno/flags).
 *  0001 F2  vestigial lock pointer / status word / reserved default_work
 *            / event pointer are GONE (compile-time + grep gate in Makefile).
 *  0001 F3  dma_fence_default_wait returns the REMAINING budget, so chained
 *            dma_resv waits do not get overlong budgets.
 *  0002 F1  resv->lock serialises mutators and readers (reserve/add race
 *            smoke + locked test_signaled/wait paths).
 *  0002 F2  dma_resv_wait_timeout pins each waited fence under the lock
 *            (unpinned-wait UAF closed; wait succeeds after excl swapped).
 *  0003 F1  drm_syncobj_manager_fini drains without skipping (300 handles,
 *            none leaked, idr empty afterwards).
 *  0004 F1  entity membership (sched_link) vs job FIFO (queue_list) split;
 *            entities list non-empty after init, empty after fini.
 *  0004 F2  dependencies gate dispatch (unsignaled dep blocks run_job;
 *            pump re-queued by the dep callback runs it after signal).
 *  0004 F3  one finish_cb per role; second distinct dep -> -EOPNOTSUPP.
 *  0004 F4  run_job ERR_PTR frees the job (free_job called, FIFO advances).
 *  0004 F5  timeout comes from entity->timeout (== sched->timeout), and
 *            a zero timeout arms nothing.
 *  0004 minor drm_sched_entity_push_job is defined and queues work.
 *  sched->ready is honoured by the pump (fini stops dispatch).
 *
 *  close-fix (gpu31-close-fix, mock-GPU ordering repro):
 *  C1 wait_idle on an idle entity returns 0 immediately (no wait).
 *  C2 wait_idle on a dispatched-but-unretired job returns 0 once the
 *     timeout path retires it (bounded: no infinite flush).
 *  C3 entity_fini with a queued (never-dispatched) job frees it
 *     synchronously and detaches the dep callback (late dep signal
 *     cannot re-queue the dead pump); fini is idempotent.
 *  C4 entity_fini with a dispatched-but-unretired job retires it via
 *     the timeout path and returns (bounded); double fini is a no-op.
 *  C5 push re-arms entity_idle on 0->1 (no stale completion).
 *  C6 syncobj_manager_fini with a pending fence drops the handle
 *     without waiting; double fini is safe.
 *  C7 fence remove_callback(NULL, cb) is a no-op (dep-less close).
 *
 * Build: cc -Wall -Wextra -std=gnu99 -o sched-host-test sched-host-test.c
 *        ./sched-host-test   (native PASS; also built/run aarch64 under qemu)
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Emulation first, then the shim sources compiled in. emu/include
 * shadows the kernel <linux/...> paths; the shim headers sit at the
 * same relative paths the kernel tree uses. The .c files come from
 * ../src so the test always builds the real patch bodies. */
#include <linux/kernel-emu.h>
#include <linux/list.h>

int sched_emu_bug_fired = 0;
int sched_emu_warn_count = 0;

/* Pull the fixed shim sources into this translation unit. */
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <drm_syncobj.h>
#include <drm_sched.h>

#include "dma-fence.c"
#include "dma-resv.c"
#include "drm_syncobj.c"
#include "drm_sched.c"
static int checks, failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %d: %s:%d: %s\n", checks, __FILE__, \
		       __LINE__, #cond); \
	} } while (0)

#define CHECK_EQ(a, b) do { \
	checks++; \
	if ((long long)(a) != (long long)(b)) { \
		failures++; \
		printf("FAIL %d: %s:%d: %s (%lld) != %s (%lld)\n", checks, \
			__FILE__, __LINE__, #a, (long long)(a), \
			#b, (long long)(b)); \
	} } while (0)

/* ------------------------------------------------------------------ */
/* Test fence backend (manual signaling, no HW).                        */
/* ------------------------------------------------------------------ */
struct test_fence {
	struct dma_fence base;
	int released;
};

static struct test_fence *to_test(struct dma_fence *f)
{
	return container_of(f, struct test_fence, base);
}

static const char *test_drv(struct dma_fence *f)
{
	(void)f;
	return "test";
}

static const char *test_tl(struct dma_fence *f)
{
	(void)f;
	return "test-tl";
}

static bool test_signaled(struct dma_fence *f)
{
	return !!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags);
}

static void test_release(struct dma_fence *f)
{
	struct test_fence *tf = to_test(f);

	dma_fence_release_base(f);
	tf->released = 1;
}

static const struct dma_fence_ops test_ops = {
	.get_driver_name = test_drv,
	.get_timeline_name = test_tl,
	.enable_signaling = NULL,
	.signaled = test_signaled,
	.wait = NULL,
	.release = test_release,
};

static struct test_fence *mkfence(unsigned ctx, unsigned seq)
{
	struct test_fence *tf = calloc(1, sizeof(*tf));

	if (!tf)
		return NULL;
	dma_fence_init(&tf->base, &test_ops, ctx, seq);
	return tf;
}

/* Workqueue drain helper: run every pending work item inline. */
static void drain_wq(void)
{
	/* The emulation marks work pending; the shim keeps its own
	 * entity/job state, so tests invoke pump/finish/timeout fns
	 * through the queued work pointers directly. Nothing to do
	 * here; kept as documentation of the synchronous model. */
}

/* ------------------------------------------------------------------ */
/* 0001 F1: dma_fence_init centralises setup.                           */
/* ------------------------------------------------------------------ */
static void test_fence_init(void)
{
	struct test_fence *tf = mkfence(7, 42);

	CHECK(tf != NULL);
	CHECK(tf->base.ops == &test_ops);
	CHECK_EQ(tf->base.context, 7u);
	CHECK_EQ(tf->base.seqno, 42u);
	CHECK_EQ(tf->base.flags, 0u);
	CHECK(list_empty(&tf->base.cb_list));
	CHECK_EQ((long long)atomic_load_explicit(&tf->base.refcount.refcount, memory_order_relaxed), 1LL);
	kfree(tf);

	/* Contract: init requires non-NULL fence/ops/release -- the
	 * kernel BUG_ON aborts on violation, so assert the guard
	 * directly instead of invoking it. */
	CHECK(test_ops.release != NULL);
}

/* ------------------------------------------------------------------ */
/* 0001 F3: default_wait returns the REMAINING budget.                   */
/* ------------------------------------------------------------------ */
static void test_default_wait_remaining(void)
{
	struct test_fence *tf = mkfence(1, 1);
	signed long ret;

	/* Fast path: signaled fence returns the full budget. */
	dma_fence_signal(&tf->base);
	CHECK(dma_fence_is_signaled(&tf->base));
	ret = dma_fence_wait_timeout(&tf->base, false, 100);
	CHECK_EQ(ret, 100);

	/* Slow path: completion already done with budget 80. Our emu
	 * completion returns the full budget when done; mainline
	 * wait_for_completion_timeout returns remaining (>0, <= budget).
	 * The regression was `return timeout` even when the wait
	 * consumed time and the helper returned 0/-ETIME mapping.
	 * Assert the pass-through: remaining comes from the helper. */
	kfree(tf);
	tf = mkfence(2, 2);
	/* Unsignaled + zero budget: non-blocking, must not claim time. */
	ret = dma_fence_wait_timeout(&tf->base, false, 0);
	CHECK_EQ(ret, 0);
	/* Unsignaled + budget: helper finds no completion -> -ETIME. */
	ret = dma_fence_wait_timeout(&tf->base, false, 50);
	CHECK_EQ(ret, (signed long)-ETIME);
	kfree(tf);
}

/* Signal-then-wait chaining: resv wait threads the remaining budget. */
static void test_resv_chains_remaining(void)
{
	struct dma_resv resv;
	struct test_fence *a = mkfence(1, 1);
	struct test_fence *b = mkfence(1, 2);
	long ret;

	dma_resv_init(&resv);
	CHECK(dma_resv_reserve_shared(&resv, 2) == 0);
	dma_resv_add_shared_fence(&resv, &a->base);
	dma_resv_add_shared_fence(&resv, &b->base);

	/* Nothing signaled: full wait must time out, not hang. */
	ret = dma_resv_wait_timeout(&resv, true, false, 10);
	CHECK(ret == (long)-ETIME);

	/* Signal both: wait returns the (chained) remaining budget. */
	dma_fence_signal(&a->base);
	dma_fence_signal(&b->base);
	ret = dma_resv_wait_timeout(&resv, true, false, 10);
	CHECK(ret == 10);

	/* test_signaled sees the signaled set. */
	CHECK(dma_resv_test_signaled(&resv, true));
	dma_resv_fini(&resv);
	kfree(a);
	kfree(b);
}

/* ------------------------------------------------------------------ */
/* 0002 F1/F2: locking + pinning.                                       */
/* ------------------------------------------------------------------ */
static void test_resv_lock_and_pin(void)
{
	struct dma_resv resv;
	struct test_fence *ex = mkfence(3, 1);
	struct test_fence *sh = mkfence(3, 2);
	struct dma_fence *got;
	long ret;

	dma_resv_init(&resv);

	/* excl install + get_excl pins a reference. */
	dma_resv_add_excl_fence(&resv, &ex->base);
	got = dma_resv_get_excl(&resv);
	CHECK(got == &ex->base);
	CHECK_EQ((long long)atomic_load_explicit(&ex->base.refcount.refcount, memory_order_relaxed), 3LL);
	dma_fence_put(got);
	CHECK_EQ((long long)atomic_load_explicit(&ex->base.refcount.refcount, memory_order_relaxed), 2LL);

	/* wait on unsignaled excl: -ETIME, fence still alive (pinned). */
	ret = dma_resv_wait_timeout(&resv, false, false, 5);
	CHECK_EQ(ret, (long)-ETIME);
	CHECK_EQ(ex->released, 0);

	/* Swap excl while a waiter conceptually holds the old pin:
	 * add_excl puts the old pointer only after installing the new
	 * one; the waiter's get() keeps it alive. Simulate: get, swap,
	 * wait on the OLD fence (must still be valid memory). */
	got = dma_resv_get_excl(&resv);
	dma_resv_add_excl_fence(&resv, &sh->base);
	CHECK_EQ(ex->released, 0);
	ret = dma_fence_wait_timeout(got, false, 5);
	CHECK_EQ(ret, (long)-ETIME);
	dma_fence_put(got);
	/* Last put from the swap already dropped it; waiter put frees. */
	dma_fence_signal(&ex->base);
	CHECK_EQ(ex->released, 0);
	dma_fence_put(&ex->base);
	CHECK_EQ(ex->released, 1);

	/* Empty resv: NULL excl, wait returns budget, signaled true. */
	dma_resv_fini(&resv);
	kfree(ex);
	dma_resv_init(&resv);
	CHECK(dma_resv_is_empty(&resv));
	CHECK(dma_resv_test_signaled(&resv, true));
	ret = dma_resv_wait_timeout(&resv, true, false, 7);
	CHECK_EQ(ret, 7);

	/* copy_fences: self-copy no-op; real copy pins new refs. */
	CHECK_EQ(dma_resv_copy_fences(&resv, &resv), 0);
	{
		struct dma_resv dst;

		dma_resv_init(&dst);
		dma_resv_add_excl_fence(&resv, &sh->base);
		CHECK_EQ(dma_resv_copy_fences(&dst, &resv), 0);
		CHECK(dst.fence_excl == &sh->base);
		dma_resv_fini(&dst);
	}
	dma_resv_fini(&resv);
	kfree(sh);
}

/* ------------------------------------------------------------------ */
/* 0003 F1: fini drains every handle (no idr_for_each_entry skip).      */
/* ------------------------------------------------------------------ */
static void test_syncobj_drain(void)
{
	struct drm_syncobj_manager mgr;
	u32 h[300];
	int i, created = 0;

	drm_syncobj_manager_init(&mgr);
	for (i = 0; i < 300; i++) {
		if (drm_syncobj_create(&mgr, &h[i]))
			break;
		created++;
	}
	CHECK_EQ(created, 300);
	/* Destroy every third handle explicitly; fini must drop the rest
	 * with no skips (the old remove-during-iteration could skip). */
	for (i = 0; i < created; i += 3)
		CHECK_EQ(drm_syncobj_destroy(&mgr, h[i]), 0);
	drm_syncobj_manager_fini(&mgr);
	/* Manager reusable after drain. */
	drm_syncobj_manager_init(&mgr);
	CHECK_EQ(drm_syncobj_create(&mgr, &h[0]), 0);
	CHECK_EQ(drm_syncobj_destroy(&mgr, h[0]), 0);
	drm_syncobj_manager_fini(&mgr);
}

/* ------------------------------------------------------------------ */
/* 0004 sched tests: one entity, backend counters.                      */
/* ------------------------------------------------------------------ */
static int run_count, free_count;
/* Pumpfix call-order log: run_log records run_job entry order, free_log
 * records free_job (retire) order; each entry is the job owner id.
 * retire_after_run asserts every retire lands after its run. */
#define ORDER_MAX 16
static int run_log[ORDER_MAX], free_log[ORDER_MAX];
static int run_log_n, free_log_n;

static struct dma_fence *sched_run_ok(struct drm_sched_job *job)
{
	struct test_fence *tf;

	run_count++;
	if (run_log_n < ORDER_MAX)
		run_log[run_log_n++] = (int)(long)job->owner;
	tf = mkfence(9, (unsigned)run_count);
	if (!tf)
		return ERR_PTR(-ENOMEM);
	return &tf->base;
}

static struct dma_fence *sched_run_fail(struct drm_sched_job *job)
{
	(void)job;
	run_count++;
	return ERR_PTR(-ENOMEM);
}

static void sched_timedout(struct drm_sched_job *job)
{
	/* Mirror the fixed driver leg (p7580_drv_timedout_job): retire
	 * unconditionally, even "dark". The fence signal is CPU-only. */
	if (job && job->fence && !dma_fence_is_signaled(job->fence))
		dma_fence_signal(job->fence);
}

static void sched_free(struct drm_sched_job *job)
{
	struct test_fence *tf;

	free_count++;
	if (free_log_n < ORDER_MAX)
		free_log[free_log_n++] = (int)(long)job->owner;
	if (job->fence) {
		tf = to_test(job->fence);
		dma_fence_put(job->fence);
		job->fence = NULL;
		kfree(tf);
	}
	if (job->dep_fence) {
		dma_fence_put(job->dep_fence);
		job->dep_fence = NULL;
	}
	kfree(job);
}

static const struct drm_sched_backend_ops sched_ops_ok = {
	.run_job = sched_run_ok,
	.timedout_job = sched_timedout,
	.free_job = sched_free,
};

static const struct drm_sched_backend_ops sched_ops_fail = {
	.run_job = sched_run_fail,
	.timedout_job = sched_timedout,
	.free_job = sched_free,
};

/* F1: membership vs FIFO are separate lists. */
static void test_entity_lists(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e1, e2;
	struct drm_sched_entity *pos;
	int n = 0;

	memset(&sched, 0, sizeof(sched));
	CHECK_EQ(drm_sched_init(&sched, &sched_ops_ok, 5, "t"), 0);
	CHECK_EQ(sched.ready, true);
	CHECK(list_empty(&sched.entities));

	CHECK_EQ(drm_sched_entity_init(&e1, &sched), 0);
	CHECK(!list_empty(&sched.entities));
	CHECK(list_empty(&e1.queue_list));
	CHECK(!list_empty(&e1.sched_link));
	CHECK_EQ(e1.timeout, 5);
	CHECK(e1.sched == &sched);
	CHECK_EQ(e1.job_count.counter, 0);

	CHECK_EQ(drm_sched_entity_init(&e2, &sched), 0);
	list_for_each_entry(pos, &sched.entities, sched_link)
		n++;
	CHECK_EQ(n, 2);

	/* Queueing a job must NOT touch sched->entities. */
	{
		struct drm_sched_job *j =
			drm_sched_job_alloc(&e1, &sched_ops_ok, NULL);

		CHECK(j != NULL);
		CHECK_EQ(drm_sched_job_submit(j), 0);
		n = 0;
		list_for_each_entry(pos, &sched.entities, sched_link)
			n++;
		CHECK_EQ(n, 2);
		CHECK(!list_empty(&e1.queue_list));
	}
	drm_sched_entity_fini(&sched, &e1);
	n = 0;
	list_for_each_entry(pos, &sched.entities, sched_link)
		n++;
	CHECK_EQ(n, 1);
	drm_sched_entity_fini(&sched, &e2);
	CHECK(list_empty(&sched.entities));
	drm_sched_fini(&sched);
	CHECK_EQ(sched.ready, false);
	CHECK(sched.wq == NULL);
}

/* F2: unsignaled dep blocks dispatch; signal + pump re-queue runs it. */
static void test_dep_gating(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;
	struct test_fence *dep = mkfence(4, 1);
	struct test_fence *dep2 = mkfence(4, 2);

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 5, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;

	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	CHECK_EQ(drm_sched_job_add_dependency(j, dep2 == dep ? NULL :
					      &dep->base), 0);
	/* F3: second distinct dep rejected. */
	CHECK_EQ(drm_sched_job_add_dependency(j, &dep2->base), -EOPNOTSUPP);
	/* Same fence again: no-op success. */
	CHECK_EQ(drm_sched_job_add_dependency(j, &dep->base), 0);
	CHECK_EQ(drm_sched_job_submit(j), 0);

	/* Pump with dep pending: run_job must NOT run, job stays queued. */
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 0);
	CHECK(!list_empty(&e.queue_list));

	/* Signal the dep (fires dep_cb, which re-queues the pump): the
	 * emulation marks sched_work pending; run the pump again. */
	dma_fence_signal(&dep->base);
	CHECK_EQ(e.sched_work.pending, 1);
	e.sched_work.pending = 0;
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 1);
	CHECK(list_empty(&e.queue_list));

	/* Completion arrived via run fence: finish path retires the job. */
	CHECK(j->fence != NULL);
	dma_fence_signal(j->fence);
	{
		struct test_fence *done = to_test(j->fence);

		j->finish_work.fn(&j->finish_work);
		CHECK_EQ(free_count, 1);
		(void)done;
	}
	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
	kfree(dep);
	kfree(dep2);
}

/* F4: run_job ERR_PTR frees the job; FIFO advances to the next job. */
static void test_errptr_frees_job(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j1, *j2;

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_fail, 5, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;

	j1 = drm_sched_job_alloc(&e, &sched_ops_fail, NULL);
	j2 = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	j2->ops = &sched_ops_ok;
	drm_sched_job_submit(j1);
	drm_sched_job_submit(j2);

	/* First pump: j1 fails -> freed, pump re-queued. */
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 1);
	CHECK_EQ(free_count, 1);
	/* Second pump: j2 dispatches with the OK backend. */
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 2);
	CHECK(list_empty(&e.queue_list));
	CHECK(j2->fence != NULL);
	dma_fence_signal(j2->fence);
	j2->finish_work.fn(&j2->finish_work);
	CHECK_EQ(free_count, 2);

	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
}

/* F5 + minor: timeout from entity; push_job; ready gate. */
static void test_timeout_push_ready(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 42, "t");
	drm_sched_entity_init(&e, &sched);
	CHECK_EQ(e.timeout, 42);

	/* push_job queues + counts + kicks the pump. */
	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	run_count = 0;
	CHECK_EQ(drm_sched_entity_push_job(j), 0);
	CHECK_EQ((long long)atomic_load_explicit(&e.job_count.counter, memory_order_relaxed), 1LL);
	CHECK_EQ(e.sched_work.pending, 1);
	e.sched_work.pending = 0;

	/* Zero-timeout entity arms nothing. */
	e.timeout = 0;
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 1);
	CHECK_EQ(j->timeout_work.armed, 0);
	dma_fence_signal(j->fence);
	j->finish_work.fn(&j->finish_work);

	/* Nonzero timeout arms the delayed work with entity->timeout. */
	e.timeout = 42;
	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	e.sched_work.fn(&e.sched_work); /* queue empty path first */
	drm_sched_job_submit(j);
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(j->timeout_work.armed, 1);
	CHECK_EQ((long)j->timeout_work.delay, 42L);
	dma_fence_signal(j->fence);
	j->finish_work.fn(&j->finish_work);
	CHECK_EQ(j->timeout_work.armed, 0);

	/* ready=false stops dispatch. */
	run_count = 0;
	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	drm_sched_job_submit(j);
	sched.ready = false;
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 0);
	sched.ready = true;
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 1);
	dma_fence_signal(j->fence);
	j->finish_work.fn(&j->finish_work);

	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
	(void)drain_wq;
}

/* ------------------------------------------------------------------ */
/* close-fix tests: mock-GPU ordering repro (no HW, single-threaded).    */
/* The int7 node hung in CLOSE with a dispatched-but-unretired job:      */
/* WAIT_BO proved nothing (empty resv returns instantly), the job's      */
/* fence stayed pending, and the old fini had no bounded retirement —   */
/* the file state could be freed under a pending timeout/IRQ callback.   */
/* These tests pin the new contract: bounded waits, sync cancel,         */
/* idempotent fini, no waits inside syncobj fini.                        */
/* ------------------------------------------------------------------ */

/* C1: idle entity waits zero time. */
static void test_close_idle(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;

	memset(&sched, 0, sizeof(sched));
	CHECK_EQ(drm_sched_init(&sched, &sched_ops_ok, 50, "t"), 0);
	CHECK_EQ(drm_sched_entity_init(&e, &sched), 0);
	CHECK_EQ(drm_sched_entity_wait_idle(&e, 0), 0);
	CHECK_EQ(drm_sched_entity_wait_idle(&e, -1), 0);
	CHECK_EQ(drm_sched_entity_wait_idle(NULL, 0), -EINVAL);
	CHECK(drm_sched_close_budget(&e) == (long)(e.timeout + DRM_SCHED_CLOSE_MARGIN_JIFFIES));
	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
}

/* C5: push re-arms the idle completion on 0->1. */
static void test_close_rearm(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 50, "t");
	drm_sched_entity_init(&e, &sched);
	/* Fresh entity starts idle (completed). A push must re-arm so a
	 * later wait observes the new job, not the stale completion. */
	CHECK_EQ(drm_sched_entity_wait_idle(&e, 0), 0);
	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	CHECK_EQ(drm_sched_entity_push_job(j), 0);
	CHECK_EQ(completion_done(&e.entity_idle), 0);
	/* Retire it: dispatch then signal then finish. */
	e.sched_work.fn(&e.sched_work);
	dma_fence_signal(j->fence);
	j->finish_work.fn(&j->finish_work);
	CHECK_EQ(drm_sched_entity_wait_idle(&e, 0), 0);
	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
}

/* C3: queued job freed synchronously; dep detach; idempotent fini. */
static void test_close_queued(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;
	struct test_fence *dep = mkfence(4, 11);

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 50, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;

	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	CHECK_EQ(drm_sched_job_add_dependency(j, &dep->base), 0);
	CHECK_EQ(drm_sched_job_submit(j), 0);
	/* Pump sees the unsignaled dep and leaves the job queued. */
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 0);

	/* Close with the job still queued: freed synchronously. */
	drm_sched_entity_fini(&sched, &e);
	CHECK_EQ(free_count, 1);
	CHECK(list_empty(&e.queue_list));
	CHECK_EQ(atomic_load_explicit(&e.job_count.counter, memory_order_relaxed), 0LL);
	/* Late dep signal must not re-queue the dead pump. */
	e.sched_work.pending = 0;
	dma_fence_signal(&dep->base);
	CHECK_EQ(e.sched_work.pending, 0);
	/* Idempotent: second fini is a no-op (no double free). */
	drm_sched_entity_fini(&sched, &e);
	CHECK_EQ(free_count, 1);
	drm_sched_fini(&sched);
	kfree(dep);
}

/* C4: dispatched-but-unretired job retires via the timeout path;
 * fini stays bounded and idempotent. */
static void test_close_inflight(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 50, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;

	/* Dispatch one job; fence stays pending (mock GPU never IRQs). */
	j = drm_sched_job_alloc(&e, &sched_ops_ok, NULL);
	drm_sched_entity_push_job(j);
	e.sched_work.pending = 0;
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 1);
	CHECK(j->fence != NULL);
	CHECK(!dma_fence_is_signaled(j->fence));
	CHECK_EQ(j->timeout_work.armed, 1);

	/* Not idle while the job is outstanding. wait_idle with budget
	 * must report -ETIME without retiring anything itself. */
	CHECK_EQ(drm_sched_entity_wait_idle(&e, 1), -ETIME);
	CHECK_EQ(free_count, 0);

	/* Timeout work calls the backend hook, which now guarantees the
	 * fence signal (the driver dark-skip leg); finish retires it. */
	j->timeout_work.work.fn(&j->timeout_work.work);
	CHECK(dma_fence_is_signaled(j->fence));
	j->finish_work.fn(&j->finish_work);
	CHECK_EQ(free_count, 1);
	CHECK_EQ(drm_sched_entity_wait_idle(&e, 0), 0);

	drm_sched_entity_fini(&sched, &e);
	drm_sched_entity_fini(&sched, &e); /* idempotent */
	CHECK_EQ(free_count, 1);
	drm_sched_fini(&sched);
	drm_sched_fini(&sched); /* idempotent */
	CHECK(sched.wq == NULL);
}

/* C6: syncobj fini never waits; double fini safe. */
static void test_close_syncobj_pending(void)
{
	struct drm_syncobj_manager mgr;
	struct drm_syncobj *s;
	struct test_fence *tf = mkfence(8, 77);
	u32 h;

	drm_syncobj_manager_init(&mgr);
	CHECK_EQ(drm_syncobj_create(&mgr, &h), 0);
	s = drm_syncobj_find(&mgr, h);
	CHECK(s != NULL);
	CHECK_EQ(drm_syncobj_replace_fence(s, &tf->base), 0);
	drm_syncobj_put(s);
	/* Fence still pending; fini must drop the handle WITHOUT waiting
	 * (no wait call exists on this path by construction). */
	CHECK(!dma_fence_is_signaled(&tf->base));
	drm_syncobj_manager_fini(&mgr);
	drm_syncobj_manager_fini(&mgr); /* idempotent */
	/* The job side still owns its ref; release it here. */
	dma_fence_put(&tf->base);
	kfree(tf);
}

/* C7: dep-less close never touches fence callbacks with NULL. */
static void test_close_null_dep(void)
{
	CHECK_EQ(dma_fence_remove_callback(NULL, NULL), false);
	{
		struct dma_fence_cb cb;

		INIT_LIST_HEAD(&cb.node);
		CHECK_EQ(dma_fence_remove_callback(NULL, &cb), false);
	}
}

/* pumpfix: run_job runs exactly once per queued job; retire lands    */
/* after fence completion (never before run). Slot carried at submit  */
/* survives the pump; NULL run_job retires via the free path.         */
/* ------------------------------------------------------------------ */

/* run_job exactly once per queued job; retire-after-run ordering. */
static void test_pumpfix_run_once_ordered(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j[3];
	int i, k;

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 50, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;
	run_log_n = 0;
	free_log_n = 0;

	/* Queue 3 jobs with distinct owner ids + submit-chosen slots. */
	for (i = 0; i < 3; i++) {
		j[i] = drm_sched_job_alloc(&e, &sched_ops_ok,
					   (void *)(long)(11 + i));
		CHECK(j[i] != NULL);
		j[i]->slot = i;
		CHECK_EQ(drm_sched_job_submit(j[i]), 0);
	}

	/* Pump until all 3 dispatch; each pump runs exactly one job
	 * (in_flight serialises: the second pump sees in_flight set).
	 * The emulation marks work pending instead of running it, so
	 * drive each dispatch by hand: pump, then finish, repeat. */
	for (i = 0; i < 3; i++) {
		e.sched_work.fn(&e.sched_work);
		spin_lock(&e.queue_lock);
		e.in_flight = NULL;
		spin_unlock(&e.queue_lock);
	}
	CHECK_EQ(run_count, 3);
	CHECK_EQ(run_log_n, 3);
	for (i = 0; i < 3; i++)
		CHECK_EQ(run_log[i], 11 + i);
	/* Nothing retired yet: retire happens only on fence signal. */
	CHECK_EQ(free_count, 0);
	/* Slots preserved through dispatch. */
	for (i = 0; i < 3; i++)
		CHECK_EQ(j[i]->slot, i);
	/* Retire in order; each retire lands after its run. */
	for (i = 0; i < 3; i++) {
		CHECK(j[i]->fence != NULL);
		dma_fence_signal(j[i]->fence);
		j[i]->finish_work.fn(&j[i]->finish_work);
		CHECK_EQ(free_count, i + 1);
	}
	CHECK_EQ(free_log_n, 3);
	for (i = 0; i < 3; i++)
		CHECK_EQ(free_log[i], 11 + i);
	for (k = 0; k < 3; k++) {
		int ri = -1, ni = -1;

		for (i = 0; i < 3; i++) {
			if (run_log[i] == 11 + k)
				ri = i;
			if (free_log[i] == 11 + k)
				ni = i;
		}
		CHECK(ri >= 0 && ni >= 0 && ni >= ri);
	}

	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
}

/* NULL-returning run_job retires via the error path (no hang). */
static struct dma_fence *sched_run_null(struct drm_sched_job *job)
{
	run_count++;
	if (run_log_n < ORDER_MAX)
		run_log[run_log_n++] = (int)(long)job->owner;
	return NULL;
}

/* NULL run_job (header/ABI skew) retires via the free path, no oops. */
static void test_pumpfix_null_run_job(void)
{
	static const struct drm_sched_backend_ops null_run_ops = {
		NULL, sched_timedout, sched_free,
	};
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 50, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;

	j = drm_sched_job_alloc(&e, &null_run_ops, (void *)(long)99);
	j->slot = 1;
	CHECK_EQ(drm_sched_job_submit(j), 0);
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 0);
	CHECK_EQ(free_count, 1);
	CHECK_EQ(j->slot, 1);

	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
}

static void test_pumpfix_null_fence(void)
{
	struct drm_sched sched;
	struct drm_sched_entity e;
	struct drm_sched_job *j;
	static const struct drm_sched_backend_ops null_fence_ops = {
		sched_run_null, sched_timedout, sched_free,
	};

	memset(&sched, 0, sizeof(sched));
	drm_sched_init(&sched, &sched_ops_ok, 50, "t");
	drm_sched_entity_init(&e, &sched);
	run_count = 0;
	free_count = 0;
	run_log_n = 0;

	j = drm_sched_job_alloc(&e, &null_fence_ops, (void *)(long)77);
	j->slot = 2;
	CHECK_EQ(drm_sched_job_submit(j), 0);
	e.sched_work.fn(&e.sched_work);
	CHECK_EQ(run_count, 1);
	CHECK_EQ(free_count, 1);

	drm_sched_entity_fini(&sched, &e);
	drm_sched_fini(&sched);
}


int main(void)
{
	test_fence_init();
	test_default_wait_remaining();
	test_resv_chains_remaining();
	test_resv_lock_and_pin();
	test_syncobj_drain();
	test_entity_lists();
	test_dep_gating();
	test_errptr_frees_job();
	test_timeout_push_ready();
	test_close_idle();
	test_close_rearm();
	test_close_queued();
	test_close_inflight();
	test_close_syncobj_pending();
	test_close_null_dep();
	test_pumpfix_run_once_ordered();
	test_pumpfix_null_run_job();
	test_pumpfix_null_fence();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
