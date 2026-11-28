/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_drv.c - render-minor-only platform driver for Mali-T720
 * (Exynos 7580, j7elte) on Nura 3.10.108.
 *
 * Binds the stock node (never edited):
 *   mali@0x11400000, compatible "arm,mali",
 *   reg 0x11400000/0x5000, IRQs (index order) job/mmu/gpu = SPI
 *   282/283/281, 6 clocks (fin_pll, fout_g3d_pll, aclk_g3d, mout_g3d,
 *   dout_aclk_g3d, aclk_g3d_400), power-domain <&pd_g3d> (0x60).
 *
 * Registers one DRM device with NO modeset (driver_features has no
 * DRIVER_MODESET) and DRIVER_RENDER_ONLY set, so the 3.10 core
 * allocates ONLY the render minor (no primary, no controlD); no
 * /dev/dri/cardN appears and /dev/fb0 + fbdev X stay untouched.
 *
 * Six v5.2 ioctls (0x00 SUBMIT, 0x01 WAIT_BO, 0x02 CREATE_BO,
 * 0x03 MMAP_BO, 0x04 GET_PARAM, 0x05 GET_BO_OFFSET), byte-identical to
 * Linux v5.2 include/uapi/drm/panfrost_drm.h. SUBMIT programs the job
 * slots via the jobs slice; power/clocks/regulator via the power
 * slice; DVFS via the devfreq slice.
 *
 * Probe order: ioremap -> present regs -> power -> devfreq ->
 * GEM/MM -> IRQs -> DRM attach. Remove unwinds in reverse.
 */

#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drmP.h>
#include <drm/drm.h>

#include <linux/dma-fence.h>		/* 0001 */
#include <linux/dma-resv.h>		/* 0002 */
#include <drm_syncobj.h>		/* 0003 */
#include <drm_sched.h>			/* 0004 */
#include <linux/bug.h>			/* opsfix: BUILD_BUG_ON */

#include "panfrost_drm.h"
#include "panfrost7580_gem.h"
#include "panfrost7580_ioctl.h"
#include "panfrost7580_jm_hw.h"
#include "panfrost7580_job.h"
#include "panfrost7580_mmu.h"
#include "panfrost7580_power.h"
#include "pf7580_devfreq.h"
#include "panfrost7580_drv.h"

/* ------------------------------------------------------------------ */
/* SUBMIT glue: staged ioctl (kernel-memory arrays) -> jobs core      */
/* ------------------------------------------------------------------ */

/*
 * p7580_drv_submit - validate + reserve + sched-dispatch one SUBMIT.
 *
 * @drv:   device state (job slots, MMU, GEM allocator).
 * @dev:   DRM device (GEM lookups + dev_private == &drv->gd).
 * @file_priv: caller DRM file (BO handles + out-sync scope).
 * @args:  staged SUBMIT (in_syncs/bo_handles point at KERNEL memory
 *         staged by panfrost_ioctl_submit via copy_from_user).
 *
 * Steps (mirrors v5.2 panfrost_job_push on the 0001-0004 shims):
 *  1. validate jc/requirements/counts (shared p7580_jc_valid +
 *     P7580_MAX_* caps; slot legal + present).
 *  2. resolve in_sync handles -> fences (0003 find_fence, ref held).
 *  3. look up + pin every BO (GEM ref held), reserve each resv
 *     (0002 plain mutex), map unmapped pages into AS 0 and track them
 *     on drv->bo_list for the remove/unload unmap walk.
 *  4. sched job alloc on the slot entity + deps (in-fences, implicit
 *     excl fences) + submit; out-sync install (0003 replace_fence).
 * Errors drop every taken ref and release every taken lock; the
 * caller (ioctl) never touches userspace here.
 *
 * Power contract (F-SUB1): the caller (the SUBMIT ioctl override below)
 * holds one runtime-PM ref across this call for the synchronous
 * MMU-map + sched-queue leg. The async leg is covered separately: one
 * extra live-job ref is armed just before drm_sched_job_submit and
 * released in p7580_drv_free_job after the job retires, so the block
 * cannot autosuspend between the ioctl return and the run_job worker
 * (or the timeout/recover path) touching HEAD/START/RESET registers.
 * p7580_drv_pm_put is only ever called for a ref this file took via
 * p7580_drv_pm_get (never balances someone else's get).
 */
/* Live-job helpers (defined after the backend ops; free_job calls release). */
static void p7580_drv_job_pm_arm(struct p7580_drv *drv);
static void p7580_drv_job_pm_release(struct p7580_drv *drv);

/* gpu31-chainsplit: driver-owned shadow descriptor shared by the nodes
 * of one split chain (definition with the split helpers below). free_job
 * drops this job's reference. */
struct p7580_chain_shadow;
static void p7580_drv_shadow_put(struct p7580_chain_shadow *sh);

/* Scheduler backend ops for the 0004 shim. The sched job owner IS
 * the submit job; the file state is recovered from the entity. */
static struct drm_sched p7580_drv_sched;
static int p7580_drv_sched_live;
static DEFINE_MUTEX(p7580_drv_sched_lock);
/* gpu31-watchdog: dispatch timeout seed for drm_sched_init and the
 * job_timeout_ms debugfs knob. 750 ms bounds a silent hang to <1 s
 * (timeout + one recovery) while staying 2x above the worst measured
 * frame (512^2 readback 373 ms of CPU detile; GPU jobs are tens of
 * us). Runtime writes apply to entities created afterwards (each
 * client run opens a new file) and to the live sched->timeout. */
static unsigned int p7580_drv_job_timeout_ms = P7580_JOB_TIMEOUT_MS;

/* Bounded SUBMIT/run_job trace (schedtrace): ST-IOCTL/ST-QUEUE see
 * the ioctl entry and the 0004 queue handoff; ST-RUN/ST-PMGET see
 * the async worker entry and the run_job PM re-get verdict.
 * Shared budget: first 8 SUBMITs print (0 = off). Plain static
 * (NOT module_param_named): the integrator's object rule defines
 * KBUILD_MODNAME at Image link time, but slice-level -fsyntax-only
 * checks compile this file standalone (same reason job.c keeps
 * p7580_job_trace a plain static). Racy dec under concurrent
 * ioctls overshoots by at most a line or two; harmless. */
static unsigned int p7580_sched_trace = 8;

/* Bounded SUBMIT error attribution (gpu31-multidep): every early
 * error return in p7580_drv_submit routes its errno through here so
 * the next silicon assert carries stage + errno instead of a bare
 * ioctl rc. Shares the ST budget discipline (first 8 SUBMITs print).
 * stage is a short verb: validate/in_sync/bo/job/ops/dep/queue/
 * out_sync. */
static long p7580_submit_err(const char *stage, long err)
{
	if (p7580_sched_trace && err) {
		p7580_sched_trace--;
		pr_info("p7580_sched: submit_err stage=%s rc=%ld\n", stage, err);
	}
	return err;
}

/* gpu31-iova: dedicated budget for the map/unmap trace (map prints
 * once per fresh bind, unmap once per free-time unbind; the fault
 * path re-arms via trace_arm like the job trace). Raised by the
 * debugfs iova_arm knob (jobs slice); 0 stops it. */
unsigned int p7580_iova_trace = 64;

/* gpu31-chainsplit: one line per split submit (head jc, node count,
 * shadow BO base) for the first 8 SUBMITs; the per-node dispatch lines
 * already print each shadow jc. */
static unsigned int p7580_chain_trace = 8;

static unsigned int p7580_drv_run_seq = 0;

/* gpu31-boleak: take a reference on a BO named by a drv->bo_list entry.
 *
 * bo_list entries are weak (drv.h struct p7580_drv_bo): unlinking runs
 * from the object's destructor, so "entry still linked" does not by
 * itself mean the refcount is nonzero -- between kref_put() reaching
 * zero and p7580_gem_gpu_unmap() taking bo_lock the object is dying and
 * its entry is still linked. kref_get_unless_zero() turns that race
 * into a clean miss (the caller skips the entry) instead of a
 * resurrection that would hand out a pointer the destructor is about
 * to free. Every caller of this helper runs with bo_lock held and uses
 * the reference to dereference the BO after unlocking. */
static int p7580_drv_gem_try_get(struct drm_gem_object *obj)
{
	if (!obj)
		return 0;
	return kref_get_unless_zero(&obj->refcount);
}

static struct dma_fence *p7580_drv_run_job(struct drm_sched_job *sched_job)
{
	struct p7580_submit_job *job = sched_job ? sched_job->owner : NULL;
	struct drm_sched_entity *ent = sched_job ? sched_job->entity : NULL;
	struct p7580_drv_file *df;
	struct p7580_drv *drv;
	int slot;

	/* ST-RUN (entry): first statement -- before any deref/derivation.
	 * Numeric (%lu) so kptr_restrict cannot censor it. Proves the
	 * 0004 pump called OUR run_job; per-guard bail prints below name
	 * the exact reject. runexp= is our own address (no symbol lookup
	 * needed); ST-OPS run= must equal it. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: run job=%lu ent=%lu owner=%lu ops=%lu runexp=%lu\n",
			(unsigned long)sched_job, (unsigned long)ent,
			(unsigned long)(sched_job ? sched_job->owner : NULL),
			(unsigned long)(sched_job ? sched_job->ops : NULL),
			(unsigned long)&p7580_drv_run_job);
	}
	if (!sched_job || !job) {
		if (p7580_sched_trace) {
			p7580_sched_trace--;
			pr_info("p7580_sched: run_bail NULLJOB job=%lu\n",
				(unsigned long)sched_job);
		}
		return ERR_PTR(-EINVAL);
	}
	/* Monotonic entry stamp: free_job prints ran=N so the
	 * dispatch->retire order is provable even if a print is lost. */
	job->run_seq = ++p7580_drv_run_seq;

