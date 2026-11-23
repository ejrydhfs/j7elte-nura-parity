/*
 * exynos_decon_kms.h - reuse contract for the vendor DECON window engine
 *
 * Install path in the kernel tree: include/linux/exynos_decon_kms.h
 * Source of the implementation:    drivers/video/exynos/decon_7580/
 *
 * Copyright (c) 2026
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This header is the ONLY interface between a KMS/atomic subdriver and the
 * vendor fbdev DECON driver (drivers/video/exynos/decon_7580/).  The subdriver
 * never includes "decon.h": every structure below is opaque or fb-free, and
 * nothing here mentions struct fb_info, struct fb_var_screeninfo, ION or
 * Android sync fences.
 *
 * Rationale, the per-function locking/PM table and the exact
 * EXPORT_SYMBOL() list: docs/reports/decon-kms-api.md.
 *
 * Two layers are exported.  A subdriver should use layer A; layer B exists so
 * that the plane/fence/fbdev slices can reach the register-level primitives
 * without another round of refactoring, and layer B is what the shim patch
 * makes callable (those functions are static in the vendor tree today).
 *
 *  layer A: struct decon_kms_dev + decon_kms_*_() entry points (ABI-stable)
 *  layer B: raw vendor symbols, prototypes below, no struct layouts exposed
 *
 * Context rules, applying to every entry point unless stated otherwise:
 *
 *  - All entry points may sleep and MUST be called from process context, with
 *    the DECON power domain (pd_disp) and DECON clocks on.  The shim takes
 *    pm_runtime_get_sync()/put() around the calls that touch SFRs, so callers
 *    do not need to touch runtime PM themselves.  They MUST NOT be called from
 *    atomic context, from an IRQ handler, or from a spinlock.
 *  - The vblank callback is the single exception: it runs in hard IRQ context
 *    (DECON FRAME interrupt) with decon->slock held.  It must be lock-free,
 *    must not sleep and must not call back into this API.
 *  - Only one client may hold struct decon_kms_dev at a time: the DECON
 *    driver is single-instance (global decon_int_drvdata, decon.h:36) and the
 *    shim keeps a single-owner flag, so a second decon_kms_get() returns
 *    -EBUSY until the first handle is put.
 */
#ifndef _LINUX_EXYNOS_DECON_KMS_H
#define _LINUX_EXYNOS_DECON_KMS_H

#include <linux/types.h>

struct device;
struct dma_buf;

/* Bump when a struct layout or a semantic below changes. */
#define DECON_KMS_API_VERSION		1

/*
 * Opaque handle.  Points at the vendor struct decon_device; the subdriver
 * must never dereference it.
 */
struct decon_kms_dev;

/* ------------------------------------------------------------------------
 * layer A: fb-free data transfer objects
 * ------------------------------------------------------------------------ */

/* Same values as enum decon_idma_type (decon_common.h). */
enum decon_kms_idma {
	DECON_KMS_IDMA_G0 = 0,
	DECON_KMS_IDMA_G1,
	DECON_KMS_IDMA_VG0,
	DECON_KMS_IDMA_VG1,
	DECON_KMS_IDMA_VGR0,
	DECON_KMS_IDMA_VGR1,
	DECON_KMS_IDMA_G2,
	DECON_KMS_IDMA_G3,
	DECON_KMS_IDMA_MAX,
};

/* Same values as enum decon_blending (decon.h). */
enum decon_kms_blending {
	DECON_KMS_BLENDING_NONE = 0,
	DECON_KMS_BLENDING_PREMULT = 1,
	DECON_KMS_BLENDING_COVERAGE = 2,
};

/* Same values as the DECON_WIN_STATE_* union in struct decon_win_config. */
enum decon_kms_win_state {
	DECON_KMS_WIN_DISABLED = 0,
	DECON_KMS_WIN_COLOR,
	DECON_KMS_WIN_BUFFER,
};

struct decon_kms_rect {
	__s32 x;
	__s32 y;
	__u32 w;
	__u32 h;
};

