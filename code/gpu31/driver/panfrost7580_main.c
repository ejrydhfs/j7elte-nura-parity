/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_main.c - render-minor-only platform driver for Mali-T720
 * (Exynos 7580, j7elte) on Nura 3.10.108.
 *
 * Binds the STOCK node (never edited):
 *   mali@0x11400000, compatible "arm,mali",
 *   reg <0 0x11400000 0 0x5000>, interrupts <0 282 0> <0 283 0> <0 281 0>
 *   (index order 0/1/2 = job/mmu/gpu; names ignored like kbase),
 *   clocks fin_pll/fout_g3d_pll/aclk_g3d/mout_g3d/dout_aclk_g3d/
 *   aclk_g3d_400, samsung,power-domain = <&pd_g3d> (0x60, genpd-owned).
 *
 * Registers a DRM device with NO modeset (no DRIVER_MODESET) and no
 * primary minor (DRIVER_RENDER_ONLY): with the Mode-B render-only core
 * patch (G31_Kms) the same attach allocates ONLY renderD128, no
 * /dev/dri/cardN at all, so the X platform probe can never see this
 * device. No CRTC/
 * encoder/connector, no fbdev conflicts; vendor DECON keeps the panel.
 *
 * Six v5.2 ioctls (byte-identical UAPI, panfrost_drm.h): SUBMIT 0x00,
 * WAIT_BO 0x01, CREATE_BO 0x02, MMAP_BO 0x03, GET_PARAM 0x04,
 * GET_BO_OFFSET 0x05.
 *
 * Probe order: mem -> ioremap -> power (first power-on) -> present-reg
 * read -> GEM init -> MMU alloc + program_as under PM ref -> job slots
 * -> devfreq -> IRQs -> DRM attach. No register is touched before the
 * power domain/clocks are up (F-SUB1); remove runs the reverse under a
 * PM ref (cores idle-checked, IRQs masked first). No DT edits, no
 * sysmmu, no display-driver changes.
 */

#include <linux/device.h>
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
#include <linux/slab.h>

#include <drm/drmP.h>
#include <drm/drm.h>

#include <linux/dma-fence.h>		/* 0001 */
#include <linux/dma-resv.h>		/* 0002 */
#include <drm_syncobj.h>		/* 0003 */
#include <drm_sched.h>			/* 0004 */

#include "panfrost_drm.h"
#include "panfrost7580_gem.h"
#include "panfrost7580_ioctl.h"
#include "panfrost7580_jm_hw.h"
#include "panfrost7580_job.h"
#include "panfrost7580_mmu.h"
#include "panfrost7580_power.h"
#include "panfrost7580_syncobj.h"
#include "pf7580_devfreq.h"
#include "panfrost7580_drv.h"

/* ------------------------------------------------------------------ */
/* MMU/GPU IRQ handlers (JOB_INT demux lives in the jobs slice)      */
/* ------------------------------------------------------------------ */

/* MMU_INT_STAT lives at MMU base + 0x0C (pf7580_as.h layout); reuse
 * the mmu slice's AS offsets: MMU base = regs + 0x2000. Cleared via
 * MMU_INT_CLEAR (+0x04), masked via MMU_INT_MASK (+0x08). */
#define P7580_MMU_BASE_OFF	0x2000u
#define P7580_MMU_INT_CLEAR	0x04u
#define P7580_MMU_INT_MASK	0x08u
#define P7580_MMU_INT_STAT	0x0Cu

irqreturn_t p7580_drv_mmu_irq(int irq, void *arg)
{
	struct p7580_drv *drv = arg;
	u32 stat;

	(void)irq;
	if (!drv)
		return IRQ_NONE;

	stat = readl(drv->regs + P7580_MMU_BASE_OFF + P7580_MMU_INT_STAT);
	if (!stat)
		return IRQ_NONE;

	/* Log the faulting AS (decode via the mmu slice), clear, keep
	 * going: recovery runs in process context (timedout path). */
	{
		unsigned int as;

		for (as = 0; as < 4; as++) {
			u32 bit = (1u << as) | (1u << (as + 16));

			if (stat & bit) {
				u32 fst = readl(drv->regs +
						P7580_MMU_AS_BASE +
						as * P7580_MMU_AS_STRIDE +
						P7580_AS_FAULTSTATUS);
				u32 flo = readl(drv->regs + P7580_MMU_AS_BASE +
						as * P7580_MMU_AS_STRIDE +
						P7580_AS_FAULTADDR_LO);
				u32 fhi = readl(drv->regs + P7580_MMU_AS_BASE +
						as * P7580_MMU_AS_STRIDE +
						P7580_AS_FAULTADDR_HI);

				dev_err(drv->dev,
					"panfrost7580: MMU fault AS%u: %s/%s stat %08x addr %08x%08x\n",
					as, p7580_mmu_fault_code(fst),
					p7580_mmu_fault_atype(fst), fst,
					fhi, flo);
			}
		}
	}
	writel(stat, drv->regs + P7580_MMU_BASE_OFF + P7580_MMU_INT_CLEAR);
	return IRQ_HANDLED;
}

