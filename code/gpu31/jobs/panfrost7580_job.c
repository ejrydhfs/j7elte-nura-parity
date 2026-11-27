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
 * Fences: each job owns exactly one ref (job->done_fence), which IS
 * the 0004 arm-time completion fence (sched_job->fence, same pointer,
 * shared ref): allocated once by the sched alloc_fence hook before
 * queueing, installed on out_sync during SUBMIT, and signaled by the
 * JOB_INT handler (or the fault-recovery path, F-JOB4). p7580_job_run
 * binds it (never creates a second fence). job->render_done_fence is a
 * non-owning alias of the same fence for the legacy out_sync leg; only
 * done_fence is ever signaled or put. Fault recovery signals done_fence
 * exactly once so waiters never hang (see p7580_job_recover).
 */
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/string.h>

/* At integration these resolve to the landed backport files:
 *  drivers/base/{dma-fence,dma-resv}.{c,h} (0001/0002) and
 *  drivers/gpu/drm/{drm_syncobj,drm_sched}.{c,h} (0003/0004).
 * The -I$(STAGE)/drivers/gpu/drm form below matches the staged dir. */
#include <linux/dma-fence.h>		/* 0001 */
#include <linux/dma-resv.h>		/* 0002 */
#include <drm_syncobj.h>		/* 0003 */
#include <drm_sched.h>			/* 0004 */

#include "panfrost7580_job.h"
#include "panfrost7580_mmu.h"

/* GEM completion invalidate (gem/panfrost7580_gem.c): pre-signal walk
 * over the job's owned BO pointers. Declared here (not via gem.h) so
 * this TU stays decoupled from the GEM BO struct layout; the linker
 * resolves it inside the panfrost7580 module. Takes opaque void**
 * (job->bos) + count; no refs taken or dropped. IRQ-safe. */
void p7580_gem_job_bos_pre_signal_sync(void **bos, unsigned int bo_count);

/* ------------------------------------------------------------------ */
/* Bounded job-lifecycle tracing + always-on ERR diagnostics (errtrace). */
/*                                                                     */
/* p7580_job_trace = number of SUBMITs to trace from load (0 = off).   */
/* Each traced job prints at most 4 lines: dispatch (slot, jc, cfg     */
/* actually written to CONFIG_NEXT), post-start (RAWSTAT + JS_STATUS), */
/* IRQ (JOB_INT_STAT bits), and fence-signal (which slot retired). The */
/* trace budget lives in jd->trace_left and is consumed under hw_lock, */
/* so the log volume is bounded even at Mesa frame rates (~3000 jobs/s */
/* would otherwise spam dmesg into a DoS). Dumps use pr_info so they   */
/* survive the default loglevel.                                       */
/*                                                                     */
/* ERR discipline (independent of the budget): ANY non-zero JS_STATUS  */
/* (>= 0x40 fault) observed in the IRQ ERR leg, the DONE leg (ERR bit */
/* set alongside DONE), or the timedout path prints a pr_err           */
/* p7580_job: ERR line immediately, plus the HEAD/TAIL and JOB_INT     */
/* state. Timeouts print a pr_err line too. Those lines always print   */
/* -- the fault detail is never budget-suppressed. Normal-path lines   */
/* (dispatch/post-start/IRQ-DONE/fence-signal) keep the old gate:      */
/* silent once trace_left hits 0.                                      */
/*                                                                     */
/* Debug knobs (debugfs, armable at any time):                          */
/*   /sys/kernel/debug/p7580/trace_arm  write N: trace_arm=N,           */
/*     trace_left=N (re-arms the bounded trace; 0 stops it).           */
/*   /sys/kernel/debug/p7580/dump       read: last dispatch jc/cfg/    */
/*     as/affinity/seq + live per-slot STATUS/HEAD/TAIL +              */
/*     JOB_INT_STAT/RAWSTAT/MASK (seq_file; safe after a fault).       */
/* ------------------------------------------------------------------ */

/* Default: trace the first 8 jobs (covers egltest startup + frames).
 * Plain static (NOT module_param_named): the integrator's object rule
 * defines KBUILD_MODNAME at Image link time, but the slice-level
 * -fsyntax-only / -c checks compile this file standalone without it.
 * Runtime tuning is the jd->trace_left budget (seeded from this). */
#define P7580_JOB_TRACE_DEFAULT	8u
static unsigned int p7580_job_trace = P7580_JOB_TRACE_DEFAULT;

/* Always-on ERR line (+ optional ERR-MAP): prints regardless of
 * trace_left. HEAD/TAIL are read by the caller under hw_lock and
 * passed in to keep this helper lock-agnostic. gpu31-iova: when the
 * faulting job carried a BO snapshot the driver-registered resolver
 * hook renders it into a second pr_err line (recycled-IOVA alias
 * decidable from dmesg alone); NULL hook/snapshot keeps the
 * historic single-line form. */
static void p7580_job_err_pin(int slot, uint32_t js_status,
			      uint32_t job_int_stat, uint32_t rawstat,
			      uint64_t head, uint64_t tail,
			      unsigned int seqno, uint64_t jc,
			      const uint32_t *sw,
			      uint32_t cfg,
			      int (*resolve)(void *priv, char *buf,
					     size_t len,
					     const uint32_t *handles,
					     const uint64_t *addrs,
					     const uint32_t *sizes, uint32_t n),
			      void *resolve_priv,
			      const uint32_t *handles,
			      const uint64_t *addrs,
			      const uint32_t *sizes, uint32_t n)
{
	char map[256];
	int mlen = 0;

	pr_err("p7580_job: ERR slot=%d js_status=0x%08x (%s) stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc=0x%llx cfg=0x%08x sw=%08x %08x %08x %08x %08x %08x %08x %08x\n",
		slot, js_status, p7580_js_status_name(js_status),
		job_int_stat, rawstat,
		(unsigned long long)head, (unsigned long long)tail,
		seqno, (unsigned long long)jc, cfg,
		sw ? sw[0] : 0, sw ? sw[1] : 0, sw ? sw[2] : 0, sw ? sw[3] : 0,
		sw ? sw[4] : 0, sw ? sw[5] : 0, sw ? sw[6] : 0, sw ? sw[7] : 0);
	if (resolve && handles && addrs && sizes && n) {
		mlen = resolve(resolve_priv, map, sizeof(map),
			       handles, addrs, sizes, n);
		if (mlen > 0) {
			map[sizeof(map) - 1] = '\0';
			pr_err("p7580_job: ERR-MAP slot=%d seq=%u nbos=%u %s\n",
				slot, seqno, n, map);
		}
	}
}

/* ------------------------------------------------------------------ */
/* gpu31-firstfault: event ring + one-shot first-fault dump.          */
/*                                                                    */
/* Why a ring instead of printk: the long-run fault appears after a   */
/* random number of clean frames (one in ~1-3 x 300-frame runs) and   */
/* the serial console makes every traced dispatch ~10 ms, which both  */
/* perturbs the race and overflows the dmesg ring (measured: the      */
/* first 5 s of a faulting run were dropped). A 128-entry RAM ring    */
/* costs a few stores per event and is dumped exactly once, with the  */
/* descriptor pages, at the first fault or timeout.                   */
/* ------------------------------------------------------------------ */
static const char *p7580_ev_name(unsigned int kind)
{
	switch (kind) {
	case P7580_EV_START:
		return "start";
	case P7580_EV_DONE:
		return "done";
	case P7580_EV_ERR:
		return "err";
	case P7580_EV_TIMEOUT:
		return "timeout";
	case P7580_EV_RECOVER:
		return "recover";
	case P7580_EV_SUSPEND:
		return "suspend";
	case P7580_EV_SUSPEND_BUSY:
		return "suspend-busy";
	case P7580_EV_RESUME:
		return "resume";
	case P7580_EV_CYCLE:
		return "cycle";
	case P7580_EV_DUMP:
		return "dump";
	default:
		return "?";
	}
}

void p7580_job_ev(struct p7580_job_dev *jd, unsigned int kind, int slot,
		  uint32_t seq, uint64_t jc, uint32_t a, uint32_t b,
		  uint32_t c)
{
	struct p7580_ev *e;
	unsigned long flags;

	if (!jd || !jd->ev_arm)
		return;
	spin_lock_irqsave(&jd->hw_lock, flags);
	e = &jd->ev[jd->ev_head & (P7580_EV_MAX - 1u)];
	/* 3.10 has no ktime_get_ns(): convert explicitly. */
	e->ns = (uint64_t)ktime_to_ns(ktime_get());
	e->jc = jc;
	e->kind = kind;
	e->slot = (uint32_t)slot;
	e->seq = seq;
	e->a = a;
	e->b = b;
	e->c = c;
	jd->ev_head++;
	spin_unlock_irqrestore(&jd->hw_lock, flags);
}

