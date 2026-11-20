/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_job.c - job slot management and SUBMIT path for the
 * Mali-T720 MP2 (Exynos 7580, j7elte), Nura 3.10.108 panfrost port.
 *
 * Mirrors upstream v5.2 panfrost_job.c structure (slot select, hw
 * submit, IRQ demux, timeout recovery) adapted to 3.10 + the T720:
 *
 *  v5.2 construct              | this port
 *  ----------------------------+------------------------------------
 *  NUM_JOB_SLOTS (features)    | P7580_NUM_JOB_SLOTS = 3 (js_present 0x7)
 *  FLUSH_REDUCTION cfg/flush_id| dropped (not in t72x feature list)
 *  JOBCHAIN_DISAMBIGUATION     | dropped (not in t72x feature list)
 *  10649 START_MMU             | always set (t72x issue list HAS 10649)
 *  pm_runtime get/put          | dropped (vendor pd-g3d notifier owns
 *                              | power; M0 already resolves clocks)
 *  devfreq record_transition   | hook point left for G31_Power (no-op here)
 *  reservation_object (ww)     | backport dma_resv (plain mutex, 0002)
 *  drm_syncobj (timeline)      | backport binary syncobj (0003)
 *  drm_sched full ops          | backport FIFO shim ops (0004):
 *                              | run_job/timedout_job/free_job only
 *  resubmit_jobs/karma/stop    | dropped (shim has no stop/resubmit)
 *                              | recovery restarts slots directly
 *
 * SHIM-CALLSITES: every call into the 0001-0004 shims below is marked
 * with the patch number so G31_Sched/G31_DrmCore can re-verify them
 * against the landed tree at integration.
 * Locking: hw_lock (spinlock, IRQ-safe) serialises the HEAD/CONFIG/
 * COMMAND sequence per slot and the in_flight[] slot assignment. The
 * IRQ handler only touches in_flight/done_fence under that lock and
 * never sleeps. timedout_job and p7580_job_recover run in process
 * context (shim timeout workqueue): they take reset_lock, then
 * hw_lock only for register pokes.
 *
 * Fences: each job owns exactly one ref (job->done_fence), created in
 * p7580_job_run. job->render_done_fence is a non-owning alias of the
 * same fence handed to the sched/out-sync paths; only done_fence is
 * ever signaled or put. Fault recovery signals done_fence exactly
 * once so waiters never hang (see p7580_job_recover).
 */
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/log2.h>

/* At integration these resolve to the landed backport files:
 *  drivers/base/{dma-fence,dma-resv}.{c,h} (0001/0002) and
 *  drivers/gpu/drm/{drm_syncobj,drm_sched}.{c,h} (0003/0004).
 * The -I$(STAGE)/drivers/gpu/drm form below matches the staged dir. */
#include <linux/dma-fence.h>		/* 0001 */
#include <linux/dma-resv.h>		/* 0002 */
#include <drm_syncobj.h>		/* 0003 */
#include <drm_sched.h>			/* 0004 */

#include "panfrost7580_job.h"

/* ------------------------------------------------------------------ */
/* Bounded job-lifecycle tracing (jobtrace ticket).                    */
/*                                                                     */
/* p7580_job_trace = number of SUBMITs to trace from load (0 = off).   */
/* Each traced job prints at most 3 lines: dispatch (slot, jc, cfg     */
/* actually written to CONFIG_NEXT), IRQ (JOB_INT_STAT bits), and      */
/* fence-signal (which slot retired). The trace budget lives in        */
/* jd->trace_left and is consumed under hw_lock, so the log volume is  */
/* bounded even at Mesa frame rates (~3000 jobs/s would otherwise      */
/* spam dmesg into a DoS). Dumps use pr_info so they survive the      */
/* default loglevel.                                                   */
/* ------------------------------------------------------------------ */

/* Default: trace the first 8 jobs (covers egltest startup + frames).
 * Plain static (NOT module_param_named): the integrator's object rule
 * defines KBUILD_MODNAME at Image link time, but the slice-level
 * -fsyntax-only / -c checks compile this file standalone without it.
 * Runtime tuning is the jd->trace_left budget (seeded from this). */
