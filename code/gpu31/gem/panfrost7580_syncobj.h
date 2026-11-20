/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_syncobj.h - generic DRM syncobj ioctl handlers for the
 * T720 port (Exynos 7580, Nura 3.10.108, j7elte).
 *
 * Why this file exists: Mesa 20.3.5 panfrost hard-asserts on
 * drmSyncobjCreate at context creation (pan_context.c:1632-1633:
 * "assert(!ret && ctx->syncobj)"). The stock 3.10 core has NO generic
 * syncobj ioctls (drm.h ends at 0xBA;drm_drv.c dispatches only its own
 * core table), so SYNCOBJ_CREATE arrived as -EINVAL. The shim backport
 * (0003) provides the per-file handle table; this file wires the
 * UAPI surface Mesa actually calls.
 *
 * UAPI source of truth: v5.2-era drm.h (Mesa 20.3.5 bundles it at
 * include/drm-uapi/drm.h): SYNCOBJ_CREATE 0xBF, DESTROY 0xC0,
 * HANDLE_TO_FD 0xC1, FD_TO_HANDLE 0xC2, WAIT 0xC3, RESET 0xC4,
 * SIGNAL 0xC5. The 20.x libdrm wrappers emit exactly these numbers
 * (disassembled, aarch64): CREATE 0xc00864bf, DESTROY 0xc00864c0,
 * HANDLE_TO_FD 0xc01864c1, FD_TO_HANDLE 0xc01864c2, WAIT 0xc02864c3,
 * RESET/SIGNAL 0xc01064c4/5. Payload structs are the 3.10-era
 * (40-byte WAIT, no deadline_nsec; handle struct has fd+pad, no
 * point field, 16 bytes).
 *
 * Scope (binary syncobj only, same omissions as the 0003 shim):
 * no timeline payloads, no eventfd, no WAIT_FOR_SUBMIT/WAIT_AVAILABLE
 * (rejected -EINVAL), no sync_file fd passing (3.10 has no
 * sync_file.c). HANDLE_TO_FD/FD_TO_HANDLE support ONLY plain
 * dma-fence export/import via anon_inode fd (flags==0); any
 * *_SYNC_FILE flag is -EINVAL, unknown flags -EINVAL.
 *
 * File state: file_priv->driver_priv leads with struct
 * drm_syncobj_manager (same layout contract as GEM's p7580_file_priv
 * and the driver's p7580_drv_file: mgr first). Handlers take the
 * manager via driver_priv + 0, so GEM's and the driver's own file
 * states both work unchanged.
 */

#ifndef PANFROST7580_SYNCOBJ_H
#define PANFROST7580_SYNCOBJ_H

struct drm_device;
struct drm_file;

int panfrost_syncobj_create(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);
int panfrost_syncobj_destroy(struct drm_device *dev, void *data,
			     struct drm_file *file_priv);
int panfrost_syncobj_handle_to_fd(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);
int panfrost_syncobj_fd_to_handle(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);
int panfrost_syncobj_wait(struct drm_device *dev, void *data,
			  struct drm_file *file_priv);
int panfrost_syncobj_reset(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);
int panfrost_syncobj_signal(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);

/* fops-level dispatcher: routes 3.10-unknown ioctl numbers (core has
 * no SYNCOBJ rows) to the handlers above; everything else falls
 * through to drm_ioctl. compat32 maps to the same handlers (payload
 * structs are u32/__u64/__s32-width-clean, no pointer fields). */
long panfrost_syncobj_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg);
#ifdef CONFIG_COMPAT
long panfrost_syncobj_compat_ioctl(struct file *filp, unsigned int cmd,
				   unsigned long arg);
#endif

#endif /* PANFROST7580_SYNCOBJ_H */