/* GPU_INT_RAWSTAT 0x020 / CLEAR 0x024 (jobs jm_hw.h P7580_JOB_INT_* is
 * the JOB block at 0x1000; the GPU block is separate). MULTIPLE_FAULT
 * is bit 7 (kbase); RESET_COMPLETED bit 8 (power slice). */
#define P7580_GPU_IRQ_MULTI_FAULT	(1u << 7)

irqreturn_t p7580_drv_gpu_irq(int irq, void *arg)
{
	struct p7580_drv *drv = arg;
	u32 stat;

	(void)irq;
	if (!drv)
		return IRQ_NONE;

	stat = readl(drv->regs + P7580_GPU_INT_RAWSTAT);
	if (!stat)
		return IRQ_NONE;

	if (stat & P7580_GPU_IRQ_MULTI_FAULT)
		dev_err(drv->dev,
			"panfrost7580: GPU MULTIPLE_FAULT %08x\n", stat);
	writel(stat, drv->regs + P7580_GPU_INT_CLEAR);
	return IRQ_HANDLED;
}

static irqreturn_t p7580_drv_job_irq(int irq, void *arg)
{
	struct p7580_drv *drv = arg;

	if (!drv)
		return IRQ_NONE;
	return p7580_job_irq(irq, &drv->jd) ? IRQ_HANDLED : IRQ_NONE;
}

/* ------------------------------------------------------------------ */
/* DRM driver hooks                                                   */
/* ------------------------------------------------------------------ */

static int p7580_drv_load(struct drm_device *dev, unsigned long flags)
{
	(void)dev;
	(void)flags;
	return 0;
}

static int p7580_drv_unload(struct drm_device *dev)
{
	(void)dev;
	return 0;
}

static int p7580_drv_open(struct drm_device *dev, struct drm_file *file_priv)
{
	struct p7580_drv *drv = dev->dev_private;
	struct p7580_drv_file *df;
	int ret;

	df = kzalloc(sizeof(*df), GFP_KERNEL);
	if (!df)
		return -ENOMEM;
	ret = p7580_drv_file_init(df, drv);
	if (ret) {
		kfree(df);
		return ret;
	}
	file_priv->driver_priv = df;
	return 0;
}

static void p7580_drv_preclose(struct drm_device *dev,
			       struct drm_file *file_priv)
{
	(void)dev;
	(void)file_priv;
}

static void p7580_drv_postclose(struct drm_device *dev,
				struct drm_file *file_priv)
{
	struct drm_device *ddev = dev;
	struct p7580_drv *drv;
	struct p7580_drv_file *df = file_priv->driver_priv;
	int pm_held = 0;

	(void)dev;
	if (!df)
		return;
	/* Close-after-idle is the observed watchdog path: the fd dies
	 * while the block is runtime-suspended, and entity fini +
	 * queued-job retirement touches HEAD/START/RESET below. Hold
	 * one PM ref across file_fini (which takes its own inner hold
	 * for the entity walk; nesting gets is refcounted, both must
	 * be put). On get failure the block is dark: workers fail
	 * closed via their own arms and fini still flushes CPU-side
	 * state so nothing leaks. */
	drv = (ddev && ddev->dev_private) ? ddev->dev_private : NULL;
	if (!drv)
		drv = df->drv;
	if (drv && p7580_drv_teardown_hold(drv))
		pm_held = 1;
	file_priv->driver_priv = NULL;
	p7580_drv_file_fini(df);
	kfree(df);
	if (pm_held)
		p7580_drv_teardown_release(drv);
}

static void p7580_drv_lastclose(struct drm_device *dev)
{
	(void)dev;
}

/* SUBMIT override: full dispatch through the jobs/sched/MMU path.
 * The ioctl's copy_from_user staging lives in GEM's
 * panfrost_ioctl_submit; but that helper targets GEM's own job_dev /
 * job_fctx pairing. Our file state carries the same layout plus the
 * driver pointer + sched entities, so dispatch here instead: stage
 * the user arrays locally, then run p7580_drv_submit (validate ->
 * reserve -> MMU-map -> sched -> out-sync). GEM's helper is not
 * called. */
int panfrost_ioctl_submit_override(struct drm_device *dev, void *data,
				   struct drm_file *file_priv);

/* Generic syncobj surface (syncobj.c): fops-routed, NOT in the
 * driver ioctl table (3.10 core table owns nr >= 0xA0). */
long panfrost_syncobj_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg);
#ifdef CONFIG_COMPAT
long panfrost_syncobj_compat_ioctl(struct file *filp, unsigned int cmd,
				    unsigned long arg);
#endif

/* fops: render-minor-only mmap via the GEM fault path. */
static const struct file_operations p7580_drv_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = panfrost_syncobj_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = panfrost_syncobj_compat_ioctl,
#else
	.compat_ioctl = NULL,