#define P7580_JOB_TRACE_DEFAULT	8u

static unsigned int p7580_job_trace = P7580_JOB_TRACE_DEFAULT;
/* ------------------------------------------------------------------ */
/* Register access                                                    */
/* ------------------------------------------------------------------ */

static inline void p7580_writel(struct p7580_job_dev *jd, uint32_t reg,
				uint32_t val)
{
	writel(val, jd->regs + reg);
}

static inline uint32_t p7580_readl(struct p7580_job_dev *jd, uint32_t reg)
{
	return readl(jd->regs + reg);
}

static inline void p7580_js_write(struct p7580_job_dev *jd, int slot,
				  uint32_t off, uint32_t val)
{
	p7580_writel(jd, P7580_JS_REG(slot, off), val);
}

static inline uint32_t p7580_js_read(struct p7580_job_dev *jd, int slot,
				     uint32_t off)
{
	return p7580_readl(jd, P7580_JS_REG(slot, off));
}

/* ------------------------------------------------------------------ */
/* Job fence: one ordered context per slot, monotonic seqno.           */
/* Mirrors v5.2 panfrost_fence_create (queue/emit_seqno/fence_context) */
/* on top of the 0001 fence ops (enable_signaling/signaled/wait/      */
/* release contract, release() calls dma_fence_release_base()).       */
/* ------------------------------------------------------------------ */

struct p7580_fence {
	struct dma_fence base;
	uint64_t seqno;
	int slot;
};

static inline struct p7580_fence *to_p7580_fence(struct dma_fence *f)
{
	return container_of(f, struct p7580_fence, base);
}

static const char *p7580_fence_driver(struct dma_fence *f)
{
	(void)f;
	return "panfrost7580";
}

static const char *p7580_fence_timeline(struct dma_fence *f)
{
	struct p7580_fence *pf = to_p7580_fence(f);

	switch (pf->slot) {
	case 0:
		return "panfrost7580-js-0";
	case 1:
		return "panfrost7580-js-1";
	case 2:
		return "panfrost7580-js-2";
	default:
		return NULL;
	}
}

static bool p7580_fence_enable_signaling(struct dma_fence *f)
{
	/* No SW-arm needed: HW IRQ is the signal source. Report "already
	 * signaled during arming" only if the bit is already set, which
	 * the 0001 add_callback re-checks. */
	return dma_fence_is_signaled(f);
}

static bool p7580_fence_signaled(struct dma_fence *f)
{
	/* Bit test only; the IRQ sets the bit via dma_fence_signal(). */
	return !!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &f->flags);
}

static void p7580_fence_release(struct dma_fence *f)
{
	struct p7580_fence *pf = to_p7580_fence(f);

	dma_fence_release_base(f);	/* 0001: frees waiter queue */
	kfree(pf);
}

static const struct dma_fence_ops p7580_fence_ops = {
	.get_driver_name = p7580_fence_driver,
	.get_timeline_name = p7580_fence_timeline,
	.enable_signaling = p7580_fence_enable_signaling,
	.signaled = p7580_fence_signaled,
	.wait = NULL,			/* 0001 default poll path */
	.release = p7580_fence_release,
};

/* Caller must hold no locks; returns ERR_PTR on OOM. seqno_out is the
 * per-slot emit counter owned by the sched slice (jd->emit_seqno). */
