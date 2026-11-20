/* SPDX-License-Identifier: MIT */
/*
 * pf7580.h - userspace client for the panfrost7580 v5.2 DRM UAPI.
 *
 * Thin wrappers over the six ioctls owned by code/gpu31/gem
 * (panfrost7580_ioctl.c) on the GEM core owned by code/gpu31/drmcore
 * (panfrost_ioctls_310.h rows):
 *
 *   GET_PARAM(0x04)  CREATE_BO(0x02)  MMAP_BO(0x03)
 *   SUBMIT(0x00)     WAIT_BO(0x01)    GET_BO_OFFSET(0x05)
 *
 * UAPI source of truth: code/gpu31/gem/panfrost_drm.h (byte-identical to
 * Linux v5.2 include/uapi/drm/panfrost_drm.h). The structs below are
 * field-identical mirrors with pf7580_ names so this client stays
 * self-contained (only <drm/drm.h> + libc, no libdrm, no system
 * panfrost_drm.h needed); pf7580.c static-asserts the wire sizes and
 * ioctl encodings against the system header when it is present.
 *
 * Two backends behind one API:
 *   pf7580_open(path)       real render node (DRM ioctl path).
 *   pf7580_open_mock()      in-memory stub that enforces the same
 *                           validation rules as panfrost7580_ioctl.c,
 *                           so the self-tests run on any build host.
 *
 * All calls return 0 or -errno. On open failure 'err' is filled.
 */

#ifndef PF7580_H
#define PF7580_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GET_PARAM ids (v5.2: only GPU_PROD_ID exists). */
#define PF7580_PARAM_GPU_PROD_ID	0u

/* T720 product id the j7elte gate requires. */
#define PF7580_T720_PROD_ID		0x0720u

/* SUBMIT requirement bits (v5.2: only FS exists). */
#define PF7580_JD_REQ_FS		(1u << 0)

/* Validation limits mirrored from panfrost_ioctl_submit(). */
#define PF7580_SUBMIT_MAX_SYNCS		16u
#define PF7580_SUBMIT_MAX_BOS		64u

/* Driver ioctl numbers: v5.2 panfrost_drm.h command codes 0x00-0x05. */
#define PF7580_CMD_SUBMIT		0x00
#define PF7580_CMD_WAIT_BO		0x01
#define PF7580_CMD_CREATE_BO		0x02
#define PF7580_CMD_MMAP_BO		0x03
#define PF7580_CMD_GET_PARAM		0x04
#define PF7580_CMD_GET_BO_OFFSET	0x05

/* Field-identical mirrors of v5.2 struct drm_panfrost_*. */
struct pf7580_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

struct pf7580_wait_bo {
	uint32_t handle;
	uint32_t pad;
	int64_t timeout_ns;	/* absolute CLOCK_MONOTONIC */
};

struct pf7580_create_bo {
	uint32_t size;
	uint32_t flags;		/* reserved, must be zero */
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;	/* per-fd GPU address, never zero */
};

struct pf7580_mmap_bo {
	uint32_t handle;
	uint32_t flags;		/* reserved, must be zero */
	uint64_t offset;	/* fake offset for mmap() on the DRM fd */
};

struct pf7580_get_param {
	uint32_t param;
	uint32_t pad;		/* must be zero */
	uint64_t value;
};

struct pf7580_get_bo_offset {
	uint32_t handle;
	uint32_t pad;		/* must be zero */
	uint64_t offset;
};

/* Wire sizes must stay v5.2-identical. */
_Static_assert(sizeof(struct pf7580_submit) == 40, "pf7580_submit wire size");
_Static_assert(sizeof(struct pf7580_wait_bo) == 16, "pf7580_wait_bo wire size");
_Static_assert(sizeof(struct pf7580_create_bo) == 24, "pf7580_create_bo wire size");
_Static_assert(sizeof(struct pf7580_mmap_bo) == 16, "pf7580_mmap_bo wire size");
_Static_assert(sizeof(struct pf7580_get_param) == 16, "pf7580_get_param wire size");
_Static_assert(sizeof(struct pf7580_get_bo_offset) == 16, "pf7580_get_bo_offset wire size");

struct pf7580_dev;

/* Open a real render node (e.g. /dev/dri/renderD128). */
int pf7580_open(const char *path, struct pf7580_dev **out,
		char *err, size_t errlen);