/*
 * Source and destination geometry of one window.  Mirrors struct decon_frame
 * exactly: (x,y) is the top-left offset inside the buffer/stride (src) or the
 * screen (dst), (f_w,f_h) is the full stride/size of the buffer, (w,h) is the
 * visible size.  The hardware cannot scale: src.w == dst.w and src.h == dst.h.
 */
struct decon_kms_frame {
	__s32 x;
	__s32 y;
	__u32 w;
	__u32 h;
	__u32 f_w;
	__u32 f_h;
};

/*
 * One window of one frame.  Array index == DECON window number.
 *
 * @state:     DISABLED, COLOR (solid fill) or BUFFER (scan out @addr).
 * @color:     COLOR only, 0x00RRGGBB.
 * @fourcc:    BUFFER only, DRM_FORMAT_* (see include/uapi/drm/drm_fourcc.h).
 *             The shim owns the fourcc -> enum decon_pixel_format table and
 *             rejects anything it cannot program with -EINVAL.  Supported on
 *             exynos7580: ARGB8888, ABGR8888, RGBA8888, BGRA8888, XRGB8888,
 *             XBGR8888, RGBX8888, BGRX8888, RGBA5551, RGB565, NV12, NV21.
 * @idma_type:which DMA channel feeds the window.  Window 0 must use the
 *             channel the bootloader/vendor driver left on it (G0 on J7).
 *             Two enabled windows must not share a channel: the shim drops the
 *             second one as the vendor code does (decon_reg_chmap_validate()).
 * @blending:  must be NONE on window 0.
 * @plane_alpha: 0..255; 0/255 mean "no per-plane alpha".
 * @addr:      DECON IOVAs, plane_cnt of them per @fourcc.  Obtain them from
 *             decon_kms_map_dmabuf(); for a 2-plane format addr[1] may be left
 *             0 when the planes are contiguous (the shim computes the plane
 *             offset exactly like decon_calc_plane_offset()).
 * @protection: window carries protected (DRM) content; aggregated per commit
 *             and pushed through exynos_smc() by the shim.
 */
struct decon_kms_win {
	enum decon_kms_win_state	state;
	__u32				color;
	__u32				fourcc;
	enum decon_kms_idma		idma_type;
	enum decon_kms_blending		blending;
	int				plane_alpha;
	dma_addr_t			addr[3];
	struct decon_kms_frame		src;
	struct decon_kms_frame		dst;
	struct decon_kms_rect		block_area;
	bool				protection;
};

/* Read-only description of the bound DECON instance. */
struct decon_kms_caps {
	unsigned int	max_win;	/* windows the DT exposes (7 on J7) */
	unsigned int	default_win;	/* window the fbdev driver owns */
	bool		video_mode;	/* psr_mode == DECON_VIDEO_MODE (true on J7) */
	bool		sw_trigger;	/* trig_mode == DECON_SW_TRIG (true on J7) */
};

/* ------------------------------------------------------------------------
 * layer A: entry points
 * ------------------------------------------------------------------------ */

/*
 * decon_kms_get - obtain a handle on the probed DECON instance
 * @decon_dev: the DECON platform device (&pdev->dev).
 *
 * In the recommended binding (decon-kms-api.md 6, option A) nobody calls this
 * from outside: the vendor decon_probe() calls decon_kms_attach() itself, in
 * the same context, right where it would have called register_framebuffer().
 * A caller that has to look the device up instead can use
 *     of_find_device_by_node(of_find_compatible_node(NULL, NULL,
 *                             "samsung,exynos5-decon_driver"))
 * but that variant has the lifetime and dual-ownership failure modes listed in
 * the report; prefer the in-probe hand-off.
 *
 * Returns the handle or an ERR_PTR(): -ENODEV when the DECON driver is not
 * compiled in / has not probed, -EBUSY if another client holds it.
 * Does not touch the hardware, does not register anything, and may be called
 * before the panel is on.  Process context, may sleep.
 */