static struct dma_fence *p7580_fence_create(struct p7580_job_dev *jd,
					    int slot, uint64_t *seqno_out)
{
	struct p7580_fence *pf;

	pf = kzalloc(sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return ERR_PTR(-ENOMEM);

	pf->slot = slot;
	pf->seqno = ++(*seqno_out);

	/* REVIEW-FIX (gpu31-review 0001 F1/F2): the landed 0001 has no
	 * dma_fence_init and its struct keeps a dangling lock pointer;
	 * until G31_Sched lands the F1/F2 fix, init the fields by hand
	 * exactly as the fixed helper will: kref, completion, cb_list,
	 * cb_lock, cleared flags, ops/context/seqno. Nothing here
	 * dereferences fence->lock. */
	kref_init(&pf->base.refcount);
	init_completion(&pf->base.completion);
	INIT_LIST_HEAD(&pf->base.cb_list);
	spin_lock_init(&pf->base.cb_lock);
	pf->base.flags = 0;
	pf->base.ops = &p7580_fence_ops;
	pf->base.context = jd->fence_ctx[slot];
	pf->base.seqno = (unsigned)pf->seqno;
	return &pf->base;
}

/* ------------------------------------------------------------------ */
/* Device init/fini                                                   */
/* ------------------------------------------------------------------ */

int p7580_job_dev_init(struct p7580_job_dev *jd, void __iomem *regs,
		       uint32_t js_present, uint64_t shader_present)
{
	int i;
	uint32_t mask = 0;

	if (!jd || !regs)
		return -EINVAL;

	jd->regs = regs;
	jd->js_present = js_present;
	jd->shader_present = shader_present;
	spin_lock_init(&jd->hw_lock);
	mutex_init(&jd->reset_lock);
	/* Snapshot the module param once: later writes change the param
	 * but not an already-probed device (bounded by construction). */
	jd->trace_left = p7580_job_trace;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		jd->in_flight[i] = NULL;
		mask |= P7580_MK_JS_MASK(i);
	}

	/* Clear any stale slot IRQs, then arm DONE+ERR for present slots. */
	p7580_writel(jd, P7580_JOB_INT_CLEAR, mask);
	p7580_writel(jd, P7580_JOB_INT_MASK, mask & (js_present | (js_present << 16)));
	return 0;
}

void p7580_job_dev_fini(struct p7580_job_dev *jd)
{
	if (!jd)
		return;
	/* Mask everything; the IRQ handler is freed by the caller. */
	p7580_writel(jd, P7580_JOB_INT_MASK, 0);
}

