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
	/* gpu31-firstfault: the live descriptor this job's shadow copy
	 * was taken from (the chain node's original GPU address), or 0
	 * when the dispatched jc IS the original descriptor. The
	 * fault-time page dump prints both pages so the shadow copy and
	 * the Mesa page can be compared word for word. */
	uint64_t src_jc;
	uint32_t requirements;		/* PANFROST_JD_REQ_* */
	uint32_t as_nr;			/* address space bound to slot */
	uint64_t affinity;		/* shader-present mask */
	uint32_t flush_id;		/* recorded, unused (no FLUSH_REDUCTION) */
	uint32_t out_sync;		/* syncobj handle for the arm-time fence
					 * (0 = none; validated at SUBMIT,
					 * installed from sched_job->fence
					 * during the ioctl) */
	/* SUBMIT-time chain-head descriptor snapshot (gpu31-lastjob,
	 * extended gpu31-submitsnap): the 8 words at jc, read from the
	 * mapped descriptor BO before submit-sync (same invalidate the
	 * completion path runs, so the snapshot sees CPU bytes).
	 * desc[4] carries bit0 (job_descriptor_size) + type + index
	 * for the chain_mode/dispatch consumer, desc[5] the dep pair;
	 * the ERR/TIMEOUT/dispatch/PTEWALK lines print all 8 words
	 * next to the fault-time bytes for the working-vs-faulting
	 * discriminator. All zeros when the submit-time snapshot was
	 * unavailable (cross-page jc, unmapped page, jc outside every
	 * submitted BO). */
	uint32_t desc[8];
	/* gpu31-iova: BO identity snapshot for fault-time mapping dump.
	 * Stamped at SUBMIT (stable handles + IOVA/size while the job
	 * owns refs): iova_bo_count entries of {handle, iova, size}.
	 * The ERR pin walks them via the driver's resolver hook (no
	 * GEM dereference in IRQ context). */
	uint32_t *iova_handles;
	uint64_t *iova_addrs;
	uint32_t *iova_sizes;
	uint32_t iova_bo_count;
	/* Fences. done_fence owns the arm-time fence ref and is signaled
	 * by the JOB_INT handler (or the fault-recovery path, F-JOB4).
	 * Under the 0004 arm-time contract done_fence IS sched_job->fence
	 * (same pointer, shared ref); render_done_fence is kept only as a
	 * non-owning alias for the legacy out_sync leg (F-JOB3: never
	 * put twice). in_fences are explicit SUBMIT deps (owned here). */
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
	/* Opaque owning drm_sched_job: the IRQ fault leg kicks this job's
	 * timeout/recovery work immediately through the 0004 shim instead
	 * of leaving the fence unsignalled for the full retry budget. */
	void *sched_job;
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

	/* gpu31-chainsplit: driver-owned shadow descriptor BO shared by
	 * every node of one split chain (opaque; submit.c owns its
	 * layout and refcount). NULL when this submit ran as a single
	 * original descriptor (one-node chain with no deps). chain_len
	 * is the number of nodes the walk found; chain_pos is this
	 * node's 0-based position in that walk (diagnostics: dmesg
	 * lines prove the split order). */
	void *chain_shadow;
	uint32_t chain_pos;
	uint32_t chain_len;
	/* gpu31-storm58: first-fault page inputs, captured at SUBMIT.
	 * fbd is descriptor word 10 for a fragment node (type 9), read
	 * from the descriptor BO during the chain walk: the one-shot
	 * dump needs the SFBD address, and desc[] is only 8 words, so
	 * the old job->desc[10] read was out of bounds (on int42 it
	 * fed an unrelated pointer into the "fbd" page dump).
	 * jc_age counts SUBMITs this file made between the last one
	 * that touched the BO holding jc and this job: 1 means the
	 * descriptor page was written by the previous SUBMIT, 2 by the
	 * previous frame's FS submit. A small age at fault time is a
	 * recycled BO, and if a job is still in flight on it that is
	 * the storm's userspace-write race. */
	uint32_t fbd;
	uint32_t jc_age;
};

