/*
 * Samsung Exynos 7580 DECON DRM/KMS - driver registration, probe and bind
 *
 * This file is the integration point of the DECON KMS slices.  It owns:
 *
 *   - the drm_driver (DRIVER_ATOMIC | DRIVER_MODESET | DRIVER_GEM |
 *     DRIVER_PRIME) and its DRIVER_NAME, which must stay "exynos": Mesa's
 *     kmsro pairs /usr/lib/dri/exynos_dri.so with the render node by driver
 *     name (docs/reports/decon-kms-req.md, requirement 2);
 *   - the platform_driver that binds ONLY
 *     "samsung,exynos5-decon_driver" (never the dsim node, never a new
 *     compatible - the shipped dt.img is used unchanged);
 *   - registration of card0 through drm_platform_init(), at late_initcall;
 *   - the vblank plumbing (drm_vblank_init + get/enable/disable_vblank and
 *     the DECON FRAME interrupt trampoline) that the fence slice arms flip
 *     events from;
 *   - the ordered construction of the subdriver objects
 *     (planes -> crtc -> dsi encoder/connector -> fbdev) and their unwind.
 *
 * Binding rules (docs/reports/decon-kms-coexist.md 4.3):
 *
 *   - late_initcall, not module_platform_driver: level 7 in drivers/gpu/ is
 *     after the vendor DSIM/panel driver (level 7 in drivers/video/), so the
 *     panel is already lit when we probe, and it is before the vendor DECON
 *     fbdev driver, which was moved to late_initcall_sync and is the fallback
 *     when this probe returns an error;
 *   - every failure path returns an error to the driver core and unwinds what
 *     this file registered; it never marks the device bound, so the vendor
 *     `decon` fbdev driver can attach and restore today's /dev/fb0 and X11;
 *   - no device tree change.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_gem_cma_helper.h>

#include <linux/exynos_decon_kms.h>

#include "exynos_drm7580.h"

#define DRIVER_NAME		"exynos"
#define DRIVER_DESC		"Samsung Exynos 7580 DECON DRM/KMS"
#define DRIVER_DATE		"20261123"
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0
#define DRIVER_PATCHLEVEL	0

/*
 * The only node KMS may claim.  The DSIM node stays with the vendor
 * dsim_drv.c (level 7, drivers/video/, linked first) and the KMS DSI encoder
 * calls into it; claiming it here would take the panel away from both.
 */
#define DECON_COMPATIBLE	"samsung,exynos5-decon_driver"
#define DSIM_COMPATIBLE		"samsung,exynos5-mipi-dsi"

/* The DECON has one video pipe; the CRTC is created at index 0. */
#define EXYNOS7580_PIPE		0

/* The KMS device is a singleton: there is one decon_0 in the shipped DT. */
static struct exynos7580_device *exynos7580_device;

/*
 * Vblank users of the single pipe.  The DECON FRAME interrupt has one handler
 * owned by the vendor shim (decon_kms_register_vblank_cb()), so the DRM
 * refcount is kept here instead of in struct exynos7580_device, whose layout
 * is fixed by the shared contract header.
 */
static atomic_t exynos7580_vblank_users = ATOMIC_INIT(0);

static const struct file_operations exynos7580_fops;

/* ------------------------------------------------------------------------
 * vblank / DRM_IRQ plumbing
 *
 * The vendor DECON driver owns the interrupt; decon_kms_register_vblank_cb()
 * installs our callback on the FRAME interrupt and enables it while a callback
 * is registered.  DRM only needs the three driver hooks below plus a
 * drm_handle_vblank() call from the IRQ: get_vblank_counter() feeds
 * drm_vblank_count() (the core accounts for missed interrupts), which is what
 * drm_crtc_arm_vblank_event() in the fence slice timestamps its events with.
 * ------------------------------------------------------------------------ */

static u32 exynos7580_get_vblank_counter(struct drm_device *drm, int pipe)
{
	if (pipe != EXYNOS7580_PIPE)
		return 0;

	return drm_vblank_count(drm, pipe);
}

/*
 * Runs in hard IRQ context on every frame-done interrupt of the DECON.
 * Must not sleep and must not call back into the decon_kms API.
 */
static void exynos7580_vblank_isr(void *data)
{
	struct exynos7580_device *ex = data;

	if (!ex || !ex->dev)
		return;

	drm_handle_vblank(ex->drm, EXYNOS7580_PIPE);
	exynos7580_vblank_irq(ex->drm, EXYNOS7580_PIPE);
}

