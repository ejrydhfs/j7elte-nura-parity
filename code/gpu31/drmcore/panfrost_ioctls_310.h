#ifndef _PANFROST_IOCTLS_310_H_
#define _PANFROST_IOCTLS_310_H_
/*
 * panfrost_ioctls_310.h — legacy 3.10 DRM_IOCTL_DEF rows for the six v5.2
 * panfrost ioctls (UAPI numbers from v5.2 panfrost_drm.h: SUBMIT=0x00,
 * WAIT_BO=0x01, CREATE_BO=0x02, MMAP_BO=0x03, GET_PARAM=0x04,
 * GET_BO_OFFSET=0x05, all DRM_COMMAND_BASE-relative).
 *
 * v5.2 table (panfrost_drv.c:340-351, for reference):
 *   SUBMIT:        DRM_RENDER_ALLOW | DRM_AUTH
 *   WAIT_BO:       DRM_RENDER_ALLOW
 *   CREATE_BO:     DRM_RENDER_ALLOW
 *   MMAP_BO:       DRM_RENDER_ALLOW
 *   GET_PARAM:     DRM_RENDER_ALLOW
 *   GET_BO_OFFSET: DRM_RENDER_ALLOW
 * 3.10 mapping: DRM_RENDER_ALLOW -> DRM_UNLOCKED (see drm_compat_310.h);
 * SUBMIT keeps DRM_AUTH so the legacy authenticated gate still applies.
 *
 * The driver defines panfrost_ioctl_<submit|wait_bo|create_bo|mmap_bo|
 * get_param|get_bo_offset>(struct drm_device *, void *, struct drm_file *)
 * with the 3.10 drm_ioctl_t signature (same as v5.2 handlers) in its own
 * .c file, then instantiates:
 *
 *   static struct drm_ioctl_desc panfrost_ioctls[] = {
 *       PANFROST_IOCTL_310(SUBMIT, submit),
 *       ...
 *   };
 *
 * and sets .ioctls/.num_ioctls before drm_platform_init() (cf. exynos
 * exynos_ioctls + DRM_IOCTL_DEF_DRV rows, exynos_drm_drv.c:221-248,284-285).
 * GET_PARAM/GET_BO_OFFSET need no fence/sched support and come up first
 * (B1 in the inventory); SUBMIT needs the fence->resv->syncobj->sched
 * chain (patches/drm-backport/0001-0005, owned by G31_Sched) landed.
 *
 * Per-minor routing note: 3.10 drm_ioctl() dispatches purely on the ioctl
 * number (drm_drv.c:399-411); the render-vs-primary routing is enforced by
 * G31_Kms' Mode-B core changes (render minor via drm_get_minor(...,
 * DRM_MINOR_RENDER), always-authenticated render open, DRM_MASTER-on-render
 * deny). Nothing in this header routes minors.
 */
#include <drm/drmP.h>
#include "drm_compat_310.h"

/* Driver ioctl numbers: v5.2 panfrost_drm.h command codes 0x00-0x05. */
#define DRM_PANFROST_SUBMIT_310		0x00
#define DRM_PANFROST_WAIT_BO_310	0x01
#define DRM_PANFROST_CREATE_BO_310	0x02
#define DRM_PANFROST_MMAP_BO_310	0x03
#define DRM_PANFROST_GET_PARAM_310	0x04
#define DRM_PANFROST_GET_BO_OFFSET_310	0x05

/* UAPI command macros must match v5.2 byte-identical values. */
#define DRM_IOCTL_PANFROST_SUBMIT_310		DRM_IOW(DRM_COMMAND_BASE + \
		DRM_PANFROST_SUBMIT_310, struct drm_panfrost_submit_310)
#define DRM_IOCTL_PANFROST_WAIT_BO_310		DRM_IOW(DRM_COMMAND_BASE + \
		DRM_PANFROST_WAIT_BO_310, struct drm_panfrost_wait_bo_310)
#define DRM_IOCTL_PANFROST_CREATE_BO_310	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_PANFROST_CREATE_BO_310, struct drm_panfrost_create_bo_310)
#define DRM_IOCTL_PANFROST_MMAP_BO_310		DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_PANFROST_MMAP_BO_310, struct drm_panfrost_mmap_bo_310)
#define DRM_IOCTL_PANFROST_GET_PARAM_310	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_PANFROST_GET_PARAM_310, struct drm_panfrost_get_param_310)
#define DRM_IOCTL_PANFROST_GET_BO_OFFSET_310	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_PANFROST_GET_BO_OFFSET_310, struct drm_panfrost_get_bo_offset_310)

/*
 * Minimal struct mirrors of v5.2 panfrost_drm.h (field-identical; the
 * driver compiles against these if the G31_Gem-owned full UAPI mirror is
 * not yet present). Kept in this header so the -fsyntax-only compile
 * check is self-contained; at driver land time G31_Gem's mirror wins and
 * these typedefs must be dropped in favour of it (UAPI stays byte-
 * identical either way).
 */
struct drm_panfrost_submit_310 {
	__u64 jc;
	__u64 in_syncs;
	__u32 in_sync_count;
	__u32 out_sync;
	__u64 bo_handles;
	__u32 bo_handle_count;
	__u32 requirements;
};

struct drm_panfrost_wait_bo_310 {
	__u32 handle;
	__u32 pad;
	__s64 timeout_ns;
};

struct drm_panfrost_create_bo_310 {
	__u32 size;
	__u32 flags;
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

struct drm_panfrost_mmap_bo_310 {
	__u32 handle;
	__u32 flags;
	__u64 offset;
};

struct drm_panfrost_get_param_310 {
	__u32 param;
	__u32 pad;
	__u64 value;
};

struct drm_panfrost_get_bo_offset_310 {
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

/*
 * Legacy 3.10 row constructors. DRM_IOCTL_DEF_DRV (drmP.h) takes the
 * short name (DRM_PANFROST_SUBMIT) plus driver cmd; the _310 suffixes
 * above disambiguate from the full UAPI header when both are included.
 * Flags mirror v5.2 (see table above).
 */
#define PANFROST_IOCTL_310(n, fn, fl) \
	DRM_IOCTL_DEF_DRV(PANFROST_##n##_310, panfrost_ioctl_##fn, fl)

#define PANFROST_IOCTLS_310						\
	PANFROST_IOCTL_310(SUBMIT, submit, DRM_RENDER_ALLOW | DRM_AUTH),	\
	PANFROST_IOCTL_310(WAIT_BO, wait_bo, DRM_RENDER_ALLOW),		\
	PANFROST_IOCTL_310(CREATE_BO, create_bo, DRM_RENDER_ALLOW),	\
	PANFROST_IOCTL_310(MMAP_BO, mmap_bo, DRM_RENDER_ALLOW),		\
	PANFROST_IOCTL_310(GET_PARAM, get_param, DRM_RENDER_ALLOW),	\
	PANFROST_IOCTL_310(GET_BO_OFFSET, get_bo_offset, DRM_RENDER_ALLOW)

#endif /* _PANFROST_IOCTLS_310_H_ */