struct decon_kms_dev *decon_kms_get(struct device *decon_dev);

/* Drop the handle.  Does not power anything down; the caller is expected to
 * have blanked the display first.  Process context. */
void decon_kms_put(struct decon_kms_dev *d);

/* Read-only capabilities of the handle; the pointer is owned by @d and stays
 * valid until decon_kms_put().  Safe to call at any time, from any context. */
const struct decon_kms_caps *decon_kms_caps(struct decon_kms_dev *d);

/*
 * decon_kms_enable - put the DECON in the "on" state and start scanout
 *
 * Equivalent to the vendor decon_enable(): pm_runtime_get_sync(), DSIM
 * s_stream(1) on the vendor v4l2 subdev, iovmm_activate(), decon_reg_init(),
 * decon_reg_start().  The panel must already be powered and initialised by the
 * DSI side; this call does not run the panel power/reset lists.
 *
 * Takes decon->output_lock internally.  Process context, may sleep, can take
 * tens of ms (DSIM command traffic).  Idempotent: a second call returns 0.
 * A DSIM error is returned as-is (-EIO/-ENODEV from the vendor
 * v4l2_subdev_call() on s_stream).
 */
int decon_kms_enable(struct decon_kms_dev *d);

/*
 * decon_kms_disable - flush pending commits, stop scanout, drop the clocks
 *
 * Wraps the vendor decon_disable() including its LPD branches (unreachable on
 * a video-mode panel like the J7's, see decon-kms-api.md 7.7).
 * Flushes the update kthread worker,
 * disables the FRAME interrupt, stops the DECON, DSIM s_stream(0),
 * pm_runtime_put_sync().  Idempotent.  Process context, may sleep.
 *
 * Must be called before decon_kms_put() if a commit happened; otherwise the
 * update worker may still reference the window buffers this driver mapped.
 */
int decon_kms_disable(struct decon_kms_dev *d);

/*
 * decon_kms_commit - program exactly one frame, synchronously
 *
 * @wins: array of @nr_wins descriptors, index == window number; windows at
 *        index >= @nr_wins are programmed as DISABLED.
 *
 * Sequence performed by the shim (the vendor decon_update_regs() body):
 * wait in-fences, exynos_smc() protection change, HW-trigger mask, shadow
 * protect all windows, program WINCON/WIN_MAP/VIDOSD_x/VIDW_ADDx/WHOLE/OFFSET/
 * BLENDEQ, shadow unprotect, decon_reg_start(), wait for vsync, poll
 * DECON_UPDATE for completion, release the previous buffers.
 *
 * Blocking: waits up to 300 ms for the vsync and 300 ms for the shadow update
 * to land, so this is the atomic-commit tail, not a fast path.  It must be
 * called with the DECON enabled.  Takes decon->output_lock and
 * decon->mutex internally.  Process context; sleeps; signals do not abort the
 * wait (the vendor wait_event_interruptible() path returns "success" on
 * -ERESTARTSYS, so EINTR is not propagated).
 *
 * Errors: -EAGAIN when the DECON is not in the ON state, -EINVAL for
 * geometry/format/blending the hardware cannot express (no scaling, window
 * width < 128 bytes, x-alignment, blending on window 0), -ENOMEM on
 * allocation failure.  All of these are detected before any shadow register is
 * reopened, so the previous frame stays on screen.
 *
 * A shadow-update timeout is NOT propagated: the vendor commit body logs it
 * and calls decon_dump() (decon_core.c:2126-2130) and returns void, so the
 * frame may be stale while decon_kms_commit() reports success.  Watch for
 * "timeout of updating decon registers" in dmesg.
 *
 * Buffer lifetime: after a successful return the shim has released its
 * references (it cached them for the current frame).  Releasing the dma-buf
 * that is on screen is safe only after the next successful commit or after
 * decon_kms_disable().
 */
int decon_kms_commit(struct decon_kms_dev *d,
		     const struct decon_kms_win *wins, unsigned int nr_wins);

