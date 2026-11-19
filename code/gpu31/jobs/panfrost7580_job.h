/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_job.h - submit-path state for the T720 panfrost port.
 *
 * Submitter: one open-file context owns one drm_sched_entity per job
 * slot (NUM_JOB_SLOTS == P7580_JM_MAX_SLOTS == 3); one sched_job per
 * SUBMIT ioctl batch; run_job programs the slot and returns the
 * in-flight fence; JOB_INT IRQ completion signals it; timedout_job
 * runs the slot reset + GPU resume path.
 *
 * The in-tree build (with __KERNEL__) sees real lock types; the host
 * unit test (__KERNEL__ unset) sees opaque placeholders so jm_hw.h
 * stays pure-C.
 */
#ifdef __KERNEL__
#include <linux/io.h>
#else
#define __iomem
#include <stdint.h>
#endif

#ifndef PANFROST7580_JOB_H
#define PANFROST7580_JOB_H

#include "panfrost7580_jm_hw.h"

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#endif

/* ------------------------------------------------------------------ */
/* Driver topology                                                    */
/* ------------------------------------------------------------------ */
#define P7580_NUM_JOB_SLOTS	P7580_JM_MAX_SLOTS
#define P7580_MAX_IN_SYNCS	16u	/* cap on SUBMIT in_sync_count */
#define P7580_MAX_SUBMIT_BOS	64u	/* cap on SUBMIT bo_handle_count */

/* Per-submit job: mirrors v5.2 struct panfrost_job (panfrost_job.h). */
struct p7580_submit_job {
	/* Atom identity for logs/dumps: SUBMIT order per file context. */
	uint32_t seqno;

	/* Routing + HW program. */
	int slot;			/* 0..2, from requirements */
	uint64_t jc;			/* GPU address of job descriptor */
	uint32_t requirements;		/* PANFROST_JD_REQ_* */
	uint32_t as_nr;			/* address space bound to slot */
	uint64_t affinity;		/* shader-present mask */
	uint32_t flush_id;		/* recorded, unused (no FLUSH_REDUCTION) */

	/* Fences. done_fence is signaled by the JOB_INT handler;
	 * render_done_fence is the sched retirement fence returned by
	 * run_job. in_fences are explicit SUBMIT deps (owned here). */
	void *done_fence;
	void *render_done_fence;
	void **in_fences;
	uint32_t in_fence_count;

	/* BO reservation set (file-private handles + resv pointers). */
	uint32_t *bo_handles;
	uint32_t bo_count;
};

/* Per-file submit context: one sched entity per slot. */
struct p7580_file_ctx {
	uint32_t next_seqno;
	void *sched_entity[P7580_NUM_JOB_SLOTS];
	void *syncobj_mgr;		/* backport drm_syncobj_manager */
};

#ifdef __KERNEL__
struct p7580_job_dev {
	void __iomem *regs;
	uint32_t js_present;		/* from GPU_JS_PRESENT @probe */
	uint64_t shader_present;	/* affinity source */
	struct p7580_submit_job *in_flight[P7580_NUM_JOB_SLOTS];
	spinlock_t hw_lock;		/* serialises HEAD/CONFIG/COMMAND */
	struct mutex reset_lock;	/* serialises fault recovery */
	unsigned int fence_ctx[P7580_NUM_JOB_SLOTS];
	uint64_t emit_seqno[P7580_NUM_JOB_SLOTS];
};
#else
struct p7580_job_dev {
	void *regs;
	uint32_t js_present;
	uint64_t shader_present;
	struct p7580_submit_job *in_flight[P7580_NUM_JOB_SLOTS];
	void *hw_lock;
	void *reset_lock;
	void *sched[P7580_NUM_JOB_SLOTS];
	void *fence_ctx;
};
#endif

/* ------------------------------------------------------------------ */
/* Entry points implemented in panfrost7580_job.c (in-tree names in    */
/* comments so the integrator can map them to drm_driver hooks).       */
/* ------------------------------------------------------------------ */
#ifdef __KERNEL__
int p7580_job_dev_init(struct p7580_job_dev *jd, void __iomem *regs,
		       uint32_t js_present, uint64_t shader_present);
#else
int p7580_job_dev_init(struct p7580_job_dev *jd, void *regs,
		       uint32_t js_present, uint64_t shader_present);
#endif
/* Program HEAD_NEXT/AFFINITY_NEXT/CONFIG_NEXT + COMMAND_NEXT=START.   */
void p7580_job_hw_submit(struct p7580_job_dev *jd,
			 struct p7580_submit_job *job);
/* JOB_INT handler: demux DONE/ERR per slot (IRQ context). */
int p7580_job_irq(int irq, struct p7580_job_dev *jd);

/* Scheduler backend: run_job / timedout_job / free_job. */
void *p7580_job_run(struct p7580_job_dev *jd,
		    struct p7580_submit_job *job);
void p7580_job_timedout(struct p7580_job_dev *jd,
			struct p7580_submit_job *job);
void p7580_job_free(struct p7580_submit_job *job);

/* SUBMIT ioctl body: validate -> resolve syncobj deps -> reserve BOs  */
/* -> sched submit -> install out-sync fence. */
int p7580_job_submit(struct p7580_job_dev *jd, struct p7580_file_ctx *fctx,
		     const struct p7580_submit *args);

/* Fault recovery: stop slots, GPU soft reset, resume (non-IRQ ctx). */
int p7580_job_recover(struct p7580_job_dev *jd, int fault_slot,
		      uint32_t status);

/* Idle check for suspend/close paths. */
int p7580_job_is_idle(struct p7580_job_dev *jd);

#endif /* PANFROST7580_JOB_H */