void p7580_job_ev_arm(struct p7580_job_dev *jd, unsigned int on)
{
	unsigned long flags;

	if (!jd)
		return;
	spin_lock_irqsave(&jd->hw_lock, flags);
	jd->ev_head = 0;
	jd->ev_arm = on ? 1u : 0u;
	jd->ev_dumped = 0;
	memset(jd->ev, 0, sizeof(jd->ev));
	spin_unlock_irqrestore(&jd->hw_lock, flags);
}

void p7580_job_set_desc_dump(struct p7580_job_dev *jd,
			     int (*fn)(void *priv, uint64_t gpu,
				       const char *tag), void *priv)
{
	unsigned long flags;

	if (!jd)
		return;
	spin_lock_irqsave(&jd->hw_lock, flags);
	jd->desc_dump = fn;
	jd->desc_dump_priv = priv;
	spin_unlock_irqrestore(&jd->hw_lock, flags);
}

/* gpu31-watchdog: wire the driver's dispatch-timeout accessors. The
 * timeout value itself lives with the drm_sched seed (driver), so the
 * jobs slice only renders job_timeout_ms. */
void p7580_job_set_timeout_hooks(struct p7580_job_dev *jd,
				 unsigned int (*get)(void *priv),
				 int (*set)(void *priv, unsigned int ms),
				 void *priv)
{
	unsigned long flags;

	if (!jd)
		return;
	spin_lock_irqsave(&jd->hw_lock, flags);
	jd->get_timeout_ms = get;
	jd->set_timeout_ms = set;
	jd->timeout_priv = priv;
	spin_unlock_irqrestore(&jd->hw_lock, flags);
}

/* Dump the ring (oldest first) and the pages that decide the fault:
 * the dispatched descriptor's page, the live (Mesa) page it was copied
 * from when the job ran from a shadow, the page holding the fragment
 * framebuffer pointer from the inline payload (SFBD + tiler descriptor
 * live there), and the first page of any >= 1 MiB BO in the job's
 * submit snapshot (the Mesa tiler heap: producer/consumer handshake
 * state). Process context (timeout work): the dumper may sleep. */
void p7580_job_first_fault(struct p7580_job_dev *jd,
			   struct p7580_submit_job *job, uint32_t status)
{
	unsigned int i, n, start;
	uint64_t t0 = 0;

	if (!jd || !jd->ev_arm || jd->ev_dumped)
		return;
	jd->ev_dumped = 1;

	n = jd->ev_head < P7580_EV_MAX ? jd->ev_head : P7580_EV_MAX;
	start = jd->ev_head - n;
	pr_err("p7580_job: EVRING begin head=%u n=%u status=0x%08x jc=0x%llx seq=%u src=0x%llx fbd=0x%x jc_age=%u\n",
	       jd->ev_head, n, status,
	       job ? (unsigned long long)job->jc : 0ull,
	       job ? job->seqno : 0u,
	       job ? (unsigned long long)job->src_jc : 0ull,
	       job ? job->fbd : 0u,
	       job ? job->jc_age : 0u);
	for (i = 0; i < n; i++) {
		const struct p7580_ev *e =
			&jd->ev[(start + i) & (P7580_EV_MAX - 1u)];

		if (!t0)
			t0 = e->ns;
		pr_err("p7580_job: EV %-12s dt=%8llu us slot=%u seq=%u jc=0x%llx a=%08x b=%08x c=%08x\n",
		       p7580_ev_name(e->kind),
		       (unsigned long long)((e->ns - t0) / 1000ull),
		       e->slot, e->seq, (unsigned long long)e->jc,
		       e->a, e->b, e->c);
	}
	pr_err("p7580_job: EVRING end\n");

	if (!jd->desc_dump)
		return;
	if (job) {
		jd->desc_dump(jd->desc_dump_priv, job->jc, "jc");
		if (job->src_jc && job->src_jc != job->jc)
			jd->desc_dump(jd->desc_dump_priv, job->src_jc, "src");
		/* Fragment job: payload word 10 (+40) is the framebuffer
		 * descriptor (SFBD) and the SFBD's tiler section at
		 * FBD+0xC0 names the polygon list + heap. Dump the page
		 * the recorded SFBD pointer falls in (the address is
		 * captured at SUBMIT: desc[] is only 8 words, so reading
		 * desc[10] here was an out-of-bounds read that produced
		 * a bogus address on int42). */
		if (job->fbd)
			jd->desc_dump(jd->desc_dump_priv,
				      (uint64_t)job->fbd, "fbd");
		/* Tiler heap candidate: the largest BO in the snapshot. */
		if (job->iova_addrs && job->iova_sizes) {
			unsigned int best = 0;
			uint32_t bsz = 0;

			for (i = 0; i < job->iova_bo_count; i++) {
				if (job->iova_sizes[i] > bsz) {
					bsz = job->iova_sizes[i];
					best = i;
				}
			}
			if (bsz >= 0x100000u)
				jd->desc_dump(jd->desc_dump_priv,
					      job->iova_addrs[best], "heap");
		}
	}
}

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
/* gpu31-iova: fault-time mapping resolver registration (process
 * context, before faults). The driver registers a hook that renders
 * the faulting job's {handle,iova,size} snapshot into the ERR pin;
 * the hook runs in IRQ context (no GEM dereference, no sleeping
 * locks, no MMIO - the jobs slice never dereferences GEM objects
 * itself). NULL fn clears. Under hw_lock (same lock as the ERR
 * legs, so registration races no in-flight pin). */
void p7580_job_set_iova_resolver(struct p7580_job_dev *jd,
				 int (*fn)(void *priv, char *buf, size_t len,
					   const uint32_t *handles,
					   const uint64_t *addrs,
					   const uint32_t *sizes, uint32_t n),
				 void *priv)
{
	unsigned long flags;

	if (!jd)
		return;
	spin_lock_irqsave(&jd->hw_lock, flags);
	jd->iova_resolve = fn;
	jd->iova_resolve_priv = priv;
	spin_unlock_irqrestore(&jd->hw_lock, flags);
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

	/* 0001 provides dma_fence_init (F1/F2 fixed): centralised
	 * kref/completion/cb_list/cb_lock/flags + ops/context/seqno. */
	dma_fence_init(&pf->base, &p7580_fence_ops,
		       jd->fence_ctx[slot], (unsigned)pf->seqno);
	return &pf->base;
}

static void p7580_job_debugfs_init(struct p7580_job_dev *jd);
static void p7580_job_debugfs_fini(struct p7580_job_dev *jd);

/* ------------------------------------------------------------------ */
/* Device init/fini                                                   */
/* ------------------------------------------------------------------ */