#endif
	.mmap = p7580_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
};

/* Legacy 3.10 rows: DRM_IOCTL_DEF_DRV takes (ioctl, func, flags).
 * SUBMIT keeps DRM_AUTH (legacy gate); the Mode-B core change makes
 * the render node always-authenticated and denies DRM_MASTER ioctls
 * on it, so user-space Mesa works unprivileged while modeset stays
 * card0-only. All six are DRM_UNLOCKED (v5.2 DRM_RENDER_ALLOW). */
static struct drm_ioctl_desc p7580_drv_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PANFROST_SUBMIT, panfrost_ioctl_submit_override,
			  DRM_AUTH | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PANFROST_WAIT_BO, panfrost_ioctl_wait_bo,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PANFROST_CREATE_BO, panfrost_ioctl_create_bo,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PANFROST_MMAP_BO, panfrost_ioctl_mmap_bo,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PANFROST_GET_PARAM, panfrost_ioctl_get_param,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PANFROST_GET_BO_OFFSET,
			  panfrost_ioctl_get_bo_offset, DRM_UNLOCKED),
};

static struct drm_driver p7580_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY,
	.load = p7580_drv_load,
	.unload = p7580_drv_unload,
	.open = p7580_drv_open,
	.preclose = p7580_drv_preclose,
	.postclose = p7580_drv_postclose,
	.lastclose = p7580_drv_lastclose,
	.gem_free_object = p7580_gem_free_object,
	.gem_vm_ops = &p7580_gem_vm_ops,
	.dumb_create = p7580_dumb_create,
	.dumb_map_offset = p7580_dumb_map_offset,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export = p7580_gem_prime_export,
	.gem_prime_import = p7580_gem_prime_import,
	.gem_prime_get_sg_table = p7580_gem_prime_get_sg_table,
	.gem_prime_import_sg_table = p7580_gem_prime_import_sg_table,
	.gem_prime_vmap = p7580_gem_prime_vmap,
	.gem_prime_vunmap = p7580_gem_prime_vunmap,
	.ioctls = p7580_drv_ioctls,
	.num_ioctls = 0,	/* filled at probe (DRM_ARRAY_SIZE) */
	.fops = &p7580_drv_fops,
	.name = "panfrost",
	.desc = "Mali-T720 render-only (Exynos 7580)",
	.date = "20261119",
	/* 1.1: the first version Mesa 26/27 accepts (its panfrost/kmod backend
	 * refuses < 1.1).  1.3 adds SYSTEM_TIMESTAMP, 1.4 SET_LABEL_BO and 1.6
	 * QUERY_BO_INFO, none of which this ioctl table implements, so stop at
	 * 1.1: BO_HEAP/NOEXEC flags and the GET_BO_OFFSET import path only. */
	.major = 1,
	.minor = 1,
	.patchlevel = 0,
};

/* ------------------------------------------------------------------ */
/* Power management (delegates to the power + devfreq slices)         */
/* ------------------------------------------------------------------ */

static int p7580_drv_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct p7580_drv *drv;

	if (!ddev)
		return 0;
	drv = ddev->dev_private;
	/* Do not gate the clocks while a job owns the block: run_job /
	 * recover touch HEAD/START/RESET registers asynchronously, and
	 * every live job holds its own PM ref — but a stray extra
	 * suspend (autosuspend racing queue, teardown ordering) must
	 * still refuse rather than darken the block under MMIO.
	 * -EBUSY tells the PM core to retry after the job retires
	 * (its free_job put lets the next suspend succeed). */
	if (drv && !p7580_job_is_idle(&drv->jd))
		return -EBUSY;
	if (drv && drv->dvfs)
		pf7580_dvfs_suspend(drv->dvfs);
	if (drv && drv->power)
		return pf7580_runtime_suspend(drv->power);
	return 0;
}

static int p7580_drv_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct p7580_drv *drv;
	int ret;

	if (!ddev)
		return 0;
	drv = ddev->dev_private;
	if (drv && drv->power) {
		ret = pf7580_runtime_resume(drv->power);
		if (ret)
			return ret;
	}
	/* A POWER_FAULT latch left by a previous fault survives the
	 * soft reset inside the power-on above (measured on int39: after
	 * one fault every following job re-reported 0x41 until the block
	 * was cycled by a suspend). Clear it here, before any job can be
	 * dispatched, and let the reprogram below rebuild AS/IRQs on the
	 * fresh core state (gpu31-readback). */
	if (drv && drv->jd.power_cycle &&
	    p7580_job_slots_latched_fault(&drv->jd)) {
		int crc = drv->jd.power_cycle(drv->jd.pm_dev);

		pr_err("p7580_job: resume-cycle rc=%d (latched JS fault)\n",
		       crc);
	}
	/* Resume re-arms what reset + power-off lost (mainline
	 * panfrost_device_resume order: soft_reset -> power_on ->
	 * mmu_enable(0) -> job_enable_interrupts; kbase_pm_clock_on:
	 * MMU reprogram, then JOB unmask last). AS0 TRANSTAB/MEMATTR
	 * first so descriptor fetch faults nowhere, then CLEAR stale
	 * slot bits and unmask DONE+ERR for present slots. Powered by
	 * the resume above; safe to touch MMIO here. */
	if (drv) {
		p7580_mmu_program_as(drv->regs, P7580_DRV_AS_NR,
				     drv->mmu.root_dma,
				     P7580_MEMATTR_KBASE_LO,
				     P7580_MEMATTR_KBASE_HI);
		p7580_job_dev_rearm(&drv->jd);
	}
	if (drv && drv->dvfs)
		pf7580_dvfs_resume(drv->dvfs);
	return 0;
}