/* Per-file submit context: one sched entity per slot. */
struct p7580_file_ctx {
	uint32_t next_seqno;
	void *sched_entity[P7580_NUM_JOB_SLOTS];
	void *syncobj_mgr;		/* backport drm_syncobj_manager */
};
/* ------------------------------------------------------------------ */
/* gpu31-firstfault: in-memory job-event ring.                        */
/*                                                                    */
/* The long-run fault is rare (one frame in tens/hundreds) and the    */
/* serial console cannot carry a per-dispatch trace without perturbing*/
/* the timing that triggers it. So the dispatch, IRQ, timeout,        */
/* recovery and runtime-PM events are recorded into a fixed ring in   */
/* RAM (a few stores, no printk, no MMIO unless the ring is armed)    */
/* and dumped once, with the descriptor pages, at the first fault.    */
/* all-zero jc/a/b/c rings are still useful: they show the ordering.  */
/* ------------------------------------------------------------------ */
#define P7580_EV_MAX	128u
enum p7580_ev_kind {
	P7580_EV_START = 1,	/* hw_submit: a=status pre, b=cmd post,
				 * c=status post (armed: MMIO probed) */
	P7580_EV_DONE,		/* IRQ DONE: a=js_status, b=int bits */
	P7580_EV_ERR,		/* IRQ ERR: a=js_status, b=int bits */
	P7580_EV_TIMEOUT,	/* timedout: a=js_status */
	P7580_EV_RECOVER,	/* recover: a=latched, b=cycled, c=rc */
	P7580_EV_SUSPEND,	/* runtime suspend accepted */
	P7580_EV_SUSPEND_BUSY,	/* runtime suspend refused (-EBUSY) */
	P7580_EV_RESUME,	/* runtime resume */
	P7580_EV_CYCLE,		/* power cycle completed: a=rc */
	P7580_EV_DUMP,		/* ring dump marker */
};
struct p7580_ev {
	uint64_t ns;		/* ktime_get() at record time */
	uint64_t jc;
	uint32_t kind;		/* enum p7580_ev_kind */
	uint32_t slot;
	uint32_t seq;
	uint32_t a;
	uint32_t b;
	uint32_t c;
};