/* In-memory stub backend (no device needed). */
int pf7580_open_mock(struct pf7580_dev **out);
void pf7580_close(struct pf7580_dev *dev);
/* Underlying DRM fd, or -1 for the mock backend. */
int pf7580_fd(const struct pf7580_dev *dev);
int pf7580_is_mock(const struct pf7580_dev *dev);

/* One-shot fault injection for the mock backend: the next call of any
 * ioctl wrapper fails with -fail_err (a positive errno). Zero disables. */
void pf7580_mock_fail_next(struct pf7580_dev *dev, int fail_err);

/* The six ioctls. */
int pf7580_get_param(struct pf7580_dev *dev, uint32_t param, uint64_t *value);
int pf7580_create_bo(struct pf7580_dev *dev, uint32_t size,
		     uint32_t *handle, uint64_t *gpu_offset);
int pf7580_mmap_bo(struct pf7580_dev *dev, uint32_t handle,
		   uint64_t *map_offset);
int pf7580_get_bo_offset(struct pf7580_dev *dev, uint32_t handle,
			 uint64_t *gpu_offset);
int pf7580_wait_bo(struct pf7580_dev *dev, uint32_t handle,
		   int64_t timeout_ns);

struct pf7580_submit_args {
	uint64_t jc;			/* GPU VA of the job descriptor */
	const uint32_t *in_syncs;	/* optional syncobj handles */
	uint32_t in_sync_count;
	uint32_t out_sync;		/* optional syncobj handle */
	const uint32_t *bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;		/* PF7580_JD_REQ_* */
};

int pf7580_submit(struct pf7580_dev *dev,
		  const struct pf7580_submit_args *args);

/* Generic DRM syncobj ioctls (0xBF-0xC5; routed by the driver's fops
 * dispatcher, NOT the panfrost table). Needed so the submit test can
 * wait on the job's OWN fence via out_sync: WAIT_BO is vacuous (the job
 * fence is never attached to the BO resv — close-fix 476b75d), so only
 * the out_sync fence proves the GPU retired the job. Mock backend:
 * in-memory signalled/pending table mirroring the 0003 shim. Wire
 * shapes are the 20.x drm.h contract (HANDLE 16 B, WAIT 32 B); the
 * selftest prints them in --print-abi (gpu31-abialign). */
struct pf7580_syncobj_handle {
	uint32_t handle;
	uint32_t flags;
	int32_t fd;
	uint32_t pad;
};
struct pf7580_syncobj_wait {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles;
	uint32_t flags;
	uint32_t first_signaled;
	uint32_t pad;
};
int pf7580_syncobj_create(struct pf7580_dev *dev, uint32_t flags,
			  uint32_t *handle);
int pf7580_syncobj_destroy(struct pf7580_dev *dev, uint32_t handle);
/* Mesa 20.3 fence-snapshot path (pan_screen.c panfrost_fence_create):
 * export a syncobj to an fd and import it into a fresh syncobj.
 * Flags bit 0 (SYNC_FILE) is tolerated (libdrm sets it unconditionally);
 * all other bits are -EINVAL. Returns the installed fd / new handle. */
int pf7580_syncobj_handle_to_fd(struct pf7580_dev *dev, uint32_t handle,
				uint32_t flags, int *fd);
int pf7580_syncobj_fd_to_handle(struct pf7580_dev *dev, int fd,
				uint32_t flags, uint32_t *handle);
/* Wait on count syncobj handles (handles array of count u32). timeout_ns
 * is absolute CLOCK_MONOTONIC; flags: bit 0 WAIT_ALL (default ANY);
 * first_signaled (non-NULL, ANY mode) gets the first signaled index. */
int pf7580_syncobj_wait(struct pf7580_dev *dev, const uint32_t *handles,
			uint32_t count, int64_t timeout_ns, uint32_t flags,
			uint32_t *first_signaled);

/* CPU mapping helpers. Real backend: mmap() on the DRM fd at the fake
 * offset returned by MMAP_BO. Mock backend: malloc-backed shadow memory
 * of the same size. Either way the caller must pf7580_cpu_unmap() it.
 * On real-backend failure the exact request (errno/offset/len) is left
 * in pf7580_cpu_map_errno/_off/_len for the selftest to print. */
extern int pf7580_cpu_map_errno;
extern uint64_t pf7580_cpu_map_off;
extern size_t pf7580_cpu_map_len;
void *pf7580_cpu_map(struct pf7580_dev *dev, uint32_t handle,
		     uint64_t map_offset, size_t size);
int pf7580_cpu_unmap(struct pf7580_dev *dev, void *addr, size_t size);

const char *pf7580_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* PF7580_H */