static int p7580_drv_pm_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct p7580_drv *drv;

	if (!ddev)
		return 0;
	drv = ddev->dev_private;
	/* Suspend devfreq polling before the clock gates (devfreq slice:
	 * monitor must stop while runtime PM gates aclk_g3d). */
	if (drv && drv->dvfs)
		pf7580_dvfs_suspend(drv->dvfs);
	if (drv && drv->power)
		return pf7580_pm_suspend(dev, drv->power);
	return 0;
}

static int p7580_drv_pm_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct p7580_drv *drv;
	int ret;

	if (!ddev)
		return 0;
	drv = ddev->dev_private;
	if (drv && drv->power) {
		/* Power slice: unconditional full power-on (pd-g3d was
		 * off, so GPU state is lost); genpd sequencing stays in
		 * the power slice. */
		ret = pf7580_pm_resume(dev, drv->power);
		if (ret)
			return ret;
	}
	if (drv && drv->dvfs)
		pf7580_dvfs_resume(drv->dvfs);
	return 0;
}

static const struct dev_pm_ops p7580_drv_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(p7580_drv_pm_suspend, p7580_drv_pm_resume)
	SET_RUNTIME_PM_OPS(p7580_drv_runtime_suspend,
			   p7580_drv_runtime_resume, NULL)
};

/* ------------------------------------------------------------------ */
/* Probe / remove                                                     */
/* ------------------------------------------------------------------ */

static const struct of_device_id p7580_of_match[] = {
	{ .compatible = "arm,mali-t720" },
	/* Stock DT carries only "arm,mali"; match it SoC-gated by the
	 * reg address check below (0x11400000). Refusing other arm,mali
	 * SoCs here keeps this driver off foreign boards. */
	{ .compatible = "arm,mali" },
	{ }
};
MODULE_DEVICE_TABLE(of, p7580_of_match);

/* GPU feature registers (MIDGARD, mainline panfrost_regs.h / kbase
 * mali_midg_regmap.h).  These are the GPU block offsets, not the JOB/MMU
 * interrupt offsets used further down this file. */
#define P7580_GPU_ID			0x000u
#define P7580_GPU_L2_FEATURES		0x004u
#define P7580_GPU_CORE_FEATURES		0x008u
#define P7580_GPU_TILER_FEATURES	0x00Cu
#define P7580_GPU_MEM_FEATURES		0x010u
#define P7580_GPU_MMU_FEATURES		0x014u
#define P7580_GPU_AS_PRESENT		0x018u
#define P7580_GPU_JS_PRESENT		0x01Cu
#define P7580_GPU_AFBC_FEATURES		0x04Cu
#define P7580_GPU_THREAD_MAX_THREADS	0x0A0u
#define P7580_GPU_THREAD_MAX_WORKGROUP	0x0A4u
#define P7580_GPU_THREAD_MAX_BARRIER	0x0A8u
#define P7580_GPU_THREAD_FEATURES	0x0ACu
#define P7580_GPU_TEXTURE_FEATURES(n)	(0x0B0u + ((n) * 4u))
#define P7580_GPU_JS_FEATURES(n)	(0x0C0u + ((n) * 4u))
#define P7580_GPU_SHADER_PRESENT_LO	0x100u
#define P7580_GPU_SHADER_PRESENT_HI	0x104u
#define P7580_GPU_TILER_PRESENT_LO	0x110u
#define P7580_GPU_TILER_PRESENT_HI	0x114u
#define P7580_GPU_L2_PRESENT_LO		0x120u
#define P7580_GPU_L2_PRESENT_HI		0x124u
#define P7580_GPU_COHERENCY_FEATURES	0x300u
#define P7580_GPU_THREAD_TLS_ALLOC	0x310u
#define P7580_GPU_STACK_PRESENT_LO	0xE00u
#define P7580_GPU_STACK_PRESENT_HI	0xE04u

/* Experimental (Mesa 26) convergence: snapshot the feature registers once,
 * while probe holds the block powered, and answer GET_PARAM from it.  Reads
 * follow the mainline panfrost_gpu_init_features() order/offsets. */