	/* Probenumeric: trust the SUBMIT-stamped file/driver pointers on
	 * the job (stamped next to slot at queue time); the entity-pointer
	 * container_of math is fallback-only (range-checked) for jobs
	 * queued before this Stamp existed. ent itself is used only for
	 * the fallback -- never trusted blindly. */
	df = (struct p7580_drv_file *)job->file_priv;
	drv = (struct p7580_drv *)job->drv_priv;
	slot = job->slot;
	if (slot < 0 || slot >= P7580_NUM_JOB_SLOTS || !df || !drv) {
		struct p7580_drv_file *edf;
		if (p7580_sched_trace) {
			p7580_sched_trace--;
			pr_info("p7580_sched: run_fallback seq=%u slot=%d sslot=%d\n",
				job->seqno, slot, sched_job->slot);
		}
		edf = ent ? container_of(ent, struct p7580_drv_file, entity[0]) : NULL;
		if (edf && ent >= &edf->entity[0] &&
		    ent < &edf->entity[P7580_NUM_JOB_SLOTS]) {
			if (!df)
				df = edf;
			if (!drv)
				drv = df->drv;
			if (slot < 0 || slot >= P7580_NUM_JOB_SLOTS)
				slot = (int)(ent - &edf->entity[0]);
		}
	}
	if (slot < 0 || slot >= P7580_NUM_JOB_SLOTS) {
		if (p7580_sched_trace) {
			p7580_sched_trace--;
			pr_info("p7580_sched: run_bail BADSLOT seq=%u slot=%d\n",
				job->seqno, slot);
		}
		return ERR_PTR(-EINVAL);
	}
	job->slot = slot;
	if (!df || (ent && (ent < &df->entity[0] ||
			    ent >= &df->entity[P7580_NUM_JOB_SLOTS]))) {
		/* ent outside the stamped df: trust the stamp, distrust ent. */
		if (p7580_sched_trace) {
			p7580_sched_trace--;
			pr_info("p7580_sched: run_bail BADENT seq=%u ent=%lu df=%lu\n",
				job->seqno, (unsigned long)ent,
				(unsigned long)df);
		}
		if (!df)
			return ERR_PTR(-ENODEV);
	}
	if (!drv)
		drv = df->drv;
	if (!drv) {
		if (p7580_sched_trace) {
			p7580_sched_trace--;
			pr_info("p7580_sched: run_bail NODRV seq=%u slot=%d df=%lu\n",
				job->seqno, slot, (unsigned long)df);
		}
		return ERR_PTR(-ENODEV);
	}
	/* Async worker context (0004 wq): the ioctl's synchronous PM
	 * ref is long gone (returned) and the live-job ref only
	 * guarantees the block was continuously powered since queue
	 * time -- it cannot resurrect a block that suspended despite
	 * that (get/put counting bug elsewhere) or that failed
	 * resume. Re-get here so HEAD/AFFINITY/CONFIG/START writes
	 * below never hit a dark block; fail the job (ERR_PTR, freed
	 * by the 0004 F4 path) instead of hanging the bus. */
	/* ST-RUN (dispatched): slot + jc the job will program. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: run_job seq=%u slot=%d sslot=%d jc=0x%llx owner=%lu\n",
			job->seqno, slot, sched_job->slot,
			(unsigned long long)job->jc, (unsigned long)job);
	}
	if (p7580_drv_pm_get(drv) < 0) {
		/* ST-PMGET (fail): dark block (negative errno only;
		 * pm_runtime_get_sync returns 1 when already active,
		 * which the helper normalises to 0 = powered), job
		 * failed closed. */
		if (p7580_sched_trace) {
			p7580_sched_trace--;
			pr_info("p7580_sched: pm_get FAIL seq=%u slot=%d\n",
				job->seqno, slot);
		}
		return ERR_PTR(-EIO);
	}
	/* ST-PMGET (held): re-get succeeded, HEAD/START writes allowed. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: pm_get HELD seq=%u slot=%d\n",
			job->seqno, slot);
	}
	{
		/* Arm-time fence (gpu31-armfence): bind sched_job->fence
		 * (allocated at arm time, already on out_sync since
		 * SUBMIT) as this job's done_fence (one shared ref) and
		 * program the slot. No out_sync work here: the install
		 * happened during the ioctl. */
		struct dma_fence *armed = sched_job->fence;
		struct dma_fence *fence;

		if (!armed)
			return ERR_PTR(-ENODATA);
		/* Bind the arm-time fence: one shared ref, never a
		 * second allocation (the shim pump verifies pointer
		 * identity on return). */
		job->done_fence = armed;
		dma_fence_get(armed);
		p7580_job_hw_submit(&drv->jd, job);
		fence = armed;
		/* Non-owning alias for the legacy out_sync leg. */
		job->render_done_fence = fence;

		/* gpu31-storm58: BO exclusivity is published at SUBMIT and
		 * only there. An arm-time re-publish is NOT harmless: the
		 * resv holds exactly one exclusive fence (the 0002 shim's
		 * dma_resv_add_excl_fence replaces it), and this path runs
		 * asynchronously, after a LATER submit of the same BOs may
		 * already have published a newer fence. The older fence
		 * then replaced the newer one, so Mesa's BO-cache fetch
		 * (WAIT_BO with timeout 0) saw the BO idle while the newer
		 * submit's job was still reading it, and rebuilt the next
		 * frame into it under the GPU: the int42 fragment 0x58
		 * storm. SUBMIT publishes the split's final fence in serial
		 * order (under serial_lock), which is the newest fence the
		 * BO can carry; the get_excl leg at queue time keeps
		 * later submits ordered behind it. */
		p7580_drv_pm_put(drv);
		return fence;
	}
}

/* gpu31-ptewalk: fault-time AS0 page-table walk for the faulting jc.
 * Bounded, PM-held, process-context only (timedout workqueue before
 * recover tears state down): walk the 4-level LPAE tables from
 * drv->mmu.root_dma over at most 4 kmap/kunmap pairs and report the
 * leaf PTE for jc, then compare it against the owning BO's current
 * page (BO identity from the faulting job's SUBMIT-time snapshot via
 * its referenced object pointers). Prints one pr_err line:
 * leaf phys vs BO page phys + the first 32 descriptor bytes re-read
 * from the leaf's physical page via kmap in the same handler.
 * MATCH => GPU-side stale fetch (TLB/cache, not tables);
 * MISMATCH => page-table/IOVA lifetime bug. Pure CPU reads + the
 * BO's own dma mapping; never touches the TLB. Skips dark (fails
 * closed: PM get failed) and never sleeps beyond kmap/kunmap on
 * lowmem table pages (same discipline as the GEM debug-read leg).
 * Bounded: one page walk + one 32-B re-read, always. */
static noinline void p7580_drv_ptewalk_pin(struct p7580_drv *drv,
					    struct p7580_submit_job *job)
{
	dma_addr_t cur;
	unsigned int l;
	u64 e = 0;
	u64 jc;
	u64 jc_page;
	unsigned int slot;
	unsigned int seq;
	u64 leaf_pte = 0;
	phys_addr_t leaf_phys = 0;
	struct drm_gem_object *owner = NULL;
	struct p7580_bo *owner_bo = NULL;
	struct drm_gem_object *live_obj = NULL;
	struct p7580_bo *live_bo = NULL;
	u64 live_iova = 0;
	size_t live_size = 0;
	u64 snap_iova = 0;
	size_t snap_size = 0;
	unsigned long owner_iova = 0;
	unsigned int owner_npages = 0;
	unsigned int page_idx = 0;
	unsigned int page_off = 0;
	phys_addr_t bo_phys = 0;
	int bo_ok = 0;
	u32 pw[128];
	unsigned int wi;
	int have_bytes = 0;

	pr_err("p7580_job: TMD-WALKER drv=%lu job=%lu\n",
	       (unsigned long)drv, (unsigned long)job);
	if (!drv || !job)
		return;
	if (!drv->mmu.root_dma)
		return;
	jc = job->jc;
	slot = (unsigned int)job->slot;
	seq = job->seqno;
	jc_page = jc & PAGE_MASK;
	cur = drv->mmu.root_dma;
	for (l = 0; l < 3; l++) {
		u64 *cpu;
		struct page *pg;
		unsigned int shift = 12 + (3 - l) * 9;
		unsigned int idx = (unsigned int)((jc_page >> shift) & 0x1FFu);

		pg = pfn_to_page((unsigned long)(cur >> PAGE_SHIFT));
		cpu = kmap(pg);
		e = cpu[idx];
		kunmap(pg);
		if ((e & 3ULL) != 3ULL) {
			pr_err("p7580_job: PTEWALK slot=%d seq=%u jc=0x%llx level=%u UNMAPPED\n",
			       slot, seq, (unsigned long long)jc, l);
			return;
		}
		cur = (dma_addr_t)(e & ~(u64)0xFFFULL);
	}
	{
		u64 *cpu;
		struct page *pg;
		unsigned int idx = (unsigned int)((jc_page >> 12) & 0x1FFu);

		pg = pfn_to_page((unsigned long)(cur >> PAGE_SHIFT));
		cpu = kmap(pg);
		e = cpu[idx];
		kunmap(pg);
	}
	if ((e & 3ULL) != 1ULL) {
		pr_err("p7580_job: PTEWALK slot=%d seq=%u jc=0x%llx leaf=0x%llx NOT-LEAF\n",
		       slot, seq, (unsigned long long)jc,
		       (unsigned long long)e);
		return;
	}
	leaf_pte = e;
	leaf_phys = (phys_addr_t)(e & ~(u64)0xFFFULL);
	/* Owner resolution, two views. (1) SUBMIT-time snapshot: jc
	 * inside one submitted BO's IOVA. The job's bos[] array holds
	 * one referenced pointer per handle (F-JT2, stable across
	 * close); safe to dereference here in process context while
	 * the job owns the refs. (2) Live view: scan drv->bo_list
	 * under bo_lock for an IOVA range containing jc, taking a GEM
	 * ref while linked (weak entry -> try_get; a dying object is
	 * skipped, see p7580_drv_gem_try_get). Snapshot-miss + live-hit
	 * (int29 NO-OWNER with jc inside an ERR-MAP range) means a stale
	 * snapshot, not an unmapped jc; miss + miss means genuinely
	 * unowned. */
	if (job->bos && job->bo_count) {
		unsigned int i;

		for (i = 0; i < job->bo_count; i++) {
			struct drm_gem_object *o =
				(struct drm_gem_object *)job->bos[i];
			struct p7580_bo *b;

			if (!o)
				continue;
			b = to_p7580_bo(o);
			if (b && b->iova_valid && jc >= b->iova &&
			    jc + 32 <= b->iova + b->base.size) {
				owner = o;
				owner_bo = b;
				snap_iova = b->iova;
				snap_size = b->base.size;
				break;
			}
		}
	}
	mutex_lock(&drv->bo_lock);
	{
		struct p7580_drv_bo *le;

		list_for_each_entry(le, &drv->bo_list, node) {
			if (jc >= le->iova && jc + 32 <= le->iova + le->size) {
				if (!p7580_drv_gem_try_get(le->obj))
					continue;
				live_obj = le->obj;
				live_iova = le->iova;
				live_size = le->size;
				break;
			}
		}
	}
	mutex_unlock(&drv->bo_lock);
	if (live_obj)
		live_bo = to_p7580_bo(live_obj);
	/* Fall back to the live owner when the snapshot missed, so the
	 * MATCH/MISMATCH comparison still runs against the BO that is
	 * actually bound at fault time. */
	if (!owner && live_bo) {
		owner = live_obj;
		owner_bo = live_bo;
	}
	if (owner && owner_bo) {
		owner_iova = owner_bo->iova;
		owner_npages = p7580_bo_npages(owner);
		page_idx = (unsigned int)((jc - owner_bo->iova) >> PAGE_SHIFT);
		page_off = (unsigned int)((jc - owner_bo->iova) & (PAGE_SIZE - 1));
		bo_phys = p7580_bo_dma_addr(owner, page_idx);
		bo_ok = (bo_phys != 0);
	} else {
		page_off = (unsigned int)(jc & (PAGE_SIZE - 1));
	}
	/* Unconditional GPU-view dump (gpu31-jddecode): 128 words
	 * (512 B) at leaf_phys + page_off, re-read from the leaf's
	 * physical page via kmap in the same handler: the 32-B header
	 * (w0..w7), the fragment payload (bounds + framebuffer pointer,
	 * w8..w15) and the following 448 B of pool bytes. Runs even
	 * with NO-OWNER (page_off is then the jc offset within its
	 * page). The JD body (64-B FRAGMENT_JOB) never crosses a page
	 * (pool allocs are 64-B aligned); if the 512-B window would
	 * cross, the in-page prefix is dumped and the line is marked
	 * JD-TRUNC (word count in jd_words). */
	if (page_off + 32 > PAGE_SIZE) {
		pr_err("p7580_job: PTEWALK slot=%d seq=%u jc=0x%llx leaf_phys=0x%llx bo_phys=0x%llx CROSS-PAGE snap=0x%llx/0x%zx live=0x%llx/0x%zx\n",
		       slot, seq, (unsigned long long)jc,
		       (unsigned long long)leaf_phys,
		       (unsigned long long)bo_phys,
		       (unsigned long long)snap_iova, snap_size,
		       (unsigned long long)live_iova, live_size);
		if (live_obj)
			drm_gem_object_unreference_unlocked(live_obj);
		return;
	} else {
		void *kv;
		struct page *ppg =
			pfn_to_page((unsigned long)(leaf_phys >> PAGE_SHIFT));
		unsigned int jd_words =
			(PAGE_SIZE - page_off) >> 2;

		if (jd_words > 128)
			jd_words = 128;
		/* gpu31-cachedir: this is a kernel READ of BO bytes.
		 * Clean-then-invalidate the owner BO when the leaf really
		 * points at it, so the dump shows DRAM (CPU WC writes /
		 * GPU bytes) instead of a stale cached line and loses no
		 * dirty line. NO-OWNER/MISMATCH pages are not BO pages
		 * and are read as-is. */
		if (owner && bo_ok && leaf_phys == bo_phys)
			p7580_gem_invalidate_for_cpu(owner);
		kv = kmap(ppg);
		for (wi = 0; wi < jd_words; wi++)
			pw[wi] = ((u32 *)((char *)kv + page_off))[wi];
		for (; wi < 128; wi++)
			pw[wi] = 0;
		kunmap(ppg);
		have_bytes = 1;
		pr_err("p7580_job: JDBODY slot=%d seq=%u jc=0x%llx words=%u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       slot, seq, (unsigned long long)jc, jd_words,
		       pw[0], pw[1], pw[2], pw[3],
		       pw[4], pw[5], pw[6], pw[7],
		       pw[8], pw[9], pw[10], pw[11],
		       pw[12], pw[13], pw[14], pw[15]);
		pr_err("p7580_job: JDBODY slot=%d seq=%u jc=0x%llx +64 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       slot, seq, (unsigned long long)jc,
		       pw[16], pw[17], pw[18], pw[19],
		       pw[20], pw[21], pw[22], pw[23],
		       pw[24], pw[25], pw[26], pw[27],
		       pw[28], pw[29], pw[30], pw[31]);
		pr_err("p7580_job: JDBODY slot=%d seq=%u jc=0x%llx +128 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       slot, seq, (unsigned long long)jc,
		       pw[32], pw[33], pw[34], pw[35],
		       pw[36], pw[37], pw[38], pw[39],
		       pw[40], pw[41], pw[42], pw[43],
		       pw[44], pw[45], pw[46], pw[47]);
	}
	if (have_bytes && owner && owner_bo) {
		pr_err("p7580_job: PTEWALK slot=%d seq=%u jc=0x%llx leaf=0x%llx leaf_phys=0x%llx bo_phys=0x%llx %s snap=0x%llx/0x%zx live=0x%llx/0x%zx iova=0x%lx npages=%u w=%08x %08x %08x %08x %08x %08x %08x %08x sw=%08x %08x %08x %08x %08x %08x %08x %08x\n",
		       slot, seq, (unsigned long long)jc,
		       (unsigned long long)leaf_pte,
		       (unsigned long long)leaf_phys,
		       (unsigned long long)bo_phys,
		       (bo_ok && leaf_phys == bo_phys) ? "MATCH" : "MISMATCH",
		       (unsigned long long)snap_iova, snap_size,
		       (unsigned long long)live_iova, live_size,
		       owner_iova, owner_npages,
		       pw[0], pw[1], pw[2], pw[3],
		       pw[4], pw[5], pw[6], pw[7],
		       job->desc[0], job->desc[1], job->desc[2], job->desc[3],
		       job->desc[4], job->desc[5], job->desc[6], job->desc[7]);
	} else {
		pr_err("p7580_job: PTEWALK slot=%d seq=%u jc=0x%llx leaf=0x%llx leaf_phys=0x%llx NO-OWNER snap=0x%llx/0x%zx live=0x%llx/0x%zx w=%08x %08x %08x %08x %08x %08x %08x %08x sw=%08x %08x %08x %08x %08x %08x %08x %08x\n",
		       slot, seq, (unsigned long long)jc,
		       (unsigned long long)leaf_pte,
		       (unsigned long long)leaf_phys,
		       (unsigned long long)snap_iova, snap_size,
		       (unsigned long long)live_iova, live_size,
		       pw[0], pw[1], pw[2], pw[3],
		       pw[4], pw[5], pw[6], pw[7],
		       job->desc[0], job->desc[1], job->desc[2], job->desc[3],
		       job->desc[4], job->desc[5], job->desc[6], job->desc[7]);
	}
	if (live_obj)
		drm_gem_object_unreference_unlocked(live_obj);
}