int p7580_job_dev_init(struct p7580_job_dev *jd, void __iomem *regs,
		       uint32_t js_present, uint64_t shader_present,
		       dma_addr_t pgd_dma, void *pm_dev,
		       int (*pm_get)(void *dev), void (*pm_put)(void *dev),
		       int (*power_cycle)(void *dev))
{
	int i;
	uint32_t mask = 0;

	if (!jd || !regs)
		return -EINVAL;

	jd->regs = regs;
	jd->js_present = js_present;
	jd->shader_present = shader_present;
	jd->pgd_dma = pgd_dma;
	jd->pm_dev = pm_dev;
	jd->pm_get = pm_get;
	jd->pm_put = pm_put;
	jd->power_cycle = power_cycle;
	spin_lock_init(&jd->hw_lock);
	mutex_init(&jd->reset_lock);
	/* Arm-time fence timelines (gpu31-armfence): one dma-fence
	 * context per slot + zeroed per-slot emit seqno, so the first
	 * SUBMIT's alloc_fence hands out a valid (context, seqno).
	 * dma_fence_context_alloc never fails (monotonic counter). */
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		jd->fence_ctx[i] = dma_fence_context_alloc(1);
		jd->emit_seqno[i] = 0;
	}
	/* Snapshot the module param once: later writes change the param
	 * but not an already-probed device (bounded by construction).
	 * trace_arm is the re-armable source: the debugfs knob rewrites
	 * both fields, so a re-arm survives budget exhaustion. */
	jd->trace_left = p7580_job_trace;
	jd->trace_arm = p7580_job_trace;
	jd->last.valid = 0;
	jd->dbg_dir = NULL;
	jd->chain_mode = 0;
	/* gpu31-firstfault defaults: ring disarmed, no page dumper order
	 * (wired at probe), FS keeps its own entity. Everything new is
	 * inert until the ev_arm / serialize_fs knobs are written. */
	jd->ev_head = 0;
	jd->ev_arm = 0;
	jd->ev_dumped = 0;
	jd->desc_dump = NULL;
	jd->desc_dump_priv = NULL;
	jd->serialize_fs = 0;
	/* gpu31-watchdog: injection disarmed, timeout knob unwired. */
	jd->hang_arm = 0;
	jd->get_timeout_ms = NULL;
	jd->set_timeout_ms = NULL;
	jd->timeout_priv = NULL;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		jd->in_flight[i] = NULL;
		mask |= P7580_MK_JS_MASK(i);
	}

	/* Clear any stale slot IRQs (both views: CLEAR drains RAWSTAT via
	 * the in-tree CLEAR semantics), then unmask DONE+ERR for present
	 * slots. Per [KT] mali_midg_regmap.h:203-206 RAWSTAT is the latch
	 * and STAT the masked AND delivered view, so an unmasked STAT is a
	 * subset of RAWSTAT; this read-back proves CLEAR actually drained
	 * before the first START (bounded: two extra reads at probe). */
	p7580_writel(jd, P7580_JOB_INT_CLEAR, mask);
	p7580_writel(jd, P7580_JOB_INT_MASK, mask & (js_present | (js_present << 16)));
	pr_info("p7580_job: irq-arm rawstat=0x%08x stat=0x%08x mask=0x%08x js_present=0x%x\n",
		p7580_readl(jd, P7580_JOB_INT_RAWSTAT),
		p7580_readl(jd, P7580_JOB_INT_STAT),
		p7580_readl(jd, P7580_JOB_INT_MASK), js_present);
	/* Debug knobs (debugfs, armable at any time): p7580/trace_arm
	 * (write N re-arms) + p7580/dump (read last-job + slot regs).
	 * debugfs returns ERR_PTR(-ENODEV) when disabled: IS_ERR_OR_NULL
	 * keeps the driver alive on non-debug builds. */
	p7580_job_debugfs_init(jd);
	return 0;
}

void p7580_job_dev_fini(struct p7580_job_dev *jd)
{
	if (!jd)
		return;
	p7580_job_debugfs_fini(jd);
	/* Mask everything; the IRQ handler is freed by the caller. */
	p7580_writel(jd, P7580_JOB_INT_MASK, 0);
}

/* Re-arm after GPU reset / runtime resume: CLEAR any bits latched
 * while dark, then unmask DONE+ERR for present slots. Caller must
 * hold power (probe/dev_init, resume path, recover path all do).
 * Same CLEAR+MASK pair as dev_init without touching in_flight. */
void p7580_job_dev_rearm(struct p7580_job_dev *jd)
{
	uint32_t mask = 0;
	int i;

	if (!jd)
		return;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++)
		mask |= P7580_MK_JS_MASK(i);
	p7580_writel(jd, P7580_JOB_INT_CLEAR, mask);
	p7580_writel(jd, P7580_JOB_INT_MASK,
		     mask & (jd->js_present | (jd->js_present << 16)));
}

/* Idle when no slot has a queued command, no job is in flight and no
 * present slot reports ACTIVE in JS_STATUS. gpu31-readback: the software
 * view alone is not enough to gate a power-off -- recovery clears
 * in_flight while the JM may still be executing, and a latched fault code
 * (0x41 = ACTIVE|POWER_FAULT) reads COMMAND_NEXT==0. Powering the cores
 * down in that window is a GPU power event (measured: 22 power-offs with
 * "did not idle" per bench run, then POWER_FAULT storms). Our own HARD_STOP
 * leaves STOPPED (0x03, bit 0 set) in the slot, which is not activity. */
int p7580_job_is_idle(struct p7580_job_dev *jd)
{
	int i;

	if (!jd)
		return 1;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		uint32_t st;

		if (p7580_js_read(jd, i, P7580_JS_COMMAND_NEXT))
			return 0;
		if (jd->in_flight[i])
			return 0;
		if (!(jd->js_present & (1u << i)))
			continue;
		st = p7580_js_read(jd, i, P7580_JS_STATUS);
		if ((st & 0x1u) && !p7580_js_status_is_stop(st))
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
	/* gpu31-firstfault: armed probe values (JS_STATUS before START,
	 * COMMAND_NEXT / JS_STATUS right after) + the in-flight slot
	 * occupant seen before this dispatch (a non-NULL previous job
	 * here would mean a double dispatch on one slot). */
	uint32_t st_pre = 0, cmd_post = 0, st_post = 0;
	struct p7580_submit_job *prev = NULL;
	int armed = jd->ev_arm;

	/* COMMAND_NEXT must be free: a stuck slot means recovery, not a
	 * second submit on top. */
	if (WARN_ON(p7580_js_read(jd, slot, P7580_JS_COMMAND_NEXT)))
		return;

	spin_lock_irqsave(&jd->hw_lock, flags);

	if (armed) {
		prev = jd->in_flight[slot];
		st_pre = p7580_js_read(jd, slot, P7580_JS_STATUS);
	}

	/* gpu31-watchdog hang_inject: mask this slot's DONE+ERR bits for
	 * one dispatch. The job still runs and retires in hardware, but
	 * no IRQ is delivered, so the fence waits for the sched timeout:
	 * the deterministic form of the silent hang (STAT/RAWSTAT 0,
	 * COMMAND_NEXT drained, NO_EXCEPTION 0x08). Recovery's
	 * dev_rearm() restores the mask. hang_arm is 0 unless written
	 * through debugfs, so the production path is untouched. */
	if (jd->hang_arm) {
		jd->hang_arm--;
		p7580_writel(jd, P7580_JOB_INT_MASK,
			     (jd->js_present | (jd->js_present << 16)) &
			     ~P7580_MK_JS_MASK(slot));
		pr_info("p7580_job: hang_inject slot=%d seq=%u irq-masked\n",
			slot, job->seqno);
	}

	p7580_js_write(jd, slot, P7580_JS_HEAD_NEXT_LO, lo);
	p7580_js_write(jd, slot, P7580_JS_HEAD_NEXT_HI, hi);

	p7580_js_write(jd, slot, P7580_JS_AFF_NEXT_LO,
		       (uint32_t)(job->affinity & 0xFFFFFFFFu));
	p7580_js_write(jd, slot, P7580_JS_AFF_NEXT_HI,
		       (uint32_t)(job->affinity >> 32));

	cfg = p7580_js_config(job->as_nr);
	/* Chain-bit experiment variant A (gpu31-lastjob): when the
	 * chain head's descriptor bit0 (job_descriptor_size/chain
	 * marker) is set, honour it in CONFIG_NEXT. T720 has no
	 * JOBCHAIN_DISAMBIGUATION HW, but kbase still programs the
	 * CHAIN bit from the atom; mode 1 mirrors that. Gated by the
	 * chain_mode knob (0 = off, stock 0x83700 behaviour). */
	if (jd->chain_mode == 1 && (job->desc[4] & 0x1u))
		cfg |= P7580_JS_CFG_JOB_CHAIN_FLAG;
	p7580_js_write(jd, slot, P7580_JS_CONFIG_NEXT, cfg);

	jd->in_flight[slot] = job;
	/* Last-dispatch snapshot (always, not budget-gated): the ERR /
	 * timeout / dump paths report the faulting job from here, so
	 * fault detail survives budget exhaustion. Under hw_lock. The
	 * sw[] copy is 8 plain u32 stores, IRQ-safe (no GEM deref). */
	jd->last.valid = 1;
	jd->last.slot = (unsigned int)slot;
	jd->last.seqno = job->seqno;
	jd->last.jc = job->jc;
	jd->last.cfg = cfg;
	jd->last.as_nr = job->as_nr;
	jd->last.affinity = job->affinity;
	jd->last.sw[0] = job->desc[0];
	jd->last.sw[1] = job->desc[1];
	jd->last.sw[2] = job->desc[2];
	jd->last.sw[3] = job->desc[3];
	jd->last.sw[4] = job->desc[4];
	jd->last.sw[5] = job->desc[5];
	jd->last.sw[6] = job->desc[6];
	jd->last.sw[7] = job->desc[7];

	/* GO. One wmb() before START: the HEAD/AFFINITY/CONFIG writes
	 * must be visible to the JM before the command latches (same
	 * order kbase_job_hw_submit uses; the lock alone is not a
	 * device-store barrier on this weakly-ordered SoC). */
	wmb();
	p7580_js_write(jd, slot, P7580_JS_COMMAND_NEXT, P7580_JS_CMD_START);
	if (armed) {
		cmd_post = p7580_js_read(jd, slot, P7580_JS_COMMAND_NEXT);
		st_post = p7580_js_read(jd, slot, P7580_JS_STATUS);
	}
	/* One-shot RAWSTAT/JS_STATUS snapshot right after START (numeric,
	 * unconditional for the first 8 dispatches only, then silent):
	 * proves whether the slot latched the command or faulted
	 * synchronously even if no IRQ ever arrives. Bounded: the same
	 * jobtrace budget as the dispatch line. */

	/* Bounded dispatch trace: slot, HEAD jc, and the CONFIG_NEXT
	 * value actually written (START_MMU/FLUSH bits visible), one
	 * line per traced job. Consumes one budget unit; IRQ + fence
	 * prints below consume their own units, so one job costs <= 3. */
	if (jd->trace_left) {
		uint32_t rawstat, js_status;
		jd->trace_left--;
		rawstat = p7580_readl(jd, P7580_JOB_INT_RAWSTAT);
		js_status = p7580_readl(jd, P7580_JS_REG(slot, P7580_JS_STATUS));
		pr_info("p7580_job: dispatch slot=%d jc=0x%llx cfg=0x%08x as=%u aff=0x%llx seq=%u sw=%08x %08x %08x %08x %08x %08x %08x %08x chain_mode=%u\n",
			slot, (unsigned long long)job->jc, cfg, job->as_nr,
			(unsigned long long)job->affinity, job->seqno,
			job->desc[0], job->desc[1], job->desc[2], job->desc[3],
			job->desc[4], job->desc[5], job->desc[6], job->desc[7],
			jd->chain_mode);
		pr_info("p7580_job: post-start slot=%d rawstat=0x%08x js_status=0x%08x (%s)\n",
			slot, rawstat, js_status,
			p7580_js_status_name(js_status));
	}

	spin_unlock_irqrestore(&jd->hw_lock, flags);

	/* gpu31-firstfault: ring the dispatch outside hw_lock (the ring
	 * takes it). a = JS_STATUS before START (bit31 = the slot was
	 * still occupied by a previous job when this START was written),
	 * b = COMMAND_NEXT readback (0 = the JM consumed the START),
	 * c = JS_STATUS right after (0x08 = not latched yet). */
	if (armed)
		p7580_job_ev(jd, P7580_EV_START, slot, job->seqno, job->jc,
			     st_pre | (prev ? 0x80000000u : 0u),
			     cmd_post, st_post);
}
/* ------------------------------------------------------------------ */
/* IRQ handler: demux JOB_INT_STAT per slot. IRQ context: no sleeps,  */
/* no mutexes, only spinlock + cache maintenance + fence signal +     */
/* JOB_INT_CLEAR.                                                      */
/*                                                                     */
/* DONE(j): invalidate the job's BOs (pre-signal sync), then signal   */
/*   job->done_fence (wakes sched retirement + any WAIT_BO waiters);  */
/*   clear in_flight under hw_lock. A userspace wait returning thus   */
/*   implies visible data (gpu31-coherency-race): the CPU invalidate   */
/*   that used to run in free_job now runs here, before the signal.   */
/* ERR(j): freeze the chain with COMMAND_NEXT=NOP (kbase/panfrost     */
/*   shape) so a faulted job cannot re-issue; the timeout/recovery   */
/*   path (process context) reads STATUS/HEAD/TAIL and resets. Here  */
/*   just CLEAR the sticky bit(s) so the line deasserts.              */
/*                                                                     */
/* Completion condition (jobabi §4/§7): IRQ fires iff the slot's DONE */
/* or ERR bit is set in JOB_INT_STAT (masked view of RAWSTAT). The   */
/* dispatched job completes when this handler observes               */
/* STAT & MK_JS_MASK(slot) with the DONE bit set, clears the slot    */
/* bit(s), syncs the BOs, and dma_fence_signal()s the in-flight      */
/* fence -- the job timeout (timedout_job -> recover) only fires     */
/* when no IRQ arrives.                                               */
/* ------------------------------------------------------------------ */