/* Last-job snapshot: what the ERR pins and the dump knob report. */
/* Updated (under hw_lock) at every hw_submit dispatch for the next */
/* OUTCOME print (DONE/ERR/timeout), so the fault register capture */
/* follows the faulting submit even after the 8-job startup budget */
/* is exhausted. Plain data: no pointers, no locks, seq_file-safe. */
struct p7580_job_last {
	unsigned int valid;	/* 0 until the first dispatch */
	unsigned int slot;	/* job->slot at dispatch */
	unsigned int seqno;	/* job->seqno at dispatch */
	uint64_t jc;		/* GPU address latched to HEAD_NEXT */
	uint32_t cfg;		/* CONFIG_NEXT value actually written */
	uint32_t as_nr;		/* job->as_nr at dispatch */
	uint64_t affinity;	/* job->affinity at dispatch */
	uint32_t sw[8];		/* SUBMIT-time descriptor words */
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
	 * trace_arm is the re-armable source the trace_arm debugfs knob
	 * writes: p7580_job_trace_arm() copies trace_arm into trace_left,
	 * so Main/phone can re-arm tracing at any time (echo N >
	 * .../p7580/trace_arm). Seed: P7580_JOB_TRACE_DEFAULT. */
	unsigned int trace_left;
	unsigned int trace_arm;
	/* Last dispatch snapshot: rewritten (under hw_lock) at every
	 * hw_submit, so the ERR/timeout paths always identify the
	 * faulting job's slot/jc/cfg even after the budget is out. */
	struct p7580_job_last last;
	/* debugfs dir handle (drivers/gpu/drm/panfrost/p7580): owned by
	 * job.c, created at dev_init, removed at dev_fini. Opaque void*
	 * so this header stays dentry-free under -fsyntax-only. */
	void *dbg_dir;
	/* PM device + get/put hooks (driver-owned) so the dump path can
	 * take a runtime-PM ref before touching JOB_INT/JS registers:
	 * without it a dump read while autosuspended hits a dark bus
	 * (external abort). Opaque void pointer plus int-returning
	 * hooks, so this header stays device/PM-free under
	 * -fsyntax-only. Wired by p7580_job_dev_init. */
	void *pm_dev;
	int (*pm_get)(void *dev);
	void (*pm_put)(void *dev);
	/* Fault-recovery core power cycle (gpu31-readback), wired by
	 * p7580_job_dev_init from the driver TU (the jobs slice never
	 * includes the power slice). Called from p7580_job_recover()
	 * with reset_lock held and a live PM ref owned by the caller,
	 * only when a POWER_FAULT latch survived the GPU reset: that
	 * latch is only cleared by cycling the cores, and until it is
	 * cleared every following job on the slot reports 0x41 again.
	 * MMU AS0 reprogram + JOB_INT re-arm happen after it returns.
	 * NULL when unwired: recovery then behaves as before. */
	int (*power_cycle)(void *dev);
	/* Chain-bit experiment (gpu31-lastjob): runtime switch, no
	 * rebuild needed to A/B. 0 = off (both variants disabled,
	 * submit bytes untouched); 1 = variant A (set JS_CONFIG
	 * JOB_CHAIN_FLAG when the chain head's descriptor bit0 is
	 * set); 2 = variant B (clear descriptor bit0, next stays
	 * NULL, before submit-sync). Wired to the chain_mode debugfs
	 * knob below; defaults to 0. Plain data, no locks beyond the
	 * hw_lock writers already hold on the dispatch path. */
	unsigned int chain_mode;
	/* gpu31-iova: fault-time mapping resolver (ERR-pin dump).
	 * Registered by the driver at probe; runs in IRQ context
	 * (snapshot arrays only: no GEM dereference, no sleeping
	 * locks, no MMIO). NULL when unregistered. */
	int (*iova_resolve)(void *priv, char *buf, size_t len,
			    const uint32_t *handles, const uint64_t *addrs,
			    const uint32_t *sizes, uint32_t n);
	void *iova_resolve_priv;
	/* gpu31-firstfault: event ring + first-fault dump (declarations
	 * above). ev_arm is set by the ev_arm debugfs knob; it enables
	 * the per-dispatch MMIO probes (status/command) AND the one-shot
	 * ring + page dump at the first fault/timeout. ev_dumped makes
	 * the dump one-shot (re-arm with a new ev_arm write). */
	struct p7580_ev ev[P7580_EV_MAX];
	unsigned int ev_head;
	unsigned int ev_arm;
	unsigned int ev_dumped;
	/* gpu31-firstfault: descriptor-page dumper (driver-owned, process
	 * context only: resolves the IOVA to a BO and kmaps it). The jobs
	 * slice never dereferences GEM; NULL when unwired. */
	int (*desc_dump)(void *priv, uint64_t gpu, const char *tag);
	void *desc_dump_priv;
	/* gpu31-firstfault: 1 = queue the FS (slot 0) submit on the
	 * chain slot's entity so it is strictly ordered behind the
	 * tiler by one FIFO instead of relying on the cross-entity
	 * dependency alone. Runtime A/B knob (serialize_fs). */
	unsigned int serialize_fs;
	/* gpu31-watchdog: deterministic silent-hang injection. hang_arm
	 * = number of upcoming dispatches whose slot DONE+ERR bits are
	 * masked at START, so the job runs and retires in hardware but
	 * no IRQ is ever delivered: the exact silent signature (fence
	 * unsignalled, STAT/RAWSTAT 0, COMMAND_NEXT drained) that only
	 * the sched timeout can break. The timeout/recover path re-arms
	 * the mask. Debugfs hang_inject, default 0 (inert). */
	unsigned int hang_arm;
	/* gpu31-watchdog: sched dispatch-timeout accessors (job_timeout_ms
	 * debugfs). The timeout lives in the driver's drm_sched seed, so
	 * the jobs slice only forwards; NULL set = read-only knob. */
	unsigned int (*get_timeout_ms)(void *priv);
	int (*set_timeout_ms)(void *priv, unsigned int ms);
	void *timeout_priv;
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
		       dma_addr_t pgd_dma, void *pm_dev,
		       int (*pm_get)(void *dev), void (*pm_put)(void *dev),
		       int (*power_cycle)(void *dev));
#else
#include <stdint.h>
typedef uint64_t p7580_dma_addr_t;
int p7580_job_dev_init(struct p7580_job_dev *jd, void *regs,
		       uint32_t js_present, uint64_t shader_present,
		       p7580_dma_addr_t pgd_dma);
#endif
void p7580_job_dev_fini(struct p7580_job_dev *jd);
/* True when a present job slot still reports a latched HW fault code in
 * JS_STATUS after a reset/power-on (POWER_FAULT 0x41 and friends; our
 * own HARD_STOP's INTERRUPTED/STOPPED/TERMINATED are not faults). The
 * driver's resume path uses it to clear such a latch with a core power
 * cycle before the first job is dispatched; recover() uses it for the
 * same reason after the GPU-level reset. Process context, block must be
 * powered (runtime-PM ref held). */
int p7580_job_slots_latched_fault(struct p7580_job_dev *jd);
/* Debug knobs (errtrace): re-arm the trace budget (copies N into both
 * trace_arm and trace_left under hw_lock; N=0 stops tracing). Dump
 * renders the last-dispatch snapshot + live slot registers into buf
 * (seq_file show; returns 0, prints "no job yet" when !valid). */