static int exynos7580_enable_vblank(struct drm_device *drm, int pipe)
{
	struct exynos7580_device *ex = to_exynos7580(drm);
	int ret;

	if (pipe != EXYNOS7580_PIPE)
		return -EINVAL;
	if (!ex || !ex->decon)
		return -ENODEV;

	if (atomic_inc_return(&exynos7580_vblank_users) > 1)
		return 0;

	ret = decon_kms_register_vblank_cb(ex->decon, exynos7580_vblank_isr, ex);
	if (ret) {
		atomic_dec(&exynos7580_vblank_users);
		DRM_ERROR("failed to arm DECON FRAME interrupt: %d\n", ret);
		return ret;
	}

	return 0;
}

static void exynos7580_disable_vblank(struct drm_device *drm, int pipe)
{
	struct exynos7580_device *ex = to_exynos7580(drm);

	if (pipe != EXYNOS7580_PIPE || !ex || !ex->decon)
		return;

	if (!atomic_dec_and_test(&exynos7580_vblank_users))
		return;

	decon_kms_register_vblank_cb(ex->decon, NULL, NULL);
}

/* ------------------------------------------------------------------------
 * PRIME export of CMA (dumb/scanout) buffers
 *
 * KWin creates its scanout buffers through CREATE_DUMB and then exports them
 * with drmPrimeHandleToFD() for the GPU (docs/reports/decon-kms-req.md,
 * requirement 21).  The generic 3.10 helper drm_gem_prime_export() needs the
 * sg-table/vmap callbacks below; import of foreign buffers is the dmabuf
 * slice's exynos7580_gem_prime_import(), see exynos_drm7580.h.
 *
 * These callbacks describe drm_gem_cma_object buffers.  That is exactly what
 * CREATE_DUMB allocates here; a buffer imported from another exporter is not
 * re-exported through this path by any supported user.
 * ------------------------------------------------------------------------ */

static struct sg_table *exynos7580_gem_prime_get_sg_table(
					struct drm_gem_object *obj)
{
	struct drm_gem_cma_object *cma_obj = to_drm_gem_cma_obj(obj);
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	sg_dma_address(sgt->sgl) = cma_obj->paddr;
	sg_dma_len(sgt->sgl) = obj->size;

	return sgt;
}

static void *exynos7580_gem_prime_vmap(struct drm_gem_object *obj)
{
	return to_drm_gem_cma_obj(obj)->vaddr;
}

static void exynos7580_gem_prime_vunmap(struct drm_gem_object *obj,
					void *vaddr)
{
}

/* ------------------------------------------------------------------------
 * Atomic commit
 *
 * The generic drm_atomic_helper_commit() is deliberately not used: the DECON
 * link enable (DSIM stream on, DECON clocks, iovmm) can fail, and it has to
 * happen before any plane state is prepared or swapped so that a failure
 * leaves userspace's previous state untouched.  The window programming itself
 * (one decon_kms_commit() from the last plane hook) and the vblank-armed flip
 * event are owned by the plane and fence slices respectively.
 * ------------------------------------------------------------------------ */

static int exynos7580_atomic_commit(struct drm_device *drm,
				    struct drm_atomic_state *state, bool async)
{
	struct exynos7580_device *ex = to_exynos7580(drm);
	int ret;

	if (async)
		return -EBUSY;
	if (!ex)
		return -ENODEV;

	ret = exynos7580_crtc_enable_link(ex);
	if (ret) {
		DRM_ERROR("DECON link enable failed: %d\n", ret);
		return ret;
	}

	ret = drm_atomic_helper_prepare_planes(drm, state);
	if (ret)
		return ret;

	drm_atomic_helper_swap_state(drm, state);

	exynos7580_commit_tail(state);

	return 0;
}

static const struct drm_mode_config_funcs exynos7580_mode_config_funcs = {
	.fb_create	= exynos7580_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= exynos7580_atomic_commit,
};

/* ------------------------------------------------------------------------
 * Initial modeset
 *
 * The fbdev slice owns fb0 and the drm_fb_helper behind it; its
 * exynos7580_fbdev_init() performs the coexist-mandated
 * drm_modeset_lock_all() + drm_fb_helper_restore_fbdev_mode() sequence
 * (through drm_fbdev_cma_restore_mode()) and returns an error when fb0 did
 * not register or the CRTC does not scan it out.  Main cannot repeat that
 * call: the helper is not part of the shared contract and the fence/plane
 * slices own the commit path.  What main does own is the postcondition - a
 * modeset that registered fb0 but left the CRTC inactive is a probe failure,
 * because the vendor fallback would otherwise find its own fb0 already taken.
 * ------------------------------------------------------------------------ */