static void p7580_drv_timedout_job(struct drm_sched_job *sched_job)
{
	struct p7580_submit_job *job = sched_job ? sched_job->owner : NULL;
	struct p7580_drv_file *df = job ?
		(struct p7580_drv_file *)job->file_priv : NULL;
	struct p7580_drv *drv = job ?
		(struct p7580_drv *)job->drv_priv : NULL;
	void *f;
	int pmraw;

	if (!drv && df)
		drv = df->drv;
	if (!drv)
		return;
	/* Timeout workqueue (process context): the recover path below
	 * re-touches COMMAND_NEXT/GPU_CMD/INT registers. The live-job
	 * ref keeps the block powered, but re-get here so a dark block
	 * (suspend racing the timeout) fails closed instead of hanging
	 * the bus.
	 *
	 * Close guarantee (gpu31-close-fix): the fence ALWAYS signals
	 * on this path, even when recovery is skipped dark. The sched
	 * close wait (entity_fini -> wait_idle) terminates only because
	 * retirement is unconditional: skipping the signal here would
	 * leave job_count elevated forever and wedge every close behind
	 * a pending job (the int7 CLOSE watchdog signature). Signalling
	 * is CPU-only (fence callbacks + completion), never MMIO, so it
	 * is safe dark. p7580_job_timedout already signals the job's own
	 * fence on the same terms after recovery; this covers the dark
	 * early-return so both exits retire. */
	pmraw = p7580_drv_pm_get(drv);
	pr_err("p7580_job: TMD-ENTER sched_job=%lu job=%lu drv=%lu pmraw=%d\n",
	       (unsigned long)sched_job, (unsigned long)job,
	       (unsigned long)drv, pmraw);
	if (pmraw < 0) {
		f = job ? job->done_fence : NULL;
		if (f && !dma_fence_is_signaled((struct dma_fence *)f))
			dma_fence_signal((struct dma_fence *)f);
		return;
	}
	/* gpu31-ptewalk: decisive fault-time read. Runs BEFORE recover
	 * tears state down (p7580_job_timedout -> recover rebinds AS0)
	 * in the same PM-held window: PT walk vs BO page comparison. */
	pr_err("p7580_job: TMD-PREWALK job=%lu drv=%lu\n",
	       (unsigned long)job, (unsigned long)drv);
	p7580_drv_ptewalk_pin(drv, job);
	p7580_job_timedout(&drv->jd, job);
	p7580_drv_pm_put(drv);
}

static void p7580_drv_free_job(struct drm_sched_job *sched_job)
{
	struct p7580_submit_job *job = sched_job ? sched_job->owner : NULL;
	struct p7580_drv_file *df = job ?
		(struct p7580_drv_file *)job->file_priv : NULL;
	struct p7580_drv *drv = job ?
		(struct p7580_drv *)job->drv_priv : NULL;
	uint32_t i;

	if (!drv && df)
		drv = df->drv;
	/* F-JT2 + gpu31-coherency: completion-side DMA sync. free_job
	 * runs on finish_work strictly after the fence signal (0004:
	 * cancel timeout, put fence, THEN free_job), so every
	 * referenced BO is post-GPU-write here. Walk the job's owned
	 * BO pointers (referenced at SUBMIT, stored on job->bos) and
	 * retire each via p7580_gem_job_done_sync
	 * (dma_sync_single_for_cpu per bound SHMEM page; CMA/imported
	 * and never-bound BOs no-op inside), then drop the job's refs.
	 * No MMIO, never sleeps, no locks: safe beside the PM release
	 * below. */
	if (job && job->bo_count && job->bos) {
		for (i = 0; i < job->bo_count; i++) {
			struct drm_gem_object *obj =
				(struct drm_gem_object *)job->bos[i];

			if (!obj)
				continue;
			p7580_gem_job_done_sync(obj);
			drm_gem_object_unreference_unlocked(obj);
			job->bos[i] = NULL;
		}
	}
	/* gpu31-chainsplit: one reference per queued node; the last node
	 * to retire unmaps the shared shadow descriptor (gpu_unmap also
	 * unlinks its bo_list entry). Done before the PM release below,
	 * same lifetime rule as the BO unrefs. */
	if (job && job->chain_shadow)
		p7580_drv_shadow_put(
			(struct p7580_chain_shadow *)job->chain_shadow);
	/* ST-RETIRE: finish_work reached free_job (fence already
	 * signalled by the 0004 retire path); proves the full
	 * queue -> dispatch -> retire chain completed. Numeric (%lu)
	 * so kptr_restrict cannot censor it. ran=N is the run_job
	 * entry count stamped on the job (order provable even if the
	 * ST-RUN print was lost); sslot= is the carried sched slot. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: retire seq=%u slot=%d sslot=%d ran=%u owner=%lu\n",
			job ? job->seqno : 0, job ? job->slot : -1,
			sched_job ? sched_job->slot : -1,
			job ? job->run_seq : 0, (unsigned long)job);
	}
	p7580_job_free(job);
	/* Balances p7580_drv_job_pm_arm() at queue time: the async
	 * leg is over (retired or run_job dispatch failure), drop
	 * the job-lifetime power hold so the block can idle-suspend.
	 * Completion-IRQ safety: the IRQ handler only reads STAT +
	 * CLEARs + signals the fence (finish path) — it runs strictly
	 * before fence signal, and free_job runs strictly after
	 * (finish_work: cancel timeout, put fence, THEN free_job), so
	 * no IRQ demux MMIO can run after this put suspends the block.
	 * Timeout/recover MMIO likewise precedes free_job (timeout
	 * work calls timedout_job, then the fence signals, then
	 * finish_work calls free_job). df->drv is stable here:
	 * file_fini (entity teardown) waits for queued jobs via the
	 * 0004 fini/flush path before the file state (and its drv
	 * back-pointer) is freed. */
	if (drv)
		p7580_drv_job_pm_release(drv);
}

/* Arm-time fence allocator (0004 alloc_fence hook): one fence per
 * SUBMIT, allocated before queueing so out_sync is live at ioctl
 * return. Context: per-slot ordered timeline; seqno: per-slot emit
 * counter shared with the jobs slice. dma_fence_init + NOT signaled
 * (completion comes from the JOB_INT IRQ or the timeout path). */
static struct dma_fence *p7580_drv_alloc_fence(struct drm_sched_job *sched_job)
{
	struct p7580_submit_job *job = sched_job ? sched_job->owner : NULL;
	struct p7580_drv *drv;

	if (!sched_job || !job)
		return NULL;
	if (job->slot < 0 || job->slot >= P7580_NUM_JOB_SLOTS)
		return NULL;
	/* Dispatch context was stamped at SUBMIT (file_priv/drv_priv);
	 * same pattern as run_job/timedout_job/free_job. */
	drv = (struct p7580_drv *)job->drv_priv;
	if (!drv)
		return NULL;
	return p7580_job_alloc_fence(&drv->jd, job->slot,
				     &drv->jd.emit_seqno[job->slot]);
}

static const struct drm_sched_backend_ops p7580_drv_sched_ops = {
	.run_job = p7580_drv_run_job,
	.alloc_fence = p7580_drv_alloc_fence,
	.timedout_job = p7580_drv_timedout_job,
	.free_job = p7580_drv_free_job,
};

/* Probenumeric: strong run_job address for the shim ST-OPS cross-check.
 * The 0004 TU declares p7580_drv_run_job_addr() (weak default -> NULL);
 * this strong definition wins the link and returns our run_job, so
 * silicon compares ST-OPS run= against runexp= with no symbol lookup. */
drm_sched_run_fn p7580_drv_run_job_addr(void)
{
	return p7580_drv_run_job;
}

/* Opsfix: the 0004 shim header is the single authoritative struct
 * definition (both TUs include <drm_sched.h> via -Idrivers/gpu/drm).
 * Silicon ST-OPS showed ops=NULL despite a correct store + correct call
 * site; if either TU ever drifts, the build breaks here instead of
 * retiring the job silently. Values (arm64 3.10): ops=24 slot=232 ran=236
 * (see docs/reports/gpu31-opsfix). File-scope pin: a noinline probe
 * the linker keeps, so /proc/kallsyms + ST-OPS dmesg can be compared
 * against these exact numbers. */
static void __maybe_unused p7580_drv_sched_layout_pin(void)
{
	BUILD_BUG_ON(offsetof(struct drm_sched_job, ops) != 24);
	BUILD_BUG_ON(offsetof(struct drm_sched_job, slot) != 232);
	BUILD_BUG_ON(offsetof(struct drm_sched_job, ran) != 236);
}

/* Power guard (F-SUB1). Every MMIO site in this file runs under a
 * p7580_drv_pm_get/put pair; the get fails closed (caller must touch
 * no register afterwards). pm_runtime_get_sync may return 1 (already
 * active) or 0 (resumed now): both mean powered, and this helper
 * normalises both to 0. Only a negative errno means dark, and only
 * then does the caller take the dark path (all callers test
 * `< 0`, never truthiness, so 1 can never take the dark early-return).
 * Returns 0 powered, negative errno dark. */