void p7580_job_trace_arm(struct p7580_job_dev *jd, unsigned int n);
/* gpu31-iova: map/unmap trace budget knob (iova_arm). The counter
 * lives in the driver (submit.c); this registers its address so the
 * debugfs write path can re-arm it. NULL unregisters. */
void p7580_job_register_iova_budget(unsigned int *budget);
/* Chain-bit experiment knob (gpu31-lastjob): write 0/1/2 selects the
 * variant (0 off, 1 = A set cfg CHAIN flag on desc-bit0, 2 = B clear
 * desc bit0 via the driver's pre-submit word patch). Applied on the
 * dispatch path under hw_lock; no rebuild needed to A/B. */
void p7580_job_chain_set(struct p7580_job_dev *jd, unsigned int mode);
struct seq_file;
int p7580_job_dump_show(struct p7580_job_dev *jd, struct seq_file *m);
/* CLEAR + unmask after reset/resume (power held by caller). */
void p7580_job_dev_rearm(struct p7580_job_dev *jd);
/* Program HEAD_NEXT/AFFINITY_NEXT/CONFIG_NEXT + COMMAND_NEXT=START.   */
void p7580_job_hw_submit(struct p7580_job_dev *jd,
			 struct p7580_submit_job *job);
/* JOB_INT handler: demux DONE/ERR per slot (IRQ context). */
int p7580_job_irq(int irq, struct p7580_job_dev *jd);

/* Scheduler backend: alloc_fence / run_job / timedout_job / free_job.
 * alloc_fence allocates the arm-time completion fence (the job's ONLY
 * fence); run_job binds that same fence and programs the slot. */
struct dma_fence;
struct dma_fence *p7580_job_alloc_fence(struct p7580_job_dev *jd,
					int slot, uint64_t *seqno_out);
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

/* gpu31-iova: fault-time mapping resolver. The driver registers a hook
 * (+priv) that the ERR pin calls in IRQ context to render the faulting
 * job's {handle,iova,size} snapshot (stamped at SUBMIT) into the pin
 * line; NULL hook (or NULL job snapshot) prints the jc-only form. The
 * hook must be IRQ-safe: no GEM dereference, no locks that sleep, no
 * MMIO - the driver resolves from its bo_list snapshot. Process
 * context (dump_show) may reuse the same hook with a bigger buffer. */
void p7580_job_set_iova_resolver(struct p7580_job_dev *jd,
				 int (*fn)(void *priv, char *buf, size_t len,
					   const uint32_t *handles,
					   const uint64_t *addrs,
					   const uint32_t *sizes, uint32_t n),
				 void *priv);

/* Idle check for suspend/close paths. */
int p7580_job_is_idle(struct p7580_job_dev *jd);

/* gpu31-firstfault: event ring + first-fault descriptor-page dump.
 * p7580_job_ev() appends one record (IRQ-safe: one spinlock, no MMIO,
 * no printk); kind is enum p7580_ev_kind. p7580_job_ev_arm() resets
 * the ring and arms/disarms the per-dispatch MMIO probes and the
 * one-shot dump. p7580_job_set_desc_dump() registers the driver's
 * page dumper (process context). p7580_job_first_fault() is called by
 * the timeout/fault paths with the faulting job and dumps ring + pages
 * once per arm. */
void p7580_job_ev(struct p7580_job_dev *jd, unsigned int kind, int slot,
		  uint32_t seq, uint64_t jc, uint32_t a, uint32_t b,
		  uint32_t c);
void p7580_job_ev_arm(struct p7580_job_dev *jd, unsigned int on);
void p7580_job_set_desc_dump(struct p7580_job_dev *jd,
			     int (*fn)(void *priv, uint64_t gpu,
				       const char *tag), void *priv);
/* gpu31-watchdog: sched dispatch-timeout accessors behind the
 * job_timeout_ms debugfs knob. The driver owns the timeout (it is the
 * drm_sched seed), so the jobs slice only forwards get/set; NULL set
 * leaves the knob read-only. Process context, no MMIO. */
void p7580_job_set_timeout_hooks(struct p7580_job_dev *jd,
				 unsigned int (*get)(void *priv),
				 int (*set)(void *priv, unsigned int ms),
				 void *priv);
void p7580_job_first_fault(struct p7580_job_dev *jd,
			   struct p7580_submit_job *job, uint32_t status);

#endif /* PANFROST7580_JOB_H */
