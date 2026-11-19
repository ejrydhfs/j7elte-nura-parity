/* SPDX-License-Identifier: MIT */
/*
 * drm.h - minimal userspace stand-in for the kernel drm.h.
 *
 * The driver's UAPI mirror (code/gpu31/gem/panfrost_drm.h, byte-identical
 * to Linux v5.2 include/uapi/drm/panfrost_drm.h) includes "drm.h" for the
 * kernel types (__u32/__u64/__s32/__s64) and DRM_IOW/DRM_IOWR. <drm/drm.h>
 * already provides all of those on a userspace build host, so this shim
 * is just a redirect to it.
 */
#ifndef PF7580_CLIENT_DRM_H
#define PF7580_CLIENT_DRM_H

#include <drm/drm.h>

#endif /* PF7580_CLIENT_DRM_H */
