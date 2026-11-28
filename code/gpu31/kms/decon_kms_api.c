/*
 * decon_kms_api.c - reuse shim between the vendor DECON window engine and the
 * DECON DRM/KMS subdriver.
 *
 * Interface: <linux/exynos_decon_kms.h> (contract, slice DeconApi).
 *
 * Nothing in here changes vendor behaviour: every entry point either wraps an
 * existing vendor function or performs the same steps the vendor probe/fbdev
 * paths perform.  The vendor fbdev driver keeps working untouched when no DRM
 * client calls this file.
 *
 * Two layers:
 *
 *   layer A (decon_kms_*)   the stable, fb-free surface: attach/detach the
 *                           DECON, enable/disable the pipe, commit one frame
 *                           from DMA addresses, vblank, runtime PM, dma-buf
 *                           mapping.  This is what the DRM subdriver uses.
 *   layer B (decon_export_*) raw vendor symbols for callers that need to drive
 *                           the register-level API themselves.
 *
 * Locking.  The vendor code has three locks and this file respects them
 * without adding an order of its own:
 *
 *   decon->output_lock   serialises enable/disable and the frame commit; taken
 *                        by decon_enable()/decon_disable() themselves and by
 *                        decon_kms_commit()/decon_kms_commit_async() around
 *                        the register programming.
 *   decon->mutex         of the update kthread, only touched by pm_runtime.
 *   kms_lock             this file's own mutex: protects the single-instance
 *                        handle, its refcount and the dma-buf mapping list.
 *                        It is never held while taking a DRM modeset lock, so
 *                        the only nesting is  drm modeset lock -> kms_lock ->
 *                        decon->output_lock, which cannot cycle.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <linux/ion.h>
#include <linux/exynos_ion.h>
#include <linux/exynos_iovmm.h>
#include <linux/fb.h>
#include <linux/debugfs.h>
#include <linux/v4l2-subdev.h>
#include <linux/exynos_decon_kms.h>

#include "../../../staging/android/sw_sync.h"

#include "decon.h"
#include "decon_helper.h"

struct decon_kms_dev {
	struct decon_device	*decon;
	struct decon_kms_caps	caps;
	decon_kms_vblank_cb	vblank_cb;
	void			*vblank_data;
	unsigned int		refcount;
};

/* DECON is a single-instance driver (get_decon_drvdata() ignores its id). */
static struct decon_kms_dev *kms_instance;
static DEFINE_MUTEX(kms_lock);

static void decon_kms_arm_frame(struct decon_device *decon, const char *where);

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */

static int decon_kms_to_vendor(const struct decon_kms_win *win,
			       struct decon_win_config *cfg)
{
	enum decon_pixel_format format;

	memset(cfg, 0, sizeof(*cfg));

	switch (win->state) {
	case DECON_KMS_WIN_DISABLED:
		cfg->state = DECON_WIN_STATE_DISABLED;
		return 0;
	case DECON_KMS_WIN_COLOR:
		cfg->state = DECON_WIN_STATE_COLOR;
		cfg->color = win->color;
		break;
	case DECON_KMS_WIN_BUFFER:
		format = decon_fourcc_to_format(win->fourcc);
		if (format >= DECON_PIXEL_FORMAT_MAX) {
			decon_err("unsupported fourcc 0x%08x\n", win->fourcc);
			return -EINVAL;
		}
		cfg->state = DECON_WIN_STATE_BUFFER;
		cfg->format = format;
		cfg->fd_idma[0] = -1;
		cfg->fd_idma[1] = -1;
		cfg->fd_idma[2] = -1;
		cfg->fence_fd = -1;
		cfg->plane_alpha = win->plane_alpha;
		cfg->blending = win->blending;
		cfg->idma_type = win->idma_type;
		cfg->block_area.x = win->block_area.x;
		cfg->block_area.y = win->block_area.y;
		cfg->block_area.w = win->block_area.w;
		cfg->block_area.h = win->block_area.h;
		cfg->src.x = win->src.x;
		cfg->src.y = win->src.y;
		cfg->src.w = win->src.w;
		cfg->src.h = win->src.h;
		cfg->src.f_w = win->src.f_w;
		cfg->src.f_h = win->src.f_h;
		break;
	default:
		return -EINVAL;
	}

	cfg->dst.x = win->dst.x;
	cfg->dst.y = win->dst.y;
	cfg->dst.w = win->dst.w;
	cfg->dst.h = win->dst.h;
	cfg->dst.f_w = win->dst.f_w;
	cfg->dst.f_h = win->dst.f_h;
	cfg->protection = win->protection;

	return 0;
}

/*
 * Build one frame in @regs.  Shared by the synchronous and the queued commit.
 * The caller holds decon->output_lock.
 */
