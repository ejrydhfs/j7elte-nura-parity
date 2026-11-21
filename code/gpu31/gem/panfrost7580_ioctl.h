/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_ioctl.h - v5.2 UAPI handler prototypes for the T720 port.
 *
 * v5.2 reference: panfrost_drv.c:329-340 ioctl table, handlers in
 * panfrost_gem.c/panfrost_job.c/panfrost_device.c. The table itself
 * (DRM_IOCTL_DEF rows, DRM_RENDER_ALLOW mapping) is owned by G31_DrmCore
 * (panfrost_ioctls_310.h); this header declares the six handler bodies
 * implemented in panfrost7580_ioctl.c so DrmCore can reference them.
 * Prototypes use the 3.10 drm_ioctl_t signature
 * (int fn(struct drm_device *, void *, struct drm_file *)).
 *
 * File-private state behind struct drm_file->driver_priv
 * (p7580_file_priv): per-fd syncobj manager (0003 series header) + the
 * jobs-slice per-fd context (opaque here; owned by the jobs slice).
 * Allocated on first ioctl that needs it, freed in the driver's
 * .postclose hook. The real struct drm_syncobj_manager comes from
 * drivers/gpu/drm/drm_syncobj.h (staged by the Makefile's kcheck);
 * this header needs only the pointer width, so it forward-declares
 * and keeps -Wall clean.
 */

#ifndef PANFROST7580_IOCTL_H
#define PANFROST7580_IOCTL_H

struct drm_device;
struct drm_file;

int panfrost_ioctl_submit(struct drm_device *dev, void *data,
			  struct drm_file *file_priv);
int panfrost_ioctl_wait_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);
int panfrost_ioctl_create_bo(struct drm_device *dev, void *data,
			     struct drm_file *file_priv);
int panfrost_ioctl_mmap_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);
int panfrost_ioctl_get_param(struct drm_device *dev, void *data,
			     struct drm_file *file_priv);
int panfrost_ioctl_get_bo_offset(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);
/* DEBUG-ONLY (gpu31-gpuwrite, strip before cutover): kernel-side BO
 * word readback after completion. Routed via the syncobj fops
 * dispatcher (nr 0xCF), NOT the six-ioctl driver table. Takes a
 * handle + byte offset, returns the u32 at that offset after the
 * same dma_sync_for_cpu maintenance free_job runs. */
int panfrost_debug_read_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);

int p7580_file_priv_init(struct drm_device *dev, struct drm_file *file_priv);
void p7580_file_priv_fini(struct drm_device *dev, struct drm_file *file_priv);

#endif /* PANFROST7580_IOCTL_H */