/* Idle when no slot has a queued command and no job is in flight. */
int p7580_job_is_idle(struct p7580_job_dev *jd)
{
	int i;

	if (!jd)
		return 1;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		if (p7580_js_read(jd, i, P7580_JS_COMMAND_NEXT))
			return 0;
		if (jd->in_flight[i])
			return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* HW submit: NEXT-view programming + START. Must be called with      */
/* preemption-safe context; takes hw_lock internally. Mirrors [KB]    */
/* kbase_job_hw_submit and [V5] panfrost_job_hw_submit with the T720  */
/* cfg from p7580_js_config() (no FLUSH_REDUCTION / no flush_id).     */
/* ------------------------------------------------------------------ */

void p7580_job_hw_submit(struct p7580_job_dev *jd,
			 struct p7580_submit_job *job)
{
	unsigned long flags;
	uint32_t cfg;
	int slot = job->slot;
	uint32_t lo = (uint32_t)(job->jc & 0xFFFFFFFFu);
	uint32_t hi = (uint32_t)(job->jc >> 32);

	/* COMMAND_NEXT must be free: a stuck slot means recovery, not a
	 * second submit on top. */
	if (WARN_ON(p7580_js_read(jd, slot, P7580_JS_COMMAND_NEXT)))
		return;

	spin_lock_irqsave(&jd->hw_lock, flags);

	p7580_js_write(jd, slot, P7580_JS_HEAD_NEXT_LO, lo);
	p7580_js_write(jd, slot, P7580_JS_HEAD_NEXT_HI, hi);

	p7580_js_write(jd, slot, P7580_JS_AFF_NEXT_LO,
		       (uint32_t)(job->affinity & 0xFFFFFFFFu));
	p7580_js_write(jd, slot, P7580_JS_AFF_NEXT_HI,
		       (uint32_t)(job->affinity >> 32));

	cfg = p7580_js_config(job->as_nr);
	p7580_js_write(jd, slot, P7580_JS_CONFIG_NEXT, cfg);

	jd->in_flight[slot] = job;

	/* GO. */
	p7580_js_write(jd, slot, P7580_JS_COMMAND_NEXT, P7580_JS_CMD_START);

	/* Bounded dispatch trace: slot, HEAD jc, and the CONFIG_NEXT
	 * value actually written (START_MMU/FLUSH bits visible), one
	 * line per traced job. Consumes one budget unit; IRQ + fence
	 * prints below consume their own units, so one job costs <= 3. */
	if (jd->trace_left) {
		jd->trace_left--;
		pr_info("p7580_job: dispatch slot=%d jc=0x%llx cfg=0x%08x as=%u aff=0x%llx seq=%u\n",
			slot, (unsigned long long)job->jc, cfg, job->as_nr,
			(unsigned long long)job->affinity, job->seqno);
	}

	spin_unlock_irqrestore(&jd->hw_lock, flags);
}
/* ------------------------------------------------------------------ */
/* IRQ handler: demux JOB_INT_STAT per slot. IRQ context: no sleeps,  */
/* no mutexes, only spinlock + fence signal + JOB_INT_CLEAR.          */
/*                                                                    */
/* DONE(j): signal job->done_fence (wakes sched retirement + any     */
/*   WAIT_BO waiters); clear in_flight under hw_lock.                 */
/* ERR(j): leave COMMAND_NEXT programmed; the timeout/recovery path  */
/*   (process context) reads STATUS/HEAD/TAIL and resets. Here just   */
/*   CLEAR the sticky bit so the line deasserts.                      */
/* ------------------------------------------------------------------ */

int p7580_job_irq(int irq, struct p7580_job_dev *jd)
{
	uint32_t status;
	int j;

	(void)irq;
	if (!jd)
		return 0;	/* IRQ_NONE; int kept for request_irq shape */

	status = p7580_readl(jd, P7580_JOB_INT_STAT);
	if (!status)
		return 0;	/* IRQ_NONE */

	for (j = 0; j < P7580_NUM_JOB_SLOTS; j++) {
		uint32_t m = P7580_MK_JS_MASK(j);
		uint32_t js_status;
		int sig = 0;

		if (!(status & m))
			continue;

		p7580_writel(jd, P7580_JOB_INT_CLEAR, m);

		if (status & P7580_JOB_ERR_BIT(j)) {
			/* Fault path: record only; recovery runs in
			 * process context (timedout_job). */
			p7580_js_write(jd, j, P7580_JS_COMMAND_NEXT,
				       P7580_JS_CMD_NOP);
		}

		if (status & P7580_JOB_DONE_BIT(j)) {
			struct p7580_submit_job *job;
			unsigned long flags;

			spin_lock_irqsave(&jd->hw_lock, flags);
			job = jd->in_flight[j];
			jd->in_flight[j] = NULL;
			/* Bounded IRQ trace: raw STAT bits plus the slot
			 * STATUS register, so DONE vs ERR vs silent-skip
			 * is decidable from dmesg. Skipped jobs print
			 * nothing here (no IRQ fires); executed jobs print
			 * this line. */
			js_status = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_STATUS));
			if (jd->trace_left) {
				jd->trace_left--;
				pr_info("p7580_job: irq slot=%d stat=0x%08x js_status=0x%08x (%s) has_job=%d\n",
					j, status, js_status,
					p7580_js_status_name(js_status),
					job != NULL);
			}
			spin_unlock_irqrestore(&jd->hw_lock, flags);

			if (job && job->done_fence) {
				sig = dma_fence_signal((struct dma_fence *)job->done_fence); /* 0001 */
				/* Bounded fence-signal trace: 0 = first
				 * signal (normal retire), -EINVAL =
				 * already signaled (IRQ/timeout race,
				 * idempotent by design). */
				if (jd->trace_left) {
					unsigned long f;

					spin_lock_irqsave(&jd->hw_lock, f);
					if (jd->trace_left) {
						jd->trace_left--;
						pr_info("p7580_job: fence signal slot=%d seq=%u sig_rc=%d\n",
							j, job->seqno, sig);
					}
					spin_unlock_irqrestore(&jd->hw_lock, f);
				}
			}
		} else if (status & P7580_JOB_ERR_BIT(j)) {
			/* ERR without DONE: same bounded trace so faults
			 * are visible even when no fence retires here
			 * (recovery signals it in process context). */
			unsigned long flags;
			uint32_t es;

			spin_lock_irqsave(&jd->hw_lock, flags);
			es = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_STATUS));
			if (jd->trace_left) {
				jd->trace_left--;
				pr_info("p7580_job: irq-err slot=%d stat=0x%08x js_status=0x%08x (%s)\n",
					j, status, es,
					p7580_js_status_name(es));
			}
			spin_unlock_irqrestore(&jd->hw_lock, flags);
		}
		status &= ~m;
	}
	return 1;	/* IRQ_HANDLED */
}