int p7580_job_irq(int irq, struct p7580_job_dev *jd)
{
	uint32_t status, rawstat;
	int j;

	(void)irq;
	if (!jd)
		return 0;	/* IRQ_NONE; int kept for request_irq shape */

	rawstat = p7580_readl(jd, P7580_JOB_INT_RAWSTAT);
	status = p7580_readl(jd, P7580_JOB_INT_STAT);
	if (!status)
		return 0;	/* IRQ_NONE */
	/* Bounded entry probe (numeric, unconditional): proves the line
	 * fired and whether RAWSTAT ever latched without STAT (mask
	 * wrong) versus neither latched (slot never executed). */
	pr_info("p7580_job: irq-enter rawstat=0x%08x stat=0x%08x\n",
		rawstat, status);

	for (j = 0; j < P7580_NUM_JOB_SLOTS; j++) {
		uint32_t m = P7580_MK_JS_MASK(j);
		uint32_t js_status;
		int sig = 0;

		/* Loop on the union of the masked (STAT) and raw
		 * (RAWSTAT) views: if MASK ever wrongly excludes a
		 * completed slot, RAWSTAT still demuxes it here instead
		 * of timing out. Matches kbase_job_done's RAWSTAT tail
		 * re-check (jm_hw.c:442-455). CLEAR drains both views. */
		if (!((status | rawstat) & m))
			continue;

		p7580_writel(jd, P7580_JOB_INT_CLEAR, m);

		if ((status | rawstat) & P7580_JOB_ERR_BIT(j)) {
			/* Fault path: freeze the chain (NOP) so the slot
			 * cannot re-issue; recovery runs in process
			 * context (timedout_job -> recover). */
			p7580_js_write(jd, j, P7580_JS_COMMAND_NEXT,
				       P7580_JS_CMD_NOP);
		}

		if ((status | rawstat) & P7580_JOB_DONE_BIT(j)) {
			struct p7580_submit_job *job;
			unsigned long flags;
			uint32_t head_lo, head_hi, tail_lo, tail_hi;
			uint64_t head, tail;
			unsigned int seq = 0;
			uint64_t jc = 0;
			const uint32_t *swp = NULL;
			uint32_t lcfg = 0;
			const uint32_t *eh = NULL;
			const uint64_t *ea = NULL;
			const uint32_t *ez = NULL;
			uint32_t en = 0;

			spin_lock_irqsave(&jd->hw_lock, flags);
			job = jd->in_flight[j];
			jd->in_flight[j] = NULL;
			/* Bounded IRQ trace: raw STAT bits plus the slot
			 * STATUS register, so DONE vs ERR vs silent-skip
			 * is decidable from dmesg. Skipped jobs print
			 * nothing here (no IRQ fires); executed jobs print
			 * this line. */
			js_status = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_STATUS));
			head_lo = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_HEAD_LO));
			head_hi = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_HEAD_HI));
			tail_lo = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_TAIL_LO));
			tail_hi = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_TAIL_HI));
			head = ((uint64_t)head_hi << 32) | head_lo;
			tail = ((uint64_t)tail_hi << 32) | tail_lo;
			if (job) {
				seq = job->seqno;
				jc = job->jc;
				swp = job->desc;
				/* gpu31-iova: snapshot stamped at SUBMIT
				 * while the job owned refs; the resolver
				 * renders it without touching GEM. */
				eh = job->iova_handles;
				ea = job->iova_addrs;
				ez = job->iova_sizes;
				en = job->iova_bo_count;
			} else if (jd->last.valid &&
				   jd->last.slot == (unsigned int)j) {
				seq = jd->last.seqno;
				jc = jd->last.jc;
				swp = jd->last.sw;
			}
			if (jd->last.valid &&
			    jd->last.slot == (unsigned int)j)
				lcfg = jd->last.cfg;
			/* ERR alongside DONE: always pin (never budgeted).
			 * JS_STATUS can latch a fault code even when the
			 * DONE bit also set (short chain / sticky bit). */
			if (p7580_js_status_is_fault(js_status))
				p7580_job_err_pin(j, js_status, status,
						  rawstat, head, tail,
						  seq, jc, swp, lcfg,
						  jd->iova_resolve,
						  jd->iova_resolve_priv,
						  eh, ea, ez, en);
			if (jd->trace_left) {
				jd->trace_left--;
				pr_info("p7580_job: irq slot=%d stat=0x%08x js_status=0x%08x (%s) has_job=%d\n",
					j, status, js_status,
					p7580_js_status_name(js_status),
					job != NULL);
			}
			spin_unlock_irqrestore(&jd->hw_lock, flags);
			/* gpu31-firstfault: ring the completion (outside
			 * hw_lock). a = JS_STATUS at DONE, b = the STAT |
			 * RAWSTAT bits that demuxed here. */
			p7580_job_ev(jd, P7580_EV_DONE, j, seq, jc, js_status,
				     status | rawstat, 0);
			/* Coherency-race fix: invalidate the job's BOs BEFORE
			 * the fence signals, so any waiter woken by the
			 * signal (WAIT_BO dma_resv_wait, syncobj WAIT,
			 * sched finish_cb) observes GPU-written bytes.
			 * p7580_gem_job_bos_pre_signal_sync walks
			 * job->bos/bo_count with the clean-then-invalidate
			 * pair per bound SHMEM page (gpu31-cachedir: clean
			 * = sync_for_device(TO_DEVICE), then invalidate =
			 * sync_for_cpu(FROM_DEVICE); no MMIO, no sleeps,
			 * no locks: dcache-line ops + dsb only, IRQ-safe;
			 * the swiotlb leg is the identity fast path).
			 * free_job keeps a second (post-signal)
			 * job_done_sync + unref walk -- belt and braces for
			 * waiters that attach after the free (mmap/CPU
			 * reads), and for paths where the IRQ never fires
			 * (recover signals instead; see timedout/recover
			 * below). */
			if (job && job->bo_count && job->bos)
				p7580_gem_job_bos_pre_signal_sync(job->bos,
								  job->bo_count);
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
		} else if ((status | rawstat) & P7580_JOB_ERR_BIT(j)) {
			/* ERR without DONE: ALWAYS pin (never budgeted) so
			 * faults are visible even when no fence retires
			 * here (recovery signals it in process context).
			 * The bounded irq-err trace stays for the
			 * budget-gated startup detail. */
			unsigned long flags;
			uint32_t es;
			struct p7580_submit_job *fault_job;
			uint32_t head_lo, head_hi, tail_lo, tail_hi;
			uint64_t head, tail;
			unsigned int seq = 0;
			uint64_t jc = 0;
			const uint32_t *swp = NULL;
			uint32_t lcfg = 0;
			const uint32_t *eh = NULL;
			const uint64_t *ea = NULL;
			const uint32_t *ez = NULL;
			uint32_t en = 0;

			spin_lock_irqsave(&jd->hw_lock, flags);
			fault_job = jd->in_flight[j];
			es = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_STATUS));
			head_lo = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_HEAD_LO));
			head_hi = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_HEAD_HI));
			tail_lo = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_TAIL_LO));
			tail_hi = p7580_readl(jd, P7580_JS_REG(j, P7580_JS_TAIL_HI));
			head = ((uint64_t)head_hi << 32) | head_lo;
			tail = ((uint64_t)tail_hi << 32) | tail_lo;
			if (jd->in_flight[j]) {
				seq = jd->in_flight[j]->seqno;
				jc = jd->in_flight[j]->jc;
				swp = jd->in_flight[j]->desc;
				eh = jd->in_flight[j]->iova_handles;
				ea = jd->in_flight[j]->iova_addrs;
				ez = jd->in_flight[j]->iova_sizes;
				en = jd->in_flight[j]->iova_bo_count;
			} else if (jd->last.valid &&
				   jd->last.slot == (unsigned int)j) {
				seq = jd->last.seqno;
				jc = jd->last.jc;
				swp = jd->last.sw;
			}
			if (jd->last.valid &&
			    jd->last.slot == (unsigned int)j)
				lcfg = jd->last.cfg;
			p7580_job_err_pin(j, es, status, rawstat,
					  head, tail, seq, jc,
					  swp, lcfg,
					  jd->iova_resolve,
					  jd->iova_resolve_priv,
					  eh, ea, ez, en);
			if (jd->trace_left) {
				jd->trace_left--;
				pr_info("p7580_job: irq-err slot=%d stat=0x%08x js_status=0x%08x (%s)\n",
					j, status, es,
					p7580_js_status_name(es));
			}
			spin_unlock_irqrestore(&jd->hw_lock, flags);
			/* gpu31-firstfault: ring the fault and request the
			 * one-shot page dump. The dump needs process
			 * context (GEM resolve + kmap), so it rides the
			 * timeout work this ERR leg queues below: that
			 * work sees jd->ev_dumped == 0 and dumps before
			 * recovery mutates anything. */
			p7580_job_ev(jd, P7580_EV_ERR, j, seq, jc, es,
				     status | rawstat, 0);
			/* Latency fix (gpu31-latency): before this kick the
			 * faulted job's fence was only signalled by the full
			 * sched timeout (timedout_job -> recover), so one
			 * fault stalled the frame for the whole retry
			 * budget (a 300-frame bench showed 28 fps instead
			 * of 290 from two such stalls). Queue the job's
			 * timeout work at delay 0: recovery still runs in
			 * workqueue context on the same ordered wq (FIFO
			 * order preserved) and still signals the fence, but
			 * within milliseconds. A job whose fence already
			 * signalled is skipped by drm_sched_job_timeout. */
			if (fault_job && fault_job->sched_job)
				drm_sched_job_kick_timeout(
					(struct drm_sched_job *)
						fault_job->sched_job);
		}
		status &= ~m;
	}
	return 1;	/* IRQ_HANDLED */
}

