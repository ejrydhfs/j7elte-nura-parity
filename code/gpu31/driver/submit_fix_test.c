/* SPDX-License-Identifier: GPL-2.0 */
/*
 * submit_fix_test.c - host test for the SUBMIT-deadlock fixes
 * (docs/reports/gpu31-submit-fix.md).
 *
 * Pure-C models of the exact kernel rules the fixes rely on; no device,
 * no threads, no Xorg risk. Mirrors the style of jobs/job_test.c.
 *
 *  T1 resv-lock discipline (S1): the fixed submit path must call
 *     dma_resv_get_excl() with resv->lock UNHELD (the 0002 shim locks
 *     internally; the old outer lock self-deadlocked the plain
 *     non-recursive mutex in D state).
 *  T2 resume-error abort (S2): a failed pm_runtime_get_sync() must abort
 *     the submit before any MMU-map/HEAD programming (dispatch == 0) and
 *     balance the usage count (put_noidle == 1).
 *  T3 timeout signals the timed-out job (S3): a job whose START was
 *     swallowed (stuck slot, never in flight) must still end signaled
 *     exactly once within one timeout + one recovery.
 *  T4 recovery is bounded: a slot whose COMMAND_NEXT never drains still
 *     exits recover() with -ETIMEDOUT after <= RESET_TIMEOUT_MS of
 *     (model) jiffies; fences are still signaled on that path.
 *  T5 signal idempotence: double signal returns -EINVAL the second time;
 *     the timedout guard (is_signaled check) skips an IRQ-won race.
 *  T6 spurious timeout: an already-signaled fence is never re-signaled.
 *
 * Build: cc -Wall -Wextra -o submit_fix_test submit_fix_test.c
 *        && ./submit_fix_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* P7580_JOB_TIMEOUT_MS / P7580_RESET_TIMEOUT_MS from jm_hw.h. */
#define JOB_TIMEOUT_MS		5000u
#define RESET_TIMEOUT_MS	3000u

static int fails = 0;

#define CHECK(cond) do {					\
	if (!(cond)) {						\
		printf("FAIL %d: %s\n", __LINE__, #cond);	\
		fails++;					\
	}							\
} while (0)

/* ------------------------------------------------------------------ */
/* T1: non-recursive mutex model of resv->lock + 0002 get_excl.        */
/* ------------------------------------------------------------------ */

struct fake_mutex {
	int locked;	/* 0 free, 1 held */
};

static int fake_mutex_lock(struct fake_mutex *m)
{
	/* Plain mutex: locking a held mutex sleeps uninterruptibly.
	 * The model reports the deadlock instead of sleeping. */
	if (m->locked)
		return -1;	/* would sleep forever (D state) */
	m->locked = 1;
	return 0;
}

static void fake_mutex_unlock(struct fake_mutex *m)
{
	m->locked = 0;
}

/* 0002 shim model: get_excl locks internally, exactly like the real one. */
struct fake_fence {
	int signaled;	/* bit */
	int signals;	/* dma_fence_signal calls that took effect */
};

static struct fake_fence *fake_get_excl(struct fake_mutex *m,
					 struct fake_fence *excl,
					 int *deadlock)
{
	if (fake_mutex_lock(m)) {
		*deadlock = 1;
		return NULL;
	}
	/* ... read excl pointer (ref taken) ... */
	fake_mutex_unlock(m);
	return excl;
}

static void run_t1(void)
{
	struct fake_mutex lock;
	struct fake_fence excl;
	int deadlock;
	struct fake_fence *f;

	/* Old shape: outer lock held across get_excl -> self-deadlock. */
	memset(&lock, 0, sizeof(lock));
	memset(&excl, 0, sizeof(excl));
	deadlock = 0;
	fake_mutex_lock(&lock);
	f = fake_get_excl(&lock, &excl, &deadlock);
	CHECK(deadlock == 1);
	CHECK(f == NULL);
	fake_mutex_unlock(&lock);

	/* Fixed shape (what the patch emits): no outer lock. */
	memset(&lock, 0, sizeof(lock));
	deadlock = 0;
	f = fake_get_excl(&lock, &excl, &deadlock);
	CHECK(deadlock == 0);
	CHECK(f == &excl);
	CHECK(lock.locked == 0);	/* get_excl released internally */
}

/* ------------------------------------------------------------------ */
/* T2: pm_runtime_get_sync error aborts before dispatch.               */
/* ------------------------------------------------------------------ */

static int model_submit(int pm_ret, int *dispatched, int *put_noidle)
{
	int ret;

	/* Fixed override shape (submit.c): */
	ret = pm_ret;	/* pm_runtime_get_sync(drv->dev) */
	if (ret < 0) {
		/* pm_runtime_put_noidle(drv->dev); */
		(*put_noidle)++;
		return ret;
	}
	/* p7580_drv_submit(...): MMU-map + HEAD/START programming. */
	(*dispatched)++;
	return 0;
}