/* ------------------------------------------------------------------ */
/* Scheduler backend (0004 ops: run_job / timedout_job / free_job).   */
/* The 0004 shim also supports add_dependency via fence callbacks;    */
/* in_sync + implicit BO fences are attached as shim dependencies by  */
/* the SUBMIT path before drm_sched_job_submit().                    */
/* ------------------------------------------------------------------ */

void *p7580_job_run(struct p7580_job_dev *jd, struct p7580_submit_job *job)
{
	struct dma_fence *fence;

	fence = p7580_fence_create(jd, job->slot, &jd->emit_seqno[job->slot]);
	if (IS_ERR(fence))
		return fence;

	/* One ref per job, owned by done_fence. render_done_fence is a
	 * non-owning alias (F-JOB3: never put a second time). Dropping a
	 * stale previous fence here keeps submit_args (which starts with
	 * NULL fences) and re-run paths under the same rule. */
	if (job->done_fence && job->done_fence != fence)
		dma_fence_put((struct dma_fence *)job->done_fence); /* 0001 */
	dma_fence_get(fence);			/* 0001: void, NULL-safe */
	job->done_fence = fence;

	p7580_job_hw_submit(jd, job);
	/* Non-owning alias: consumers (sched/out-sync) must take their
	 * own ref (e.g. syncobj replace takes one). */
	job->render_done_fence = fence;
	return fence;
}

void p7580_job_free(struct p7580_submit_job *job)
{
	uint32_t i;

	if (!job)
		return;
	if (job->in_fences) {
		for (i = 0; i < job->in_fence_count; i++)
			dma_fence_put((struct dma_fence *)job->in_fences[i]); /* 0001 */
		kfree(job->in_fences);
	}
	/* done_fence owns the single run_job ref; render_done_fence is
	 * only an alias (F-JOB3), so clear it without putting. */
	job->render_done_fence = NULL;
	if (job->done_fence) {
		dma_fence_put((struct dma_fence *)job->done_fence); /* 0001 */
		job->done_fence = NULL;
	}
	kfree(job->bo_handles);
	kfree(job);
}
/*
 * Timeout: the per-job delayed_work in the 0004 shim fires when the
 * done_fence has not signaled within P7580_JOB_TIMEOUT_MS of dispatch.
 * Runs in process context (workqueue), so it may sleep for reset.
 * Spurious if the IRQ just signaled -> bail out.
 *
 * SUBMIT-DEADLOCK FIX: after recovery, signal this job's own fence if
 * it is still pending. Recovery latches jd->in_flight[slot], which may
 * be a *different* (newer) job than the one that timed out -- e.g. the
 * stuck slot swallowed a START (hw_submit WARN_ON bail leaves the new
 * fence unsignaled and the job out of flight). Without this, that fence
 * never signals: no second timeout re-arms it and its finish_cb never
 * fires. The signal is bounded (one timeout period + one recovery) and
 * idempotent (guarded by is_signaled, same F-JOB4 precedent); the 0001
 * fence carries no error state, so a plain signal marks completion and
 * the fault status stays in the kernel log for the Mesa retry decision.
 */