int p7580_drv_pm_get(struct p7580_drv *drv)
{
	int ret;

	if (!drv || !drv->dev)
		return -ENODEV;
	ret = pm_runtime_get_sync(drv->dev);
	if (ret < 0) {
		/* Resume failed (clock/reset/cores): the GPU is dark;
		 * programming HEAD/START now would wedge the slot with
		 * no IRQ ever coming (unbounded fence wait). put_noidle
		 * balances the failed get without re-entering resume. */
		pm_runtime_put_noidle(drv->dev);
		return ret;
	}
	/* Normalise pm_runtime_get_sync's 1 (already active) to 0:
	 * 0 and 1 both mean powered. */
	return 0;
}

void p7580_drv_pm_put(struct p7580_drv *drv)
{
	if (!drv || !drv->dev)
		return;
	pm_runtime_mark_last_busy(drv->dev);
	pm_runtime_put_autosuspend(drv->dev);
}
/* Live-job ref: armed at queue time, released in free_job. Paired
 * get/put around the whole async job lifetime (queue -> run ->
 * retire, including the timeout/recover path which re-touches
 * COMMAND_NEXT/RESET registers). atomic_t pm_ref counts them so a
 * leaked (never-freed) job is visible in sysfs/debug rather than
 * silently pinning the power domain on. */
static void p7580_drv_job_pm_arm(struct p7580_drv *drv)
{
	atomic_inc(&drv->pm_ref);
	/* Second get: first is the ioctl's synchronous leg. */
	(void)p7580_drv_pm_get(drv);
}

static void p7580_drv_job_pm_release(struct p7580_drv *drv)
{
	atomic_dec(&drv->pm_ref);
	p7580_drv_pm_put(drv);
}

/* Teardown hold (F-SUB1 teardown leg): one PM ref across a whole
 * unmap/invalidate/mask walk so the AS block cannot suspend mid-walk.
 * Returns 1 if power is held (MMIO allowed), 0 if the block is dark
 * (caller must skip MMIO but still free CPU-side table/memory). Puts
 * are balanced 1:1 via teardown_release; never balances another
 * leg's ref. */
/* gpu31-iova: register the map/unmap trace budget with the jobs
 * slice so the iova_arm debugfs knob (jobs debugfs_init) re-arms it.
 * Called once at probe; NULL at remove releases it. */
void p7580_drv_register_iova_trace(unsigned int *budget)
{
	p7580_job_register_iova_budget(budget);
}

int p7580_drv_teardown_hold(struct p7580_drv *drv)
{
	return p7580_drv_pm_get(drv) == 0;
}

void p7580_drv_teardown_release(struct p7580_drv *drv)
{
	p7580_drv_pm_put(drv);
}

/* Unmap one BO's IOVA range. Table writes (p7580_mmu_unmap) are
 * CPU-side dma_sync + kmap and safe while dark; only the TLB
 * invalidate is MMIO. When invalidate == 0 (dark block) the caller
 * skips the TLB flush: stale entries die with the context teardown
 * (AS unbound + root freed at remove; idle suspend preserves no GPU
 * state that a later resume could misread because resume always
 * re-programs TRANSTAB + invalidates before the next job). */
int p7580_drv_unmap_bo(struct p7580_drv *drv, struct drm_gem_object *obj,
		       int invalidate)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	unsigned int npages = p7580_bo_npages(obj);
	unsigned int i;

	for (i = 0; i < npages; i++)
		p7580_mmu_unmap(&drv->mmu, bo->iova +
				((u64)i << PAGE_SHIFT));
	if (invalidate)
		p7580_mmu_invalidate(drv->regs, P7580_DRV_AS_NR,
				     bo->iova >> PAGE_SHIFT, npages, 1);
	/* gpu31-coherency: tear down the bind-time DMA mapping now the
	 * PTEs no longer reference the pages. CPU-side only, safe dark. */
	p7580_gem_dma_unbind(obj);
	return 0;
}

/* gpu31-iova: free-time GPU unmap (called from GEM free BEFORE the
 * IOVA range is recycled). Clears the BO's leaf PTEs, flushes the TLB
 * for the exact range under a PM-held window, unlinks the bo_list
 * entry (gpu31-boleak: a weak, non-owning entry -- unlinking drops no
 * reference and is what lets the object finish dying), then tears
 * down the DMA mapping. Order: leaves + TLB flush happen while the
 * IOVA node is still linked, so the range is unmapped everywhere
 * before iova_free reuses it. Never-bound BOs (no bo_list entry)
 * no-op after the DMA unbind. Dark block: leaves still cleared +
 * list/DMA torn down CPU-side; the TLB flush is skipped and re-covered
 * by resume (TRANSTAB + invalidate before the next job). Runs under
 * dev->struct_mutex from gem_free_object: takes bo_lock (leaf; same
 * order as map_bo) and at most one PM get/put; never sleeps beyond
 * those. */
void p7580_gem_gpu_unmap(struct drm_gem_object *obj)
{
	struct drm_device *ddev;
	struct p7580_drv *drv;
	struct p7580_bo *bo;
	unsigned long iova = 0;
	unsigned int npages = 0;
	int found = 0;
	int pm_held = 0;

	if (!obj)
		return;
	bo = to_p7580_bo(obj);
	ddev = obj->dev;
	if (!ddev)
		goto out_dma;
	drv = ddev->dev_private ? ddev->dev_private : NULL;
	if (!drv)
		goto out_dma;
	/* Snapshot the range under bo_lock; unlink the entry now so a
	 * concurrent submit re-mapping the same object re-adds cleanly
	 * (map_bo re-checks under the same lock). The entry owns no
	 * reference (gpu31-boleak), so unlinking drops nothing: this is
	 * the unlink the destructor performs, not a put. */
	mutex_lock(&drv->bo_lock);
	{
		struct p7580_drv_bo *e, *tmp;

		list_for_each_entry_safe(e, tmp, &drv->bo_list, node) {
			if (e->obj != obj)
				continue;
			iova = (unsigned long)bo->iova;
			npages = p7580_bo_npages(obj);
			list_del(&e->node);
			mutex_unlock(&drv->bo_lock);
			kfree(e);
			found = 1;
			goto out_unmap;
		}
	}
	mutex_unlock(&drv->bo_lock);
out_dma:
	/* Never bound (or driver gone): only the DMA mapping can exist. */
	p7580_gem_dma_unbind(obj);
	return;
out_unmap:
	if (!found || !npages)
		goto out_dma;
	/* Leaf-clear is CPU-side (dma_sync + kmap) and PM-safe. */
	{
		unsigned int i;

		for (i = 0; i < npages; i++)
			p7580_mmu_unmap(&drv->mmu, iova +
					((u64)i << PAGE_SHIFT));
	}
	/* TLB invalidate needs the AS block powered; hold one PM ref
	 * for the window (fail-closed dark: resume re-programs +
	 * invalidates before the next job, same contract as the
	 * remove-walk dark leg). wmb() before the invalidate is inside
	 * p7580_mmu_invalidate (LOCKADDR latch barrier, F-MMU2). */
	pm_held = p7580_drv_teardown_hold(drv);
	if (pm_held)
		p7580_mmu_invalidate(drv->regs, P7580_DRV_AS_NR,
				     iova >> PAGE_SHIFT, npages, 1);
	if (pm_held)
		p7580_drv_teardown_release(drv);
	p7580_gem_dma_unbind(obj);
	/* gpu31-iova trace: one line per free-time unmap (never-bound
	 * BOs return via out_dma and print nothing). */
	if (p7580_iova_trace) {
		p7580_iova_trace--;
		pr_info("p7580_iova: unmap obj=%lu iova=0x%lx npages=%u dark=%d\n",
			(unsigned long)obj, iova, npages, !pm_held);
	}
}



int p7580_drv_sched_ensure(void)
{
	int ret = 0;

	mutex_lock(&p7580_drv_sched_lock);
	if (!p7580_drv_sched_live) {
		ret = drm_sched_init(&p7580_drv_sched,
				     &p7580_drv_sched_ops,
				     msecs_to_jiffies(p7580_drv_job_timeout_ms),
				     "panfrost7580");
		if (!ret)
			p7580_drv_sched_live = 1;
	}
	mutex_unlock(&p7580_drv_sched_lock);
	return ret;
}

/* gpu31-watchdog: job_timeout_ms get/set (jobs slice debugfs hook).
 * The value is the drm_sched dispatch budget: it is copied into
 * entity->timeout at entity_init (per open), so a write takes effect
 * for the next client; the live sched->timeout is updated too. Clamp
 * to 100..60000 ms (below the poll granularity this part can resolve,
 * above the 5 s the port shipped with). */
unsigned int p7580_drv_get_job_timeout_ms(void *priv)
{
	(void)priv;
	return p7580_drv_job_timeout_ms;
}

int p7580_drv_set_job_timeout_ms(void *priv, unsigned int ms)
{
	(void)priv;
	if (ms < 100u || ms > 60000u)
		return -EINVAL;
	mutex_lock(&p7580_drv_sched_lock);
	p7580_drv_job_timeout_ms = ms;
	if (p7580_drv_sched_live)
		p7580_drv_sched.timeout = msecs_to_jiffies(ms);
	mutex_unlock(&p7580_drv_sched_lock);
	pr_info("p7580_sched: job_timeout_ms=%u (new entities)\n", ms);
	return 0;
}

/* Per-file entities on the shared scheduler workqueue. */
struct p7580_shadow_pin {
	struct list_head node;
	struct drm_gem_object *obj;
};

int p7580_drv_file_init(struct p7580_drv_file *df, struct p7580_drv *drv)
{
	int i, ret;

	if (!df || !drv)
		return -EINVAL;
	/* R1: the shared sched workqueue does not exist until the first
	 * ensure. drm_sched_entity_init takes sched->lock, so running it
	 * against a never-initialised (zeroed) sched wedges the opener.
	 * Ensure here so open stays O(1) with no waits on the fast path
	 * (ensure only inits once; later opens hit the live flag). */
	ret = p7580_drv_sched_ensure();
	if (ret)
		return ret;
	df->drv = drv;
	df->next_seqno = 0;
	df->entities_live = 0;
	INIT_LIST_HEAD(&df->shadow_pins);
	df->shadow_pin_count = 0;
	mutex_init(&df->serial_lock);
	df->serial_prev = NULL;
	drm_syncobj_manager_init(&df->mgr);
	df->job_fctx = NULL;
	df->job_dev = &drv->jd;
	for (i = 0; i < P7580_NUM_JOB_SLOTS; i++) {
		ret = drm_sched_entity_init(&df->entity[i],
					    &p7580_drv_sched);
		if (ret) {
			while (--i >= 0)
				drm_sched_entity_fini(&p7580_drv_sched,
						      &df->entity[i]);
			drm_syncobj_manager_fini(&df->mgr);
			return ret;
		}
	}
	df->entities_live = 1;
	return 0;
}

void p7580_drv_file_fini(struct p7580_drv_file *df)
{
	struct p7580_drv *drv;
	int pm_held = 0;
	int i;

	if (!df)
		return;
	drv = df->drv;
	/* Teardown can run while runtime-suspended (close after idle):
	 * entity fini retires queued jobs whose free_job drops the
	 * live-job PM ref, and the 0004 fini/flush path runs the
	 * retirement workqueue. Hold one PM ref across the walk so a
	 * concurrent run_job/timeout worker (still covered by its own
	 * re-get) and the fini flush both see a powered block. The
	 * free_job puts below are per-job refs, NOT this hold: this
	 * hold is released once, after the last entity is torn down.
	 * On get failure the block is already dark: MMIO inside fini
	 * (there is none directly) is skipped by the workers'
	 * fail-closed arms, and the CPU-side list flush below still
	 * runs so no job/fence leaks. */
	/* Bounded close (gpu31-close-fix): the hold below is a single
	 * pm_runtime_get_sync. Its only unbounded phase is waiting out a
	 * concurrent suspend/resume transition, which is itself bounded
	 * (runtime_suspend/resume callbacks poll READY with 100 us steps
	 * and 100 ms caps; no step sleeps unbounded). The entity loop
	 * then cancels queued jobs synchronously and waits BOUNDED for
	 * dispatched jobs (0004 wait_idle: one timeout + 8 s margin),
	 * whose retirement is unconditional (timedout_job always signals
	 * the fence, even dark). Syncobj fini last: it drops manager refs
	 * without waiting on any fence, so no waiter can be wedged by
	 * teardown order. Double fini is safe (entities_live guards the
	 * loop, manager drain of an empty idr is a no-op). */
	if (drv && p7580_drv_teardown_hold(drv))
		pm_held = 1;
	if (df->entities_live) {
		for (i = 0; i < P7580_NUM_JOB_SLOTS; i++)
			drm_sched_entity_fini(&p7580_drv_sched,
					      &df->entity[i]);
		df->entities_live = 0;
	}
	/* gpu31-shadowpin: all queued jobs are drained above, so nothing
	 * can still read a shadow chain; drop the lifetime pins. */
	{
		struct p7580_shadow_pin *pin, *tmp;
		unsigned int n = 0;

		list_for_each_entry_safe(pin, tmp, &df->shadow_pins, node) {
			list_del(&pin->node);
			drm_gem_object_unreference_unlocked(pin->obj);
			kfree(pin);
			n++;
		}
		df->shadow_pin_count = 0;
		if (n)
			pr_info("p7580 shadow unpin n=%u\n", n);
	}
	/* Drop the serial-chain reference (if any): no new dispatch can
	 * attach it after the entities above drained, and the job that
	 * armed it still owns its own ref via out_sync/retire. */
	mutex_lock(&df->serial_lock);
	if (df->serial_prev) {
		dma_fence_put(df->serial_prev);
		df->serial_prev = NULL;
	}
	mutex_unlock(&df->serial_lock);
	drm_syncobj_manager_fini(&df->mgr);
	if (pm_held)
		p7580_drv_teardown_release(drv);
	df->drv = NULL;
	df->job_dev = NULL;
}