/* ------------------------------------------------------------------ */
/* Scheduler backend (0004 ops: alloc_fence / run_job / timedout_job  */
/* / free_job). The 0004 shim also supports add_dependency via fence   */
/* callbacks; in_sync + implicit BO fences are attached as shim        */
/* dependencies by the SUBMIT path before drm_sched_job_submit().     */
/* Arm-time contract (gpu31-armfence): alloc_fence runs once per job  */
/* at arm time (before queueing); the fence it returns IS             */
/* sched_job->fence (installed on out_sync during SUBMIT). run_job    */
/* binds that same fence onto the submit job (one shared ref, never  */
/* a second allocation) and programs the slot; the IRQ / timeout      */
/* paths signal it.                                                   */
/* ------------------------------------------------------------------ */

/* Allocate the job's completion fence (0004 alloc_fence hook): one
 * ordered-context fence per slot submission, monotonic per-slot seqno.
 * This is the ONLY fence allocation for the job; run_job binds the
 * returned fence instead of creating another one. */
struct dma_fence *p7580_job_alloc_fence(struct p7580_job_dev *jd,
					int slot, uint64_t *seqno_out)
{
	return p7580_fence_create(jd, slot, seqno_out);
}

void *p7580_job_run(struct p7580_job_dev *jd, struct p7580_submit_job *job)
{
	struct dma_fence *fence = (struct dma_fence *)job->done_fence;

	/* Bind the arm-time fence: the sched job's fence (already on
	 * out_sync since SUBMIT) becomes this submit job's done_fence
	 * (one shared ref, no second allocation). A NULL bind means
	 * arm failed closed: fail the dispatch (F4 path frees it). */
	if (!fence)
		return ERR_PTR(-ENODATA);
	dma_fence_get(fence);			/* 0001: void, NULL-safe */
	job->done_fence = fence;

	p7580_job_hw_submit(jd, job);
	/* Non-owning alias: consumers (sched/out-sync) must take their
	 * own ref (e.g. syncobj replace takes one). Under the arm-time
	 * contract this aliases the same fence installed at SUBMIT. */
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
	kfree(job->bos);
	kfree(job->bo_handles);
	kfree(job->iova_handles);
	kfree(job->iova_addrs);
	kfree(job->iova_sizes);
	job->bos = NULL;
	job->bo_handles = NULL;
	job->iova_handles = NULL;
	job->iova_addrs = NULL;
	job->iova_sizes = NULL;
	job->iova_bo_count = 0;
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
	/* gpu31-firstfault: one-shot ring + descriptor-page dump BEFORE
	 * recover() touches the slots. Arrives here either from the job
	 * sched timeout (silent job) or from the ERR leg's delay-0 kick
	 * (a real JS_STATUS fault), so both signatures are covered by
	 * the same capture. */
	p7580_job_ev(jd, P7580_EV_TIMEOUT, slot, job->seqno, job->jc,
		     status, 0, 0);
	p7580_job_first_fault(jd, job, status);
	/* Always-on timeout line (never budgeted): slot + JS_STATUS +
	 * JOB_INT views + HEAD/TAIL + last-dispatch jc, so a silent
	 * slot (no IRQ at all) is still diagnosable after the budget
	 * is out. Process context: plain reads, no lock needed for
	 * the registers; last snapshot is stable (only hw_submit under
	 * hw_lock writes it, and this slot is stuck). */
	{
		uint32_t t_stat, t_raw;
		uint32_t h_lo, h_hi, t_lo, t_hi;
		uint64_t head, tail;

		t_stat = p7580_readl(jd, P7580_JOB_INT_STAT);
		t_raw = p7580_readl(jd, P7580_JOB_INT_RAWSTAT);
		h_lo = p7580_readl(jd, P7580_JS_REG(slot, P7580_JS_HEAD_LO));
		h_hi = p7580_readl(jd, P7580_JS_REG(slot, P7580_JS_HEAD_HI));
		t_lo = p7580_readl(jd, P7580_JS_REG(slot, P7580_JS_TAIL_LO));
		t_hi = p7580_readl(jd, P7580_JS_REG(slot, P7580_JS_TAIL_HI));
		head = ((uint64_t)h_hi << 32) | h_lo;
		tail = ((uint64_t)t_hi << 32) | t_lo;
		pr_err("p7580_job: TIMEOUT slot=%d js_status=0x%08x (%s) stat=0x%08x rawstat=0x%08x head=0x%llx tail=0x%llx seq=%u jc_age=%u jc=0x%llx cfg=0x%08x sw=%08x %08x %08x %08x %08x %08x %08x %08x\n",
			slot, status, p7580_js_status_name(status),
			t_stat, t_raw,
			(unsigned long long)head, (unsigned long long)tail,
			job->seqno, job->jc_age, (unsigned long long)job->jc,
			(jd->last.valid && jd->last.slot == (unsigned int)slot) ? jd->last.cfg : 0,
			job->desc[0], job->desc[1], job->desc[2], job->desc[3],
			job->desc[4], job->desc[5], job->desc[6], job->desc[7]);
		/* gpu31-iova: same mapping line as the ERR pin (process
		 * context here, but the hook is IRQ-safe anyway). */
		if (jd->iova_resolve && job->iova_handles &&
		    job->iova_addrs && job->iova_sizes &&
		    job->iova_bo_count) {
			char tmap[256];
			int tlen = jd->iova_resolve(
				jd->iova_resolve_priv, tmap, sizeof(tmap),
				job->iova_handles, job->iova_addrs,
				job->iova_sizes, job->iova_bo_count);
			if (tlen > 0) {
				tmap[sizeof(tmap) - 1] = '\0';
				pr_err("p7580_job: ERR-MAP slot=%d seq=%u nbos=%u %s\n",
					slot, job->seqno,
					job->iova_bo_count, tmap);
			}
		}
	}
	p7580_job_recover(jd, slot, status);

	/* Bounded-completion guarantee: recovery signals the job latched in
	 * in_flight[slot], which may differ from this timed-out job (see
	 * above). If ours is still pending, sync its BOs first, then signal
	 * it here so its finish_cb/syncobj/WAIT_BO waiters wake within one
	 * timeout plus one recovery instead of hanging forever, and with
	 * visible data (same pre-signal order as the IRQ DONE path).
	 * Idempotent: the IRQ or a concurrent recover may have signaled it
	 * first. Process context here, but the helper is IRQ-safe anyway. */
	f = (struct dma_fence *)job->done_fence;
	if (f && !dma_fence_is_signaled(f)) { /* 0001 */
		if (job->bo_count && job->bos)
			p7580_gem_job_bos_pre_signal_sync(job->bos,
							  job->bo_count);
		dma_fence_signal(f); /* 0001 */
	}
}
/* ------------------------------------------------------------------ */
/* SUBMIT ioctl body. Steps (mirrors v5.2 panfrost_job_push, adapted  */
/* to the 0002/0003/0004 shims):                                       */
/*  1. validate jc/requirements/handles (8-byte jc align, slot legal) */
/*  2. resolve in_sync handles -> fences (0003 find_fence)            */
/*  3. attach fences as sched dependencies (0004 add_dependency)      */
/*  4. reserve BOs, attach excl fence on completion (0002)            */
/*  5. submit to per-slot entity (0004 job_alloc/arm/submit): the arm */
/*     allocates the completion fence, installed on out_sync below    */
/*  6. install arm-time out_sync fence (0003 replace_fence)            */
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

	/* 6. out-sync: install the arm-time fence (sched_job->fence is
	 * already allocated at arm time, before queueing). render alias
	 * is fine: replace takes its own ref on the fence. */
	if (args->out_sync && ops->sync_replace) {
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

/* Read every present slot's JS_STATUS after the GPU reset: true when a
 * HW fault code survived it. Our own HARD_STOP leaves INTERRUPTED /
 * STOPPED / TERMINATED (0x02/0x03/0x04) in the stopped slots, so those
 * are skipped; anything else >= 0x40 is a latched fault, and
 * POWER_FAULT in particular is only cleared by cycling the cores
 * (gpu31-readback: without the cycle every following job on the slot
 * re-reports 0x41 until the block is power-cycled by a suspend). */
int p7580_job_slots_latched_fault(struct p7580_job_dev *jd)
{
	int i;

	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		uint32_t st;

		if (!(jd->js_present & (1u << i)))
			continue;
		st = p7580_js_read(jd, i, P7580_JS_STATUS);
		if (p7580_js_status_is_fault(st) &&
		    !p7580_js_status_is_stop(st))
			return 1;
	}
	return 0;
}

