/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_drv.h - shared device/file state for the panfrost7580
 * render-only platform driver (Exynos 7580 / Mali-T720, j7elte).
 *
 * Stock DT node (never edited): mali@0x11400000, "arm,mali",
 * reg 0x11400000/0x5000, IRQs index order 0/1/2 = job/mmu/gpu
 * (SPI 282/283/281), 6 clocks, power-domain <&pd_g3d> (0x60).
 *
 * Layout contracts (both required by the reused slice code):
 *  - struct p7580_drv starts with struct p7580_dev gd, so the GEM
 *    helpers (which take dev->dev_private as struct p7580_dev *)
 *    operate on our device state directly.
 *  - struct p7580_drv_file starts with mgr/job_fctx/job_dev in the
 *    same order as GEM's struct p7580_file_priv, so GEM's ioctl
 *    helpers (CREATE_BO/MMAP_BO/WAIT_BO/GET_PARAM/GET_BO_OFFSET)
 *    work unchanged on our file state.
 */
#ifndef PANFROST7580_DRV_H
#define PANFROST7580_DRV_H

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <drm_syncobj.h>		/* 0003 */
#include <drm_sched.h>			/* 0004 */
#include "panfrost7580_gem.h"
#include "panfrost7580_job.h"
#include "panfrost7580_mmu.h"
#include "panfrost7580_power.h"
#include "pf7580_devfreq.h"

/* One GPU address space on this part (all slots share it). */
#define P7580_DRV_AS_NR		0u

/* Bound-BO record: one per BO mapped into the context tables, so the
 * remove/unload path can unmap + invalidate before tearing down.
 *
 * gpu31-boleak: the entry is NON-OWNING -- it holds no GEM reference.
 * A list-owned reference can never be dropped: its only unlink runs in
 * p7580_gem_free_object() -> p7580_gem_gpu_unmap(), and free_object
 * only runs once the refcount is zero, which the list reference itself
 * prevents. Every BO ever submitted then stays alive forever (its
 * pages, its private shmem inode and its IOVA range with it).
 * The entry is unlinked (under bo_lock) by p7580_gem_gpu_unmap() at
 * free and by p7580_drv_unmap_bo() at remove, both before the BO's
 * memory is released, so an entry seen under bo_lock names a live
 * object -- except for the window between the refcount reaching zero
 * and the destructor taking bo_lock. A walker that needs to *use* the
 * object must therefore take a reference with
 * p7580_drv_gem_try_get() (kref_get_unless_zero, submit.c), which
 * loses that race instead of resurrecting a dying object. Callers that
 * already hold a reference (a GEM handle lookup, job->bos) need none. */
struct p7580_drv_bo {
	struct list_head node;
	struct drm_gem_object *obj;	/* weak: valid while linked */
	/* gpu31-iova: IOVA + size of the live mapping (for the
	 * overlap detector + fault-time identity; stable while the
	 * entry is linked under bo_lock). */
	uint64_t iova;
	size_t size;
};

struct p7580_drv {
	/* MUST be first: GEM uses dev->dev_private as struct p7580_dev *. */
	struct p7580_dev gd;
	struct device *dev;
	void __iomem *regs;
	int irq_job;
	int irq_mmu;
	int irq_gpu;
	struct p7580_job_dev jd;	/* job slots */
	struct p7580_mmu mmu;		/* GPU page tables */
	struct pf7580_power *power;	/* clocks/regulator/PM */
	struct pf7580_dvfs *dvfs;	/* devfreq (may be NULL) */
	struct mutex bo_lock;		/* serialises bo_list */
	struct list_head bo_list;
	atomic_t pm_ref;		/* live-job runtime-PM refs (see submit.c) */
	/* Mesa 26 GET_PARAM snapshot: read once at probe while the block is
	 * powered (values are constant for the part). */
	struct {
		uint64_t gpu_id;		/* GPU_ID register */
		uint64_t shader_present;
		uint64_t tiler_present;
		uint64_t l2_present;
		uint64_t stack_present;
		uint32_t as_present;
		uint32_t js_present;
		uint32_t l2_features;
		uint32_t core_features;
		uint32_t tiler_features;
		uint32_t mem_features;
		uint32_t mmu_features;
		uint32_t thread_features;
		uint32_t max_threads;
		uint32_t thread_max_workgroup_sz;
		uint32_t thread_max_barrier_sz;
		uint32_t coherency_features;
		uint32_t afbc_features;
		uint32_t texture_features[4];
		uint32_t js_features[16];
		uint32_t nr_core_groups;
		uint32_t thread_tls_alloc;
	} feat;
};

struct drm_device;
/* Fill *value for one DRM_PANFROST_PARAM_* from the probe snapshot.
 * Returns -EINVAL for parameters this driver does not expose. */
int p7580_drv_get_param(struct drm_device *ddev, uint32_t param, uint64_t *value);

/* gpu31-storm58: bounded BO-age ledger size (see p7580_drv_file). */
#define P7580_BO_AGE_SLOTS	64u

/* Per-file state. Leading layout mirrors GEM's struct p7580_file_priv
 * (mgr, job_fctx, job_dev) so GEM's ioctl helpers work unchanged. */