void p7580_job_timedout(struct p7580_job_dev *jd,
			struct p7580_submit_job *job)
{
	struct dma_fence *f = (struct dma_fence *)job->done_fence;
	uint32_t status;
	int slot = job->slot;

	if (f && dma_fence_is_signaled(f)) /* 0001 */
		return;

	status = p7580_js_read(jd, slot, P7580_JS_STATUS);
	p7580_job_recover(jd, slot, status);

	/* Bounded-completion guarantee: recovery signals the job latched in
	 * in_flight[slot], which may differ from this timed-out job (see
	 * above). If ours is still pending, signal it here so its
	 * finish_cb/syncobj/WAIT_BO waiters wake within one timeout plus
	 * one recovery instead of hanging forever. Idempotent: the IRQ or
	 * a concurrent recover may have signaled it first. */
	f = (struct dma_fence *)job->done_fence;
	if (f && !dma_fence_is_signaled(f)) /* 0001 */
		dma_fence_signal(f); /* 0001 */
}
/* ------------------------------------------------------------------ */
/* SUBMIT ioctl body. Steps (mirrors v5.2 panfrost_job_push, adapted  */
/* to the 0002/0003/0004 shims):                                       */
/*  1. validate jc/requirements/handles (8-byte jc align, slot legal) */
/*  2. resolve in_sync handles -> fences (0003 find_fence)            */
/*  3. attach fences as sched dependencies (0004 add_dependency)      */
/*  4. reserve BOs, attach excl fence on completion (0002)            */
/*  5. submit to per-slot entity (0004 job_alloc/submit)              */
/*  6. install out_sync fence (0003 replace_fence)                    */
/*                                                                    */
/* BO lookup / resv lock / syncobj table live in the GEM + DrmCore    */
/* slices; this file takes them as callbacks so -c compiles standalone.*/
/* ------------------------------------------------------------------ */

struct p7580_submit_ctx {
	/* GEM slice: handle -> (resv + vm mapping). */
	int (*bo_reserve)(void *priv, uint32_t handle, void **resv);
	void *priv;
	/* DrmCore slice: syncobj handle -> fence. */
	struct dma_fence *(*sync_find)(void *priv, uint32_t handle);
	int (*sync_replace)(void *priv, uint32_t handle,
			    struct dma_fence *fence);
	void *sync_priv;
	/* Sched slice: alloc + add_dependency + submit on slot entity. */
	int (*sched_submit)(void *priv, int slot,
			    struct p7580_submit_job *job);
	void *sched_priv;
	/* MMU slice: validate that jc is mapped for as_nr. */
	int (*jc_validate)(void *priv, uint64_t jc, uint32_t as_nr);
	void *mm_priv;
};

int p7580_job_submit_args(struct p7580_job_dev *jd, struct p7580_file_ctx *fc,
			  const struct p7580_submit *args,
			  struct p7580_submit_ctx *ops)
{
	struct p7580_submit_job *job;
	uint32_t i;
	int slot, ret;

	if (!jd || !fc || !args || !ops)
		return -EINVAL;

	/* 1. validate (F-JOB1: single shared predicate; jc must be
	 * nonzero and 8-byte aligned). */
	if (!p7580_jc_valid(args->jc))
		return -EINVAL;
	if (args->requirements & ~(P7580_JD_REQ_FS |
				   P7580_JD_REQ_ONLY_COMPUTE))
		return -EINVAL;
	if (args->in_sync_count > P7580_MAX_IN_SYNCS)
		return -EINVAL;
	if (args->bo_handle_count > P7580_MAX_SUBMIT_BOS)
		return -EINVAL;

	slot = p7580_job_slot(args->requirements);
	if (slot < 0 || slot >= P7580_NUM_JOB_SLOTS)
		return -EINVAL;
	if (!(jd->js_present & (1u << slot)))
		return -ENODEV;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;

	job->seqno = fc->next_seqno++;
	job->slot = slot;
	job->jc = args->jc;
	job->requirements = args->requirements;
	job->as_nr = 0;	/* single address space; Mmu slice owns numbering */
	job->affinity = jd->shader_present;