static int decon_kms_program(struct decon_device *decon,
			     struct decon_reg_data *regs,
			     const struct decon_kms_win *wins,
			     unsigned int nr_wins)
{
	struct decon_win_config cfg[MAX_DECON_WIN];
	unsigned int i;
	int ret;

	if (!decon->pdata || !nr_wins ||
	    nr_wins > decon->pdata->max_win ||
	    decon->pdata->max_win > MAX_DECON_WIN)
		return -EINVAL;

	for (i = 0; i < decon->pdata->max_win; i++) {
		if (i >= nr_wins) {
			memset(&cfg[i], 0, sizeof(cfg[i]));
			cfg[i].state = DECON_WIN_STATE_DISABLED;
			continue;
		}

		ret = decon_kms_to_vendor(&wins[i], &cfg[i]);
		if (ret)
			return ret;

		if (wins[i].state == DECON_KMS_WIN_BUFFER) {
			ret = decon_export_regs_param_addr(decon, regs, i,
							   &wins[i]);
			if (ret)
				return ret;
		}
	}

	/* int93: one block per commit describing exactly what will be
	 * programmed; compare a working kmscube commit with KWin's. */
	decon_info("decon_kms_program: nwin=%u max_win=%u num_of_window(pre)=%u\n",
		   nr_wins, decon->pdata->max_win, regs->num_of_window);
	for (i = 0; i < nr_wins; i++) {
		decon_info("decon_kms_program: win[%u] kms_state=%u vendor_state=%u fourcc=0x%08x format=%u idma=%u blend=%u alpha=%d addr=0x%llx,0x%llx,0x%llx src=%d,%d %ux%u f=%ux%u dst=%d,%d %ux%u\n",
			   i, wins[i].state, cfg[i].state, wins[i].fourcc,
			   cfg[i].format, cfg[i].idma_type, cfg[i].blending,
			   cfg[i].plane_alpha,
			   (unsigned long long)wins[i].addr[0],
			   (unsigned long long)wins[i].addr[1],
			   (unsigned long long)wins[i].addr[2],
			   wins[i].src.x, wins[i].src.y, wins[i].src.w,
			   wins[i].src.h, wins[i].src.f_w, wins[i].src.f_h,
			   wins[i].dst.x, wins[i].dst.y, wins[i].dst.w,
			   wins[i].dst.h);
	}

	ret = decon_export_set_win_config(decon, regs, cfg,
					  decon->pdata->max_win);
	decon_info("decon_kms_program: set_win_config rc=%d num_of_window=%u wincon0=0x%08x wincon1=0x%08x wincon2=0x%08x\n",
		   ret, regs->num_of_window, regs->wincon[0],
		   regs->wincon[1] ? regs->wincon[1] : 0,
		   regs->wincon[2] ? regs->wincon[2] : 0);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* dma-buf mapping (exporter agnostic; the vendor path is ION-only)          */
/* ------------------------------------------------------------------------ */

struct decon_kms_map {
	struct list_head	list;
	struct dma_buf		*buf;
	struct dma_buf_attachment *att;
	struct sg_table		*sgt;
	dma_addr_t		addr;
	bool			iommu_mapped;
};

static LIST_HEAD(kms_maps);

int decon_kms_map_dmabuf(struct decon_kms_dev *d, struct dma_buf *buf,
			 unsigned int id, size_t size, dma_addr_t *addr,
			 void **cookie)
{
	struct decon_kms_map *m;
	struct sg_table *sgt;
	struct dma_buf_attachment *att;
	size_t len;
	int ret;

	if (!d || !buf || !addr || !cookie)
		return -EINVAL;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	att = dma_buf_attach(buf, d->decon->dev);
	if (IS_ERR(att)) {
		ret = PTR_ERR(att);
		goto err_free;
	}

	sgt = dma_buf_map_attachment(att, DMA_TO_DEVICE);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	len = size ? size : buf->size;

	m->addr = iovmm_map(d->decon->dev, sgt->sgl, 0, len,
			    DMA_TO_DEVICE, id);
	if (IS_ERR_VALUE(m->addr)) {
		/*
		 * No IOMMU domain on this device: fall back to the physical
		 * address of the first chunk, which is what the hardware sees
		 * when the DECON is not behind a sysmmu.
		 */
		if ((long)m->addr == -ENOSYS || (long)m->addr == -ENODEV) {
			m->addr = sg_dma_address(sgt->sgl);
			m->iommu_mapped = false;
		} else {
			ret = (int)m->addr;
			goto err_unmap;
		}
	} else {
		m->iommu_mapped = true;
	}

	/*
	 * Keep the buffer alive for as long as the mapping exists: a caller may
	 * drop its own reference right after the commit.
	 */
	get_dma_buf(buf);

	m->buf = buf;
	m->att = att;
	m->sgt = sgt;

	mutex_lock(&kms_lock);
	list_add_tail(&m->list, &kms_maps);
	mutex_unlock(&kms_lock);

	*addr = m->addr;
	*cookie = m;

	return 0;

err_unmap:
	dma_buf_unmap_attachment(att, sgt, DMA_TO_DEVICE);
err_detach:
	dma_buf_detach(buf, att);
err_free:
	kfree(m);
	return ret;
}
EXPORT_SYMBOL_GPL(decon_kms_map_dmabuf);

void decon_kms_unmap_dmabuf(struct decon_kms_dev *d, void *cookie)
{
	struct decon_kms_map *m = cookie;

	if (!d || !m)
		return;

	mutex_lock(&kms_lock);
	list_del(&m->list);
	mutex_unlock(&kms_lock);

	if (m->iommu_mapped)
		iovmm_unmap(d->decon->dev, m->addr);

	dma_buf_unmap_attachment(m->att, m->sgt, DMA_TO_DEVICE);
	dma_buf_detach(m->buf, m->att);
	dma_buf_put(m->buf);
	kfree(m);
}
EXPORT_SYMBOL_GPL(decon_kms_unmap_dmabuf);

/* ------------------------------------------------------------------------ */
/* attach / detach                                                          */
/* ------------------------------------------------------------------------ */

const struct decon_kms_caps *decon_kms_caps(struct decon_kms_dev *d)
{
	return d ? &d->caps : NULL;
}
EXPORT_SYMBOL_GPL(decon_kms_caps);

struct decon_kms_dev *decon_kms_get(struct device *decon_dev)
{
	struct platform_device *pdev;
	struct decon_device *decon;
	struct decon_kms_dev *d;
	int ret;

	if (!decon_dev || !decon_dev->of_node)
		return ERR_PTR(-ENODEV);

	pdev = to_platform_device(decon_dev);

	mutex_lock(&kms_lock);

	d = kms_instance;
	if (d) {
		if (d->decon->dev != decon_dev) {
			mutex_unlock(&kms_lock);
			return ERR_PTR(-EBUSY);
		}
		d->refcount++;
		mutex_unlock(&kms_lock);
		return d;
	}

	decon = platform_get_drvdata(pdev);
	if (decon && !decon->kms_owned) {
		/*
		 * The vendor fbdev probe already owns the pipe (coexistence
		 * fallback).  Two owners of one DECON is exactly what
		 * decon-kms-coexist.md forbids.
		 */
		dev_err(decon_dev, "DECON already owned by the fbdev driver\n");
		mutex_unlock(&kms_lock);
		return ERR_PTR(-EBUSY);
	}

	if (!decon) {
		/*
		 * KMS binds samsung,exynos5-decon_driver itself, so the vendor
		 * probe never ran: bring the controller up here with the shared
		 * probe body (clocks, IRQ, media entity, windows, kthread,
		 * pm_runtime) and without a framebuffer.
		 */
		ret = decon_probe_common(pdev, false);
		if (ret) {
			dev_err(decon_dev, "DECON KMS bring-up failed: %d\n", ret);
			mutex_unlock(&kms_lock);
			return ERR_PTR(ret);
		}

		decon = platform_get_drvdata(pdev);
		if (!decon) {
			mutex_unlock(&kms_lock);
			return ERR_PTR(-ENODEV);
		}
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d) {
		mutex_unlock(&kms_lock);
		return ERR_PTR(-ENOMEM);
	}

	d->decon = decon;
	d->refcount = 1;
	d->caps.max_win = decon->pdata->max_win;
	d->caps.default_win = decon->pdata->default_win;
	d->caps.video_mode = (decon->pdata->psr_mode == DECON_VIDEO_MODE);
	d->caps.sw_trigger = (decon->pdata->trig_mode == DECON_SW_TRIG);

	kms_instance = d;

	mutex_unlock(&kms_lock);

	dev_info(decon_dev, "DECON handed to KMS (%ux%u, %u windows)\n",
		 decon->lcd_info ? decon->lcd_info->xres : 0,
		 decon->lcd_info ? decon->lcd_info->yres : 0,
		 d->caps.max_win);

	return d;
}
EXPORT_SYMBOL_GPL(decon_kms_get);

/*
 * Tear down what decon_kms_get() built.  Mirrors the unwind of the vendor
 * probe (decon_core.c fail_* labels) so that the vendor fbdev driver can bind
 * the same device afterwards if the DRM probe fails.
 */
static void decon_kms_teardown(struct decon_kms_dev *d)
{
	struct decon_device *decon = d->decon;
	struct platform_device *pdev = to_platform_device(decon->dev);
	struct decon_kms_map *m, *tmp;

	mutex_lock(&kms_lock);
	list_for_each_entry_safe(m, tmp, &kms_maps, list) {
		list_del(&m->list);
		if (m->iommu_mapped)
			iovmm_unmap(decon->dev, m->addr);
		dma_buf_unmap_attachment(m->att, m->sgt, DMA_TO_DEVICE);
		dma_buf_detach(m->buf, m->att);
		dma_buf_put(m->buf);
		kfree(m);
	}
	mutex_unlock(&kms_lock);

	decon->kms_teardown = true;
	{
		struct decon_psr_info psr;

		decon_to_psr_info(decon, &psr);
		decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 0);
	}
	wake_up_interruptible_all(&decon->vsync_info.wait);
	wake_up_interruptible_all(&decon->wait_frmdone);
	msleep(20);

	if (decon->state != DECON_STATE_OFF)
		decon_disable(decon);

	if (decon->update_regs_thread) {
		unsigned long st = ACCESS_ONCE(decon->update_regs_thread->state);

		if (st & TASK_UNINTERRUPTIBLE) {
			decon_err("teardown: update worker in D state (0x%lx), skipping kthread_stop\n", st);
		} else {
			kthread_stop(decon->update_regs_thread);
			decon_info("teardown: update worker stopped\n");
		}
	}

	decon_unregister_entity(decon);

	if (decon->ion_client)
		ion_client_destroy(decon->ion_client);

	if (decon->lpd_wq) {
		destroy_workqueue(decon->lpd_wq);
		decon->lpd_wq = NULL;
	}

	decon_int_destroy_psr_thread(decon);

	if (decon->vsync_info.thread)
		kthread_stop(decon->vsync_info.thread);
	decon_int_destroy_vsync_thread(decon);

	/* int66: balance the explicit clock enables from decon_probe_common(). */
	{
		struct clk *cks[5] = {
			decon->res.pclk, decon->res.aclk, decon->res.eclk,
			decon->res.vclk, decon->res.aclk_disp,
		};
		int k;

		for (k = 0; k < 5; k++)
			if (!IS_ERR_OR_NULL(cks[k]))
				clk_disable_unprepare(cks[k]);
	}

	decon_put_clocks(decon);
	iovmm_deactivate(decon->dev);

	pm_runtime_put_sync(decon->dev);
	pm_runtime_disable(decon->dev);

	debugfs_remove_recursive(decon->debug_root);
	decon->debug_root = NULL;

	platform_set_drvdata(pdev, NULL);
	if (decon_int_drvdata == decon)
		decon_int_drvdata = NULL;

	decon_info("DECON released by KMS\n");
}