int p7580_job_recover(struct p7580_job_dev *jd, int fault_slot,
		      uint32_t status)
{
	int i, ret = 0;
	int cycled = 0;
	int do_cycle;
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

	/* 3. GPU reset. A GPU-level SOFT/HARD reset clears the JM's job
	 * state but NOT the "the block lost power with work queued"
	 * condition: measured on int40, the very next START after the
	 * soft reset latched JS_STATUS POWER_FAULT (0x41) on the other
	 * slot within 1 ms, every time (gpu31-firstfault). The core power
	 * cycle below performs the reset as part of a complete power-on,
	 * so take that path whenever it is wired: reset + cores + JM
	 * state then match a cold power-up, and no latch survives.
	 * Fall back to the plain reset only when the hook is missing. */
	do_cycle = (jd->power_cycle && jd->pm_dev) ? 1 : 0;
	if (!do_cycle) {
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
		p7580_writel(jd, P7580_GPU_INT_CLEAR,
			     P7580_GPU_IRQ_RESET_COMPLETED);
	}

	/* 4. Complete core power cycle (gpu31-readback + gpu31-firstfault).
	 * Cycle on every recovery, not only on a latched 0x41: a plain
	 * soft reset leaves the JM reporting POWER_FAULT for the next job
	 * on either slot (measured), which turned one fault into a
	 * fragment-0x58 / chain-0x41 / power-cycle storm for the rest of
	 * the process. The caller holds a live runtime-PM ref, so the
	 * domain stays up. */
	if (do_cycle) {
		int crc = jd->power_cycle(jd->pm_dev);

		cycled = 1;
		if (crc)
			pr_err("p7580_job: recover-cycle FAIL rc=%d\n", crc);
		p7580_job_ev(jd, P7580_EV_CYCLE, fault_slot, 0, 0,
			     (uint32_t)crc, 0, 0);
	}

	/* 5. re-arm the slot IRQs the handler demuxes. Order mirrors
	 * kbase_pm_clock_on (mali_kbase_pm_driver.c: MMU first, then
	 * JOB unmask last at :1109): TRANSTAB + MEMATTR + TRANSCFG(0) +
	 * UPDATE rebind AS0 BEFORE CLEAR+unmask, so the first post-reset
	 * job fetches its descriptor through valid tables. Mainline
	 * panfrost_device_resume runs the same order (gpu_soft_reset ->
	 * power_on -> mmu_enable(0) -> job_enable_interrupts). */
	p7580_mmu_program_as(jd->regs, 0, jd->pgd_dma,
			     P7580_MEMATTR_KBASE_LO, P7580_MEMATTR_KBASE_HI);
	p7580_job_dev_rearm(jd);

	/* 6. Post-recovery register state, always on (faults are rare):
	 * proves the slot is usable again (no fault code left in
	 * JS_STATUS, JOB_INT masked for the present slots) instead of
	 * leaving the next fault to be discovered by a cascade. */
	pr_err("p7580_job: recover-done slot=%d latched=0x%08x cycle=%d js0=0x%08x js1=0x%08x intmask=0x%08x drain_rc=%d\n",
	       fault_slot, status, cycled,
	       (jd->js_present & 1u) ?
			p7580_js_read(jd, 0, P7580_JS_STATUS) : 0u,
	       (jd->js_present & 2u) ?
			p7580_js_read(jd, 1, P7580_JS_STATUS) : 0u,
	       p7580_readl(jd, P7580_JOB_INT_MASK), ret);

	mutex_unlock(&jd->reset_lock);
	/* gpu31-firstfault: one ring record per recovery so the dump
	 * shows the cascade (or its absence) with timestamps. */
	p7580_job_ev(jd, P7580_EV_RECOVER, fault_slot,
		     fault_job ? fault_job->seqno : 0,
		     fault_job ? fault_job->jc : 0,
		     status, (uint32_t)cycled, (uint32_t)ret);

	/* 5. fault accounting (F-JOB4): signal exactly once so waiters  */
	/* do not hang. Skip an already-signaled fence: the IRQ won the */
	/* race and the job completed (its pre-signal sync already ran). */
	/* Sync-before-signal here too: a faulted batch may still have */
	/* completed some BO writes before faulting, and waiters must */
	/* not observe torn/stale lines just because the retire path */
	/* was recovery instead of DONE. */
	if (fault_job && fault_job->done_fence &&
	    !dma_fence_is_signaled( /* 0001 */
		    (struct dma_fence *)fault_job->done_fence)) {
		if (fault_job->bo_count && fault_job->bos)
			p7580_gem_job_bos_pre_signal_sync(fault_job->bos,
							  fault_job->bo_count);
		dma_fence_signal( /* 0001 */
			(struct dma_fence *)fault_job->done_fence);
	}

	/*
	 * Recovery hard-stopped every slot, so any other job still parked in
	 * jd->in_flight[] has an orphaned fence: nobody will ever signal it and
	 * its BO waiters (KWin's WAIT_BO) block forever.  Sweep every slot,
	 * taking a fence reference while hw_lock is held so the scheduler
	 * cannot free the job under us; signal and drop it outside the lock.
	 */
	{
		int s;

		for (s = 0; s < P7580_NUM_JOB_SLOTS; s++) {
			struct dma_fence *f = NULL;
			unsigned long fl;

			spin_lock_irqsave(&jd->hw_lock, fl);
			if (jd->in_flight[s]) {
				void *df = jd->in_flight[s]->done_fence;

				jd->in_flight[s] = NULL;
				if (df) {
					f = (struct dma_fence *)df;
					dma_fence_get(f);	/* 3.10 returns void */
				}
			}
			spin_unlock_irqrestore(&jd->hw_lock, fl);

			if (!f)
				continue;
			if (!dma_fence_is_signaled(f))
				dma_fence_signal(f);
			dma_fence_put(f);
		}
	}
	(void)status;
	return ret;
}

