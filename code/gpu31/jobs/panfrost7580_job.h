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
#include <linux/types.h>
#include <linux/dma-mapping.h>
#else
#define __iomem
#include <stdint.h>
#endif

#ifndef PANFROST7580_JOB_H
#define PANFROST7580_JOB_H

#include "panfrost7580_jm_hw.h"

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/dma-mapping.h>
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
	uint32_t out_sync;		/* syncobj handle for run_job install
					 * (0 = none; validated at SUBMIT,
					 * installed from render_done_fence
					 * alias on the dispatch worker) */

	/* Fences. done_fence owns the run_job ref and is signaled by the
	 * JOB_INT handler (or the fault-recovery path, F-JOB4);
	 * render_done_fence is a non-owning alias of the same fence
	 * handed to out_sync/sched (F-JOB3: never put twice).
	 * in_fences are explicit SUBMIT deps (owned here). */
	void *done_fence;
	void *render_done_fence;
	void **in_fences;
	uint32_t in_fence_count;

	/* BO reservation set (file-private handles + resv pointers). */
	uint32_t *bo_handles;
	uint32_t bo_count;
	/* Dispatch context stamped at SUBMIT (probenumeric): the async
	 * run_job/timedout_job/free_job workers must NOT re-derive the
	 * file state from the entity pointer (container_of(ent, ...,
	 * entity[0]) breaks under container layouts -- int14 retired
	 * without run_job because df came back wrong). Opaque void* so
	 * this header stays drm-free; submit.c casts to struct
	 * p7580_drv_file / struct p7580_drv. */
	void *file_priv;
	void *drv_priv;
	/* Monotonic run_job entry count (probenumeric): incremented as
	 * the first statement of p7580_drv_run_job, printed by free_job
	 * as ran=N. Proves call order even if a print is lost. */
	unsigned int run_seq;
	/* Owned BO object pointers for the completion retire path
	 * (F-JT2 + gpu31-coherency-race): SUBMIT stores one referenced
	 * pointer per handle here (opaque void* so this header stays
	 * drm-free); the IRQ handler pre-signal-syncs them with
	 * p7580_gem_job_bos_pre_signal_sync before the fence signals,
	 * and the driver's free_job walks them with
	 * p7580_gem_job_done_sync and drops the refs before
	 * p7580_job_free releases the array. NULL when the job
	 * references no BOs. */
	void **bos;
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
	dma_addr_t pgd_dma;		/* AS0 root (set by probe; recover
					 * rebinds it after GPU reset) */
	struct p7580_submit_job *in_flight[P7580_NUM_JOB_SLOTS];
	spinlock_t hw_lock;		/* serialises HEAD/CONFIG/COMMAND */
	struct mutex reset_lock;	/* serialises fault recovery */
	unsigned int fence_ctx[P7580_NUM_JOB_SLOTS];
	uint64_t emit_seqno[P7580_NUM_JOB_SLOTS];
	/* Job-lifecycle trace budget: decremented (under hw_lock) on each
	 * dispatch / IRQ / fence-signal print, so the log stays bounded.
	 * 0 disables tracing; set via the p7580_job_trace module param
	 * (job.c). Default P7580_JOB_TRACE_DEFAULT (first N jobs only). */
	unsigned int trace_left;
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
		       uint32_t js_present, uint64_t shader_present,
		       dma_addr_t pgd_dma);
#else
#include <stdint.h>
typedef uint64_t p7580_dma_addr_t;
int p7580_job_dev_init(struct p7580_job_dev *jd, void *regs,
		       uint32_t js_present, uint64_t shader_present,
		       p7580_dma_addr_t pgd_dma);
#endif
void p7580_job_dev_fini(struct p7580_job_dev *jd);
/* CLEAR + unmask after reset/resume (power held by caller). */
void p7580_job_dev_rearm(struct p7580_job_dev *jd);
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
/* -> sched submit -> install out-sync fence. args->in_syncs/bo_handles */
/* must point at KERNEL-staged u32 arrays (the ioctl layer copies with  */
/* copy_from_user first); this function never touches userspace         */
/* (F-JOB2: do not pass __user pointers here). */
int p7580_job_submit(struct p7580_job_dev *jd, struct p7580_file_ctx *fctx,
		     const struct p7580_submit *args);

/* Fault recovery: stop slots, GPU soft reset, resume (non-IRQ ctx). */
int p7580_job_recover(struct p7580_job_dev *jd, int fault_slot,
		      uint32_t status);

/* Idle check for suspend/close paths. */
int p7580_job_is_idle(struct p7580_job_dev *jd);

#endif /* PANFROST7580_JOB_H */
