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
 *
 * The callback is installed once from ->load() and removed in the unwind and
 * in ->unload(), never from enable_vblank()/disable_vblank(): the DRM core
 * calls those with dev->vbl_lock held (spinlock, IRQs off, drm_irq.c
 * drm_vblank_get/put), and decon_kms_register_vblank_cb() is documented as
 * process context.
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
	if (pipe != EXYNOS7580_PIPE)
		return -EINVAL;
	if (!to_exynos7580(drm))
		return -ENODEV;

	/* The FRAME interrupt is already armed from ->load(). */
	atomic_inc(&exynos7580_vblank_users);
	return 0;
}

static void exynos7580_disable_vblank(struct drm_device *drm, int pipe)
{
	if (pipe != EXYNOS7580_PIPE)
		return;

	atomic_dec(&exynos7580_vblank_users);
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

	/*
	 * int111: sg_set_buf() would take virt_to_page(cma_obj->vaddr).  For a
	 * multi-MB dma_alloc_coherent() buffer vaddr is vmalloc-mapped, so that
	 * page is meaningless and the importer (panfrost via kmsro) maps
	 * garbage into its address space -- the AS0 TRANSLATION_FAULT/READ
	 * storm in dmesg.  Use the physical base that drm_gem_cma_object
	 * already carries.
	 */
	pr_info("exynos7580_prime_get_sg_table: obj=%p import_attach=%p paddr=%pad vaddr=%p size=%zu\n",
		obj, obj->import_attach, &cma_obj->paddr, cma_obj->vaddr, obj->size);
	sg_set_page(sgt->sgl, phys_to_page(cma_obj->paddr), obj->size,
		    cma_obj->paddr & ~PAGE_MASK);
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
 * Atomic check/commit
 *
 * Both are implemented and exported by the CRTC slice
 * (exynos_drm7580_crtc.c): the fallible DECON/DSI bring-up has to run before
 * any plane state is prepared or swapped, and a NONBLOCK|PAGE_FLIP_EVENT
 * request (KWin flips) must run the same tail as a blocking one, so the
 * generic drm_atomic_helper_commit()/drm_atomic_helper_check() entry points
 * are not used directly here.  The window programming itself (one
 * decon_kms_commit() from the last plane hook) and the vblank-armed flip event
 * are owned by the plane and fence slices.
 *
 * Declared here rather than in the contract header: the header is frozen for
 * this slice, and KmsAssemble folds the two prototypes in when it assembles
 * the series.
 * ------------------------------------------------------------------------ */

int exynos7580_atomic_check(struct drm_device *drm,
			    struct drm_atomic_state *state);
int exynos7580_atomic_commit(struct drm_device *drm,
			     struct drm_atomic_state *state, bool async);

static const struct drm_mode_config_funcs exynos7580_mode_config_funcs = {
	.fb_create	= exynos7580_fb_create,
	.atomic_check	= exynos7580_atomic_check,
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
	struct drm_plane *plane = ex->crtc.primary;
	struct drm_plane_state *pstate;
	struct drm_framebuffer *fb = NULL;
	bool ok;
	int ret = 0;

	drm_modeset_lock_all(drm);

	/*
	 * int76: the postcondition must use the atomic state, not the legacy
	 * crtc->state->enable / plane->fb fields (the atomic commit path does
	 * not maintain them; int74 fixed the same mistake in the fbdev slice).
	 * Log every field so a real mismatch is unambiguous.
	 */
	pstate = plane ? plane->state : NULL;
	DRM_INFO("initial modeset state: crtc id=%u enabled=%d state=%p active=%d enable=%d planes_empty=%d primary=%p pstate=%p pstate->fb=%p legacy->fb=%p\n",
		 ex->crtc.base.id, ex->crtc.enabled, ex->crtc.state,
		 ex->crtc.state ? ex->crtc.state->active : -1,
		 ex->crtc.state ? ex->crtc.state->enable : -1,
		 list_empty(&drm->mode_config.plane_list), plane, pstate,
		 pstate ? pstate->fb : NULL, plane ? plane->fb : NULL);

	ok = ex->crtc.state && ex->crtc.state->active && ex->crtc.enabled &&
	     pstate && pstate->fb;
	if (!ok) {
		if (list_empty(&drm->mode_config.fb_list)) {
			DRM_ERROR("initial modeset: no framebuffer to attach\n");
			ret = -EIO;
		} else {
			fb = list_first_entry(&drm->mode_config.fb_list,
					      struct drm_framebuffer, head);
		}
	}
	drm_modeset_unlock_all(drm);
	if (ret || ok)
		return ret;

	/*
	 * int76: if the fb_helper path did not leave the primary plane
	 * attached to fb0, make the first frame explicit instead of failing:
	 * allocate a state, attach the fb to the primary plane and commit it
	 * through our atomic_commit (without the modeset locks held).
	 */
	{
		struct drm_atomic_state *state = drm_atomic_state_alloc(drm);
		struct drm_plane_state *sp;

		if (!state)
			return -ENOMEM;
		sp = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(sp)) {
			ret = PTR_ERR(sp);
			drm_atomic_state_free(state);
			return ret;
		}
		drm_atomic_set_fb_for_plane(sp, fb);
		DRM_INFO("initial modeset: explicit commit of fb %p on plane %p\n", fb, plane);
		ret = drm_atomic_commit(state);
		DRM_INFO("initial modeset: explicit commit ret=%d\n", ret);
		if (ret)
			return ret;
	}

	drm_modeset_lock_all(drm);
	pstate = plane ? plane->state : NULL;
	if (!ex->crtc.state || !ex->crtc.state->active || !ex->crtc.enabled ||
	    !pstate || pstate->fb != fb) {
		DRM_ERROR("initial modeset still inconsistent after the explicit commit: active=%d enabled=%d pstate->fb=%p want=%p\n",
			  ex->crtc.state ? ex->crtc.state->active : -1,
			  ex->crtc.enabled, pstate ? pstate->fb : NULL, fb);
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
	/*
	 * DECON's only hard limit is a 128-byte minimum stride, which
	 * exy7580_fb_check() enforces per format.  A 32x32 ARGB hardware
	 * cursor is exactly 128 bytes wide and must be accepted: KWin
	 * aborts when its cursor framebuffer cannot be created.
	 */
	drm->mode_config.min_width = 32;
	drm->mode_config.min_height = 32;
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

	dev_info(ex->dev, "exynos7580: load: dsi_init()\n");
	ret = exynos7580_dsi_init(ex);
	if (ret) {
		DRM_ERROR("DSI encoder/connector init failed: %d\n", ret);
		goto err_vblank;
	}
	if (!ex->connector.state && ex->connector.funcs->reset)
		ex->connector.funcs->reset(&ex->connector);

	/*
	 * Arm the FRAME interrupt before the first commit: the initial modeset
	 * below arms its flip event from vblank, so the counter must advance.
	 */
	dev_info(ex->dev, "exynos7580: load: registering DECON FRAME callback\n");
	ret = decon_kms_register_vblank_cb(ex->decon, exynos7580_vblank_isr, ex);
	if (ret) {
		DRM_ERROR("failed to arm DECON FRAME interrupt: %d\n", ret);
		goto err_vblank;
	}

	/*
	 * Last fallible step: this registers /dev/fb0 and runs the initial
	 * modeset.  exynos7580_fbdev_init() must leave no fb0 behind when it
	 * fails (otherwise the vendor fallback would only get fb1).
	 */
	dev_info(ex->dev, "exynos7580: load: fbdev_init() (card0 fb0)\n");
	ret = exynos7580_fbdev_init(ex);
	if (ret) {
		DRM_ERROR("fbdev init failed: %d\n", ret);
		goto err_vblank;
	}

	dev_info(ex->dev, "exynos7580: load: initial modeset\n");
	ret = exynos7580_initial_modeset(ex, drm);
	if (ret) {
		dev_err(ex->dev, "exynos7580: load: initial modeset failed (%d)\n", ret);
		goto err_vblank;
	}

	DRM_INFO("exynos7580: %s bound, card0 ready (atomic)\n",
		 DECON_COMPATIBLE);

	return 0;

err_vblank:
	decon_kms_register_vblank_cb(ex->decon, NULL, NULL);
	exynos7580_fbdev_fini();
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

	atomic_set(&exynos7580_vblank_users, 0);
	decon_kms_register_vblank_cb(ex->decon, NULL, NULL);

	/*
	 * fb0 first: drm_fbdev_cma_fini() destroys the fb helper and its
	 * private framebuffer, so it must not run after
	 * drm_mode_config_cleanup() has freed the CRTCs/planes/framebuffers it
	 * still points at (the devres action that used to do this runs after
	 * ->unload()).
	 */
	exynos7580_fbdev_fini();

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
	/*
	 * int84: DRM_PRIME alone does not fill DRM_CAP_PRIME; kmsro's first
	 * allocation step is drmPrimeHandleToFD(card0, dumb_handle), so the
	 * card0 drm_driver needs the two prime ioctl hooks as well (they were
	 * missing, hence DRM_CAP_PRIME=0x0 on int83).
	 */
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
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

/*
 * KWin opens card0 without O_NONBLOCK and reads DRM events from its main
 * loop.  drm_read() then sleeps until an event arrives; on this driver the
 * event queue can stay empty (no page-flip events), which wedges the
 * compositor: it stops servicing Wayland/DBus clients entirely.
 * Force non-blocking semantics for the read call so an empty queue returns
 * -EAGAIN and the caller's poll() keeps working.
 */
static ssize_t exynos7580_read(struct file *filp, char __user *buffer,
			       size_t count, loff_t *offset)
{
	struct drm_file *file_priv = filp->private_data;

	/*
	 * This tree's drm_read() has no O_NONBLOCK branch: it always sleeps
	 * in wait_event_interruptible().  KWin reads DRM events from its main
	 * loop and blocks there forever, so the compositor stops servicing
	 * Wayland clients.  Return -EAGAIN when the queue is empty instead.
	 */
	if (!file_priv || list_empty(&file_priv->event_list))
		return -EAGAIN;

	return drm_read(filp, buffer, count, offset);
}

static const struct file_operations exynos7580_fops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.poll		= drm_poll,
	.read		= exynos7580_read,
	.mmap		= drm_gem_cma_mmap,
};

/* ------------------------------------------------------------------------
 * platform_driver / late_initcall bind
 * ------------------------------------------------------------------------ */

static int exynos7580_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos7580_device *ex;
	const struct decon_kms_caps *caps;
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

	dev_info(dev, "exynos7580: probe entry (DECON node %s), binding card0\n",
		 of_node_full_name(dev->of_node));

	/*
	 * Hand-off with the vendor DECON core.  In KMS mode the vendor
	 * decon_probe() does not run (this driver owns the device), so the
	 * shim's decon_kms_get() stands the DECON up for us: clocks, SFR
	 * mapping, runtime PM/iovmm and the panel hand-off.  If it cannot, the
	 * probe fails here, before any DRM object exists.
	 */
	dev_info(dev, "exynos7580: probe: calling decon_kms_get()\n");
	ex->decon = decon_kms_get(dev);
	if (IS_ERR(ex->decon)) {
		ret = PTR_ERR(ex->decon);
		ex->decon = NULL;
		dev_err(dev, "exynos7580: probe: decon_kms_get failed (%d)\n", ret);
		if (ret == -ENODEV || ret == -EPROBE_DEFER) {
			/* exynos-mdev / DSIM / panel not up yet: retry later. */
			dev_info(dev, "exynos7580: DECON/DSIM not ready, deferring probe\n");
			ret = -EPROBE_DEFER;
		}
		goto err_drvdata;
	}
	caps = decon_kms_caps(ex->decon);
	dev_info(dev, "exynos7580: probe: decon_kms_get ok (%u windows, default %u)\n",
		 caps ? caps->max_win : 0, caps ? caps->default_win : 0);
	/*
	 * Publish our own drvdata only now.  decon_kms_get() reads
	 * platform_get_drvdata() and expects the vendor decon_device (or
	 * NULL): installing struct exynos7580_device before the call made it
	 * read a bogus kms_owned and reject the hand-off with -EBUSY
	 * ("DECON already owned by the fbdev driver").
	 */
	platform_set_drvdata(pdev, ex);
	dev_info(dev, "exynos7580: probe: decon ok, drvdata published\n");

	dev_info(dev, "exynos7580: probe: calling drm_platform_init()\n");
	ret = drm_platform_init(&exynos7580_drm_driver, pdev);
	if (ret) {
		dev_err(dev, "exynos7580: probe: drm_platform_init failed (%d)\n", ret);
		goto err_decon;
	}

	exynos7580_device = ex;
	dev_info(dev, "exynos7580: probe: success, card0 registered\n");

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

#ifdef MODULE
/*
 * int53: built as a loadable module.  module_init is level 6, which is
 * fine because nothing binds at boot: the module is only loaded by hand
 * after the vendor fbdev driver has been unbound, i.e. after the panel
 * driver and the vendor stack are fully up.  The built-in image keeps the
 * level-7 late_initcall so the vendor fbdev fallback ordering is
 * unchanged.
 */
static void __exit exynos7580_drm_exit(void)
{
	platform_driver_unregister(&exynos7580_drm_platform_driver);
}

module_init(exynos7580_drm_init);
module_exit(exynos7580_drm_exit);
#else
/*
 * int61: run at late_initcall_sync, after every late_initcall (the DSIM
 * driver, the panel driver and exynos-mdev all register at late_initcall or
 * earlier).  This is our driver, not the vendor DECON one, so the int58
 * hazard (moving the *vendor* fbdev driver to sync) does not apply: if the
 * panel/mdev is still absent the probe defers and the driver core retries.
 */
late_initcall_sync(exynos7580_drm_init);
#endif

MODULE_AUTHOR("gpu31-kms");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION("int121");