/* ------------------------------------------------------------------ */
/* Debug knobs (errtrace): re-armable budget + last-job dump.          */
/*                                                                     */
/* trace_arm file: writing N sets both trace_arm and trace_left under  */
/* hw_lock, so tracing restarts immediately for the next N job-events */
/* (dispatch/post-start/IRQ/fence-signal lines). DEFINE_SIMPLE_ATTRIBUTE */
/* keeps this to two get/set callbacks; the inode private is the      */
/* struct p7580_job_dev (data arg of debugfs_create_file).            */
/*                                                                     */
/* dump file: single_open seq_file rendering the last-dispatch         */
/* snapshot (slot/jc/cfg/as/affinity/seq) plus live per-slot           */
/* STATUS/HEAD/TAIL and the JOB_INT_STAT/RAWSTAT/MASK views. Takes a   */
/* runtime-PM ref via the jd->pm_get/put hooks wired at dev_init, so   */
/* a read while autosuspended takes the snapshot-only (dark-marked)    */
/* view instead of hitting a dark bus. No locks beyond a short         */
/* hw_lock for the snapshot copy; register reads run outside the lock */
/* (same discipline as the IRQ handler's register block).              */
/* ------------------------------------------------------------------ */

void p7580_job_trace_arm(struct p7580_job_dev *jd, unsigned int n)
{
	unsigned long flags;

	if (!jd)
		return;
	spin_lock_irqsave(&jd->hw_lock, flags);
	jd->trace_arm = n;
	jd->trace_left = n;
	spin_unlock_irqrestore(&jd->hw_lock, flags);
	pr_info("p7580_job: trace re-armed to %u\n", n);
}

void p7580_job_chain_set(struct p7580_job_dev *jd, unsigned int mode)
{
	unsigned long flags;

	if (!jd)
		return;
	if (mode > 2)
		mode = 0;
	spin_lock_irqsave(&jd->hw_lock, flags);
	jd->chain_mode = mode;
	spin_unlock_irqrestore(&jd->hw_lock, flags);
	pr_info("p7580_job: chain_mode set to %u\n", mode);
}

int p7580_job_dump_show(struct p7580_job_dev *jd, struct seq_file *m)
{
	unsigned long flags;
	struct p7580_job_last last;
	uint32_t job_stat, job_raw, job_mask;
	int i, pm = -ENODEV;

	if (!jd || !m)
		return -EINVAL;
	/* Take a PM ref so the MMIO below never hits a dark bus: the
	 * block autosuspends after PF7580_AUTOSUSPEND_MS, and a debugfs
	 * read in process context is the normal dark victim (external
	 * abort on the first JOB_INT read). get failure (or no PM hooks
	 * wired yet) degrades to the snapshot-only view rather than
	 * hanging the reader. */
	if (jd->pm_get && jd->pm_dev)
		pm = jd->pm_get(jd->pm_dev);
	/* Snapshot the last dispatch under hw_lock, then read the live
	 * registers outside it (MMIO reads must not hold a spinlock
	 * across a dark bus). */
	spin_lock_irqsave(&jd->hw_lock, flags);
	last = jd->last;
	spin_unlock_irqrestore(&jd->hw_lock, flags);

	if (!pm) {
		job_stat = p7580_readl(jd, P7580_JOB_INT_STAT);
		job_raw = p7580_readl(jd, P7580_JOB_INT_RAWSTAT);
		job_mask = p7580_readl(jd, P7580_JOB_INT_MASK);
	} else {
		job_stat = 0xdead0000u;
		job_raw = 0xdead0000u;
		job_mask = 0xdead0000u;
	}

	if (!last.valid) {
		seq_printf(m, "no job yet (trace_arm=%u trace_left=%u stat=0x%08x rawstat=0x%08x mask=0x%08x%s)\n",
			   jd->trace_arm, jd->trace_left,
			   job_stat, job_raw, job_mask,
			   pm ? " dark" : "");
		if (!pm)
			jd->pm_put(jd->pm_dev);
		return 0;
	}
	seq_printf(m, "last slot=%u seq=%u jc=0x%llx cfg=0x%08x as=%u aff=0x%llx (trace_arm=%u trace_left=%u%s)\n",
		   last.slot, last.seqno, (unsigned long long)last.jc,
		   last.cfg, last.as_nr, (unsigned long long)last.affinity,
		   jd->trace_arm, jd->trace_left, pm ? " dark" : "");
	seq_printf(m, "sw %08x %08x %08x %08x %08x %08x %08x %08x\n",
		   last.sw[0], last.sw[1], last.sw[2], last.sw[3],
		   last.sw[4], last.sw[5], last.sw[6], last.sw[7]);
	seq_printf(m, "job_int stat=0x%08x rawstat=0x%08x mask=0x%08x js_present=0x%x pgd=0x%llx\n",
		   job_stat, job_raw, job_mask, jd->js_present,
		   (unsigned long long)jd->pgd_dma);
	seq_printf(m, "faultdbg ev_arm=%u ev_head=%u ev_dumped=%u serialize_fs=%u hang_arm=%u\n",
		   jd->ev_arm, jd->ev_head, jd->ev_dumped, jd->serialize_fs,
		   jd->hang_arm);
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		uint32_t st, h_lo, h_hi, t_lo, t_hi;
		uint64_t head, tail;

		if (!pm) {
			st = p7580_readl(jd, P7580_JS_REG(i, P7580_JS_STATUS));
			h_lo = p7580_readl(jd, P7580_JS_REG(i, P7580_JS_HEAD_LO));
			h_hi = p7580_readl(jd, P7580_JS_REG(i, P7580_JS_HEAD_HI));
			t_lo = p7580_readl(jd, P7580_JS_REG(i, P7580_JS_TAIL_LO));
			t_hi = p7580_readl(jd, P7580_JS_REG(i, P7580_JS_TAIL_HI));
		} else {
			st = 0xdead0000u;
			h_lo = h_hi = t_lo = t_hi = 0xdead0000u;
		}
		head = ((uint64_t)h_hi << 32) | h_lo;
		tail = ((uint64_t)t_hi << 32) | t_lo;
		seq_printf(m, "slot%d status=0x%08x (%s) head=0x%llx tail=0x%llx\n",
			   i, st, p7580_js_status_name(st),
			   (unsigned long long)head,
			   (unsigned long long)tail);
	}
	if (!pm)
		jd->pm_put(jd->pm_dev);
	return 0;
}