static int exynos7580_initial_modeset(struct exynos7580_device *ex,
				      struct drm_device *drm)
{
	int ret = 0;

	drm_modeset_lock_all(drm);

	if (!ex->crtc.state || !ex->crtc.state->enable) {
		DRM_ERROR("CRTC is not active after the initial modeset\n");
		ret = -EIO;
	} else if (!ex->crtc.primary || !ex->crtc.primary->fb) {
		DRM_ERROR("primary plane has no fb0 after the initial modeset\n");
		ret = -EIO;
	}

	drm_modeset_unlock_all(drm);

	return ret;
}

/* ------------------------------------------------------------------------
 * load / unload
 * ------------------------------------------------------------------------ */

static int exynos7580_load(struct drm_device *drm, unsigned long flags)
{
	struct exynos7580_device *ex;
	int ret;

	/*
	 * The DRM core allocates the drm_device (drm_platform_init() ->
	 * drm_get_platform_dev()); we only keep a pointer to it and publish our
	 * own state through dev_private, which is what to_exynos7580() reads.
	 */
	ex = platform_get_drvdata(drm->platformdev);
	if (!ex)
		return -ENODEV;

	ex->drm = drm;
	ex->dev = &drm->platformdev->dev;
	drm->dev_private = ex;

	drm_mode_config_init(drm);
	drm->mode_config.min_width = 64;
	drm->mode_config.min_height = 64;
	drm->mode_config.max_width = 2048;
	drm->mode_config.max_height = 2048;
	drm->mode_config.funcs = &exynos7580_mode_config_funcs;

	ret = drm_vblank_init(drm, 1);
	if (ret) {
		DRM_ERROR("drm_vblank_init failed: %d\n", ret);
		goto err_config;
	}

	/* Planes first: the CRTC is created around planes[0]. */
	ret = exynos7580_planes_init(ex);
	if (ret) {
		DRM_ERROR("plane init failed: %d\n", ret);
		goto err_vblank;
	}

	ret = exynos7580_crtc_init(ex);
	if (ret) {
		DRM_ERROR("CRTC init failed: %d\n", ret);
		goto err_vblank;
	}

	/*
	 * Allocate the per-object atomic states.  The reset callback of the
	 * connector is called by hand after dsi_init() below:
	 * drm_mode_config_reset() forces connector->status to unknown, and
	 * only the DSI encoder knows the panel is connected.
	 */
	drm_mode_config_reset(drm);

	ret = exynos7580_dsi_init(ex);
	if (ret) {
		DRM_ERROR("DSI encoder/connector init failed: %d\n", ret);
		goto err_vblank;
	}
	if (!ex->connector.state && ex->connector.funcs->reset)
		ex->connector.funcs->reset(&ex->connector);

	/*
	 * Last fallible step: this registers /dev/fb0 and runs the initial
	 * modeset.  exynos7580_fbdev_init() must leave no fb0 behind when it
	 * fails (otherwise the vendor fallback would only get fb1).
	 */
	ret = exynos7580_fbdev_init(ex);
	if (ret) {
		DRM_ERROR("fbdev init failed: %d\n", ret);
		goto err_vblank;
	}

	ret = exynos7580_initial_modeset(ex, drm);
	if (ret)
		goto err_vblank;

	DRM_INFO("exynos7580: %s bound, card0 ready (atomic)\n",
		 DECON_COMPATIBLE);

	return 0;

err_vblank:
	drm_vblank_cleanup(drm);
err_config:
	drm_mode_config_cleanup(drm);
	drm->dev_private = NULL;
	ex->drm = NULL;
	return ret;
}

static int exynos7580_unload(struct drm_device *drm)
{
	struct exynos7580_device *ex = drm->dev_private;

	if (!ex)
		return 0;

	if (!atomic_dec_and_test(&exynos7580_vblank_users))
		atomic_set(&exynos7580_vblank_users, 0);
	decon_kms_register_vblank_cb(ex->decon, NULL, NULL);

	drm_vblank_cleanup(drm);
	drm_mode_config_cleanup(drm);

	if (ex->decon) {
		decon_kms_put(ex->decon);
		ex->decon = NULL;
	}

	drm->dev_private = NULL;
	ex->drm = NULL;

	return 0;
}

/* ------------------------------------------------------------------------
 * drm_driver
 * ------------------------------------------------------------------------ */