	/* MMU slice validates the descriptor address before HW sees it. */
	if (ops->jc_validate) {
		ret = ops->jc_validate(ops->mm_priv, job->jc, job->as_nr);
		if (ret) {
			kfree(job);
			return ret;
		}
	}

	/* 2. explicit in-fences. The ioctl layer stages plain u32 arrays
	 * into kernel memory first (copy_from_user there); by the time
	 * this runs, args->in_syncs points at kernel memory local to the
	 * caller, so plain indexed loads apply and no __user dereference
	 * happens here (F-JOB2). */
	if (args->in_sync_count) {
		const uint32_t *hs =
			(const uint32_t *)(uintptr_t)args->in_syncs;

		job->in_fences = kcalloc(args->in_sync_count,
					 sizeof(*job->in_fences), GFP_KERNEL);
		if (!job->in_fences) {
			kfree(job);
			return -ENOMEM;
		}
		for (i = 0; i < args->in_sync_count; i++) {
			struct dma_fence *f;

			if (!ops->sync_find) {
				ret = -ENOSYS;
				goto err;
			}
			f = ops->sync_find(ops->sync_priv, hs[i]);
			if (!f) {
				ret = -ENOENT;
				goto err;
			}
			job->in_fences[job->in_fence_count++] = f;
		}
	}

	/* 3-5. BO reserve + sched submit (delegated to GEM/Sched; the    */
	/* dependency/fence wiring contract is asserted by the host unit */
	/* test in job_test.c). */
	if (args->bo_handle_count) {
		const uint32_t *uh =
			(const uint32_t *)(uintptr_t)args->bo_handles;

		job->bo_handles = kcalloc(args->bo_handle_count,
					  sizeof(*job->bo_handles),
					  GFP_KERNEL);
		if (!job->bo_handles) {
			ret = -ENOMEM;
			goto err;
		}
		/* Plain copy: already kernel memory staged by the ioctl
		 * layer (F-JOB2); no userspace access here. */
		for (i = 0; i < args->bo_handle_count; i++)
			job->bo_handles[i] = uh[i];
		job->bo_count = args->bo_handle_count;
		if (ops->bo_reserve) {
			for (i = 0; i < job->bo_count; i++) {
				void *resv;
				ret = ops->bo_reserve(ops->priv,
						      job->bo_handles[i],
						      &resv);
				if (ret)
					goto err;
			}
		}
	}

	if (!ops->sched_submit) {
		ret = -ENOSYS;
		goto err;
	}
	ret = ops->sched_submit(ops->sched_priv, slot, job);
	if (ret)
		goto err;

	/* 6. out-sync. */
	if (args->out_sync && ops->sync_replace) {
		/* Alias is fine: replace takes its own ref on the fence. */
		ret = ops->sync_replace(ops->sync_priv, args->out_sync,
					(struct dma_fence *)job->render_done_fence);
		if (ret)
			goto err;
	}

	return 0;

err:
	p7580_job_free(job);
	return ret;
}
/* Legacy name kept for the report/ioctl table. */
int p7580_job_submit(struct p7580_job_dev *jd, struct p7580_file_ctx *fctx,
		     const struct p7580_submit *args)
{
	(void)jd;
	(void)fctx;
	(void)args;
	return -ENOSYS;	/* needs GEM/Sched/MMU callbacks: use submit_args */
}

/* ------------------------------------------------------------------ */
/* Fault recovery, process context ONLY (never the IRQ handler).       */
/* Serialises concurrent slot faults with reset_lock. The faulting    */
/* job's done_fence is signaled exactly once here (F-JOB4): the IRQ    */
/* path signals only DONE fences, so a faulted job would otherwise     */
/* hang its WAIT_BO/sched-retirement waiters until the sched timeout   */
/* re-entered recovery and still never signaled. dma_fence_signal is   */
/* idempotent (second call returns -EINVAL), and the is_signaled      */
/* check skips a job that completed between the fault latch and        */
/* reset_lock. The 0001 fence carries no error state, so a plain      */
/* signal marks completion; the fault status stays in the kernel log   */
/* path for the retry decision (Mesa re-submits the batch).           */
/* ------------------------------------------------------------------ */