static void p7580_capture_features(struct p7580_drv *drv)
{
	void __iomem *regs = drv->regs;
	unsigned int i, njs;

	drv->feat.gpu_id = readl(regs + P7580_GPU_ID);
	drv->feat.l2_features = readl(regs + P7580_GPU_L2_FEATURES);
	drv->feat.core_features = readl(regs + P7580_GPU_CORE_FEATURES);
	drv->feat.tiler_features = readl(regs + P7580_GPU_TILER_FEATURES);
	drv->feat.mem_features = readl(regs + P7580_GPU_MEM_FEATURES);
	drv->feat.mmu_features = readl(regs + P7580_GPU_MMU_FEATURES);
	drv->feat.as_present = readl(regs + P7580_GPU_AS_PRESENT);
	drv->feat.js_present = readl(regs + P7580_GPU_JS_PRESENT) & 0x7u;
	drv->feat.thread_features = readl(regs + P7580_GPU_THREAD_FEATURES);
	drv->feat.max_threads = readl(regs + P7580_GPU_THREAD_MAX_THREADS);
	drv->feat.thread_max_workgroup_sz =
		readl(regs + P7580_GPU_THREAD_MAX_WORKGROUP);
	drv->feat.thread_max_barrier_sz =
		readl(regs + P7580_GPU_THREAD_MAX_BARRIER);
	drv->feat.coherency_features = readl(regs + P7580_GPU_COHERENCY_FEATURES);
	drv->feat.afbc_features = readl(regs + P7580_GPU_AFBC_FEATURES);
	for (i = 0; i < 4; i++)
		drv->feat.texture_features[i] =
			readl(regs + P7580_GPU_TEXTURE_FEATURES(i));
	drv->feat.shader_present = readl(regs + P7580_GPU_SHADER_PRESENT_LO);
	drv->feat.shader_present |=
		(uint64_t)readl(regs + P7580_GPU_SHADER_PRESENT_HI) << 32;
	drv->feat.tiler_present = readl(regs + P7580_GPU_TILER_PRESENT_LO);
	drv->feat.tiler_present |=
		(uint64_t)readl(regs + P7580_GPU_TILER_PRESENT_HI) << 32;
	drv->feat.l2_present = readl(regs + P7580_GPU_L2_PRESENT_LO);
	drv->feat.l2_present |=
		(uint64_t)readl(regs + P7580_GPU_L2_PRESENT_HI) << 32;
	drv->feat.stack_present = readl(regs + P7580_GPU_STACK_PRESENT_LO);
	drv->feat.stack_present |=
		(uint64_t)readl(regs + P7580_GPU_STACK_PRESENT_HI) << 32;
	drv->feat.thread_tls_alloc = readl(regs + P7580_GPU_THREAD_TLS_ALLOC);
	drv->feat.nr_core_groups = hweight64(drv->feat.l2_present);
	njs = hweight32(drv->feat.js_present);
	for (i = 0; i < 16; i++)
		drv->feat.js_features[i] = (i < njs) ?
			readl(regs + P7580_GPU_JS_FEATURES(i)) : 0;
	pr_info("p7580_features: gpu_id=0x%llx rev=0x%llx l2=0x%08x core=0x%08x tiler=0x%08x mem=0x%08x mmu=0x%08x as=0x%x js=0x%x shader=0x%llx l2_present=0x%llx max_threads=%u thread_feat=0x%08x tls=%u afbc=0x%08x\n",
		(unsigned long long)drv->feat.gpu_id,
		(unsigned long long)(drv->feat.gpu_id & 0xffff),
		drv->feat.l2_features, drv->feat.core_features,
		drv->feat.tiler_features, drv->feat.mem_features,
		drv->feat.mmu_features, drv->feat.as_present,
		drv->feat.js_present,
		(unsigned long long)drv->feat.shader_present,
		(unsigned long long)drv->feat.l2_present,
		drv->feat.max_threads, drv->feat.thread_features,
		drv->feat.thread_tls_alloc, drv->feat.afbc_features);
}

/* PM wrappers for the jobs-slice dump path (void-arg, device-direct):
 * the jobs slice never includes pm_runtime.h; these live in the driver
 * TU where pm_runtime is available. get returns 0 when MMIO is allowed
 * (pm_runtime_get_sync's 1 = already active is normalised to 0),
 * negative errno dark; put drops the ref. */
static int p7580_job_pm_get_void(void *dev)
{
	int ret;

	if (!dev)
		return -ENODEV;
	ret = pm_runtime_get_sync((struct device *)dev);
	if (ret < 0) {
		pm_runtime_put_noidle((struct device *)dev);
		return ret;
	}
	return 0;
}

static void p7580_job_pm_put_void(void *dev)
{
	if (!dev)
		return;
	pm_runtime_mark_last_busy((struct device *)dev);
	pm_runtime_put_autosuspend((struct device *)dev);
}

/* Fault-recovery core power cycle for the jobs slice (gpu31-readback).
 * Same opaque-device argument as the PM hooks above: the jobs slice never
 * includes the power slice, and the power slice never includes DRM. */