/*
 * decon_kms_commit_async - queue a frame and return a vendor completion fence
 *
 * Same programming as decon_kms_commit() but runs in the driver's kthread and
 * returns @fence_fd, an fd for an Android sync_fence (sw_sync timeline owned by
 * the DECON driver) that fires when the frame has been latched.  This is the
 * vendor S3CFB_WIN_CONFIG ABI, NOT a dma_fence; a KMS subdriver that needs a
 * dma_fence must attach its own fence to the vblank callback instead.
 *
 * Caller owns @fence_fd and must close it.  When the DECON is off or in TUI
 * mode the vendor code returns 0 and a *pre-signalled* fence without
 * programming anything (decon_core.c:2218-2228); the shim keeps that behaviour
 * rather than invent an error the fd/ioctl ABI cannot express, so callers must
 * gate on decon_kms_enable() themselves.
 *
 * Needs a live fd table (get_unused_fd(), decon_core.c:2199).  Process
 * context, may sleep, may be called from an ioctl; NOT from a worker thread
 * that has no ->files.
 */
int decon_kms_commit_async(struct decon_kms_dev *d,
			   const struct decon_kms_win *wins,
			   unsigned int nr_wins, int *fence_fd);

/*
 * decon_kms_register_vblank_cb - get the DECON FRAME interrupt
 *
 * @cb runs in hard IRQ context on every frame-done interrupt of the DECON
 * (video mode: IRQ resource 2; command mode: the I80 IRQ and the TE GPIO IRQ).
 * It is exactly the point where the vendor driver updates
 * vsync_info.timestamp; a KMS subdriver calls drm_crtc_handle_vblank() from
 * it.  Only one callback is supported; registering again replaces it.
 * Passing @cb == NULL unregisters.  The callback must not sleep and must not
 * call back into this API.
 *
 * The shim enables the FRAME interrupt while a callback is registered
 * (decon_reg_set_int()) and leaves the vendor refcounting in
 * decon_activate_vsync() alone for the fbdev path.
 *
 * Process context (register/unregister), the callback itself runs in IRQ.
 */
typedef void (*decon_kms_vblank_cb)(void *data);

int decon_kms_register_vblank_cb(struct decon_kms_dev *d,
				 decon_kms_vblank_cb cb, void *data);

/*
 * decon_kms_wait_vblank - block for the next frame-done interrupt
 *
 * Wraps decon_wait_for_vsync().  @timeout_ms == 0 waits forever.  Process
 * context only (the vendor path takes vsync_info.irq_lock and enables/disables
 * the FRAME interrupt around the wait).  Returns -ETIMEDOUT, or 0.  Do not use
 * this in the same commit as decon_kms_commit(): that call already waits.
 */
int decon_kms_wait_vblank(struct decon_kms_dev *d, unsigned int timeout_ms);

/*
 * decon_kms_power_get/put - runtime PM of the DECON platform device
 *
 * The vendor probe already enabled pm_runtime; decon_kms_enable() performs its
 * own get_sync/put_sync pair, so a KMS subdriver needs these only to keep the
 * DECON clocked across a longer sequence (e.g. re-reading state after a
 * modeset).  Nests; process context, may sleep.
 */
int decon_kms_power_get(struct decon_kms_dev *d);
void decon_kms_power_put(struct decon_kms_dev *d);