int p7580_job_recover(struct p7580_job_dev *jd, int fault_slot,
		      uint32_t status)
{
	uint32_t raw;
	int i, ret = 0;
	unsigned long end;
	struct p7580_submit_job *fault_job = NULL;

	if (!jd || fault_slot < 0 || fault_slot >= P7580_NUM_JOB_SLOTS)
		return -EINVAL;

	mutex_lock(&jd->reset_lock);
	/* Latch the faulting in-flight job under hw_lock while the slot  */
	/* is stopped: afterwards the IRQ cannot complete it into a late */
	/* DONE signal, and a concurrent timedout_job for the same slot  */
	/* serialises on reset_lock, so the signal below fires once. */
	spin_lock_irq(&jd->hw_lock);
	fault_job = jd->in_flight[fault_slot];
	/* 1. stop the faulting slot. MULTIPLE_FAULT (GPU IRQ bit 7) is  */
	/* read by the caller from GPU_INT_STAT; here every present slot */
	/* that still shows a pending command is hard-stopped too. */
	p7580_js_write(jd, fault_slot, P7580_JS_COMMAND_NEXT,
		       P7580_JS_CMD_HARD_STOP);
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		if (i != fault_slot &&
		    p7580_js_read(jd, i, P7580_JS_COMMAND_NEXT))
			p7580_js_write(jd, i, P7580_JS_COMMAND_NEXT,
				       P7580_JS_CMD_HARD_STOP);
	}
	/* Out of flight: a late DONE bit cannot signal the same fence. */
	jd->in_flight[fault_slot] = NULL;
	spin_unlock_irq(&jd->hw_lock);

	/* 2. wait for slots to drain (sleep: workqueue context). */
	end = jiffies + msecs_to_jiffies(P7580_RESET_TIMEOUT_MS);
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		while (p7580_js_read(jd, i, P7580_JS_COMMAND_NEXT)) {
			if (time_after(jiffies, end)) {
				ret = -ETIMEDOUT;
				break;
			}
			msleep(1);
		}
	}

	/* 3. GPU reset. Prefer SOFT; HARD if RAWSTAT never completes. */
	p7580_writel(jd, P7580_GPU_CMD, P7580_GPU_CMD_SOFT_RESET);
	end = jiffies + msecs_to_jiffies(P7580_RESET_TIMEOUT_MS);
	while (!(p7580_readl(jd, P7580_GPU_INT_RAWSTAT) &
		 P7580_GPU_IRQ_RESET_COMPLETED)) {
		if (time_after(jiffies, end)) {
			p7580_writel(jd, P7580_GPU_CMD,
				     P7580_GPU_CMD_HARD_RESET);
			break;
		}
		msleep(1);
	}
	p7580_writel(jd, P7580_GPU_INT_CLEAR, P7580_GPU_IRQ_RESET_COMPLETED);

	/* 4. re-arm the slot IRQs the handler demuxes. The MMU slice   */
	/* re-programs AS_TRANSTAB before this returns (integrator      */
	/* wires the call between reset and re-arm). */
	raw = 0;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++)
		raw |= P7580_MK_JS_MASK(i);
	p7580_writel(jd, P7580_JOB_INT_CLEAR, raw);
	p7580_writel(jd, P7580_JOB_INT_MASK,
		     raw & (jd->js_present | (jd->js_present << 16)));

	mutex_unlock(&jd->reset_lock);

	/* 5. fault accounting (F-JOB4): signal exactly once so waiters  */
	/* do not hang. Skip an already-signaled fence: the IRQ won the */
	/* race and the job completed. */
	if (fault_job && fault_job->done_fence &&
	    !dma_fence_is_signaled( /* 0001 */
		    (struct dma_fence *)fault_job->done_fence))
		dma_fence_signal( /* 0001 */
			(struct dma_fence *)fault_job->done_fence);
	(void)status;
	return ret;
}