/* gpu31-firstfault: dump one 4 KiB GPU page (all 16 256-byte job
 * slots) of the memory a job descriptor, framebuffer descriptor or
 * tiler-heap header lives in. Registered on the jobs slice as the
 * first-fault dumper (process context only: it resolves the IOVA to a
 * BO, takes a ref and kmaps; the caller is the sched timeout work).
 * The read is clean-then-invalidate (p7580_drv_gpu_read), so the dump
 * shows what the GPU sees, not a stale CPU line. */
#define P7580_PAGE_DUMP_LEN	4096u

/* Defined below with the chain-split helpers. */
static int p7580_drv_gpu_read(struct p7580_drv *drv, uint64_t gpu,
			      void *dst, size_t len);

int p7580_drv_desc_dump(void *priv, uint64_t gpu, const char *tag)
{
	struct p7580_drv *drv = priv;
	uint32_t *buf;
	uint64_t base;
	unsigned int i;
	int ret;

	if (!drv || !tag)
		return -EINVAL;
	base = gpu & ~(uint64_t)(P7580_PAGE_DUMP_LEN - 1u);
	buf = kmalloc(P7580_PAGE_DUMP_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = p7580_drv_gpu_read(drv, base, buf, P7580_PAGE_DUMP_LEN);
	if (ret) {
		pr_err("p7580_job: PAGEDUMP %s gpu=0x%llx rc=%d (unmapped)\n",
		       tag, (unsigned long long)gpu, ret);
		kfree(buf);
		return ret;
	}
	pr_err("p7580_job: PAGEDUMP %s gpu=0x%llx page=0x%llx off=0x%llx\n",
	       tag, (unsigned long long)gpu, (unsigned long long)base,
	       (unsigned long long)(gpu - base));
	/* One line per 16 words (64 bytes): 64 lines cover the whole
	 * 4 KiB page. The first form printed only the first 64 bytes of
	 * every 256-byte job slot, i.e. 1 KiB with wrong offsets, which
	 * skipped exactly the interesting bytes on int42: the fragment
	 * SFBD tiler section at page+0x540 and the SFBD-adjacent
	 * descriptor at +0x5c0. slot is the 256-byte job slot the line
	 * starts in, off the page offset. */
	for (i = 0; i < P7580_PAGE_DUMP_LEN / 4u / 16u; i++) {
		const uint32_t *q = buf + i * 16u;

		pr_err("p7580_job: PAGE %s slot%02u off=%03x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		       tag, (unsigned int)(i / 4u), (unsigned int)(i * 64u),
		       q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7],
		       q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
	}
	kfree(buf);
	return 0;
}

/* gpu31-iova: IRQ-safe fault-time mapping resolver (registered on
 * struct p7580_job_dev at probe). Renders the faulting job's SUBMIT-
 * time {handle,iova,size} snapshot as "h<H>=0x<IOVA>/0x<SZ> ..." into
 * buf (or counts when buf is NULL, snprintf-style). IRQ context: only
 * the snapshot arrays + snprintf, no GEM dereference, no locks, no
 * MMIO. Truncates at the buffer end (always NUL-terminated); returns
 * the full untruncated length so the caller can size its buffer. */
int p7580_drv_iova_resolve(void *priv, char *buf, size_t len,
				  const uint32_t *handles,
				  const uint64_t *addrs,
				  const uint32_t *sizes, uint32_t n)
{
	(void)priv;
	if (!handles || !addrs || !sizes || !n)
		return 0;
	{
		size_t off = 0;
		uint32_t i;
		int w;

		for (i = 0; i < n; i++) {
			if (!buf || off >= len)
				w = snprintf(NULL, 0, "%s%u=0x%llx/0x%x",
					       i ? " " : "h",
					       handles[i],
					       (unsigned long long)addrs[i],
					       sizes[i]);
			else
				w = snprintf(buf + off, len - off,
					     "%s%u=0x%llx/0x%x",
					     i ? " " : "h",
					     handles[i],
					     (unsigned long long)addrs[i],
					     sizes[i]);
			if (w < 0)
				return (int)off;
			off += (size_t)w;
			if (buf && off >= len) {
				buf[len - 1] = '\0';
				break;
			}
		}
		return (int)off;
	}
}

/* Map one BO's pages into AS 0 if not yet bound; track on bo_list.
 * Caller (p7580_drv_submit) holds a runtime-PM ref: the invalidate()
 * below is MMIO on the AS block and MUST NOT run while the power
 * domain/clocks are dark (F-SUB1 bus-hang). */
static int p7580_drv_map_bo(struct p7580_drv *drv, struct drm_gem_object *obj)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	unsigned int npages = p7580_bo_npages(obj);
	unsigned int i;
	int ret = 0;

	mutex_lock(&drv->bo_lock);
	{
		struct p7580_drv_bo *e;

		list_for_each_entry(e, &drv->bo_list, node) {
			if (e->obj == obj) {
				mutex_unlock(&drv->bo_lock);
				return 0;
			}
			/* gpu31-iova: live-overlap detector. Two distinct
			 * live BOs must never cover the same IOVA: that
			 * means an IOVA was recycled while still mapped
			 * (the stale-TLB signature). Always-on WARN. */
			if (bo->iova < e->iova + e->size &&
			    e->iova < bo->iova + bo->base.size)
				pr_warn("p7580_iova: OVERLAP obj=%lu iova=0x%llx size=0x%zx vs obj=%lu iova=0x%llx size=0x%zx\n",
					(unsigned long)obj,
					(unsigned long long)bo->iova,
					bo->base.size,
					(unsigned long)e->obj,
					(unsigned long long)e->iova,
					e->size);
		}
	}
	mutex_unlock(&drv->bo_lock);

	/* gpu31-coherency: bind-time dma_map before the AS0 walk so PTEs
	 * carry bus addresses (p7580_bo_page_phys routes SHMEM through
	 * the dma_addrs cache) and completion sync has a mapping. */
	{
		int bret = p7580_gem_dma_bind(obj, drv->dev);

		if (bret)
			return bret;
	}

	/* gpu31-iova: IOVA-reuse fence. A recycled range can only reach
	 * here after p7580_gem_gpu_unmap cleared its leaves on free;
	 * still, invalidate the range BEFORE writing new leaves so any
	 * TLB entry surviving a dark free (flush skipped, resume pending)
	 * or a racing PTW is purged first. Barrier order: invalidate
	 * (purge old) -> map (write new) -> invalidate (publish new),
	 * matching the kbase LOCK/FLUSH/UPDATE discipline. */
	p7580_mmu_invalidate(drv->regs, P7580_DRV_AS_NR,
			     bo->iova >> PAGE_SHIFT, npages, 1);
	for (i = 0; i < npages; i++) {
		int ok = 0;
		phys_addr_t phys = p7580_bo_page_phys(obj, i, &ok);

		if (!ok) {
			ret = -EFAULT;
			goto rollback;
		}
		ret = p7580_mmu_map(&drv->mmu, bo->iova +
				    ((u64)i << PAGE_SHIFT),
				    phys, P7580_PROT_RD | P7580_PROT_WR,
				    P7580_MEMATTR_IDX_DEFAULT);
		if (ret)
			goto rollback;
	}
	p7580_mmu_invalidate(drv->regs, P7580_DRV_AS_NR,
			     bo->iova >> PAGE_SHIFT, npages, 0);

	{
		struct p7580_drv_bo *e = kzalloc(sizeof(*e), GFP_KERNEL);

		if (!e) {
			ret = -ENOMEM;
			goto rollback;
		}
		mutex_lock(&drv->bo_lock);
		/* gpu31-boleak: NON-OWNING entry -- deliberately no
		 * drm_gem_object_reference() here. The only unlink of
		 * this entry lives in p7580_gem_gpu_unmap(), which runs
		 * from p7580_gem_free_object() i.e. only once the
		 * refcount is zero; a reference owned by the entry would
		 * therefore never be dropped and every submitted BO
		 * (pages + shmem inode + IOVA) would leak forever.
		 * The object is guaranteed live here: the caller holds a
		 * handle-lookup reference (SUBMIT) or the shadow's own
		 * per-node refs. */
		e->obj = obj;
		e->iova = bo->iova;
		e->size = bo->base.size;
		list_add_tail(&e->node, &drv->bo_list);
		mutex_unlock(&drv->bo_lock);
	}
	/* gpu31-iova trace: one line per fresh map (already-bound BOs
	 * return early above and print nothing). Own budget so the
	 * 8-line job budget is never starved. */
	if (p7580_iova_trace) {
		p7580_iova_trace--;
		pr_info("p7580_iova: map obj=%lu iova=0x%llx size=0x%zx npages=%u\n",
			(unsigned long)obj, (unsigned long long)bo->iova,
			bo->base.size, npages);
	}
	return 0;

rollback:
	/* gpu31-boleak: this bind failed after programming leaves, and no
	 * bo_list entry exists for the object yet -- so nothing would ever
	 * unmap them (the free-time unmap keys on the entry). Clear what
	 * this call programmed and publish the clear, keeping "leaves
	 * programmed" exactly equivalent to "entry linked"; otherwise a
	 * freed BO could leave PTEs naming recycled pages. i is the index
	 * that failed (not yet mapped), or npages when only the entry
	 * allocation failed. */
	{
		unsigned int k = i;

		while (k--)
			p7580_mmu_unmap(&drv->mmu, bo->iova +
					((u64)k << PAGE_SHIFT));
	}
	p7580_mmu_invalidate(drv->regs, P7580_DRV_AS_NR,
			     bo->iova >> PAGE_SHIFT, npages, 0);
	return ret;
}

/* ------------------------------------------------------------------ */
/* gpu31-chainsplit: chain walk, shadow descriptors, per-node queue.  */
/*                                                                    */
/* The UAPI hands the driver one jc = chain head. A chained dispatch   */
/* runs to the last next pointer before the slot reports DONE, so the  */
/* chain's END_FLUSH_CLEAN_INV happens once, after the tiler; the      */
/* vertex -> tiler handoff is then not visible (gpu31-chainexec). The  */
/* driver splits the chain: every node is queued as its own sched job  */
/* on the SAME slot entity, so each START carries the standard         */
/* p7580_js_config() (START/END_FLUSH_CLEAN_INV) and the nodes are     */
/* ordered by a drm_sched dependency on the previous node's armed      */
/* fence. One submitted chain therefore becomes N sched jobs and one   */
/* out_sync fence: the LAST node's fence covers the whole split.       */
/*                                                                    */
/* Each node is dispatched from a driver-owned shadow copy with        */
/*   - next_job = NULL  (the JM must stop after this node),            */
/*   - dep1/dep2 = 0    (ordering is enforced by drm_sched now; the    */
/*                       original indices name nodes that are not in   */
/*                       this dispatch),                               */
/*   - index = position + 1 (well-formed standalone chain).            */
/* The 32-byte header plus the inline payload is copied; every payload */
/* field is an absolute GPU address, so the copy is relocatable.       */
/* ------------------------------------------------------------------ */