static void run_t2(void)
{
	int dispatched, put_noidle, rc;

	dispatched = 0;
	put_noidle = 0;
	rc = model_submit(-5 /* -EIO */, &dispatched, &put_noidle);
	CHECK(rc == -5);
	CHECK(dispatched == 0);	/* nothing programmed on dark GPU */
	CHECK(put_noidle == 1);	/* usage count balanced */

	dispatched = 0;
	put_noidle = 0;
	rc = model_submit(0, &dispatched, &put_noidle);
	CHECK(rc == 0);
	CHECK(dispatched == 1);
	CHECK(put_noidle == 0);
}

/* ------------------------------------------------------------------ */
/* Fence + recover models for T3-T6.                                   */
/* ------------------------------------------------------------------ */

static int fence_is_signaled(struct fake_fence *f)
{
	return f->signaled;
}

/* dma_fence_signal model: idempotent, second call returns -EINVAL. */
static int fence_signal(struct fake_fence *f)
{
	if (f->signaled)
		return -22;	/* -EINVAL */
	f->signaled = 1;
	f->signals++;
	return 0;
}

/* Recover model: drain poll bounded by RESET_TIMEOUT_MS, then signal the
 * latched in-flight job's fence (F-JOB4 rule). stuck_cmd != 0 means the
 * slot never drains. Returns 0 or -ETIMEDOUT (-110). */
#define ETIMEDOUT	110

struct fake_dev {
	struct fake_fence *in_flight;
	int cmd_next;	/* slot COMMAND_NEXT, nonzero = busy */
};

static int model_recover(struct fake_dev *d, unsigned budget_ms)
{
	unsigned waited = 0;
	int ret = 0;

	/* Drain poll (msleep(1) steps in the real code). */
	while (d->cmd_next) {
		if (waited >= budget_ms) {
			ret = -ETIMEDOUT;
			break;
		}
		waited++;
	}
	/* Clear the slot out of flight under hw_lock (model). */
	/* ... */
	if (d->in_flight && !fence_is_signaled(d->in_flight))
		fence_signal(d->in_flight);
	return ret;
}

/* Timedout model (fixed p7580_job_timedout): early-out if signaled,
 * else recover, then signal OUR fence if still pending. */
static void model_timedout(struct fake_dev *d, struct fake_fence *ours)
{
	if (ours && fence_is_signaled(ours))
		return;
	model_recover(d, RESET_TIMEOUT_MS);
	if (ours && !fence_is_signaled(ours))
		fence_signal(ours);
}

static void run_t3(void)
{
	/* Swallowed START: slot stuck busy, timed-out job never entered
	 * in_flight; in_flight holds an older stuck job (or NULL). */
	struct fake_dev d;
	struct fake_fence old, ours;

	memset(&old, 0, sizeof(old));
	memset(&ours, 0, sizeof(ours));
	memset(&d, 0, sizeof(d));
	d.cmd_next = 1;		/* stuck: never drains */
	d.in_flight = &old;	/* recover latches the OTHER job */

	model_timedout(&d, &ours);
	CHECK(fence_is_signaled(&ours));	/* S3: ours signaled */
	CHECK(ours.signals == 1);		/* exactly once */
	CHECK(fence_is_signaled(&old));		/* F-JOB4: old too */
}

static void run_t4(void)
{
	/* Same stuck slot, but assert the bound explicitly: recover
	 * returns -ETIMEDOUT and still signals. */
	struct fake_dev d;
	struct fake_fence old;
	int rc;

	memset(&old, 0, sizeof(old));
	memset(&d, 0, sizeof(d));
	d.cmd_next = 1;
	d.in_flight = &old;
	rc = model_recover(&d, RESET_TIMEOUT_MS);
	CHECK(rc == -ETIMEDOUT);
	CHECK(fence_is_signaled(&old));
}

static void run_t5(void)
{
	struct fake_fence f;
	int r1, r2;

	memset(&f, 0, sizeof(f));
	r1 = fence_signal(&f);
	r2 = fence_signal(&f);
	CHECK(r1 == 0);
	CHECK(r2 == -22);	/* -EINVAL: already signaled */
	CHECK(f.signals == 1);
}

static void run_t6(void)
{
	/* IRQ-won race: fence already signaled when timeout fires ->
	 * timedout must not touch it (no second signal). */
	struct fake_dev d;
	struct fake_fence ours;

	memset(&ours, 0, sizeof(ours));
	memset(&d, 0, sizeof(d));
	ours.signaled = 1;	/* IRQ signaled first */
	ours.signals = 1;
	d.in_flight = NULL;
	model_timedout(&d, &ours);
	CHECK(ours.signals == 1);	/* no re-signal */
}

int main(void)
{
	run_t1();
	run_t2();
	run_t3();
	run_t4();
	run_t5();
	run_t6();

	if (fails == 0)
		printf("submit_fix_test: all checks passed\n");
	else
		printf("submit_fix_test: %d FAILURE(S)\n", fails);
	return fails ? 1 : 0;
}