static int p7580_job_power_cycle_void(void *dev)
{
	struct platform_device *pdev;
	struct drm_device *ddev;
	struct p7580_drv *drv;

	if (!dev)
		return -ENODEV;
	pdev = to_platform_device((struct device *)dev);
	ddev = platform_get_drvdata(pdev);
	if (!ddev)
		return -ENODEV;
	drv = ddev->dev_private;
	if (!drv || !drv->power)
		return -ENODEV;
	return pf7580_power_cycle(drv->power);
}

int p7580_drv_get_param(struct drm_device *ddev, uint32_t param,
		       uint64_t *value)
{
	struct p7580_drv *drv = ddev ? ddev->dev_private : NULL;

	if (!drv || !value)
		return -ENODEV;

	switch (param) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID:
		*value = drv->feat.gpu_id >> 16;
		break;
	case DRM_PANFROST_PARAM_GPU_REVISION:
		*value = drv->feat.gpu_id & 0xffff;
		break;
	case DRM_PANFROST_PARAM_SHADER_PRESENT:
		*value = drv->feat.shader_present;
		break;
	case DRM_PANFROST_PARAM_TILER_PRESENT:
		*value = drv->feat.tiler_present;
		break;
	case DRM_PANFROST_PARAM_L2_PRESENT:
		*value = drv->feat.l2_present;
		break;
	case DRM_PANFROST_PARAM_STACK_PRESENT:
		*value = drv->feat.stack_present;
		break;
	case DRM_PANFROST_PARAM_AS_PRESENT:
		*value = drv->feat.as_present;
		break;
	case DRM_PANFROST_PARAM_JS_PRESENT:
		*value = drv->feat.js_present;
		break;
	case DRM_PANFROST_PARAM_L2_FEATURES:
		*value = drv->feat.l2_features;
		break;
	case DRM_PANFROST_PARAM_CORE_FEATURES:
		*value = drv->feat.core_features;
		break;
	case DRM_PANFROST_PARAM_TILER_FEATURES:
		*value = drv->feat.tiler_features;
		break;
	case DRM_PANFROST_PARAM_MEM_FEATURES:
		*value = drv->feat.mem_features;
		break;
	case DRM_PANFROST_PARAM_MMU_FEATURES:
		*value = drv->feat.mmu_features;
		break;
	case DRM_PANFROST_PARAM_THREAD_FEATURES:
		*value = drv->feat.thread_features;
		break;
	case DRM_PANFROST_PARAM_MAX_THREADS:
		*value = drv->feat.max_threads;
		break;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ:
		*value = drv->feat.thread_max_workgroup_sz;
		break;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ:
		*value = drv->feat.thread_max_barrier_sz;
		break;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES:
		*value = drv->feat.coherency_features;
		break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES0:
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES1:
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES2:
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES3:
		*value = drv->feat.texture_features[param -
			DRM_PANFROST_PARAM_TEXTURE_FEATURES0];
		break;
	case DRM_PANFROST_PARAM_JS_FEATURES0:
	case DRM_PANFROST_PARAM_JS_FEATURES1:
	case DRM_PANFROST_PARAM_JS_FEATURES2:
	case DRM_PANFROST_PARAM_JS_FEATURES3:
	case DRM_PANFROST_PARAM_JS_FEATURES4:
	case DRM_PANFROST_PARAM_JS_FEATURES5:
	case DRM_PANFROST_PARAM_JS_FEATURES6:
	case DRM_PANFROST_PARAM_JS_FEATURES7:
	case DRM_PANFROST_PARAM_JS_FEATURES8:
	case DRM_PANFROST_PARAM_JS_FEATURES9:
	case DRM_PANFROST_PARAM_JS_FEATURES10:
	case DRM_PANFROST_PARAM_JS_FEATURES11:
	case DRM_PANFROST_PARAM_JS_FEATURES12:
	case DRM_PANFROST_PARAM_JS_FEATURES13:
	case DRM_PANFROST_PARAM_JS_FEATURES14:
	case DRM_PANFROST_PARAM_JS_FEATURES15:
		*value = drv->feat.js_features[param -
			DRM_PANFROST_PARAM_JS_FEATURES0];
		break;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS:
		*value = drv->feat.nr_core_groups;
		break;
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC:
		*value = drv->feat.thread_tls_alloc;
		break;
	case DRM_PANFROST_PARAM_AFBC_FEATURES:
		*value = drv->feat.afbc_features;
		break;
	default:
		/* SYSTEM_TIMESTAMP*, ALLOWED_JM_CTX_PRIORITIES and
		 * SELECTED_COHERENCY belong to 1.3/1.5/1.6; Mesa tolerates
		 * -EINVAL there because it gates them on the version. */
		return -EINVAL;
	}
	return 0;
}