/* One shadow BO per submitted chain; every split job holds one object */
/* reference and the last release unmaps it. iova is the GPU base. */
struct p7580_chain_shadow {
	struct drm_gem_object *obj;
	uint64_t iova;
	atomic_t refs;
};

/* Locate the live BO covering [gpu, gpu+len) and take a reference.
 * The bo_list entry is weak (gpu31-boleak), so the reference is taken
 * with kref_get_unless_zero(): an entry whose object is already in
 * p7580_gem_free_object() is skipped rather than resurrected. The
 * caller's own submitted BOs are still referenced (job lookup refs),
 * so the lookup for a live descriptor always succeeds. */
static int p7580_drv_gpu_span(struct p7580_drv *drv, uint64_t gpu,
			      size_t len, struct drm_gem_object **out,
			      uint64_t *base)
{
	struct p7580_drv_bo *e;
	int found = 0;

	if (!drv || !out || !base || !len)
		return -EINVAL;
	mutex_lock(&drv->bo_lock);
	list_for_each_entry(e, &drv->bo_list, node) {
		if (e->size < len || gpu < e->iova)
			continue;
		if (gpu - e->iova > e->size - len)
			continue;
		if (!p7580_drv_gem_try_get(e->obj))
			continue;
		*out = e->obj;
		*base = e->iova;
		found = 1;
		break;
	}
	mutex_unlock(&drv->bo_lock);
	return found ? 0 : -ENOENT;
}

/* Read len bytes of (already bound) GPU memory at gpu into dst. The   */
/* read is clean-then-invalidate like the descriptor snapshot: the CPU */
/* must not observe a stale line the GPU has already overwritten.      */
static int p7580_drv_gpu_read(struct p7580_drv *drv, uint64_t gpu,
			      void *dst, size_t len)
{
	struct drm_gem_object *obj;
	struct p7580_bo *bo;
	uint64_t base = 0;
	size_t done = 0;
	int ret;

	ret = p7580_drv_gpu_span(drv, gpu, len, &obj, &base);
	if (ret)
		return ret;
	bo = to_p7580_bo(obj);
	p7580_gem_invalidate_for_cpu(obj);
	while (done < len) {
		uint64_t a = gpu + done;
		unsigned int idx = (unsigned int)((a - base) >> PAGE_SHIFT);
		unsigned int off = (unsigned int)(a & (PAGE_SIZE - 1));
		size_t n = len - done;
		void *kv;

		if (n > PAGE_SIZE - off)
			n = PAGE_SIZE - off;
		if (idx >= bo->nr_pages || !bo->pages[idx]) {
			ret = -EFAULT;
			break;
		}
		kv = kmap_atomic(bo->pages[idx]);
		memcpy((char *)dst + done, (char *)kv + off, n);
		kunmap_atomic(kv);
		done += n;
	}
	drm_gem_object_unreference_unlocked(obj);
	return ret;
}

/* Write len bytes into the freshly created shadow BO (SHMEM pages).   */
static int p7580_drv_shadow_write(struct drm_gem_object *obj, size_t off,
				  const void *src, size_t len)
{
	struct p7580_bo *bo = to_p7580_bo(obj);
	size_t done = 0;

	while (done < len) {
		size_t a = off + done;
		unsigned int idx = (unsigned int)(a >> PAGE_SHIFT);
		unsigned int poff = (unsigned int)(a & (PAGE_SIZE - 1));
		size_t n = len - done;
		void *kv;

		if (n > PAGE_SIZE - poff)
			n = PAGE_SIZE - poff;
		if (idx >= bo->nr_pages || !bo->pages[idx])
			return -EFAULT;
		kv = kmap_atomic(bo->pages[idx]);
		memcpy((char *)kv + poff, (const char *)src + done, n);
		kunmap_atomic(kv);
		done += n;
	}
	return 0;
}

/* Allocate + map the shadow BO holding `nodes` slots. The returned    */
/* object carries exactly `nodes` references (one per split job); the  */
/* create-time reference is job 0's, the rest are taken here.          */
static struct p7580_chain_shadow *p7580_drv_shadow_create(
		struct p7580_drv *drv, struct drm_device *dev,
		struct p7580_drv_file *df, unsigned int nodes)
{
	struct p7580_chain_shadow *sh;
	struct drm_gem_object *obj = NULL;
	size_t size;
	unsigned int i;
	int ret;

	if (!nodes || nodes > P7580_CHAIN_MAX_JOBS)
		return ERR_PTR(-EINVAL);
	size = (size_t)nodes * P7580_CHAIN_SLOT_STRIDE;
	sh = kzalloc(sizeof(*sh), GFP_KERNEL);
	if (!sh)
		return ERR_PTR(-ENOMEM);
	ret = p7580_gem_create(&drv->gd, dev, size, P7580_BO_SHMEM, 0,
			       &obj);
	if (ret) {
		kfree(sh);
		return ERR_PTR(ret);
	}
	for (i = 1; i < nodes; i++)
		drm_gem_object_reference(obj);
	sh->obj = obj;
	sh->iova = to_p7580_bo(obj)->iova;
	atomic_set(&sh->refs, nodes);
	ret = p7580_drv_map_bo(drv, obj);
	if (ret) {
		for (i = 0; i < nodes; i++)
			drm_gem_object_unreference_unlocked(obj);
		kfree(sh);
		return ERR_PTR(ret);
	}
	if (df) {
		struct p7580_shadow_pin *pin = kzalloc(sizeof(*pin), GFP_KERNEL);

		if (pin) {
			drm_gem_object_reference(obj);
			pin->obj = obj;
			list_add_tail(&pin->node, &df->shadow_pins);
			df->shadow_pin_count++;
			pr_info("p7580 shadow pin obj=%p iova=0x%llx refs=%d\n",
				obj, (unsigned long long)sh->iova,
				atomic_read(&sh->refs));
		}
	}
	return sh;
}

/* Drop one split job's reference; the last one unmaps the shadow BO   */
/* before dropping its final object reference (gpu_unmap unlinks the   */
/* bo_list entry and invalidates the TLB range).                      */
static void p7580_drv_shadow_put(struct p7580_chain_shadow *sh)
{
	int last;

	if (!sh)
		return;
	last = atomic_dec_and_test(&sh->refs);
	/* Do NOT unmap here.  The shadow BO lives outside the file's idr, so
	 * map_file_bos cannot re-establish its leaves; unmapping it when the
	 * last queued node is freed leaves the GPU following a stale chain
	 * pointer.  The mapping is torn down by p7580_gem_free_object() when
	 * the GEM object itself dies. */
	drm_gem_object_unreference_unlocked(sh->obj);
	if (last)
		kfree(sh);
}

/* Shared context for the per-node build/queue helper. */
struct p7580_split_ctx {
	struct p7580_drv *drv;
	struct drm_device *dev;
	struct p7580_drv_file *df;
	struct drm_sched_entity *entity;
	int slot;
	uint32_t requirements;
	uint32_t out_sync;
	struct drm_gem_object **objs;
	uint32_t *hsave;
	unsigned int nr;
	struct dma_fence **inf;
	unsigned int nf;
	struct p7580_chain_shadow *shadow;
	unsigned int nodes;
	/* gpu31-firstfault: the live (Mesa) chain-node addresses the
	 * shadows were copied from, so the fault-time page dump can show
	 * both pages. NULL when no shadow was allocated (the dispatched
	 * jc is the original). */
	const uint64_t *src;
	/* gpu31-storm58: per-node SFBD address (descriptor word 10,
	 * fragment nodes only; 0 elsewhere) and the per-BO age of each
	 * entry in objs[] (SUBMITs since this file last used that BO). */
	const uint32_t *fbd;
	const uint32_t *age;
};

/* gpu31-storm58: BO-age ledger (diagnostic). Returns how many SUBMITs
 * this file made since it last referenced the page at `iova`, then
 * records `seq` as the newest user. Direct-mapped and bounded; a
 * collision restarts the age instead of chaining. */
static uint32_t p7580_drv_bo_age_touch(struct p7580_drv_file *df,
				       uint64_t iova, uint32_t seq)
{
	unsigned int s = (unsigned int)((iova >> 12) &
					(P7580_BO_AGE_SLOTS - 1u));
	uint32_t age = 0;

	if (df->bo_age[s].iova == iova && df->bo_age[s].seq)
		age = seq - df->bo_age[s].seq;
	df->bo_age[s].iova = iova;
	df->bo_age[s].seq = seq;
	return age;
}

/* Build one sched job for chain node `pos` and queue it on the slot    */
/* entity. prev is the fence this node must wait for (pos 0: the serial */
/* predecessor; pos > 0: the previous split node's armed fence).        */
/* External in-fences and BO exclusion fences attach to node 0 only:    */
/* the split's own chain orders everything after it.                    */
/* On success *fence_out holds one caller reference to the armed fence  */
/* (may be NULL if arm failed closed). On failure nothing is queued and */
/* every reference taken by this helper is dropped.                     */
static int p7580_drv_split_queue(struct p7580_split_ctx *s,
				 unsigned int pos, uint64_t jc,
				 const uint32_t hdr[8],
				 struct dma_fence *prev,
				 struct dma_fence **fence_out)
{
	struct p7580_submit_job *job;
	struct drm_sched_job *sched_job = NULL;
	struct dma_fence *fence_ref = NULL;
	unsigned int i;
	int ret;

	*fence_out = NULL;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	job->seqno = s->df->next_seqno++;
	job->slot = s->slot;
	job->file_priv = s->df;
	job->drv_priv = s->drv;
	job->jc = jc;
	job->src_jc = (s->src && s->shadow) ? s->src[pos] : 0;
	job->requirements = s->requirements;
	job->as_nr = P7580_DRV_AS_NR;
	job->affinity = s->drv->jd.shader_present;
	job->chain_shadow = s->shadow;
	job->chain_pos = pos;
	job->chain_len = s->nodes;
	memcpy(job->desc, hdr, sizeof(job->desc));
	if (pos + 1 == s->nodes)
		job->out_sync = s->out_sync;
	/* gpu31-storm58: what the one-shot fault dump needs, captured
	 * at SUBMIT. fbd is the SFBD pointer the chain walker read from
	 * this node's live descriptor word 10 (fragment nodes only).
	 * jc_age is this file's SUBMIT age of the BO holding jc: 1 means
	 * the previous SUBMIT rewrote that page, 2 the previous frame's
	 * FS submit -- the "was the descriptor page just recycled"
	 * number. */
	job->fbd = s->fbd ? s->fbd[pos] : 0;
	for (i = 0; i < s->nr; i++) {
		struct p7580_bo *b = to_p7580_bo(s->objs[i]);

		if (jc >= b->iova && jc - b->iova < b->base.size) {
			job->jc_age = s->age ? s->age[i] : 0;
			break;
		}
	}

	/* Per-job BO references (gpu31-chainsplit): every split node owns
	 * its own set, so free_job retires each node's sync + refs in
	 * retire order and no job depends on another's lifetime. The
	 * caller's lookup references stay in objs[] until the whole
	 * split has been queued. */
	if (s->nr) {
		job->bos = kcalloc(s->nr, sizeof(*job->bos), GFP_KERNEL);
		job->iova_handles = kcalloc(s->nr,
					    sizeof(*job->iova_handles),
					    GFP_KERNEL);
		job->iova_addrs = kcalloc(s->nr,
					  sizeof(*job->iova_addrs),
					  GFP_KERNEL);
		job->iova_sizes = kcalloc(s->nr,
					  sizeof(*job->iova_sizes),
					  GFP_KERNEL);
		if (!job->bos || !job->iova_handles || !job->iova_addrs ||
		    !job->iova_sizes) {
			ret = -ENOMEM;
			goto err_job;
		}
		for (i = 0; i < s->nr; i++) {
			struct p7580_bo *sbo = to_p7580_bo(s->objs[i]);

			drm_gem_object_reference(s->objs[i]);
			job->bos[i] = s->objs[i];
			job->iova_handles[i] = s->hsave[i];
			job->iova_addrs[i] = sbo->iova;
			job->iova_sizes[i] = (uint32_t)sbo->base.size;
		}
		job->bo_count = s->nr;
		job->iova_bo_count = s->nr;
	}