/*
 * decon_kms_map_dmabuf - map a dma-buf into the DECON's IOVA space
 *
 * The exporter-agnostic replacement for the vendor ION path.  The vendor
 * decon_map_ion_handle() calls ion_iovmm_map(), which BUG_ON()s on any dma-buf
 * whose ops are not ION's (drivers/staging/android/ion/ion.c:2643), so a
 * panfrost/GEM buffer would panic the kernel if it went through
 * decon_set_win_buffer().  This entry point does:
 *
 *     dma_buf_attach(buf, decon->dev) -> dma_buf_map_attachment(DMA_TO_DEVICE)
 *     -> iovmm_map(decon->dev, sg, 0, size, DMA_TO_DEVICE, id)   (IOMMU on)
 *     or the contiguous phys address of the buffer                (IOMMU off)
 *     -> exynos_ion_sync_dmabuf_for_device() when the exporter is ION
 *
 * @id is the iovmm region id, use the DECON window number as the vendor code
 * does.  On success *@addr is the address to put in decon_kms_win.addr[0] and
 * *@cookie is an opaque handle for decon_kms_unmap_dmabuf().  @size is 0 for
 * the whole buffer.  Process context, may sleep.
 *
 * The mapping stays valid until decon_kms_unmap_dmabuf(); the shim refcounts
 * per (dma_buf, window) pair so two frames may reference the same mapping.
 */
int decon_kms_map_dmabuf(struct decon_kms_dev *d, struct dma_buf *buf,
			 unsigned int id, size_t size,
			 dma_addr_t *addr, void **cookie);
void decon_kms_unmap_dmabuf(struct decon_kms_dev *d, void *cookie);

/* ------------------------------------------------------------------------
 * layer B: raw vendor symbols
 *
 * Two groups.
 *
 * 1) Already global in the vendor tree, nothing to patch, no struct layout
 *    needed beyond the incomplete types below.  Declared by
 *    drivers/video/exynos/decon_7580/decon_common.h, which is fb-free (it only
 *    includes panels/decon_lcd.h):
 *
 *      void decon_reg_init(u32 id, enum decon_dsi_mode, struct decon_init_param *);
 *      void decon_reg_start(u32 id, enum decon_dsi_mode, struct decon_psr_info *);
 *      int  decon_reg_stop(u32 id, enum decon_dsi_mode, struct decon_psr_info *);
 *      void decon_reg_set_regs_data(u32 id, int win_idx, struct decon_regs_data *);
 *      void decon_reg_set_int(u32 id, struct decon_psr_info *, enum decon_dsi_mode, u32 en);
 *      void decon_reg_set_trigger(u32 id, enum decon_dsi_mode, enum decon_trig_mode, enum decon_set_trig);
 *      int  decon_reg_wait_for_update_timeout(u32 id, unsigned long timeout);
 *      void decon_reg_shadow_protect_win(u32 id, u32 win_idx, u32 protect);
 *      void decon_reg_activate_window(u32 id, u32 index);
 *      void decon_enable_eclk_idle_gate(u32 id, enum decon_set_eclk_idle_gate);
 *
 *    Every one of them resolves `id` through get_decon_drvdata(), a static
 *    inline in decon.h returning the global decon_int_drvdata, i.e. these are
 *    single-instance and lock-free: the caller owns serialisation.
 *
 * 2) Static today; the shim patch turns each into a decon_export_* wrapper
 *    (same code, new name, non-static, EXPORT_SYMBOL) so that the vendor
 *    struct layouts do not become part of the interface.  Prototypes below.
 * ------------------------------------------------------------------------ */

struct decon_device;		/* drivers/video/exynos/decon_7580/decon.h */
struct decon_reg_data;		/* same */
struct decon_win_config;	/* same */
struct decon_win_config_data;	/* same */

/*
 * Window bookkeeping without register access, wrapping
 * decon_acquire_window()/decon_release_windows() (decon_core.c:2836/:2733).
 * Allocates one framebuffer_alloc() scratch fb_info per window -- that
 * allocation is a driver-internal detail, the fb_info is never registered and
 * no /dev/fb0 appears because of it.  Process context, may sleep, allocates.
 */
int decon_export_acquire_windows(struct decon_device *decon);
void decon_export_release_windows(struct decon_device *decon);

/* kzalloc/kfree of struct decon_reg_data (decon.h:389; ~3 KB on arm64, too
 * big for the kernel stack).  New code in the shim, wraps nothing. */
struct decon_reg_data *decon_export_regs_alloc(void);
void decon_export_regs_free(struct decon_reg_data *regs);