static struct drm_driver exynos7580_drm_driver = {
	.driver_features	= DRIVER_ATOMIC | DRIVER_MODESET |
				  DRIVER_GEM | DRIVER_PRIME,
	.load			= exynos7580_load,
	.unload			= exynos7580_unload,
	.get_vblank_counter	= exynos7580_get_vblank_counter,
	.enable_vblank		= exynos7580_enable_vblank,
	.disable_vblank		= exynos7580_disable_vblank,
	.gem_free_object	= drm_gem_cma_free_object,
	.gem_vm_ops		= &drm_gem_cma_vm_ops,
	.gem_prime_export	= drm_gem_prime_export,
	.gem_prime_get_sg_table	= exynos7580_gem_prime_get_sg_table,
	.gem_prime_vmap		= exynos7580_gem_prime_vmap,
	.gem_prime_vunmap	= exynos7580_gem_prime_vunmap,
	.gem_prime_import	= exynos7580_gem_prime_import,
	.dumb_create		= drm_gem_cma_dumb_create,
	.dumb_map_offset	= drm_gem_cma_dumb_map_offset,
	.dumb_destroy		= drm_gem_cma_dumb_destroy,
	.fops			= &exynos7580_fops,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.date			= DRIVER_DATE,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	.patchlevel		= DRIVER_PATCHLEVEL,
};

static const struct file_operations exynos7580_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.poll		= drm_poll,
	.read		= drm_read,
	.mmap		= drm_gem_cma_mmap,
};

/* ------------------------------------------------------------------------
 * platform_driver / late_initcall bind
 * ------------------------------------------------------------------------ */

static int exynos7580_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos7580_device *ex;
	int ret;

	/* Match the DECON node only, and never the dsim/panel nodes. */
	if (!dev->of_node)
		return -ENODEV;
	if (!of_device_is_compatible(dev->of_node, DECON_COMPATIBLE))
		return -ENODEV;
	if (of_device_is_compatible(dev->of_node, DSIM_COMPATIBLE))
		return -ENODEV;
	if (!of_device_is_available(dev->of_node))
		return -ENODEV;

	ex = devm_kzalloc(dev, sizeof(*ex), GFP_KERNEL);
	if (!ex)
		return -ENOMEM;

	ex->dev = dev;
	platform_set_drvdata(pdev, ex);

	/*
	 * Hand-off with the vendor DECON core.  In KMS mode the vendor
	 * decon_probe() does not run (this driver owns the device), so the
	 * shim's decon_kms_get() stands the DECON up for us: clocks, SFR
	 * mapping, runtime PM/iovmm and the panel hand-off.  If it cannot, the
	 * probe fails here, before any DRM object exists.
	 */
	ex->decon = decon_kms_get(dev);
	if (IS_ERR(ex->decon)) {
		ret = PTR_ERR(ex->decon);
		ex->decon = NULL;
		dev_err(dev, "DECON handle unavailable (%d), vendor fbdev fallback\n",
			ret);
		goto err_drvdata;
	}

	ret = drm_platform_init(&exynos7580_drm_driver, pdev);
	if (ret) {
		dev_err(dev, "drm_platform_init failed (%d), vendor fbdev fallback\n",
			ret);
		goto err_decon;
	}

	exynos7580_device = ex;

	return 0;

err_decon:
	decon_kms_put(ex->decon);
	ex->decon = NULL;
err_drvdata:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static int exynos7580_remove(struct platform_device *pdev)
{
	exynos7580_device = NULL;

	/*
	 * drm_platform_exit() -> drm_put_dev() -> ->unload() releases the
	 * CRTC/planes/connector, the vblank core and the DECON handle.
	 */
	drm_platform_exit(&exynos7580_drm_driver, pdev);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id exynos7580_drm_dt_match[] = {
	{ .compatible = DECON_COMPATIBLE },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos7580_drm_dt_match);

static struct platform_driver exynos7580_drm_platform_driver = {
	.probe	= exynos7580_probe,
	.remove	= exynos7580_remove,
	.driver	= {
		.name		= "exynos7580-decon-drm",
		.owner		= THIS_MODULE,
		.of_match_table	= exynos7580_drm_dt_match,
	},
};

/*
 * Level 7: after the vendor DSIM/panel driver (late_initcall in
 * drivers/video/, linked before drivers/gpu/), before the vendor DECON fbdev
 * driver, which is late_initcall_sync in the KMS image and binds only if this
 * probe returned an error.  Never module_platform_driver: that is level 6,
 * before the panel exists.
 */
static int __init exynos7580_drm_init(void)
{
	return platform_driver_register(&exynos7580_drm_platform_driver);
}
late_initcall(exynos7580_drm_init);

MODULE_AUTHOR("gpu31-kms");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