	sched_job = drm_sched_job_alloc(s->entity, &p7580_drv_sched_ops,
					job);
	if (!sched_job) {
		ret = -ENOMEM;
		goto err_job;
	}
	if (!s->entity || sched_job->entity != s->entity ||
	    sched_job->ops != &p7580_drv_sched_ops ||
	    !sched_job->ops->run_job || !sched_job->ops->alloc_fence) {
		ret = -EFAULT;
		goto err_sched;
	}
	sched_job->slot = s->slot;
	/* Fault-latency kick target (gpu31-latency): the IRQ fault leg
	 * needs the owning sched job to re-arm its timeout work at delay
	 * 0. Stored only after the ops/entity validation above so the
	 * IRQ never dereferences a half-initialized pairing. */
	job->sched_job = sched_job;
	drm_sched_job_arm(sched_job);
	if (sched_job->fence) {
		fence_ref = sched_job->fence;
		dma_fence_get(fence_ref);
	}

	/* Ordering: this node waits for the previous fence. For pos 0
	 * that is the per-file serial predecessor; for every later node
	 * it is the node queued immediately before it, which is what
	 * gives the split its strict WRITE_VALUE -> VERTEX -> TILER
	 * order across separate dispatches. */
	if (prev && prev != sched_job->fence &&
	    !dma_fence_is_signaled(prev)) {
		ret = drm_sched_job_add_dependency(sched_job, prev);
		if (ret)
			goto err_sched;
	}
	if (pos == 0) {
		for (i = 0; i < s->nf; i++) {
			if (dma_fence_is_signaled(s->inf[i]))
				continue;
			ret = drm_sched_job_add_dependency(sched_job,
							   s->inf[i]);
			if (ret)
				goto err_sched;
		}
		for (i = 0; i < s->nr; i++) {
			struct p7580_bo *bo = to_p7580_bo(s->objs[i]);
			struct dma_fence *f = dma_resv_get_excl(&bo->resv);

			if (f) {
				if (!dma_fence_is_signaled(f))
					ret = drm_sched_job_add_dependency(
						sched_job, f);
				dma_fence_put(f);
				if (ret)
					goto err_sched;
			}
		}
	}

	/* Per-job runtime-PM lifetime: armed before the job becomes
	 * visible to the async worker, released in free_job. */
	p7580_drv_job_pm_arm(s->drv);
	ret = drm_sched_job_submit(sched_job);
	if (ret) {
		p7580_drv_job_pm_release(s->drv);
		goto err_sched;
	}
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: split seq=%u pos=%u/%u slot=%d jc=0x%llx rc=0\n",
			job->seqno, pos, s->nodes - 1, s->slot,
			(unsigned long long)jc);
	}
	*fence_out = fence_ref;
	return 0;

err_sched:
	if (fence_ref)
		dma_fence_put(fence_ref);
	drm_sched_job_cleanup(sched_job);
	kfree(sched_job);
err_job:
	if (job->bos) {
		for (i = 0; i < job->bo_count; i++) {
			if (job->bos[i])
				drm_gem_object_unreference_unlocked(
					job->bos[i]);
		}
		job->bo_count = 0;
	}
	p7580_job_free(job);
	return ret;
}


/* gpu31-allbos: map every BO this DRM file owns, not only the handles in
 * the submit array.  Mesa 26 references a VA that belongs to no BO of the
 * batch (kwin faults at 0x24864000, access type 2, every animation
 * window) while Mesa 20.3.5 does not, so its command stream expects the
 * context's BOs to be resident.  map_bo is idempotent (bo_list check). */
static void p7580_drv_map_file_bos(struct p7580_drv *drv,
				   struct drm_file *file_priv)
{
	struct drm_gem_object *objs[256];
	unsigned int n = 0, i;
	struct drm_gem_object *obj;
	int id;

	spin_lock(&file_priv->table_lock);
	idr_for_each_entry(&file_priv->object_idr, obj, id) {
		if (n >= ARRAY_SIZE(objs))
			break;
		drm_gem_object_reference(obj);
		objs[n++] = obj;
	}
	spin_unlock(&file_priv->table_lock);

	for (i = 0; i < n; i++) {
		p7580_drv_map_bo(drv, objs[i]);
		drm_gem_object_unreference_unlocked(objs[i]);
	}
}