void decon_kms_put(struct decon_kms_dev *d)
{
	if (!d)
		return;

	mutex_lock(&kms_lock);
	if (WARN_ON(!d->refcount)) {
		mutex_unlock(&kms_lock);
		return;
	}
	if (--d->refcount) {
		mutex_unlock(&kms_lock);
		return;
	}
	kms_instance = NULL;
	mutex_unlock(&kms_lock);

	decon_kms_teardown(d);
	kfree(d);
}
EXPORT_SYMBOL_GPL(decon_kms_put);

/*
 * Coexistence hand-off (decon-kms-coexist.md section 6, option A): called from
 * the tail of the vendor probe.  The DRM/KMS driver overrides this weak
 * definition; when it is not built in, the vendor probe registers fb0 as
 * always.
 */
int __weak decon_kms_attach(struct decon_device *decon)
{
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(decon_kms_attach);

/* ------------------------------------------------------------------------ */
/* layer A: enable / disable / commit                                        */
/* ------------------------------------------------------------------------ */

int decon_kms_enable(struct decon_kms_dev *d)
{
	struct decon_device *decon;
	int ret;

	if (!d)
		return -ENODEV;

	decon = d->decon;

	mutex_lock(&kms_lock);

	if (decon->state == DECON_STATE_ON) {
		mutex_unlock(&kms_lock);
		return 0;
	}

	/*
	 * decon_probe() leaves the pipe running in DECON_STATE_INIT, and the
	 * vendor decon_enable() short-circuits that state without programming
	 * anything (decon_core.c:901-906).  A KMS modeset must always run the
	 * full sequence (decon-kms-dsi.md 2.3: "Do not copy the vendor's
	 * DECON_STATE_INIT short-circuit"), so drop to OFF first.
	 */
	if (decon->state == DECON_STATE_INIT) {
		decon_info("decon_kms_enable: INIT -> OFF\n");
		decon->state = DECON_STATE_OFF;
	}

	decon_info("decon_kms_enable: enter state=%d psr_mode=%d trig=%d\n",
		   decon->state, decon->pdata->psr_mode, decon->pdata->trig_mode);

	ret = decon_enable(decon);

	decon_info("decon_kms_enable: decon_enable ret=%d state=%d\n", ret, decon->state);
	if (!ret) {
		decon_info("decon_kms_enable: VIDCON0=%08x VIDOUTCON0=%08x VIDINTCON0=%08x VIDINTCON1=%08x TRIGCON=%08x\n",
			   decon_read(DECON_INT, VIDCON0),
			   decon_read(DECON_INT, VIDOUTCON0),
			   decon_read(DECON_INT, VIDINTCON0),
			   decon_read(DECON_INT, VIDINTCON1),
			   decon_read(DECON_INT, TRIGCON));
		/*
		 * int70: in VIDEO mode the FRAME interrupt is armed by
		 * decon_activate_vsync(); decon_enable()'s decon_reg_init()
		 * runs after our vblank registration and can clear
		 * VIDINTCON0.  Re-arm it here and balance it in disable().
		 */
		if (decon->pdata->psr_mode == DECON_VIDEO_MODE && d->vblank_cb) {
			/* Hold one vsync reference for the pipe lifetime:
			 * decon_wait_for_vsync() activates/deactivates around
			 * every commit, and with a 0 refcount its deactivate
			 * clears INT_ENABLE, leaving the FRAME line disarmed
			 * and the DECON shadow update unlatched. */
			decon_activate_vsync(decon);
			decon_kms_arm_frame(decon, "after decon_enable/reg_start");
			decon_info("decon_kms_enable: FRAME irq armed, VIDINTCON0=%08x\n",
				   decon_read(DECON_INT, VIDINTCON0));
		}
	}

	mutex_unlock(&kms_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(decon_kms_enable);

/*
 * int72: force the FRAME interrupt on, bypassing the vsync refcount.
 * decon_activate_vsync() only programs VIDINTCON0 when the refcount goes
 * 0->1; ->load() used to take that reference before the first
 * decon_reg_init()/reg_start() cleared VIDINTCON0, so later arming saw a
 * non-zero refcount and did nothing.  Program the mask and the DSIM vsync
 * source unconditionally here, exactly like the vendor pan_display path does
 * through decon_wait_for_vsync() (decon_core.c:1228-1265).
 */
static void decon_kms_arm_frame(struct decon_device *decon, const char *where)
{
	struct decon_psr_info psr;
	int ret = 0;

	if (!decon || decon->pdata->psr_mode != DECON_VIDEO_MODE)
		return;

	decon_info("decon_kms_arm_frame(%s): VIDINTCON0 before=%08x refcount=%d\n",
		   where, decon_read(DECON_INT, VIDINTCON0),
		   (int)decon->vsync_info.irq_refcount);

	decon_info("decon_kms_arm_frame(%s): state=%d pdata dsi_mode=%d psr_mode=%d trig=%d drvdata_ok=%d output_sd=%p\n",
		   where, decon->state, decon->pdata->dsi_mode,
		   decon->pdata->psr_mode, decon->pdata->trig_mode,
		   decon_int_drvdata == decon, decon->output_sd);
	{
		u32 scratch = decon_read(DECON_INT, VIDOUTCON0);

		decon_write_mask(DECON_INT, VIDOUTCON0, scratch, ~0);
		decon_info("decon_kms_arm_frame(%s): scratch VIDOUTCON0 %08x -> %08x\n",
			   where, scratch, decon_read(DECON_INT, VIDOUTCON0));
	}
	decon_to_psr_info(decon, &psr);
	decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 1);
	decon_info("decon_kms_arm_frame(%s): VIDINTCON0 after set_int=%08x\n",
		   where, decon_read(DECON_INT, VIDINTCON0));

	if (decon->output_sd) {
		ret = v4l2_subdev_call(decon->output_sd, core, ioctl,
				       DSIM_IOC_VSYNC, (unsigned long *)1);
		decon_info("decon_kms_arm_frame(%s): DSIM_IOC_VSYNC ret=%d\n", where, ret);
	}

	decon_info("decon_kms_arm_frame(%s): VIDINTCON0 final=%08x VIDINTCON1=%08x\n",
		   where, decon_read(DECON_INT, VIDINTCON0),
		   decon_read(DECON_INT, VIDINTCON1));
	if (!decon_read(DECON_INT, VIDINTCON0))
		decon_err("decon_kms_arm_frame(%s): VIDINTCON0 still zero after arm\n", where);
}

/*
 * int70: dump the state and the four video registers the KMS path cares about.
 * Exported for the fbdev slice, which has no decon pointer of its own.
 */
void decon_kms_log_state(struct decon_kms_dev *d, const char *tag)
{
	struct decon_device *decon;

	if (!d)
		return;
	decon = d->decon;

	decon_info("%s: state=%d psr_mode=%d trig=%d VIDCON0=%08x VIDOUTCON0=%08x VIDINTCON0=%08x VIDINTCON1=%08x TRIGCON=%08x\n",
		   tag ? tag : "decon state",
		   decon->state, decon->pdata->psr_mode, decon->pdata->trig_mode,
		   decon_read(DECON_INT, VIDCON0),
		   decon_read(DECON_INT, VIDOUTCON0),
		   decon_read(DECON_INT, VIDINTCON0),
		   decon_read(DECON_INT, VIDINTCON1),
		   decon_read(DECON_INT, TRIGCON));
}
EXPORT_SYMBOL_GPL(decon_kms_log_state);

int decon_kms_disable(struct decon_kms_dev *d)
{
	struct decon_device *decon;
	int ret;

	if (!d)
		return -ENODEV;

	decon = d->decon;

	decon_info("decon_kms_disable: state=%d by %s\n",
		   decon->state, current->comm);

	mutex_lock(&kms_lock);
	if (decon->state == DECON_STATE_OFF) {
		mutex_unlock(&kms_lock);
		return 0;
	}

	decon_deactivate_vsync(decon);
	ret = decon_disable(decon);
	mutex_unlock(&kms_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(decon_kms_disable);

/* int93: read back the window and video registers the commit just wrote. */
static void decon_kms_dump_regs(struct decon_device *decon, const char *where)
{
	unsigned int i;

	if (!decon || !decon->pdata)
		return;
	decon_info("decon_kms_regs[%s]: VIDCON0=%08x VIDOUTCON0=%08x VIDINTCON0=%08x VIDINTCON1=%08x TRIGCON=%08x\n",
		   where, decon_read(DECON_INT, VIDCON0),
		   decon_read(DECON_INT, VIDOUTCON0),
		   decon_read(DECON_INT, VIDINTCON0),
		   decon_read(DECON_INT, VIDINTCON1),
		   decon_read(DECON_INT, TRIGCON));
	for (i = 0; i < decon->pdata->max_win; i++)
		decon_info("decon_kms_regs[%s]: win[%u] WINCON=%08x ADD0=0x%08x ADD2=0x%08x ADD3=0x%08x VIDOSD_A=%08x B=%08x C=%08x D=%08x WHOLE_X=%u WHOLE_Y=%u OFF_X=%u OFF_Y=%u\n",
			   where, i,
			   decon_read(DECON_INT, WINCON(i)),
			   decon_read(DECON_INT, VIDW_ADD0(i)),
			   decon_read(DECON_INT, VIDW_ADD2(i)),
			   decon_read(DECON_INT, VIDW_ADD3(i)),
			   decon_read(DECON_INT, VIDOSD_A(i)),
			   decon_read(DECON_INT, VIDOSD_B(i)),
			   decon_read(DECON_INT, VIDOSD_C(i)),
			   decon_read(DECON_INT, VIDOSD_D(i)),
			   decon_read(DECON_INT, VIDW_WHOLE_X(i)),
			   decon_read(DECON_INT, VIDW_WHOLE_Y(i)),
			   decon_read(DECON_INT, VIDW_OFFSET_X(i)),
			   decon_read(DECON_INT, VIDW_OFFSET_Y(i)));
}

int decon_kms_commit(struct decon_kms_dev *d,
		     const struct decon_kms_win *wins, unsigned int nr_wins)
{
	struct decon_device *decon;
	struct decon_reg_data *regs;
	int ret;

	if (!d || !wins || !nr_wins)
		return -EINVAL;

	decon = d->decon;

	/*
	 * int92: the KMS pipe can be left off by a disable that raced (or by
	 * the fbdev restore sequence) while a client keeps committing frames;
	 * the old code then returned -EAGAIN for every frame and the panel
	 * stayed black.  Bring the pipe back exactly like crtc_enable_link()
	 * does and retry this commit once.  Bounded: no loop, one enable.
	 */
	if (decon->state != DECON_STATE_ON) {
		int oldstate = decon->state;
		int eret;

		decon_info("decon_kms_commit: state=%d -> enable -> retry by %s\n",
			   oldstate, current->comm);
		eret = decon_kms_enable(d);
		decon_info("decon_kms_commit: enable rc=%d state=%d\n",
			   eret, decon->state);
		if (eret || decon->state != DECON_STATE_ON) {
			decon_kms_log_state(d, "commit not ON after enable");
			return eret ? eret : -EAGAIN;
		}
	}

	regs = decon_export_regs_alloc();
	if (!regs)
		return -ENOMEM;

	/*
	 * The vendor commit body (decon_update_regs) waits for the vsync and for
	 * the shadow update, i.e. it sleeps for up to ~600 ms: this is the
	 * atomic-commit tail, never an IRQ or atomic context.
	 */
	mutex_lock(&decon->output_lock);

	ret = decon_kms_program(decon, regs, wins, nr_wins);
	if (!ret) {
		/* arm after any decon_reg_start() the program performed */
		decon_kms_arm_frame(decon, "commit after program");
		decon_export_update_regs(decon, regs);
		decon_kms_dump_regs(decon, "after update_regs");
		{
			int attempt, latched = 0;

			/*
			 * The vsync wait can miss its frame: the completion may
			 * already have been consumed by the arm/update pair above,
			 * and the wait's deactivate clears VIDINTCON0, so no new
			 * interrupt arrives.  Re-arm and retry a few frames instead
			 * of leaving the shadow update unapplied (the panel then
			 * keeps the previous buffer).
			 */
			for (attempt = 0; attempt < 5; attempt++) {
				decon_kms_arm_frame(decon, "commit retry");
				if (decon_wait_for_vsync(decon, 100) == 0) {
					latched = 1;
					break;
				}
			}
			if (latched) {
				decon_kms_arm_frame(decon, "commit after wait");
				decon_kms_dump_regs(decon, "after vsync");
			} else {
				/* Registers are programmed; report but keep the frame. */
				decon_err("decon_kms_commit: vsync never latched after 5 tries\n");
				decon_kms_log_state(d, "commit vsync timeout");
			}
		}
	}

	mutex_unlock(&decon->output_lock);

	decon_export_regs_free(regs);

	return ret;
}
EXPORT_SYMBOL_GPL(decon_kms_commit);

int decon_kms_commit_async(struct decon_kms_dev *d,
			   const struct decon_kms_win *wins,
			   unsigned int nr_wins, int *fence_fd)
{
	struct decon_device *decon;
	struct decon_reg_data *regs;
	struct sync_fence *fence;
	struct sync_pt *pt;
	int fd, ret;

	if (!d || !wins || !nr_wins || !fence_fd)
		return -EINVAL;

	decon = d->decon;

	mutex_lock(&decon->output_lock);

	fd = get_unused_fd();
	if (fd < 0) {
		mutex_unlock(&decon->output_lock);
		return -EINVAL;
	}

	/*
	 * Vendor behaviour (decon_core.c:2218-2228): a commit while the pipe is
	 * off returns a pre-signalled fence and programs nothing.  Keep it - the
	 * fd ABI cannot express an error - and let callers gate on
	 * decon_kms_enable().
	 */
	if ((decon->state == DECON_STATE_OFF) || (decon->ignore_vsync == true) ||
	    (decon->out_type == DECON_OUT_TUI)) {
		decon->timeline_max++;
		pt = sw_sync_pt_create(decon->timeline, decon->timeline_max);
		fence = sync_fence_create("display", pt);
		sync_fence_install(fence, fd);
		*fence_fd = fd;
		sw_sync_timeline_inc(decon->timeline, 1);
		mutex_unlock(&decon->output_lock);
		return 0;
	}

	regs = decon_export_regs_alloc();
	if (!regs) {
		put_unused_fd(fd);
		mutex_unlock(&decon->output_lock);
		return -ENOMEM;
	}

	ret = decon_kms_program(decon, regs, wins, nr_wins);
	if (ret) {
		decon_export_regs_free(regs);
		put_unused_fd(fd);
		mutex_unlock(&decon->output_lock);
		return ret;
	}

	decon->timeline_max++;
	pt = sw_sync_pt_create(decon->timeline, decon->timeline_max);
	fence = sync_fence_create("display", pt);
	sync_fence_install(fence, fd);
	*fence_fd = fd;

	decon_lpd_block(decon);
	mutex_lock(&decon->update_regs_list_lock);
	list_add_tail(&regs->list, &decon->update_regs_list);
	mutex_unlock(&decon->update_regs_list_lock);
	queue_kthread_work(&decon->update_regs_worker, &decon->update_regs_work);

	mutex_unlock(&decon->output_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(decon_kms_commit_async);

/* ------------------------------------------------------------------------ */
/* layer A: vblank                                                          */
/* ------------------------------------------------------------------------ */

int decon_kms_register_vblank_cb(struct decon_kms_dev *d,
				 decon_kms_vblank_cb cb, void *data)
{
	struct decon_device *decon;

	if (!d)
		return -ENODEV;

	decon = d->decon;

	mutex_lock(&kms_lock);

	d->vblank_cb = cb;
	d->vblank_data = data;
	decon->kms_vblank_cb = cb;
	decon->kms_vblank_data = data;

	/*
	 * int72: registration only stores the callback.  Arming is done per
	 * decon_reg_start() by decon_kms_arm_frame(), because taking the
	 * refcounted reference here made every later refcounted arm a no-op
	 * after decon_reg_init()/reg_start() cleared VIDINTCON0.
	 */

	mutex_unlock(&kms_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(decon_kms_register_vblank_cb);

int decon_kms_wait_vblank(struct decon_kms_dev *d, unsigned int timeout_ms)
{
	if (!d)
		return -ENODEV;

	return decon_wait_for_vsync(d->decon, timeout_ms);
}
EXPORT_SYMBOL_GPL(decon_kms_wait_vblank);

/* ------------------------------------------------------------------------ */
/* layer A: runtime PM                                                      */
/* ------------------------------------------------------------------------ */

int decon_kms_power_get(struct decon_kms_dev *d)
{
	if (!d)
		return -ENODEV;

	return pm_runtime_get_sync(d->decon->dev);
}
EXPORT_SYMBOL_GPL(decon_kms_power_get);

void decon_kms_power_put(struct decon_kms_dev *d)
{
	if (d)
		pm_runtime_put_sync(d->decon->dev);
}
EXPORT_SYMBOL_GPL(decon_kms_power_put);

/* ------------------------------------------------------------------------ */
/* layer B: raw vendor symbols                                              */
/* ------------------------------------------------------------------------ */

int decon_export_acquire_windows(struct decon_device *decon)
{
	int i, ret;

	for (i = 0; i < decon->n_sink_pad; i++) {
		ret = decon_acquire_windows(decon, i);
		if (ret) {
			decon_err("failed to create decon window[%d]\n", i);
			for (; i >= 0; i--)
				decon_release_windows(decon->windows[i]);
			return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(decon_export_acquire_windows);

void decon_export_release_windows(struct decon_device *decon)
{
	int i;

	if (!decon)
		return;

	for (i = 0; i < decon->pdata->max_win; i++)
		decon_release_windows(decon->windows[i]);
}
EXPORT_SYMBOL_GPL(decon_export_release_windows);

struct decon_reg_data *decon_export_regs_alloc(void)
{
	/* ~3 KB on arm64: never put this on the kernel stack */
	return kzalloc(sizeof(struct decon_reg_data), GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(decon_export_regs_alloc);

void decon_export_regs_free(struct decon_reg_data *regs)
{
	kfree(regs);
}
EXPORT_SYMBOL_GPL(decon_export_regs_free);

int decon_export_set_win_buffer(struct decon_device *decon,
				struct decon_win_config *cfg,
				struct decon_reg_data *regs,
				unsigned int win_idx)
{
	if (!decon || !cfg || !regs || win_idx >= decon->pdata->max_win)
		return -EINVAL;

	return decon_set_win_buffer(decon, decon->windows[win_idx], cfg, regs);
}
EXPORT_SYMBOL_GPL(decon_export_set_win_buffer);

void decon_export_regs_param(struct decon_device *decon,
			     struct decon_reg_data *regs,
			     unsigned int win_idx)
{
	struct decon_regs_data win_regs;

	if (!decon || !regs || win_idx >= decon->pdata->max_win)
		return;

	memset(&win_regs, 0, sizeof(win_regs));
	decon_to_regs_param(&win_regs, regs, win_idx);
	decon_reg_set_regs_data(DECON_INT, win_idx, &win_regs);
}
EXPORT_SYMBOL_GPL(decon_export_regs_param);

void decon_export_update_regs(struct decon_device *decon,
			      struct decon_reg_data *regs)
{
	if (!decon || !regs)
		return;

	decon_update_regs(decon, regs);
}
EXPORT_SYMBOL_GPL(decon_export_update_regs);

int decon_export_get_clocks(struct decon_device *decon)
{
	if (!decon)
		return -ENODEV;

	return decon_int_get_clocks(decon);
}
EXPORT_SYMBOL_GPL(decon_export_get_clocks);

void decon_export_set_clocks(struct decon_device *decon)
{
	if (decon)
		decon_int_set_clocks(decon);
}
EXPORT_SYMBOL_GPL(decon_export_set_clocks);

void decon_export_put_clocks(struct decon_device *decon)
{
	if (decon)
		decon_put_clocks(decon);
}
EXPORT_SYMBOL_GPL(decon_export_put_clocks);

struct decon_device *decon_export_dev(void)
{
	return decon_int_drvdata;
}
EXPORT_SYMBOL_GPL(decon_export_dev);