static int p7580_job_trace_get(void *data, u64 *val)
{
	struct p7580_job_dev *jd = data;

	if (!jd || !val)
		return -EINVAL;
	*val = jd->trace_left;
	return 0;
}

static int p7580_job_trace_set(void *data, u64 val)
{
	p7580_job_trace_arm(data, (unsigned int)val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_trace_fops, p7580_job_trace_get,
			p7580_job_trace_set, "%llu\n");

/* gpu31-iova: re-armable budget for the driver's map/unmap trace
 * (p7580_iova_trace in submit.c). Same shape as trace_arm: write N
 * re-arms, 0 stops. Registered by the driver (owns the counter);
 * the jobs slice only owns the fops + registration helper so the
 * knob appears beside trace_arm in the same debugfs dir. NULL-safe:
 * get returns 0/empty when no counter is registered yet. */
static unsigned int *p7580_job_iova_budget;

static int p7580_job_iova_arm_get(void *data, u64 *val)
{
	(void)data;
	if (!val)
		return -EINVAL;
	*val = p7580_job_iova_budget ? *p7580_job_iova_budget : 0;
	return 0;
}

static int p7580_job_iova_arm_set(void *data, u64 val)
{
	(void)data;
	if (p7580_job_iova_budget)
		*p7580_job_iova_budget = (unsigned int)val;
	pr_info("p7580_job: iova trace re-armed to %llu\n", val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_iova_arm_fops, p7580_job_iova_arm_get,
			p7580_job_iova_arm_set, "%llu\n");

void p7580_job_register_iova_budget(unsigned int *budget)
{
	p7580_job_iova_budget = budget;
}

static int p7580_job_chain_get(void *data, u64 *val)
{
	struct p7580_job_dev *jd = data;

	if (!jd || !val)
		return -EINVAL;
	*val = jd->chain_mode;
	return 0;
}

static int p7580_job_chain_set_k(void *data, u64 val)
{
	p7580_job_chain_set(data, (unsigned int)val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_chain_fops, p7580_job_chain_get,
			p7580_job_chain_set_k, "%llu\n");

/* gpu31-firstfault knobs.
 * ev_arm: write 1 to reset the event ring, arm the per-dispatch MMIO
 *   probes and enable the one-shot first-fault dump; write 0 to
 *   disarm. Read: 1 = armed, 0 = off.
 * serialize_fs: write 1 to queue FS (slot 0) submits on the chain
 *   slot's entity, so the fragment job is ordered behind the tiler by
 *   one FIFO; 0 keeps the per-slot entities. Runtime A/B, no rebuild. */
static int p7580_job_ev_arm_get(void *data, u64 *val)
{
	struct p7580_job_dev *jd = data;

	if (!jd || !val)
		return -EINVAL;
	*val = jd->ev_arm;
	return 0;
}

static int p7580_job_ev_arm_set(void *data, u64 val)
{
	struct p7580_job_dev *jd = data;

	if (!jd)
		return -EINVAL;
	p7580_job_ev_arm(jd, (unsigned int)val);
	pr_info("p7580_job: ev_arm=%d (ring %u entries, dump one-shot)\n",
		jd->ev_arm, P7580_EV_MAX);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_ev_arm_fops, p7580_job_ev_arm_get,
			p7580_job_ev_arm_set, "%llu\n");

static int p7580_job_serialize_fs_get(void *data, u64 *val)
{
	struct p7580_job_dev *jd = data;

	if (!jd || !val)
		return -EINVAL;
	*val = jd->serialize_fs;
	return 0;
}

static int p7580_job_serialize_fs_set(void *data, u64 val)
{
	struct p7580_job_dev *jd = data;

	if (!jd)
		return -EINVAL;
	jd->serialize_fs = val ? 1u : 0u;
	pr_info("p7580_job: serialize_fs=%u\n", jd->serialize_fs);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_serialize_fs_fops,
			p7580_job_serialize_fs_get,
			p7580_job_serialize_fs_set, "%llu\n");

/* gpu31-watchdog knobs.
 *
 * hang_inject: write N to mask the slot IRQ for the next N dispatches
 * (the deterministic silent hang: job runs, no IRQ, fence waits the
 * full dispatch timeout). 0 disarms. Nothing else changes; recovery
 * re-arms the mask.
 *
 * job_timeout_ms: the driver's sched dispatch timeout (default
 * P7580_JOB_TIMEOUT_MS = 750 ms). A write applies to entities created
 * afterwards (each bench run opens a new file) and to the live
 * sched->timeout; the setter clamps 100..60000. Read-only when the
 * driver has not wired the hooks. */
static int p7580_job_hang_get(void *data, u64 *val)
{
	struct p7580_job_dev *jd = data;

	if (!jd)
		return -EINVAL;
	*val = jd->hang_arm;
	return 0;
}

static int p7580_job_hang_set(void *data, u64 val)
{
	struct p7580_job_dev *jd = data;

	if (!jd)
		return -EINVAL;
	jd->hang_arm = (unsigned int)val;
	pr_info("p7580_job: hang_inject=%u (IRQ masked at the next %u dispatches)\n",
		jd->hang_arm, jd->hang_arm);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_hang_fops, p7580_job_hang_get,
			p7580_job_hang_set, "%llu\n");

static int p7580_job_timeout_get(void *data, u64 *val)
{
	struct p7580_job_dev *jd = data;

	if (!jd)
		return -EINVAL;
	if (!jd->get_timeout_ms) {
		*val = P7580_JOB_TIMEOUT_MS;
		return 0;
	}
	*val = jd->get_timeout_ms(jd->timeout_priv);
	return 0;
}

static int p7580_job_timeout_set(void *data, u64 val)
{
	struct p7580_job_dev *jd = data;

	if (!jd)
		return -EINVAL;
	if (!jd->set_timeout_ms)
		return -ENODEV;
	return jd->set_timeout_ms(jd->timeout_priv, (unsigned int)val);
}

DEFINE_SIMPLE_ATTRIBUTE(p7580_job_timeout_fops, p7580_job_timeout_get,
			p7580_job_timeout_set, "%llu\n");

static int p7580_job_dump_show_seq(struct seq_file *m, void *v)
{
	struct p7580_job_dev *jd = m->private;

	(void)v;
	return p7580_job_dump_show(jd, m);
}

static int p7580_job_dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, p7580_job_dump_show_seq, inode->i_private);
}

static const struct file_operations p7580_job_dump_fops = {
	.owner = THIS_MODULE,
	.open = p7580_job_dump_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void p7580_job_debugfs_init(struct p7580_job_dev *jd)
{
	struct dentry *dir, *f;

	if (!jd)
		return;
	dir = debugfs_create_dir("p7580", NULL);
	if (IS_ERR_OR_NULL(dir))
		return;
	jd->dbg_dir = dir;
	f = debugfs_create_file("trace_arm", 0600, dir, jd,
				&p7580_job_trace_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: trace_arm knob unavailable\n");
	/* gpu31-iova: re-arms the driver map/unmap trace budget. */
	f = debugfs_create_file("iova_arm", 0600, dir, jd,
				&p7580_job_iova_arm_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: iova_arm knob unavailable\n");
	f = debugfs_create_file("chain_mode", 0600, dir, jd,
				&p7580_job_chain_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: chain_mode knob unavailable\n");
	/* gpu31-firstfault: event ring + first-fault page dump arm. */
	f = debugfs_create_file("ev_arm", 0600, dir, jd,
				&p7580_job_ev_arm_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: ev_arm knob unavailable\n");
	/* gpu31-firstfault: FS-after-chain ordering A/B. */
	f = debugfs_create_file("serialize_fs", 0600, dir, jd,
				&p7580_job_serialize_fs_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: serialize_fs knob unavailable\n");
	/* gpu31-watchdog: silent-hang injection + dispatch-timeout A/B. */
	f = debugfs_create_file("hang_inject", 0600, dir, jd,
				&p7580_job_hang_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: hang_inject knob unavailable\n");
	f = debugfs_create_file("job_timeout_ms", 0600, dir, jd,
				&p7580_job_timeout_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: job_timeout_ms knob unavailable\n");
	f = debugfs_create_file("dump", 0400, dir, jd,
				&p7580_job_dump_fops);
	if (IS_ERR_OR_NULL(f))
		pr_info("p7580_job: dump knob unavailable\n");
}

static void p7580_job_debugfs_fini(struct p7580_job_dev *jd)
{
	if (!jd || !jd->dbg_dir)
		return;
	debugfs_remove_recursive((struct dentry *)jd->dbg_dir);
	jd->dbg_dir = NULL;
}