static int p7580_drv_probe(struct platform_device *pdev)
{
	struct resource *mem;
	void __iomem *regs;
	struct p7580_drv *drv;
	u32 js_present, shader_present;
	int ret;

	if (!pdev->dev.of_node)
		return -ENODEV;

	/* Stock reg resource must be 0x11400000/0x5000 (refuse foreign
	 * arm,mali SoCs that share the bare compatible). */
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem || mem->start != 0x11400000u || resource_size(mem) < 0x5000u)
		return -ENODEV;

	regs = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	/* IRQs by INDEX (0/1/2 = job/mmu/gpu); names ignored (kbase ABI). */
	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	drv->dev = &pdev->dev;
	drv->regs = regs;
	mutex_init(&drv->bo_lock);
	INIT_LIST_HEAD(&drv->bo_list);

	drv->irq_job = platform_get_irq(pdev, 0);
	drv->irq_mmu = platform_get_irq(pdev, 1);
	drv->irq_gpu = platform_get_irq(pdev, 2);
	if (drv->irq_job < 0 || drv->irq_mmu < 0 || drv->irq_gpu < 0)
		return -ENODEV;

	/* Power FIRST (F-SUB1): no register below is touched before the
	 * domain/clocks are up. pf7580_power_probe runs the stock
	 * sequence (clk_get x6 -> regulator -> autosuspend 50 ms ->
	 * get_sync [genpd + SOFT_RESET + cores L2->shader->tiler] ->
	 * put_autosuspend) and leaves the block powered on return. */
	ret = pf7580_power_probe(&pdev->dev, regs, &drv->power);
	if (ret)
		return ret;
	/* Keep the block awake for the rest of probe (present-reg read,
	 * program_as, job_dev_init below are all MMIO). Balanced at the
	 * end of probe (or on any error leg after this point). */
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(&pdev->dev);
		goto err_power;
	}
	atomic_set(&drv->pm_ref, 0);

	/* Present bits (MMIO: powered by the get above). */
	js_present = readl(regs + P7580_GPU_JS_PRESENT);
	shader_present = readl(regs + P7580_GPU_SHADER_PRESENT_LO);
	js_present &= 0x7u;
	if (!js_present) {
		ret = -ENODEV;
		goto err_pm;
	}
	p7580_capture_features(drv);

	/* GEM allocator (IOVA window; no MMIO). */
	ret = p7580_dev_init(&drv->gd, &pdev->dev);
	if (ret)
		goto err_pm;

	/* One GPU address space, programmed now (MMIO: powered). */
	ret = p7580_mmu_alloc(&pdev->dev, &drv->mmu);
	if (ret)
		goto err_gem;
	p7580_mmu_program_as(regs, P7580_DRV_AS_NR, drv->mmu.root_dma,
			     P7580_MEMATTR_KBASE_LO, P7580_MEMATTR_KBASE_HI);

	/* Job slots (JOB_INT_CLEAR/MASK writes: powered). Recover
	 * rebinds this same root after every GPU reset, so stash it on
	 * the job device here (single AS0 on this part). */
	drv->jd.pgd_dma = drv->mmu.root_dma;
	ret = p7580_job_dev_init(&drv->jd, regs, js_present,
				 ((u64)0 << 32) | shader_present,
				 drv->mmu.root_dma, &pdev->dev,
				 p7580_job_pm_get_void, p7580_job_pm_put_void,
				 p7580_job_power_cycle_void);
	if (ret)
		goto err_mmu;
	/* R1: warm up the shared sched workqueue at probe so the first
	 * open never constructs kernel threads under the opener's
	 * mutex. Failure here is non-fatal: the first open retries
	 * via p7580_drv_sched_ensure inside file_init. */
	/* gpu31-iova: fault-time mapping dump (ERR-MAP) + map/unmap
	 * trace re-arm (iova_arm). Resolver renders the faulting
	 * job's SUBMIT-time snapshot; IRQ-safe (no GEM deref). */
	p7580_job_set_iova_resolver(&drv->jd, p7580_drv_iova_resolve,
				     drv);
	/* gpu31-firstfault: first-fault descriptor/SFBD/tiler-heap page
	 * dumper (process-context only; armed per run by ev_arm). */
	p7580_job_set_desc_dump(&drv->jd, p7580_drv_desc_dump, drv);
	/* gpu31-watchdog: job_timeout_ms knob -> the sched seed above. */
	p7580_job_set_timeout_hooks(&drv->jd,
				    p7580_drv_get_job_timeout_ms,
				    p7580_drv_set_job_timeout_ms, drv);
	p7580_drv_register_iova_trace(&p7580_iova_trace);
	ret = p7580_drv_sched_ensure();
	if (ret)
		goto err_mmu;

	/* Devfreq (optional: continues degraded-but-alive without it). */
	if (pf7580_dvfs_init(&pdev->dev, drv->power, &drv->dvfs))
		drv->dvfs = NULL;

	/* IRQs (shared lines tolerated on this SoC). */
	ret = devm_request_irq(&pdev->dev, drv->irq_job, p7580_drv_job_irq,
			       IRQF_SHARED, "panfrost7580-job", drv);
	if (ret)
		goto err_dvfs;
	ret = devm_request_irq(&pdev->dev, drv->irq_mmu, p7580_drv_mmu_irq,
			       IRQF_SHARED, "panfrost7580-mmu", drv);
	if (ret)
		goto err_dvfs;
	ret = devm_request_irq(&pdev->dev, drv->irq_gpu, p7580_drv_gpu_irq,
			       IRQF_SHARED, "panfrost7580-gpu", drv);
	if (ret)
		goto err_dvfs;

	/* DRM attach (render-minor-only; no modeset). The 3.10 core
	 * allocates struct drm_device internally (drm_get_platform_dev);
	 * recover it from the driver list, publish our state on
	 * dev_private, and stash the drm device for remove/PM. */
	p7580_drm_driver.num_ioctls = DRM_ARRAY_SIZE(p7580_drv_ioctls);
	platform_set_drvdata(pdev, NULL);
	{
		int drm_ret;
		struct drm_device *ddev;
		struct list_head *pos;

		drm_ret = drm_platform_init(&p7580_drm_driver, pdev);
		if (drm_ret)
			goto err_dvfs;
		if (list_empty(&p7580_drm_driver.device_list)) {
			drm_platform_exit(&p7580_drm_driver, pdev);
			ret = -ENODEV;
			goto err_dvfs;
		}
		pos = p7580_drm_driver.device_list.next;
		ddev = list_entry(pos, struct drm_device, driver_item);
		ddev->dev_private = drv;
		platform_set_drvdata(pdev, ddev);
	}

	/* Probe MMIO is done (IRQs installed, no register touched after
	 * this): release the probe power hold; the block idle-suspends
	 * after PF7580_AUTOSUSPEND_MS. Later MMIO re-takes it per-path. */
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);
	return 0;