int p7580_drv_submit(struct p7580_drv *drv, struct drm_device *dev,
		     struct drm_file *file_priv,
		     const struct p7580_submit *args)
{
	struct p7580_drv_file *df = file_priv->driver_priv;
	struct drm_gem_object *objs[P7580_MAX_SUBMIT_BOS];
	uint32_t hsave[P7580_MAX_SUBMIT_BOS];
	struct dma_fence *inf[P7580_MAX_IN_SYNCS];
	struct drm_sched_entity *entity;
	unsigned int i, nr = 0, nf = 0;
	/* gpu31-mmaprefs closure probe: at most 8 ledger lines per
	 * submit so the job trace budget is never starved. */
	unsigned int pin_budget = 8;
	int slot, ret;

	if (!drv || !dev || !file_priv || !args || !df)
		return (int)p7580_submit_err("validate", -EINVAL);
	if (!p7580_jc_valid(args->jc))
		return (int)p7580_submit_err("validate", -EINVAL);
	if (args->requirements & ~(P7580_JD_REQ_FS |
				   P7580_JD_REQ_ONLY_COMPUTE))
		return (int)p7580_submit_err("validate", -EINVAL);
	if (args->in_sync_count > P7580_MAX_IN_SYNCS ||
	    args->bo_handle_count > P7580_MAX_SUBMIT_BOS)
		return (int)p7580_submit_err("validate", -EINVAL);

	/* ST-IOCTL: ioctl reached the driver with a valid envelope. */
	if (p7580_sched_trace) {
		p7580_sched_trace--;
		pr_info("p7580_sched: ioctl jc=0x%llx req=0x%x bo=%u in=%u out=%u\n",
			(unsigned long long)args->jc, args->requirements,
			args->bo_handle_count, args->in_sync_count,
			args->out_sync);
	}

	slot = p7580_job_slot(args->requirements);
	if (slot < 0 || slot >= P7580_NUM_JOB_SLOTS)
		return (int)p7580_submit_err("validate", -EINVAL);
	if (!(drv->jd.js_present & (1u << slot)))
		return (int)p7580_submit_err("validate", -ENODEV);

	ret = p7580_drv_sched_ensure();
	if (ret)
		return ret;
	entity = &df->entity[slot];
	/* gpu31-firstfault: serialize_fs queues the FS (slot 0) submit on
	 * the chain slot's entity, so the fragment job is ordered behind
	 * the tiler by one FIFO instead of by a cross-entity shim
	 * dependency alone. The hardware slot stays job->slot (0); only
	 * the queue it rides changes. A/B knob, default 0. */
	if (slot == 0 && drv->jd.serialize_fs &&
	    (drv->jd.js_present & (1u << 1)))
		entity = &df->entity[1];

	/* 1. in-fences (referenced; put on every exit path). */
	if (args->in_sync_count) {
		const uint32_t *hs =
			(const uint32_t *)(uintptr_t)args->in_syncs;

		for (i = 0; i < args->in_sync_count; i++) {
			struct drm_syncobj *s =
				drm_syncobj_find(&df->mgr, hs[i]);
			struct dma_fence *f;

			if (!s) {
ret = (int)p7580_submit_err("in_sync", -ENOENT);
				goto err_inf;
			}
			f = drm_syncobj_find_fence(s);
			drm_syncobj_put(s);
			if (!f) {
				/* Empty syncobj: already signaled (0003). */
				continue;
			}
			inf[nf++] = f;
		}
	}

	/* 2. BO lookup + map into AS 0 (referenced; unref on exit). */
	p7580_drv_map_file_bos(drv, file_priv);
	if (args->bo_handle_count) {
		const uint32_t *uh =
			(const uint32_t *)(uintptr_t)args->bo_handles;

		for (i = 0; i < args->bo_handle_count; i++) {
			struct drm_gem_object *obj =
				drm_gem_object_lookup(dev, file_priv,
						      uh[i]);

			if (!obj) {
ret = (int)p7580_submit_err("bo", -ENOENT);
				goto err_objs;
			}
			objs[nr++] = obj;
			hsave[nr - 1] = uh[i];
			ret = p7580_drv_map_bo(drv, obj);
			if (ret)
				goto err_objs;
			/* gpu31-coherency + gpu31-cachedir: CPU fills
			 * descriptors/payloads before SUBMIT (ioctl copy
			 * / mmap writes); CLEAN (writeback, never
			 * invalidate) those lines to the point the GPU
			 * reads now that the BO is bound, and before
			 * anything else can invalidate the range.
			 * Before the job is queued. */
			p7580_gem_submit_sync(obj);
			/* gpu31-mmaprefs closure probe (shaderbo §5.1):
			 * page ledger + first words of this BO's first
			 * page at SUBMIT, after the clean. count must
			 * stay >= 3 for a mapped BO (page cache + BO +
			 * mapping pin; +1 per mapped PTE) and the ISA
			 * words must not be zero. Budgeted. */
			if (pin_budget) {
				pin_budget--;
				p7580_gem_debug_page_ledger(obj, "sub", 0);
			}
		}
	}

	/* 3. gpu31-chainsplit: walk the submitted chain (next pointers)
	 * and queue one sched job per node on this slot's entity. Each
	 * node gets its own START, so the standard p7580_js_config()
	 * (START/END_FLUSH_CLEAN_INV) brackets every stage and the
	 * vertex->tiler handoff crosses a real submit boundary.
	 * Ordering is a drm_sched dependency on the previous node; the
	 * ioctl's out_sync goes on the LAST node's fence only. */
	{
		uint32_t hdr[P7580_CHAIN_MAX_JOBS][P7580_CHAIN_HDR_WORDS];
		uint64_t jcs[P7580_CHAIN_MAX_JOBS];
		uint64_t srcjcs[P7580_CHAIN_MAX_JOBS];
		/* gpu31-storm58: live SFBD pointer per fragment node and
		 * the SUBMIT age of every submitted BO. */
		uint32_t fbds[P7580_CHAIN_MAX_JOBS];
		uint32_t ages[P7580_MAX_SUBMIT_BOS];
		struct p7580_chain_shadow *shadow = NULL;
		struct p7580_split_ctx sctx;
		struct dma_fence *serial_prev = NULL;
		struct dma_fence *last_fence = NULL;
		unsigned int nodes = 0, queued = 0, ci;
		int need_shadow;
		uint64_t walk = args->jc;

		ret = 0;

		/* gpu31-storm58: stamp the age ledger once per SUBMIT so
		 * every node of this split reads the same "SUBMITs since
		 * this file last used this BO" number. Bounded (nr is
		 * capped by P7580_MAX_SUBMIT_BOS), no allocation. */
		df->submit_no++;
		for (i = 0; i < nr; i++)
			ages[i] = p7580_drv_bo_age_touch(
				df, to_p7580_bo(objs[i])->iova,
				df->submit_no);

		for (;;) {
			uint64_t next;

			if (nodes == P7580_CHAIN_MAX_JOBS) {
				ret = (int)p7580_submit_err("chain", -E2BIG);
				goto err_split;
			}
			ret = p7580_drv_gpu_read(drv, walk, hdr[nodes],
						 P7580_JOB_DESC_HDR_SIZE);
			if (ret) {
				ret = (int)p7580_submit_err("chain", ret);
				goto err_split;
			}
			/* gpu31-storm58: a fragment job (type 9) carries
			 * the SFBD pointer in payload word 10. Capture it
			 * here (4 bytes; p7580_drv_gpu_read bounds-checks
			 * it against the owning BO) so the one-shot fault
			 * dump can dump the SFBD page without an
			 * out-of-bounds desc[] read. 0 when unreadable. */
			fbds[nodes] = 0;
			if (((hdr[nodes][4] >> 1) & 0x7fu) == 9u)
				(void)p7580_drv_gpu_read(drv, walk + 40u,
							 &fbds[nodes],
							 sizeof(fbds[0]));
			/* Only grounded Midgard job aggregates can be
			 * isolated into a shadow slot. */
			if (!p7580_chain_job_bytes(
					p7580_chain_hdr_type(hdr[nodes]))) {
				ret = (int)p7580_submit_err("chain", -EINVAL);
				goto err_split;
			}
			jcs[nodes] = walk;
			nodes++;
			next = p7580_chain_hdr_next(hdr[nodes - 1]);
			if (!next)
				break;
			if (!p7580_jc_valid(next)) {
				ret = (int)p7580_submit_err("chain", -EINVAL);
				goto err_split;
			}
			walk = next;
		}

		/* A one-node chain with no dependency indices is already a
		 * standalone dispatch (Mesa's fragment submit): keep the
		 * original descriptor and allocate no shadow BO. */
		need_shadow = nodes > 1 ||
			p7580_chain_hdr_dep1(hdr[0]) ||
			p7580_chain_hdr_dep2(hdr[0]);
		/* gpu31-firstfault: words 0..3 are hardware OUTPUT fields
		 * (exception_status, first_incomplete_task, fault_pointer)
		 * that Mesa never initialises. A nonzero value left in a
		 * recycled pool page would otherwise be dispatched
		 * verbatim; route such a descriptor through the shadow so
		 * it can be sanitised. */
		for (ci = 0; ci < nodes && !need_shadow; ci++) {
			if (hdr[ci][0] | hdr[ci][1] | hdr[ci][2] | hdr[ci][3]) {
				need_shadow = 1;
				pr_info("p7580_job: desc-stale nodes=%u pos=%u src=0x%llx w=%08x %08x %08x %08x (shadowed+sanitised)\n",
					nodes, ci, (unsigned long long)jcs[ci],
					hdr[ci][0], hdr[ci][1], hdr[ci][2],
					hdr[ci][3]);
			}
		}

		if (need_shadow) {
			shadow = p7580_drv_shadow_create(drv, dev, df, nodes);
			if (IS_ERR(shadow)) {
				ret = (int)p7580_submit_err("chain",
							    PTR_ERR(shadow));
				shadow = NULL;
				goto err_split;
			}
			for (ci = 0; ci < nodes; ci++) {
				uint32_t jobw[48];
				unsigned int bytes = p7580_chain_job_bytes(
					p7580_chain_hdr_type(hdr[ci]));

				/* Copy header + inline payload, then
				 * isolate: next = NULL, deps = 0,
				 * index = position + 1. */
				ret = p7580_drv_gpu_read(drv, jcs[ci], jobw,
							 bytes);
				if (ret) {
					ret = (int)p7580_submit_err("chain",
								    ret);
					goto err_split;
				}
				p7580_chain_hdr_isolate(jobw, ci);
				/* gpu31-firstfault: the JM writes the four
				 * header status words on fault (measured:
				 * word0=0x00010258, fault_pointer=jc after
				 * a 0x58). They are outputs: never feed a
				 * recycled page's leftovers back to the
				 * hardware. */
				jobw[0] = 0;
				jobw[1] = 0;
				jobw[2] = 0;
				jobw[3] = 0;
				memcpy(hdr[ci], jobw,
				       P7580_JOB_DESC_HDR_SIZE);
				ret = p7580_drv_shadow_write(shadow->obj,
					(size_t)ci * P7580_CHAIN_SLOT_STRIDE,
					jobw, bytes);
				if (ret) {
					ret = (int)p7580_submit_err("chain",
								    ret);
					goto err_split;
				}
			}
			/* Publish every rewritten descriptor to DRAM. */
			p7580_gem_submit_sync(shadow->obj);
			/* Keep the live addresses for the fault dump: jcs[]
			 * is about to be overwritten with the shadows. */
			for (ci = 0; ci < nodes; ci++)
				srcjcs[ci] = jcs[ci];
			for (ci = 0; ci < nodes; ci++)
				jcs[ci] = shadow->iova +
					(uint64_t)ci *
						P7580_CHAIN_SLOT_STRIDE;
			if (p7580_chain_trace) {
				p7580_chain_trace--;
				pr_info("p7580_sched: chain jc=0x%llx nodes=%u shadow=0x%llx stride=%u\n",
					(unsigned long long)args->jc, nodes,
					(unsigned long long)shadow->iova,
					P7580_CHAIN_SLOT_STRIDE);
			}
		}

		/* Validate out_sync before anything is queued (-ENOENT
		 * contract preserved from the single-job path). */
		if (args->out_sync) {
			struct drm_syncobj *vs =
				drm_syncobj_find(&df->mgr, args->out_sync);

			if (!vs) {
				ret = (int)p7580_submit_err("out_sync",
							    -ENOENT);
				goto err_split;
			}
			drm_syncobj_put(vs);
		}

		/* Per-file serial predecessor: one temporary ref, used by
		 * node 0 and released after the attach attempt. */
		mutex_lock(&df->serial_lock);
		serial_prev = df->serial_prev;
		if (serial_prev)
			dma_fence_get(serial_prev);
		mutex_unlock(&df->serial_lock);

		sctx.drv = drv;
		sctx.dev = dev;
		sctx.df = df;
		sctx.entity = entity;
		sctx.slot = slot;
		sctx.requirements = args->requirements;
		sctx.out_sync = args->out_sync;
		sctx.objs = objs;
		sctx.hsave = hsave;
		sctx.nr = nr;
		sctx.inf = inf;
		sctx.nf = nf;
		sctx.shadow = shadow;
		sctx.nodes = nodes;
		sctx.src = shadow ? srcjcs : NULL;
		sctx.fbd = fbds;
		sctx.age = ages;

		for (ci = 0; ci < nodes; ci++) {
			struct dma_fence *prev =
				ci ? last_fence : serial_prev;
			struct dma_fence *fence = NULL;

			ret = p7580_drv_split_queue(&sctx, ci, jcs[ci],
						    hdr[ci], prev, &fence);
			if (ci == 0 && serial_prev) {
				dma_fence_put(serial_prev);
				serial_prev = NULL;
			}
			if (ret) {
				ret = (int)p7580_submit_err("queue", ret);
				break;
			}
			if (last_fence)
				dma_fence_put(last_fence);
			last_fence = fence;
			queued++;
		}

		/* gpu31-latency (MFBD hand-off) + gpu31-storm58: the BO
		 * exclusive-fence publication of the split's final fence
		 * lives in the serial section below, under serial_lock,
		 * together with the serial_prev swap. The resv holds ONE
		 * exclusive fence (0002 add_excl replaces), so the publish
		 * order must be the file's serial submit order; otherwise
		 * an older submit's fence can land on a BO after a newer
		 * one did, a WAIT_BO then reports the BO idle while the
		 * newer job still uses it, and userspace rewrites it under
		 * the GPU (int42 fragment storm). The publish still
		 * happens before the ioctl returns, which is what the
		 * readback path needs (flush_batches then
		 * panfrost_bo_wait must see this submit's fence). */
		/* One out-fence for the whole split: install on the last
		 * queued node's armed fence. On a partial queue failure
		 * that is the last node the GPU accepted, so a waiter
		 * still observes the work that did run. */
		if (queued && args->out_sync && last_fence) {
			struct drm_syncobj *s =
				drm_syncobj_find(&df->mgr, args->out_sync);

			if (!s) {
				ret = (int)p7580_submit_err("out_sync",
							    -ENOENT);
			} else {
				int oret = drm_syncobj_replace_fence(s,
								     last_fence);

				drm_syncobj_put(s);
				if (oret)
					ret = (int)p7580_submit_err(
						"out_sync", oret);
			}
		}

		/* Publish the split's final fence as the file serial
		 * predecessor (one slot ref) AND as the BOs' exclusive
		 * fence, both under serial_lock. A partial failure
		 * publishes the last node that queued, so the next SUBMIT
		 * still orders behind everything accepted here.
		 *
		 * gpu31-storm58: the BO stamp must be inside this lock.
		 * The resv carries a single exclusive fence, so whichever
		 * SUBMIT publishes last wins; with the stamp outside the
		 * lock (and a second identical stamp in the async run_job
		 * path, now removed) an earlier submit's *older* fence could
		 * overwrite a *newer* one, making WAIT_BO report a BO
		 * busy-in-GPU as idle; Mesa's BO-cache fetch then reused
		 * the page and rewrote the descriptor/SFBD of a job still
		 * in flight. Publishing here makes the last writer the
		 * last submit in serial order, whose fence dominates every
		 * earlier submit touching the same BOs. */
		if (queued) {
			struct dma_fence *old = NULL;

			mutex_lock(&df->serial_lock);
			if (last_fence) {
				for (i = 0; i < nr; i++) {
					struct drm_gem_object *o = objs[i];

					if (!o)
						continue;
					dma_resv_add_excl_fence(
						&to_p7580_bo(o)->resv,
						last_fence);
				}
			}
			old = df->serial_prev;
			df->serial_prev = last_fence;
			if (df->serial_prev)
				dma_fence_get(df->serial_prev);
			mutex_unlock(&df->serial_lock);
			if (old)
				dma_fence_put(old);
		}

err_split:
		/* Nodes that never queued still own one shadow reference
		 * each; queued nodes release theirs in free_job. */
		if (shadow) {
			for (ci = queued; ci < nodes; ci++)
				p7580_drv_shadow_put(shadow);
		}
		if (last_fence)
			dma_fence_put(last_fence);
		if (serial_prev)
			dma_fence_put(serial_prev);
	}

	/* The ioctl's lookup references and in-fence references are no
	 * longer needed: each queued node owns its own set, and the
	 * sched dependency graph owns the external fence references. */
err_objs:
	for (i = 0; i < nr; i++)
		drm_gem_object_unreference_unlocked(objs[i]);
err_inf:
	for (i = 0; i < nf; i++)
		dma_fence_put(inf[i]);
	return ret;
}

/* SUBMIT ioctl override (wired in main.c's table instead of GEM's
 * panfrost_ioctl_submit). Stages user arrays with copy_from_user, then
 * runs p7580_drv_submit. Caps mirror the jobs slice (64 BOs / 16 syncs).
 * Must stay in sync with jobs/panfrost7580_jm_hw.h struct p7580_submit
 * (the staged local). */
int panfrost_ioctl_submit_override(struct drm_device *dev, void *data,
				   struct drm_file *file_priv)
{
	struct drm_panfrost_submit *args = data;
	struct p7580_drv_file *df = file_priv->driver_priv;
	struct p7580_drv *drv;
	uint32_t bo_handles[P7580_MAX_SUBMIT_BOS];
	uint32_t in_syncs[P7580_MAX_IN_SYNCS];
	struct p7580_submit staged;

	if (!df || !df->drv)
		return -ENODEV;
	drv = df->drv;
	if (!p7580_jc_valid(args->jc))
		return -EINVAL;
	if (args->requirements & ~PANFROST_JD_REQ_FS)
		return -EINVAL;
	if (args->in_sync_count > P7580_MAX_IN_SYNCS ||
	    args->bo_handle_count > P7580_MAX_SUBMIT_BOS)
		return -EINVAL;

	if (args->bo_handle_count) {
		if (!args->bo_handles ||
		    copy_from_user(bo_handles, (void __user *)(uintptr_t)
				   args->bo_handles,
				   args->bo_handle_count * sizeof(uint32_t)))
			return -EFAULT;
	}
	if (args->in_sync_count) {
		if (!args->in_syncs ||
		    copy_from_user(in_syncs, (void __user *)(uintptr_t)
				   args->in_syncs,
				   args->in_sync_count * sizeof(uint32_t)))
			return -EFAULT;
	}

	staged.jc = args->jc;
	staged.in_syncs = (uint64_t)(uintptr_t)in_syncs;
	staged.in_sync_count = args->in_sync_count;
	staged.out_sync = args->out_sync;
	staged.bo_handles = (uint64_t)(uintptr_t)bo_handles;
	staged.bo_handle_count = args->bo_handle_count;
	staged.requirements = args->requirements;

	/* Under runtime PM the GPU must be awake for the synchronous
	 * MMU-map + queue leg. p7580_drv_submit arms a second,
	 * job-lifetime ref before the job is visible to the async
	 * worker, so dropping this ref on return is safe: the block
	 * stays powered until free_job releases the job's own ref.
	 * Fail closed: no MMIO (not even map_bo) after a failed get. */
	{
		int ret = p7580_drv_pm_get(drv);

		if (ret < 0)
			return ret;
		ret = p7580_drv_submit(drv, dev, file_priv, &staged);
		p7580_drv_pm_put(drv);
		return ret;
	}
}