struct p7580_drv_file {
	struct drm_syncobj_manager mgr;	/* == p7580_file_priv.mgr */
	struct p7580_file_ctx *job_fctx;	/* == p7580_file_priv.job_fctx */
	struct p7580_job_dev *job_dev;	/* == p7580_file_priv.job_dev */
	struct p7580_drv *drv;
	struct drm_sched_entity entity[P7580_NUM_JOB_SLOTS];
	uint32_t next_seqno;
	int entities_live;
	/* gpu31-shadowpin: shadow BOs (chain-split copies) pinned for the
	 * file's lifetime.  They live outside the file's idr, so
	 * map_file_bos cannot re-map them; the pin keeps their leaves
	 * valid until postclose has drained every job. */
	struct list_head shadow_pins;
	unsigned int shadow_pin_count;
	/* Per-file serial chain (gpu31-serialfence): the armed fence of
	 * the previously submitted job, or NULL when no job is
	 * outstanding. Guarded by serial_lock; every SUBMIT after arm
	 * attaches the previous fence as an implicit dependency (when
	 * still unsignaled) and then replaces it with its own armed
	 * fence (one ref taken at swap, one dropped after queue).
	 * Ref discipline: the slot holds exactly one ref on a live
	 * fence; the in-flight job owns the other ref via out_sync (or
	 * its own fence pointer) until retire. Fini drops the slot ref
	 * so a queued-but-never-run job cannot leak. */
	struct mutex serial_lock;
	struct dma_fence *serial_prev;
	/* gpu31-storm58: BO-age ledger (diagnostic, bounded, no alloc).
	 * submit_no counts SUBMIT ioctls on this file; bo_age[] maps a
	 * 4 KiB-aligned IOVA to the last SUBMIT that referenced it, so a
	 * fault can answer "was the descriptor page just recycled?": the
	 * job's jc_age is submit_no - seq of the BO holding jc (1 = the
	 * previous SUBMIT, 2 = the previous frame's FS submit). A direct
	 * map: a collision restarts the age instead of chaining. */
	uint32_t submit_no;
	struct {
		uint64_t iova;
		uint32_t seq;
	} bo_age[P7580_BO_AGE_SLOTS];
};

struct p7580_submit;
struct drm_file;
struct drm_device;

/* submit.c entry points. */
int p7580_drv_sched_ensure(void);
/* gpu31-watchdog: sched dispatch-timeout (ms) accessors behind the
 * jobs slice's job_timeout_ms debugfs knob. The setter clamps
 * 100..60000, updates an already-live sched->timeout and seeds the
 * entities of later opens (entity->timeout is copied at init). */
unsigned int p7580_drv_get_job_timeout_ms(void *priv);
int p7580_drv_set_job_timeout_ms(void *priv, unsigned int ms);
int p7580_drv_file_init(struct p7580_drv_file *df, struct p7580_drv *drv);
void p7580_drv_file_fini(struct p7580_drv_file *df);

/* Full SUBMIT dispatch (submit.c): staged kernel-memory args. */
int p7580_drv_submit(struct p7580_drv *drv, struct drm_device *dev,
		     struct drm_file *file_priv,
		     const struct p7580_submit *args);

/* Power-guard helpers (submit.c). p7580_drv_pm_get takes one
 * runtime-PM ref for MMU/register access and fails closed (no MMIO
 * after a failed get); p7580_drv_pm_put drops it. Live-job refs:
 * armed at queue time, released in free_job, so the block cannot
 * autosuspend between the ioctl and the async run_job worker. */
int p7580_drv_pm_get(struct p7580_drv *drv);
void p7580_drv_pm_put(struct p7580_drv *drv);

/* MMU/GPU IRQ demux (main.c). */
irqreturn_t p7580_drv_mmu_irq(int irq, void *arg);
irqreturn_t p7580_drv_gpu_irq(int irq, void *arg);

/* Teardown guards (submit.c: PM-held wrappers around MMIO teardown).
 * postclose/remove MUST NOT touch the AS block / slot mask dark. */
int p7580_drv_teardown_hold(struct p7580_drv *drv);
void p7580_drv_teardown_release(struct p7580_drv *drv);
/* Unmap + invalidate one BO's IOVA range (MMU-table writes are
 * CPU-side and safe dark; the invalidate MMIO needs power). */
int p7580_drv_unmap_bo(struct p7580_drv *drv, struct drm_gem_object *obj,
		       int invalidate);
/* gpu31-iova: free-time GPU unmap (submit.c; called from GEM free
 * BEFORE iova_free). Clears leaf PTEs + PM-held TLB invalidate +
 * bo_list unlink + DMA unbind. */
void p7580_gem_gpu_unmap(struct drm_gem_object *obj);
/* gpu31-iova: register the map/unmap trace budget with the jobs
 * slice iova_arm knob (submit.c; called at probe/remove). */
void p7580_drv_register_iova_trace(unsigned int *budget);
/* gpu31-iova: fault-time mapping resolver + trace budget shared
 * with main.c probe (defined in submit.c; non-static for the TU
 * boundary). */
int p7580_drv_iova_resolve(void *priv, char *buf, size_t len,
			   const uint32_t *handles, const uint64_t *addrs,
			   const uint32_t *sizes, uint32_t n);
extern unsigned int p7580_iova_trace;
/* gpu31-firstfault: first-fault descriptor-page dumper registered for
 * the jobs slice (submit.c). Process context only (GEM resolve + kmap
 * + clean-then-invalidate read); prints one 4 KiB page as 16 lines of
 * 16 words, one line per 256-byte job slot. */
int p7580_drv_desc_dump(void *priv, uint64_t gpu, const char *tag);

#endif /* PANFROST7580_DRV_H */
