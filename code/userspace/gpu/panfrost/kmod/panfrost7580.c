/*
 * panfrost7580.c - milestone M0 of a Panfrost-class driver for the
 *                  ARM Mali-T720 in the Exynos 7580 (j7elte), built
 *                  against the Nura 3.10.108 kernel tree.
 *
 * This is NOT a working GPU driver.  It is the first bring-up milestone:
 *
 *   1. binds the DT node carrying "arm,mali-t720",
 *   2. maps the 0x5000 register window and claims the real clocks,
 *      regulator and (via the vendor glue) the pd-g3d power domain,
 *   3. implements the Midgard power-up/soft-reset register sequence and
 *      decodes GPU_ID / MMU_FEATURES / core-present bitmaps from silicon,
 *   4. runs - in the kernel - the same four-level LPAE page-table encoder
 *      that the userspace unit tests cover (mali_lpae.h), so the encoder
 *      can be validated on the device before any job submission exists.
 *
 * There is no DRM device, no GEM, no scheduler and no job submission here.
 * Milestones M1..M5 in design.md cover those.
 *
 * Build (against the prepared 3.10 tree):
 *     make -C <kernel> M=$PWD ARCH=arm64 CROSS_COMPILE=... modules
 *
 * Safety interlock: kbase (CONFIG_MALI_MIDGARD) owns the same registers.
 * The driver therefore refuses to probe until either kbase is disabled or
 * the module is loaded with panfrost7580.force=1.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#include "mali_hw.h"
#include "mali_lpae.h"

#define PF7580_NAME	"panfrost7580"

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force,
	"probe even though kbase may still own the GPU (default: 0)");

struct pf7580 {
	struct device		*dev;
	void __iomem		*iomem;
	struct clk		*clk_core;	/* aclk_g3d */
	struct clk		*clk_bus;	/* aclk_g3d_400 */
	struct regulator	*vdd;		/* vdd_g3d (BUCK4) */
	struct dentry		*debugfs;

	struct mutex		lock;		/* serialises power/read */
	bool			powered;

	/* decoded once per power-up */
	u32			gpu_id;
	u32			mmu_features;
	u32			as_present;
	u32			js_present;
	u32			mem_features;
	u32			l2_features;
	u32			core_features;
	u32			tiler_features;
	u32			max_threads;
	u32			max_workgroup;
	uint64_t		shader_present;
	uint64_t		tiler_present;
	uint64_t		l2_present;
};

/* ------------------------------------------------------------------ */
/* Low-level register access                                          */
/* ------------------------------------------------------------------ */
static inline u32 pf_read(struct pf7580 *pf, u32 reg)
{
	return readl(pf->iomem + reg);
}

static inline void pf_write(struct pf7580 *pf, u32 reg, u32 val)
{
	writel(val, pf->iomem + reg);
}

/*
 * Poll a register until (val & mask) == want, or timeout_us elapses.
 * 3.10 has no readl_relaxed_poll_timeout(); the panfrost code this is
 * modelled on (panfrost_gpu.c:67-83) relies on it.
 */