/*
 * Wraps decon_set_win_buffer() (decon_core.c:1516).  @cfg->fd_idma[] are ION
 * dma-buf fds, exactly the vendor ABI; this is the fbdev/ION path and is
 * exported for the fbdev-emulation slice only -- the KMS path must not use it
 * (see decon_kms_map_dmabuf() for why).  Fills regs->dma_buf_data[win_idx],
 * buf_start/vidosd/whole/offset/wincon/blendeq and writes the window's
 * fb_info var; process context, sleeps in iovmm_map(), -EINVAL on bad geometry.
 */
int decon_export_set_win_buffer(struct decon_device *decon,
				struct decon_win_config *cfg,
				struct decon_reg_data *regs,
				unsigned int win_idx);

/*
 * Wraps decon_to_regs_param() + decon_reg_set_regs_data() (decon_core.c:119,
 * decon_reg_7580.c:529): program one window from a resolved regs entry, with
 * no buffer ownership and no fb_info involvement.  This is the fb-free half of
 * decon_set_win_buffer() and the primitive the KMS plane wants.  Context: the
 * caller must already have shadow-protected the window.
 */
void decon_export_regs_param(struct decon_device *decon,
			     struct decon_reg_data *regs,
			     unsigned int win_idx);

/*
 * Same register fill, but the DMA addresses come from the caller
 * (decon_kms_win.addr[]) instead of an ION fd.  This is the missing half of
 * decon_set_win_buffer() and the primitive the KMS plane path uses; it mirrors
 * decon_core.c:1690-1701 (buf_start / whole_w / whole_h / offset_x / offset_y /
 * wincon / blendeq / vidosd_a..b / dma_buf_data) and performs the same
 * validation (format < MAX, blending < MAX, no blending on window 0, non-zero
 * and non-negative dst, dst.w * bpp / 8 >= 128 bytes, src.f_w >= dst.w,
 * decon_validate_x_alignment()).  Process context, no sleeping, needs the
 * window shadow-protected by the caller only if it is called outside
 * decon_export_update_regs().  Returns 0 or -EINVAL.
 */
int decon_export_regs_param_addr(struct decon_device *decon,
				 struct decon_reg_data *regs,
				 unsigned int win_idx,
				 const struct decon_kms_win *win);

/*
 * Wraps decon_update_regs() (decon_core.c:2066) and __decon_update_regs()
 * (decon_core.c:1916): the whole commit sequence described under
 * decon_kms_commit(), synchronously, in the caller's context.  Blocking, up to
 * ~600 ms, takes no locks of its own (the vendor decon_set_win_config() holds
 * decon->output_lock around it) so the caller must serialise.
 */
void decon_export_update_regs(struct decon_device *decon,
			      struct decon_reg_data *regs);

/*
 * Wraps the descriptor-translation loop of decon_set_win_config()
 * (decon_core.c:2185) without the fd/ion/sw_sync parts: for each window, takes
 * @cfg[i] (vendor layout, fb-free), computes wincon/winmap/vidosd/blendeq and
 * bandwidth, validates geometry and rejects scaling.  Buffers must already be
 * in regs (decon_export_regs_param() path).  Returns 0 or -EINVAL.
 */
int decon_export_set_win_config(struct decon_device *decon,
				struct decon_reg_data *regs,
				const struct decon_win_config *cfg,
				unsigned int nr_win);

/* Clocks / PM, wrapping decon_int_get_clocks() (decon-int_drv.c:130),
 * decon_int_set_clocks() (:177), decon_put_clocks() (decon_core.c:2532).
 * Process context; get/set_clocks program dividers and muxes, so the display
 * must be off. */
int decon_export_get_clocks(struct decon_device *decon);
void decon_export_set_clocks(struct decon_device *decon);
void decon_export_put_clocks(struct decon_device *decon);

/* The bound instance, or NULL.  Single-instance driver. */
struct decon_device *decon_export_dev(void);

#endif /* _LINUX_EXYNOS_DECON_KMS_H */
