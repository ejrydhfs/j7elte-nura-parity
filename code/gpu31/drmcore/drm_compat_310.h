#ifndef _DRM_COMPAT_310_H_
#define _DRM_COMPAT_310_H_
/*
 * drm_compat_310.h — minimal 3.10 compatibility header for the panfrost
 * v5.2 bring-up on Nura 3.10.108 (Exynos 7580 / Mali-T720, j7elte).
 *
 * Scope (agreed with G31_PanfrostTarget / G31_Gem / G31_Kms):
 *  - readl_poll_timeout() helper (3.10 has readl() but no iopoll.h).
 *  - DEFINE_DRM_GEM_FOPS() equivalent wiring drm_open/release/ioctl/
 *    mmap/poll/read to the driver-supplied mmap handler.
 *  - DRM_RENDER_ALLOW mapping: 3.10 has no RENDER_ALLOW bit. Plain
 *    "DRM_UNLOCKED" preserves the v5.2 per-ioctl locking shape
 *    (SUBMIT keeps DRM_AUTH so the legacy auth gate still applies);
 *    render-minor read access is enforced by G31_Kms' Mode-B core
 *    changes (render node is always-authenticated + DRM_MASTER-on-
 *    render denied there), NOT by this header.
 *  - Platform registration: use 3.10 drm_platform_init()/drm_platform_exit()
 *    (drmP.h, EXPORT_SYMBOL in drm_platform.c). No drm_dev_alloc/
 *    drm_dev_register backport — 3.10 embeds struct drm_device inside the
 *    legacy attach path (drm_fill_in_dev + drm_get_minor inside
 *    drm_get_platform_dev).
 *  - GEM: reuse present drm_gem_cma_* helpers (272-line driver +
 *    drm_gem_mmap/mmap_offset, EXPORT_SYMBOL). No drm_gem_shmem_* port
 *    (G31_Gem owns BO backing; CMA pattern confirmed present).
 *  - Present-verified only, NOT ported: drm_mm (incl. color_adjust),
 *    iova.c, dma-buf prime import/export, devfreq+old OPP, vendor
 *    pm_domains notifier (replaces genpd/device-link, which are deleted).
 *
 * This is a NEW file (no tree modification). It compiles standalone against
 * the 3.10 headers; the panfrost driver #includes it from its own sources.
 * Userspace UAPI (drivers/gpu/drm/panfrost/panfrost_drm.h, byte-identical to
 * v5.2) is owned by G31_Gem.
 */
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <drm/drmP.h>
#include <drm/drm_gem_cma_helper.h>

/*
 * readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)
 * Mainline semantic subset (include/linux/iopoll.h, absent in 3.10):
 * evaluates (val = readl(addr)) until (cond) is true; sleeps sleep_us
 * (0 = cpu_relax spin) between polls; times out after timeout_us.
 * Returns 0 on success, -ETIMEDOUT on expiry. M0 (panfrost7580.c pf_poll)
 * uses the same udelay-loop shape; this macro generalises it.
 */
#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)	\
({									\
	unsigned long __timeout = jiffies +				\
		usecs_to_jiffies(timeout_us);				\
	int __ret = -ETIMEDOUT;						\
	might_sleep_if(sleep_us);					\
	for (;;) {							\
		(val) = readl(addr);					\
		if (cond)						\
			{ __ret = 0; break; }			\
		if (time_after(jiffies, __timeout))			\
			break;						\
		if (sleep_us)						\
			usleep_range((sleep_us), (sleep_us) * 2);	\
		else							\
			cpu_relax();					\
	}								\
	__ret;								\
})

/*
 * DEFINE_DRM_GEM_FOPS(name, mmap_fn) — 3.10 spelling of the v5.2
 * DEFINE_DRM_GEM_*_FOPS helpers (absent in 3.10). Wires the standard DRM
 * entry points; the driver supplies only its GEM mmap handler
 * (drm_gem_cma_mmap for CMA-backed objects, cf. exynos fops at
 * drivers/gpu/drm/exynos/exynos_drm_drv.c:249-260 which open-codes the
 * same table with an exynos-specific mmap). .owner must be THIS_MODULE
 * (same as exynos); drm_compat_ioctl exists under CONFIG_COMPAT
 * (set: autoconf.h:76) and must be guarded for !COMPAT builds.
 */
#define DEFINE_DRM_GEM_FOPS(name, mmap_fn)				\
static const struct file_operations name = {				\
	.owner		= THIS_MODULE,					\
	.open		= drm_open,					\
	.release	= drm_release,					\
	.unlocked_ioctl	= drm_ioctl,					\
	.mmap		= (mmap_fn),					\
	.poll		= drm_poll,					\
	.read		= drm_read,					\
	.llseek		= noop_llseek,					\
}

/* Same, with 32-bit compat ioctl entry (cf. exynos #ifdef CONFIG_COMPAT). */
#define DEFINE_DRM_GEM_FOPS_COMPAT(name, mmap_fn)			\
static const struct file_operations name = {				\
	.owner		= THIS_MODULE,					\
	.open		= drm_open,					\
	.release	= drm_release,					\
	.unlocked_ioctl	= drm_ioctl,					\
	.mmap		= (mmap_fn),					\
	.poll		= drm_poll,					\
	.read		= drm_read,					\
	.llseek		= noop_llseek,					\
}

/*
 * DRM_RENDER_ALLOW — v5.2 flag for render-node ioctls (SUBMIT additionally
 * DRM_AUTH). 3.10 drm_ioctl() has no render concept: gates are DRM_AUTH /
 * DRM_MASTER / DRM_ROOT_ONLY / DRM_CONTROL_ALLOW / DRM_UNLOCKED, and the
 * render node enforcement lives in G31_Kms' Mode-B core changes. Map to
 * DRM_UNLOCKED so the locking shape of the v5.2 table is preserved
 * verbatim (every panfrost ioctl is unlocked in v5.2); SUBMIT keeps
 * DRM_AUTH so the legacy authenticated gate still applies until the
 * render-node cutover re-evaluates it.
 */
#ifndef DRM_RENDER_ALLOW
#define DRM_RENDER_ALLOW	DRM_UNLOCKED
#endif

/*
 * Platform registration reminder (no code needed beyond drmP.h):
 *   probe:  pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
 *           driver.num_ioctls = ARRAY_SIZE(table);
 *           return drm_platform_init(&driver, pdev);
 *   remove: drm_platform_exit(&driver, pdev);
 * (cf. exynos_drm_platform_probe/remove, exynos_drm_drv.c:290-310).
 * drm_platform_init() internally runs drm_fill_in_dev + MODESET control
 * minor + PRIMARY legacy minor + ->load(); the render minor is allocated
 * by the Mode-B core change via drm_get_minor(dev, &render, DRM_MINOR_RENDER)
 * (sysfs renders as renderD%d, drm_sysfs.c:513) — see panfrost_ioctls_310.h.
 */

#endif /* _DRM_COMPAT_310_H_ */