err_dvfs:
	if (drv->dvfs) {
		pf7580_dvfs_fini(drv->dvfs);
		drv->dvfs = NULL;
	}
err_mmu:
	p7580_mmu_free(&drv->mmu);
err_gem:
	p7580_dev_fini(&drv->gd);
err_pm:
	/* Balance the probe power hold (get_sync after power_probe);
	 * the block may then idle-suspend before power_remove below. */
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);
err_power:
	/* power_remove force-resumes, suspends synchronously, and
	 * disables runtime PM: the single teardown for every leg
	 * after power_probe (probe-failure or remove path shape). */
	if (drv->power) {
		pf7580_power_remove(drv->power);
		drv->power = NULL;
	}
	return ret;
}

static int p7580_drv_remove(struct platform_device *pdev)
{
	struct drm_device *ddev = platform_get_drvdata(pdev);
	struct p7580_drv *drv;
	struct p7580_drv_bo *e, *tmp;

	if (ddev)
		drv = ddev->dev_private;
	else
		drv = NULL;

	if (drv) {
		int pm_held = 0;

		/* Mask the slot IRQs first: no new DONE/ERR demux (MMIO
		 * read/CLEAR) can run while we tear the tables down. */
		p7580_job_dev_fini(&drv->jd);
		/* Hold the block awake for the invalidate() MMIO below
		 * (teardown_hold/teardown_release pair; fail-closed on
		 * dark: skip the TLB flush but still free table memory
		 * and the IOVA range). */
		if (p7580_drv_teardown_hold(drv)) {
			pm_held = 1;
		} else {
			dev_warn(drv->dev,
				 "panfrost7580: remove with GPU dark;"
				 " skipping TLB invalidate\n");
		}
		/* Unmap every bound BO before freeing the tables. The
		 * entries are weak (gpu31-boleak, see struct
		 * p7580_drv_bo): unlinking them drops no reference, the
		 * BOs keep whatever references their owners hold. */
		mutex_lock(&drv->bo_lock);
		list_for_each_entry_safe(e, tmp, &drv->bo_list, node) {
			p7580_drv_unmap_bo(drv, e->obj, pm_held);
			list_del(&e->node);
			kfree(e);
		}
		mutex_unlock(&drv->bo_lock);
		if (pm_held)
			p7580_drv_teardown_release(drv);

		if (drv->dvfs) {
			pf7580_dvfs_fini(drv->dvfs);
			drv->dvfs = NULL;
		}
		p7580_mmu_free(&drv->mmu);
		p7580_dev_fini(&drv->gd);
		if (drv->power) {
			pf7580_power_remove(drv->power);
			drv->power = NULL;
		}
	}

	drm_platform_exit(&p7580_drm_driver, pdev);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static struct platform_driver p7580_platform_driver = {
	.probe = p7580_drv_probe,
	.remove = p7580_drv_remove,
	.driver = {
		.name = "panfrost",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(p7580_of_match),
		.pm = &p7580_drv_pm_ops,
	},
};

module_platform_driver(p7580_platform_driver);

MODULE_AUTHOR("gpu31");
MODULE_DESCRIPTION("panfrost7580 render-only DRM driver (Mali-T720, Exynos 7580)");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:panfrost");