static int pf_poll(struct pf7580 *pf, u32 reg, u32 mask, u32 want,
		   unsigned long timeout_us)
{
	unsigned long waited = 0;

	while (waited < timeout_us) {
		if ((pf_read(pf, reg) & mask) == want)
			return 0;
		udelay(10);
		waited += 10;
	}
	return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Power / reset                                                      */
/* ------------------------------------------------------------------ */
static int pf7580_soft_reset(struct pf7580 *pf)
{
	int ret;

	/* Match panfrost_gpu_soft_reset(): mask everything, clear the reset
	 * bit, then issue the command and poll RAWSTAT (no IRQ handler yet). */
	pf_write(pf, MALI_GPU_INT_MASK, 0);
	pf_write(pf, MALI_GPU_INT_CLEAR, MALI_GPU_IRQ_RESET_COMPLETED);
	pf_write(pf, MALI_GPU_CMD, MALI_GPU_CMD_SOFT_RESET);

	ret = pf_poll(pf, MALI_GPU_INT_RAWSTAT, MALI_GPU_IRQ_RESET_COMPLETED,
		      MALI_GPU_IRQ_RESET_COMPLETED, 10000);
	if (ret) {
		dev_warn(pf->dev, "soft reset timed out, trying hard reset\n");
		pf_write(pf, MALI_GPU_CMD, MALI_GPU_CMD_HARD_RESET);
		ret = pf_poll(pf, MALI_GPU_INT_RAWSTAT,
			      MALI_GPU_IRQ_RESET_COMPLETED,
			      MALI_GPU_IRQ_RESET_COMPLETED, 10000);
		if (ret) {
			dev_err(pf->dev, "hard reset timed out\n");
			return ret;
		}
	}

	pf_write(pf, MALI_GPU_INT_CLEAR, MALI_GPU_IRQ_FAULT |
					 MALI_GPU_IRQ_MULTIPLE_FAULT |
					 MALI_GPU_IRQ_POWER_CHANGED |
					 MALI_GPU_IRQ_POWER_CHANGED_ALL |
					 MALI_GPU_IRQ_PERFCNT_DONE |
					 MALI_GPU_IRQ_CLEAN_CACHES_DONE);
	return 0;
}

static void pf7580_read_features(struct pf7580 *pf)
{
	pf->gpu_id        = pf_read(pf, MALI_GPU_ID);
	pf->l2_features   = pf_read(pf, MALI_GPU_L2_FEATURES);
	pf->core_features = pf_read(pf, MALI_GPU_CORE_FEATURES);
	pf->tiler_features= pf_read(pf, MALI_GPU_TILER_FEATURES);
	pf->mem_features  = pf_read(pf, MALI_GPU_MEM_FEATURES);
	pf->mmu_features  = pf_read(pf, MALI_GPU_MMU_FEATURES);
	pf->as_present    = pf_read(pf, MALI_GPU_AS_PRESENT);
	pf->js_present    = pf_read(pf, MALI_GPU_JS_PRESENT);

	pf->shader_present = pf_read(pf, MALI_GPU_SHADER_PRESENT_LO);
	pf->tiler_present  = pf_read(pf, MALI_GPU_TILER_PRESENT_LO);
	pf->l2_present     = pf_read(pf, MALI_GPU_L2_PRESENT_LO);

	pf->max_threads   = pf_read(pf, 0x0A0);	/* THREAD_MAX_THREADS */
	pf->max_workgroup = pf_read(pf, 0x0A4);	/* THREAD_MAX_WORKGROUP_SIZE */
}

static int pf7580_cores_on(struct pf7580 *pf)
{
	int ret;

	/* Same order as panfrost_gpu_power_on(): L2, shaders, tiler. */
	pf_write(pf, MALI_GPU_L2_PWRON_LO, (u32)pf->l2_present);
	ret = pf_poll(pf, MALI_GPU_L2_READY_LO, 0xFFFFFFFF,
		      (u32)pf->l2_present, 20000);
	if (ret) {
		dev_err(pf->dev, "L2 power-up timed out\n");
		return ret;
	}

	pf_write(pf, MALI_GPU_SHADER_PWRON_LO, (u32)pf->shader_present);
	ret = pf_poll(pf, MALI_GPU_SHADER_READY_LO, 0xFFFFFFFF,
		      (u32)pf->shader_present, 20000);
	if (ret) {
		dev_err(pf->dev, "shader power-up timed out\n");
		return ret;
	}

	pf_write(pf, MALI_GPU_TILER_PWRON_LO, (u32)pf->tiler_present);
	ret = pf_poll(pf, MALI_GPU_TILER_READY_LO, 0xFFFFFFFF,
		      (u32)pf->tiler_present, 1000);
	if (ret) {
		dev_err(pf->dev, "tiler power-up timed out\n");
		return ret;
	}

	return 0;
}

static void pf7580_cores_off(struct pf7580 *pf)
{
	pf_write(pf, MALI_GPU_SHADER_PWROFF_LO, (u32)pf->shader_present);
	pf_poll(pf, MALI_GPU_SHADER_PWRTRANS_LO, 0xFFFFFFFF, 0, 2000);

	pf_write(pf, MALI_GPU_TILER_PWROFF_LO, (u32)pf->tiler_present);
	pf_poll(pf, MALI_GPU_TILER_PWRTRANS_LO, 0xFFFFFFFF, 0, 2000);

	pf_write(pf, MALI_GPU_L2_PWROFF_LO, (u32)pf->l2_present);
	pf_poll(pf, MALI_GPU_L2_PWRTRANS_LO, 0xFFFFFFFF, 0, 2000);
}

static int pf7580_power_on(struct pf7580 *pf)
{
	int ret = 0;

	lockdep_assert_held(&pf->lock);

	if (pf->powered)
		return 0;

	if (pf->vdd) {
		ret = regulator_enable(pf->vdd);
		if (ret) {
			dev_err(pf->dev, "vdd_g3d enable failed: %d\n", ret);
			return ret;
		}
	}

	ret = clk_prepare_enable(pf->clk_core);
	if (ret) {
		dev_err(pf->dev, "aclk_g3d enable failed: %d\n", ret);
		goto err_vdd;
	}

	if (pf->clk_bus) {
		ret = clk_prepare_enable(pf->clk_bus);
		if (ret) {
			dev_err(pf->dev, "aclk_g3d_400 enable failed: %d\n", ret);
			goto err_core;
		}
	}

	ret = pf7580_soft_reset(pf);
	if (ret)
		goto err_bus;

	pf7580_read_features(pf);

	ret = pf7580_cores_on(pf);
	if (ret)
		goto err_bus;

	pf->powered = true;
	return 0;

err_bus:
	if (pf->clk_bus)
		clk_disable_unprepare(pf->clk_bus);
err_core:
	clk_disable_unprepare(pf->clk_core);
err_vdd:
	if (pf->vdd)
		regulator_disable(pf->vdd);
	return ret;
}

static void pf7580_power_off(struct pf7580 *pf)
{
	lockdep_assert_held(&pf->lock);

	if (!pf->powered)
		return;

	pf7580_cores_off(pf);
	pf->powered = false;

	if (pf->clk_bus)
		clk_disable_unprepare(pf->clk_bus);
	clk_disable_unprepare(pf->clk_core);
	if (pf->vdd)
		regulator_disable(pf->vdd);
}

/* ------------------------------------------------------------------ */
/* MMU page-table self test (CPU-side encode + walk)                   */
/* ------------------------------------------------------------------ */
/*
 * The real driver will need a coherent table pool; the self test only
 * exercises the encoder, so lowmem pages resolved through phys_to_virt()
 * are sufficient and honest here.  Milestone M1 replaces this with
 * dma_alloc_coherent() plus a PA->VA lookup.
 */
struct pf_test_ctx {
	int allocs;
	int frees;
};

static int pf_test_alloc(void *cookie, struct mali_table *t)
{
	struct pf_test_ctx *ctx = cookie;
	unsigned long page = __get_free_page(GFP_KERNEL | __GFP_ZERO);

	if (!page)
		return MALI_ENOMEM;
	t->va = (uint64_t *)page;
	t->pa = virt_to_phys((void *)page);
	ctx->allocs++;
	return MALI_OK;
}

static void pf_test_free(void *cookie, struct mali_table *t)
{
	struct pf_test_ctx *ctx = cookie;

	free_page((unsigned long)t->va);
	ctx->frees++;
}

static int pf_test_from_pa(void *cookie, uint64_t pa, struct mali_table *t)
{
	t->pa = pa;
	t->va = (uint64_t *)phys_to_virt(pa);
	return t->va ? MALI_OK : MALI_EINVAL;
}

static const struct mali_pgtable_ops pf_test_ops = {
	.alloc   = pf_test_alloc,
	.free    = pf_test_free,
	.from_pa = pf_test_from_pa,
	.publish = NULL,
};

static int pf7580_mmu_selftest(struct pf7580 *pf, struct seq_file *m)
{
	struct pf_test_ctx ctx = { 0, 0 };
	struct mali_pgtable pt;
	struct mali_table pgd;
	int failures = 0;
	int i;
	static const uint64_t iovas[] = {
		0x00000000ULL, 0x00001000ULL, 0x00020000ULL, 0xFFFFF000ULL,
	};

	if (pf_test_alloc(&ctx, &pgd))
		return -ENOMEM;

	mali_pgtable_init(&pt, pgd.va, pgd.pa, &pf_test_ops, &ctx, 32);

	for (i = 0; i < ARRAY_SIZE(iovas); i++) {
		uint64_t pa = 0x80000000ULL + (uint64_t)i * 0x1000;
		uint64_t got_pa = 0, attrs = 0;
		int ret;

		ret = mali_pgtable_map(&pt, iovas[i], pa, MALI_ATTR_GPU_RW);
		if (ret) {
			seq_printf(m, "  map(%llx) failed: %d\n",
				   (unsigned long long)iovas[i], ret);
			failures++;
			continue;
		}
		ret = mali_pgtable_walk(&pt, iovas[i], &got_pa, &attrs);
		if (ret || got_pa != pa || attrs != MALI_ATTR_GPU_RW) {
			seq_printf(m,
				   "  walk(%llx): ret=%d pa=%llx attrs=%llx\n",
				   (unsigned long long)iovas[i], ret,
				   (unsigned long long)got_pa,
				   (unsigned long long)attrs);
			failures++;
		}
	}

	/* Unmapped page must not resolve. */
	if (mali_pgtable_walk(&pt, 0x4000ULL, NULL, NULL) != MALI_ENOENT) {
		seq_puts(m, "  unmapped page resolved\n");
		failures++;
	}

	for (i = 0; i < ARRAY_SIZE(iovas); i++) {
		int ret = mali_pgtable_unmap(&pt, iovas[i], 1);

		if (ret) {
			seq_printf(m, "  unmap(%llx) failed: %d\n",
				   (unsigned long long)iovas[i], ret);
			failures++;
		}
	}

	/* All intermediate tables must have been returned.  The pgd is the
	 * caller-owned root and is freed below. */
	if (ctx.allocs - ctx.frees != 1) {
		seq_printf(m, "  table leak: %d allocs, %d frees\n",
			   ctx.allocs, ctx.frees);
		failures++;
	}

	seq_printf(m, "  tables allocated %d, freed %d (pgd excluded)\n",
		   ctx.allocs - 1, ctx.frees);
	seq_printf(m, "  tgbl/TRANSTAB for this pgd = 0x%llx (coherent=0)\n",
		   (unsigned long long)mali_transtab_encode(
			(uint64_t)pgd.pa, 0));
	seq_printf(m, "  memattr: mainline=0x%llx kbase=0x%llx\n",
		   (unsigned long long)MALI_MEMATTR_PANFROST_MALI,
		   (unsigned long long)MALI_MEMATTR_KBASE_LPAE);

	pf_test_free(&ctx, &pgd);

	seq_printf(m, "  result: %s\n", failures ? "FAIL" : "PASS");
	return failures ? -EINVAL : 0;
}

/* ------------------------------------------------------------------ */
/* debugfs                                                            */
/* ------------------------------------------------------------------ */
static int pf_features_show(struct seq_file *m, void *unused)
{
	struct pf7580 *pf = m->private;
	int ret = 0;

	mutex_lock(&pf->lock);
	if (!pf->powered) {
		seq_puts(m, "GPU not powered; write 1 to 'power' first\n");
		mutex_unlock(&pf->lock);
		return 0;
	}

	seq_printf(m, "GPU_ID            0x%08x (%s, r%up%u, status %u)\n",
		   pf->gpu_id,
		   MALI_GPU_ID_PRODUCT(pf->gpu_id) == MALI_PRODUCT_T72X ?
			"Mali-T720" : "unknown",
		   MALI_GPU_ID_MAJOR(pf->gpu_id),
		   MALI_GPU_ID_MINOR(pf->gpu_id),
		   MALI_GPU_ID_STATUS(pf->gpu_id));
	seq_printf(m, "MMU_FEATURES      0x%08x (VA %u bits, PA %u bits)\n",
		   pf->mmu_features,
		   MALI_MMU_VA_BITS(pf->mmu_features),
		   MALI_MMU_PA_BITS(pf->mmu_features));
	seq_printf(m, "AS_PRESENT        0x%08x (%u address spaces)\n",
		   pf->as_present, (unsigned int)hweight32(pf->as_present));
	seq_printf(m, "JS_PRESENT        0x%08x (%u job slots)\n",
		   pf->js_present, (unsigned int)hweight32(pf->js_present));
	seq_printf(m, "SHADER_PRESENT    0x%llx (%u cores)\n",
		   (unsigned long long)pf->shader_present,
		   (unsigned int)hweight64(pf->shader_present));
	seq_printf(m, "TILER_PRESENT     0x%llx\n",
		   (unsigned long long)pf->tiler_present);
	seq_printf(m, "L2_PRESENT        0x%llx\n",
		   (unsigned long long)pf->l2_present);
	seq_printf(m, "MEM_FEATURES      0x%08x\n", pf->mem_features);
	seq_printf(m, "L2_FEATURES       0x%08x\n", pf->l2_features);
	seq_printf(m, "CORE_FEATURES     0x%08x\n", pf->core_features);
	seq_printf(m, "TILER_FEATURES    0x%08x\n", pf->tiler_features);
	seq_printf(m, "THREAD_MAX        0x%08x workgroup 0x%08x\n",
		   pf->max_threads, pf->max_workgroup);
	seq_printf(m, "TRANSTAB template 0x%llx (coherent=0)\n",
		   (unsigned long long)mali_transtab_encode(0, 0));
	seq_printf(m, "memattr mainline  0x%llx\n",
		   (unsigned long long)MALI_MEMATTR_PANFROST_MALI);
	seq_printf(m, "memattr kbase      0x%llx\n",
		   (unsigned long long)MALI_MEMATTR_KBASE_LPAE);
	mutex_unlock(&pf->lock);
	return ret;
}

static int pf_features_open(struct inode *inode, struct file *file)
{
	return single_open(file, pf_features_show, inode->i_private);
}

static const struct file_operations pf_features_fops = {
	.open		= pf_features_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int pf_selftest_show(struct seq_file *m, void *unused)
{
	struct pf7580 *pf = m->private;

	seq_puts(m, "Midgard LPAE encoder self test (CPU-side)\n");
	return pf7580_mmu_selftest(pf, m);
}

static int pf_selftest_open(struct inode *inode, struct file *file)
{
	return single_open(file, pf_selftest_show, inode->i_private);
}

static const struct file_operations pf_selftest_fops = {
	.open		= pf_selftest_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t pf_power_write(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct pf7580 *pf = m->private;
	char buf[8];
	int val;
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	mutex_lock(&pf->lock);
	if (val)
		ret = pf7580_power_on(pf);
	else {
		pf7580_power_off(pf);
		ret = 0;
	}
	mutex_unlock(&pf->lock);

	return ret ? ret : len;
}

static int pf_power_show(struct seq_file *m, void *unused)
{
	struct pf7580 *pf = m->private;

	mutex_lock(&pf->lock);
	seq_printf(m, "%d\n", pf->powered);
	mutex_unlock(&pf->lock);
	return 0;
}

static int pf_power_open(struct inode *inode, struct file *file)
{
	return single_open(file, pf_power_show, inode->i_private);
}

static const struct file_operations pf_power_fops = {
	.open		= pf_power_open,
	.read		= seq_read,
	.write		= pf_power_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */
static int pf7580_probe(struct platform_device *pdev)
{
	struct pf7580 *pf;
	struct resource *res;
	int ret;

	if (!of_device_is_compatible(pdev->dev.of_node, "arm,mali-t720")) {
		dev_err(&pdev->dev, "not a Mali-T720 node\n");
		return -ENODEV;
	}

	if (!force) {
		dev_err(&pdev->dev,
			"refusing to probe: kbase owns this GPU. Disable "
			"CONFIG_MALI_MIDGARD or load with force=1.\n");
		return -ENODEV;
	}

	pf = devm_kzalloc(&pdev->dev, sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return -ENOMEM;

	pf->dev = &pdev->dev;
	mutex_init(&pf->lock);
	platform_set_drvdata(pdev, pf);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no register resource\n");
		return -ENODEV;
	}
	pf->iomem = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pf->iomem))
		return PTR_ERR(pf->iomem);

	/* aclk_g3d is the functional clock kbase uses
	 * (gpu_exynos7580.c:260); aclk_g3d_400 is the optional 400 MHz
	 * branch the DVFS code enables for high OPPs (:422).
	 */
	pf->clk_core = devm_clk_get(&pdev->dev, "aclk_g3d");
	if (IS_ERR(pf->clk_core)) {
		ret = PTR_ERR(pf->clk_core);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_err(&pdev->dev, "aclk_g3d missing: %d\n", ret);
		return ret;
	}

	pf->clk_bus = devm_clk_get(&pdev->dev, "aclk_g3d_400");
	if (IS_ERR(pf->clk_bus)) {
		if (PTR_ERR(pf->clk_bus) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_info(&pdev->dev, "aclk_g3d_400 not available, continuing\n");
		pf->clk_bus = NULL;
	}

	/* kbase looks the regulator up by name, not through a DT supply
	 * property (gpu_exynos7580.c:711), so match that. */
	pf->vdd = regulator_get(NULL, "vdd_g3d");
	if (IS_ERR(pf->vdd)) {
		ret = PTR_ERR(pf->vdd);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_info(&pdev->dev, "vdd_g3d not available (%d), continuing\n",
			 ret);
		pf->vdd = NULL;
	}

	pf->debugfs = debugfs_create_dir("panfrost7580", NULL);
	if (pf->debugfs) {
		debugfs_create_file("features", 0400, pf->debugfs, pf,
				    &pf_features_fops);
		debugfs_create_file("selftest", 0400, pf->debugfs, pf,
				    &pf_selftest_fops);
		debugfs_create_file("power", 0600, pf->debugfs, pf,
				    &pf_power_fops);
	}

	dev_info(&pdev->dev,
		 "Mali-T720 register window at %pR; kernel-side only (M0). "
		 "Nothing is submitted; no DRM node is created.\n", res);
	return 0;
}

static int pf7580_remove(struct platform_device *pdev)
{
	struct pf7580 *pf = platform_get_drvdata(pdev);

	debugfs_remove_recursive(pf->debugfs);

	mutex_lock(&pf->lock);
	pf7580_power_off(pf);
	mutex_unlock(&pf->lock);

	if (pf->vdd)
		regulator_put(pf->vdd);

	return 0;
}

static const struct of_device_id pf7580_of_match[] = {
	{ .compatible = "arm,mali-t720" },
	{ }
};
MODULE_DEVICE_TABLE(of, pf7580_of_match);

static struct platform_driver pf7580_driver = {
	.probe	= pf7580_probe,
	.remove	= pf7580_remove,
	.driver	= {
		.name		= PF7580_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= pf7580_of_match,
	},
};

module_platform_driver(pf7580_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nura GPU bring-up");
MODULE_DESCRIPTION("Mali-T720 M0 register/power/MMU bring-up for Exynos 7580 (no DRM)");
