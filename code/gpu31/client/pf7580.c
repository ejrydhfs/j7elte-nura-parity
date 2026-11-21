/* SPDX-License-Identifier: MIT */
/*
 * pf7580.c - userspace client for the panfrost7580 v5.2 DRM UAPI.
 *
 * Backend selection is per-handle: the real backend issues the six
 * ioctls on a render-node fd; the mock backend enforces the same
 * validation rules as code/gpu31/gem/panfrost7580_ioctl.c in memory
 * so the self-tests run on any build host with no device.
 *
 * Validation rules mirrored from panfrost7580_ioctl.c:
 *   GET_PARAM:     pad!=0 -> EINVAL; param!=GPU_PROD_ID -> EINVAL.
 *   CREATE_BO:     flags!=0 -> EINVAL; size==0 -> EINVAL.
 *   MMAP_BO:       flags!=0 -> EINVAL; unknown handle -> ENOENT.
 *   GET_BO_OFFSET: pad!=0 -> EINVAL; unknown handle -> ENOENT.
 *   WAIT_BO:       pad!=0 -> EINVAL; unknown handle -> ENOENT.
 *   SUBMIT:        jc==0 or (jc&7) -> EINVAL;
 *                  requirements & ~FS -> EINVAL;
 *                  in_sync_count>16 or bo_handle_count>64 -> EINVAL.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdint.h>

#include <drm/drm.h>

#include "pf7580.h"

/* Ioctl encodings, byte-identical to v5.2 panfrost_drm.h:
 * SUBMIT/WAIT_BO are IOW, the other four are IOWR. */
#define PF7580_IOCTL_SUBMIT \
	DRM_IOW(DRM_COMMAND_BASE + PF7580_CMD_SUBMIT, struct pf7580_submit)
#define PF7580_IOCTL_WAIT_BO \
	DRM_IOW(DRM_COMMAND_BASE + PF7580_CMD_WAIT_BO, struct pf7580_wait_bo)
#define PF7580_IOCTL_CREATE_BO \
	DRM_IOWR(DRM_COMMAND_BASE + PF7580_CMD_CREATE_BO, struct pf7580_create_bo)
#define PF7580_IOCTL_MMAP_BO \
	DRM_IOWR(DRM_COMMAND_BASE + PF7580_CMD_MMAP_BO, struct pf7580_mmap_bo)
#define PF7580_IOCTL_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + PF7580_CMD_GET_PARAM, struct pf7580_get_param)
#define PF7580_IOCTL_GET_BO_OFFSET \
	DRM_IOWR(DRM_COMMAND_BASE + PF7580_CMD_GET_BO_OFFSET, struct pf7580_get_bo_offset)

/* Generic syncobj ioctls (core numbers 0xBF-0xC5, NOT driver-private;
 * the driver's fops dispatcher routes them — G31_SyncobjIoctl. Wire types
 * are the 20.x drm.h shapes Mesa 20.3 was built against (verified
 * on-device 2026-11-20): HANDLE 16 B (no point tail), WAIT 32 B (no
 * deadline_nsec tail). The driver dispatches on _IOC_NR and truncates
 * longer userspace structs, so the 24/40 B current-header shapes keep
 * working through libdrm -- but THIS client encodes the 16/32 B shapes
 * exactly (gpu31-abialign table). */
struct pf7580_syncobj_create {
	uint32_t handle;
	uint32_t flags;
};
struct pf7580_syncobj_destroy {
	uint32_t handle;
	uint32_t pad;
};
#define PF7580_IOCTL_SYNCOBJ_CREATE \
	DRM_IOWR(0xBF, struct pf7580_syncobj_create)
/* 20.x drm.h says DRM_IOWR for DESTROY (libdrm 2.4.134 encodes
 * 0xc00864c0 -- verified disassembly); the old DRM_IOW spelling
 * aliases the same _IOC_NR so dispatch is identical, but new code
 * must use IOWR (direction is part of the ABI contract). */
#define PF7580_IOCTL_SYNCOBJ_DESTROY \
	DRM_IOWR(0xC0, struct pf7580_syncobj_destroy)
#define PF7580_IOCTL_SYNCOBJ_HANDLE_TO_FD \
	DRM_IOWR(0xC1, struct pf7580_syncobj_handle)
#define PF7580_IOCTL_SYNCOBJ_FD_TO_HANDLE \
	DRM_IOWR(0xC2, struct pf7580_syncobj_handle)
#define PF7580_IOCTL_SYNCOBJ_WAIT_SZ 32
#define PF7580_IOCTL_SYNCOBJ_WAIT \
	DRM_IOWR(0xC3, char[PF7580_IOCTL_SYNCOBJ_WAIT_SZ])
#define PF7580_SYNCOBJ_WAIT_ALL		(1u << 0)
/* libdrm 2.4.134 sets bit 0 (SYNC_FILE) unconditionally on HTOF/FTOH;
 * the driver tolerates it (plain-fence snapshot satisfies Mesa). */
#define PF7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE	(1u << 0)
#define PF7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE	(1u << 0)

/* Compile-time wire check: our mirrors must encode exactly like the
 * v5.2 directions above (a direction flip changes the ioctl number). */
_Static_assert(PF7580_IOCTL_SUBMIT ==
	DRM_IOW(DRM_COMMAND_BASE + 0x00, struct pf7580_submit),
	"submit encoding");
_Static_assert(PF7580_IOCTL_WAIT_BO ==
	DRM_IOW(DRM_COMMAND_BASE + 0x01, struct pf7580_wait_bo),
	"wait_bo encoding");
_Static_assert(PF7580_IOCTL_CREATE_BO ==
	DRM_IOWR(DRM_COMMAND_BASE + 0x02, struct pf7580_create_bo),
	"create_bo encoding");
_Static_assert(PF7580_IOCTL_MMAP_BO ==
	DRM_IOWR(DRM_COMMAND_BASE + 0x03, struct pf7580_mmap_bo),
	"mmap_bo encoding");
_Static_assert(PF7580_IOCTL_GET_PARAM ==
	DRM_IOWR(DRM_COMMAND_BASE + 0x04, struct pf7580_get_param),
	"get_param encoding");
_Static_assert(PF7580_IOCTL_GET_BO_OFFSET ==
	DRM_IOWR(DRM_COMMAND_BASE + 0x05, struct pf7580_get_bo_offset),
	"get_bo_offset encoding");
/* CREATE 8 B, DESTROY 8 B, HANDLE 16 B match 20.x drm.h; WAIT matches
 * the DRIVER's 32 B wait (no deadline tail -- the host drm.h added a
 * u64 deadline_nsec the 3.10 driver does not know). The driver's fops
 * dispatcher keys on _IOC_NR and truncates longer userspace structs,
 * so WAIT is encoded as a fixed 32 B payload, NOT sizeof(struct
 * drm_syncobj_wait) (40 B on current hosts). */
_Static_assert(sizeof(struct pf7580_syncobj_create) ==
	sizeof(struct drm_syncobj_create), "syncobj_create wire size");
_Static_assert(sizeof(struct pf7580_syncobj_destroy) ==
	sizeof(struct drm_syncobj_destroy), "syncobj_destroy wire size");
_Static_assert(sizeof(struct pf7580_syncobj_handle) == 16,
	"syncobj_handle driver wire size");
_Static_assert(sizeof(struct pf7580_syncobj_wait) == 32,
	"syncobj_wait driver wire size");
#define PF7580_SYNCOBJ_WAIT_SIZE 32
_Static_assert(PF7580_IOCTL_SYNCOBJ_CREATE ==
	DRM_IOWR(0xBF, struct drm_syncobj_create), "syncobj_create enc");
_Static_assert(PF7580_IOCTL_SYNCOBJ_DESTROY ==
	DRM_IOWR(0xC0, struct drm_syncobj_destroy), "syncobj_destroy enc");

/* ------------------------------------------------------------------ */
/* Device handle. */

/* Only CREATE_SIGNALED is valid on create (mainline contract). */
#define PF7580_SYNCOBJ_CREATE_SIGNALED	(1u << 0)
#define PF7580_SYNCOBJ_MAX_HANDLES	64u

#define PF7580_MOCK_MAX_BOS 64
#define PF7580_MOCK_MAX_SYNCOBJS 64
/* Mock IOVA window: nonzero, 2 MiB-spaced, inside the driver's
 * 32 MiB .. 4 GiB window (P7580_IOVA_START/SIZE). */
#define PF7580_MOCK_IOVA_BASE 0x10000000ull
#define PF7580_MOCK_IOVA_STRIDE 0x200000ull
/* Mock fake mmap offset: handle << PAGE_SHIFT, same encoding as
 * drm_gem_cma_dumb_map_offset (map_list.hash.key << PAGE_SHIFT). */
#define PF7580_MOCK_PAGE_SHIFT 12u

struct pf7580_mock_bo {
	int used;
	uint32_t handle;
	uint32_t size;
	uint64_t gpu_offset;
	uint64_t map_offset;
	void *shadow;	/* malloc(size), persistent until close */
};

struct pf7580_mock_syncobj {
	int used;
	uint32_t handle;
	int signaled;	/* 0 pending, 1 signaled (binary payload) */
	int64_t signal_at_ns;	/* sim-clock: pending fence fires here */
};

struct pf7580_dev {
	int fd;		/* real backend DRM fd, -1 for mock */
	int is_mock;
	int fail_next;	/* positive errno for one-shot fault injection */
	/* Mock state. */
	uint32_t next_handle;
	struct pf7580_mock_bo bos[PF7580_MOCK_MAX_BOS];
	uint32_t next_syncobj;
	struct pf7580_mock_syncobj syncobjs[PF7580_MOCK_MAX_SYNCOBJS];
	/* Sim-clock (mock syncobj waits): virtual CLOCK_MONOTONIC ns.
	 * WAIT compares the absolute deadline against each fence's
	 * signal_at_ns WITHOUT sleeping: a programmed signal inside
	 * the budget returns 0 and advances now_ns to the signal; a
	 * deadline that passes first advances now_ns to the deadline
	 * and returns -ETIME. Proves the block-vs-expire logic
	 * deterministically (see t_syncobj_wait_block in selftest). */
	int64_t mock_now_ns;
};

static struct pf7580_mock_bo *mock_find(struct pf7580_dev *dev,
					 uint32_t handle)
{
	unsigned i;

	for (i = 0; i < PF7580_MOCK_MAX_BOS; i++)
		if (dev->bos[i].used && dev->bos[i].handle == handle)
			return &dev->bos[i];
	return NULL;
}

/* One-shot fault injection (mock backend only). */
static int mock_inject(struct pf7580_dev *dev)
{
	if (dev->fail_next) {
		int e = dev->fail_next;

		dev->fail_next = 0;
		return -e;
	}
	return 0;
}

int pf7580_open(const char *path, struct pf7580_dev **out,
		char *err, size_t errlen)
{
	struct pf7580_dev *dev;
	int fd;

	if (!path || !out)
		return -EINVAL;
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		int e = errno;

		if (err && errlen)
			snprintf(err, errlen, "%s: %s", path, strerror(e));
		return -e;
	}
	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		close(fd);
		return -ENOMEM;
	}
	dev->fd = fd;
	dev->is_mock = 0;
	*out = dev;
	return 0;
}

int pf7580_open_mock(struct pf7580_dev **out)
{
	struct pf7580_dev *dev;

	if (!out)
		return -EINVAL;
	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -ENOMEM;
	dev->fd = -1;
	dev->is_mock = 1;
	dev->next_handle = 1;
	dev->next_syncobj = 1;
	*out = dev;
	return 0;
}

void pf7580_close(struct pf7580_dev *dev)
{
	unsigned i;

	if (!dev)
		return;
	if (dev->is_mock) {
		for (i = 0; i < PF7580_MOCK_MAX_BOS; i++)
			free(dev->bos[i].shadow);
	} else if (dev->fd >= 0) {
		close(dev->fd);
	}
	free(dev);
}

int pf7580_fd(const struct pf7580_dev *dev)
{
	if (!dev)
		return -EINVAL;
	return dev->fd;
}

int pf7580_is_mock(const struct pf7580_dev *dev)
{
	return dev ? dev->is_mock : 0;
}

void pf7580_mock_fail_next(struct pf7580_dev *dev, int fail_err)
{
	if (dev)
		dev->fail_next = fail_err;
}

/* ------------------------------------------------------------------ */
/* The six ioctls. */

int pf7580_get_param(struct pf7580_dev *dev, uint32_t param, uint64_t *value)
{
	if (!dev || !value)
		return -EINVAL;
	if (dev->is_mock) {
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		/* Mirror panfrost_ioctl_get_param(). */
		if (param != PF7580_PARAM_GPU_PROD_ID)
			return -EINVAL;
		*value = PF7580_T720_PROD_ID;
		return 0;
	}
	{
		struct pf7580_get_param p;

		memset(&p, 0, sizeof(p));
		p.param = param;
		if (ioctl(dev->fd, PF7580_IOCTL_GET_PARAM, &p))
			return -errno;
		*value = p.value;
		return 0;
	}
}

int pf7580_create_bo(struct pf7580_dev *dev, uint32_t size,
		     uint32_t *handle, uint64_t *gpu_offset)
{
	if (!dev || !handle || !gpu_offset)
		return -EINVAL;
	if (dev->is_mock) {
		unsigned i;
		struct pf7580_mock_bo *slot = NULL;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		/* Mirror panfrost_ioctl_create_bo(): flags is always 0
		 * through this wrapper, so only the size is checked. */
		if (!size)
			return -EINVAL;
		for (i = 0; i < PF7580_MOCK_MAX_BOS; i++) {
			if (!dev->bos[i].used) {
				slot = &dev->bos[i];
				break;
			}
		}
		if (!slot)
			return -ENOSPC;
		slot->shadow = calloc(1, size);
		if (!slot->shadow)
			return -ENOMEM;
		slot->used = 1;
		slot->handle = dev->next_handle++;
		slot->size = size;
		slot->gpu_offset = PF7580_MOCK_IOVA_BASE +
			(uint64_t)(slot->handle - 1) * PF7580_MOCK_IOVA_STRIDE;
		slot->map_offset = (uint64_t)slot->handle
			<< PF7580_MOCK_PAGE_SHIFT;
		*handle = slot->handle;
		*gpu_offset = slot->gpu_offset;
		return 0;
	}
	{
		struct pf7580_create_bo c;

		memset(&c, 0, sizeof(c));
		c.size = size;
		c.flags = 0;
		if (ioctl(dev->fd, PF7580_IOCTL_CREATE_BO, &c))
			return -errno;
		*handle = c.handle;
		*gpu_offset = c.offset;
		return 0;
	}
}

int pf7580_mmap_bo(struct pf7580_dev *dev, uint32_t handle,
		   uint64_t *map_offset)
{
	if (!dev || !map_offset)
		return -EINVAL;
	if (dev->is_mock) {
		struct pf7580_mock_bo *bo;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		bo = mock_find(dev, handle);
		if (!bo)
			return -ENOENT;
		*map_offset = bo->map_offset;
		return 0;
	}
	{
		struct pf7580_mmap_bo m;

		memset(&m, 0, sizeof(m));
		m.handle = handle;
		m.flags = 0;
		if (ioctl(dev->fd, PF7580_IOCTL_MMAP_BO, &m))
			return -errno;
		*map_offset = m.offset;
		return 0;
	}
}

int pf7580_get_bo_offset(struct pf7580_dev *dev, uint32_t handle,
			 uint64_t *gpu_offset)
{
	if (!dev || !gpu_offset)
		return -EINVAL;
	if (dev->is_mock) {
		struct pf7580_mock_bo *bo;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		bo = mock_find(dev, handle);
		if (!bo)
			return -ENOENT;
		*gpu_offset = bo->gpu_offset;
		return 0;
	}
	{
		struct pf7580_get_bo_offset g;

		memset(&g, 0, sizeof(g));
		g.handle = handle;
		if (ioctl(dev->fd, PF7580_IOCTL_GET_BO_OFFSET, &g))
			return -errno;
		*gpu_offset = g.offset;
		return 0;
	}
}

int pf7580_wait_bo(struct pf7580_dev *dev, uint32_t handle,
		   int64_t timeout_ns)
{
	if (!dev)
		return -EINVAL;
	if (dev->is_mock) {
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		/* Mirror panfrost_ioctl_wait_bo(): unknown handle fails;
		 * the stub has no fences, so any known BO is idle. */
		if (!mock_find(dev, handle))
			return -ENOENT;
		(void)timeout_ns;
		return 0;
	}
	{
		struct pf7580_wait_bo w;

		memset(&w, 0, sizeof(w));
		w.handle = handle;
		w.timeout_ns = timeout_ns;
		if (ioctl(dev->fd, PF7580_IOCTL_WAIT_BO, &w))
			return -errno;
		return 0;
	}
}

int pf7580_submit(struct pf7580_dev *dev,
		  const struct pf7580_submit_args *args)
{
	if (!dev || !args)
		return -EINVAL;
	/* Client-side copy of the kernel's validation so both backends
	 * reject bad submits identically. Mirrors panfrost_ioctl_submit(). */
	if (!args->jc || (args->jc & 7u))
		return -EINVAL;
	if (args->requirements & ~PF7580_JD_REQ_FS)
		return -EINVAL;
	if (args->in_sync_count > PF7580_SUBMIT_MAX_SYNCS ||
	    args->bo_handle_count > PF7580_SUBMIT_MAX_BOS)
		return -EINVAL;
	if ((args->in_sync_count && !args->in_syncs) ||
	    (args->bo_handle_count && !args->bo_handles))
		return -EINVAL;
	if (dev->is_mock) {
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		return 0;
	}
	{
		struct pf7580_submit s;

		memset(&s, 0, sizeof(s));
		s.jc = args->jc;
		s.in_syncs = (uint64_t)(uintptr_t)args->in_syncs;
		s.in_sync_count = args->in_sync_count;
		s.out_sync = args->out_sync;
		s.bo_handles = (uint64_t)(uintptr_t)args->bo_handles;
		s.bo_handle_count = args->bo_handle_count;
		s.requirements = args->requirements;
		if (ioctl(dev->fd, PF7580_IOCTL_SUBMIT, &s))
			return -errno;
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* Generic syncobj ioctls (out_sync job-fence proof). */

static struct pf7580_mock_syncobj *mock_syncobj_find(struct pf7580_dev *dev,
						     uint32_t handle)
{
	unsigned i;

	for (i = 0; i < PF7580_MOCK_MAX_SYNCOBJS; i++)
		if (dev->syncobjs[i].used && dev->syncobjs[i].handle == handle)
			return &dev->syncobjs[i];
	return NULL;
}

int64_t pf7580_mock_now(struct pf7580_dev *dev)
{
	if (!dev || !dev->is_mock)
		return -EINVAL;
	return dev->mock_now_ns;
}

void pf7580_mock_advance(struct pf7580_dev *dev, int64_t to_ns)
{
	if (!dev || !dev->is_mock)
		return;
	if (to_ns > dev->mock_now_ns)
		dev->mock_now_ns = to_ns;
}

int pf7580_mock_signal_at(struct pf7580_dev *dev, uint32_t handle,
			   int64_t fire_ns)
{
	struct pf7580_mock_syncobj *o;

	if (!dev || !dev->is_mock || !handle)
		return -EINVAL;
	o = mock_syncobj_find(dev, handle);
	if (!o)
		return -ENOENT;
	if (o->signaled)
		return 0;
	o->signal_at_ns = fire_ns;
	return 0;
}


int pf7580_syncobj_create(struct pf7580_dev *dev, uint32_t flags,
			  uint32_t *handle)
{
	if (!dev || !handle)
		return -EINVAL;
	if (flags & ~PF7580_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;
	if (dev->is_mock) {
		unsigned i;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		for (i = 0; i < PF7580_MOCK_MAX_SYNCOBJS; i++) {
			if (!dev->syncobjs[i].used) {
				dev->syncobjs[i].used = 1;
				dev->syncobjs[i].handle = dev->next_syncobj++;
				if (dev->next_syncobj == 0)
					dev->next_syncobj = 1;
				dev->syncobjs[i].signaled =
					(flags & PF7580_SYNCOBJ_CREATE_SIGNALED)
					? 1 : 0;
				dev->syncobjs[i].signal_at_ns = INT64_MAX;
				*handle = dev->syncobjs[i].handle;
				return 0;
			}
		}
		return -ENOSPC;
	}
	{
		struct pf7580_syncobj_create c;

		memset(&c, 0, sizeof(c));
		c.flags = flags;
		if (ioctl(dev->fd, PF7580_IOCTL_SYNCOBJ_CREATE, &c))
			return -errno;
		*handle = c.handle;
		return 0;
	}
}

int pf7580_syncobj_destroy(struct pf7580_dev *dev, uint32_t handle)
{
	if (!dev || !handle)
		return -EINVAL;
	if (dev->is_mock) {
		struct pf7580_mock_syncobj *o;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		o = mock_syncobj_find(dev, handle);
		if (!o)
			return -ENOENT;
		o->used = 0;
		return 0;
	}
	{
		struct pf7580_syncobj_destroy d;

		memset(&d, 0, sizeof(d));
		d.handle = handle;
		if (ioctl(dev->fd, PF7580_IOCTL_SYNCOBJ_DESTROY, &d))
			return -errno;
		return 0;
	}
}

/* Mesa fence-snapshot round-trip (gpu31-abialign §4): export + import.
 * Mock backend: the fd is faked as the syncobj handle cast to int
 * (signalled state travels with the mock entry); FTOH of any other
 * int is -EINVAL, mirroring the kernel f_op check. */
int pf7580_syncobj_handle_to_fd(struct pf7580_dev *dev, uint32_t handle,
				uint32_t flags, int *fd)
{
	if (!dev || !handle || !fd)
		return -EINVAL;
	if (flags & ~(PF7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE))
		return -EINVAL;
	if (dev->is_mock) {
		struct pf7580_mock_syncobj *o;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		o = mock_syncobj_find(dev, handle);
		if (!o)
			return -ENOENT;
		if (!o->signaled)
			return -EINVAL;	/* nothing to export (kernel parity) */
		*fd = (int)o->handle;
		return 0;
	}
	{
		struct pf7580_syncobj_handle h;

		memset(&h, 0, sizeof(h));
		h.handle = handle;
		h.flags = flags;
		h.fd = -1;
		if (ioctl(dev->fd, PF7580_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h))
			return -errno;
		*fd = h.fd;
		return 0;
	}
}

int pf7580_syncobj_fd_to_handle(struct pf7580_dev *dev, int fd,
				uint32_t flags, uint32_t *handle)
{
	if (!dev || fd < 0 || !handle)
		return -EINVAL;
	if (flags & ~(PF7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE))
		return -EINVAL;
	if (dev->is_mock) {
		struct pf7580_mock_syncobj *o;
		unsigned i;
		int rc = mock_inject(dev);

		if (rc)
			return rc;
		o = mock_syncobj_find(dev, (uint32_t)fd);
		if (!o)
			return -EINVAL;	/* foreign fd (kernel f_op parity) */
		for (i = 0; i < PF7580_MOCK_MAX_SYNCOBJS; i++) {
			if (!dev->syncobjs[i].used) {
				dev->syncobjs[i].used = 1;
				dev->syncobjs[i].handle = dev->next_syncobj++;
				if (dev->next_syncobj == 0)
					dev->next_syncobj = 1;
				dev->syncobjs[i].signaled = o->signaled;
				dev->syncobjs[i].signal_at_ns = o->signal_at_ns;
				*handle = dev->syncobjs[i].handle;
				return 0;
			}
		}
		return -ENOSPC;
	}
	{
		struct pf7580_syncobj_handle h;

		memset(&h, 0, sizeof(h));
		h.flags = flags;
		h.fd = fd;
		if (ioctl(dev->fd, PF7580_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h))
			return -errno;
		*handle = h.handle;
		return 0;
	}
}

int pf7580_syncobj_wait(struct pf7580_dev *dev, const uint32_t *handles,
			uint32_t count, int64_t timeout_ns, uint32_t flags,
			uint32_t *first_signaled)
{
	if (!dev || !handles || !count ||
	    count > PF7580_SYNCOBJ_MAX_HANDLES)
		return -EINVAL;
	if (flags & ~PF7580_SYNCOBJ_WAIT_ALL)
		return -EINVAL;
	if (dev->is_mock) {
		uint32_t i;
		int rc = mock_inject(dev);
		int all;

		if (rc)
			return rc;
		/* Sim-clock wait (no sleeping): unknown handles fail
		 * like the 0003 shim (-ENOENT); empty/signaled fences
		 * count as signaled (kernel NULL-fence parity). A
		 * pending fence "blocks" in virtual time: the first
		 * signal inside the absolute deadline advances
		 * mock_now_ns to the firing time; a deadline that
		 * passes first expires with -ETIME. Programs via
		 * pf7580_mock_signal_at(); pf7580_mock_now() reads
		 * and pf7580_mock_advance() moves the clock. */
		if (timeout_ns < 0)
			return -EINVAL;
		all = (flags & PF7580_SYNCOBJ_WAIT_ALL) != 0;
		if (all) {
			int64_t latest = dev->mock_now_ns;
			int pending = 0;

			for (i = 0; i < count; i++) {
				struct pf7580_mock_syncobj *o =
					mock_syncobj_find(dev, handles[i]);

				if (!o)
					return -ENOENT;
				if (o->signaled)
					continue;
				if (o->signal_at_ns <= dev->mock_now_ns)
					continue;
				if (o->signal_at_ns > timeout_ns) {
					pending = 1;
					continue;
				}
				if (o->signal_at_ns > latest)
					latest = o->signal_at_ns;
			}
			if (pending) {
				dev->mock_now_ns = timeout_ns;
				return -ETIME;
			}
			dev->mock_now_ns = latest;
			for (i = 0; i < count; i++) {
				struct pf7580_mock_syncobj *o =
					mock_syncobj_find(dev, handles[i]);

				o->signaled = 1;
			}
			if (first_signaled)
				*first_signaled = 0;
			return 0;
		}
		for (i = 0; i < count; i++) {
			struct pf7580_mock_syncobj *o =
				mock_syncobj_find(dev, handles[i]);

			if (!o)
				return -ENOENT;
			if (o->signaled)
				continue;
			if (o->signal_at_ns <= dev->mock_now_ns) {
				o->signaled = 1;
				if (first_signaled)
					*first_signaled = i;
				return 0;
			}
			if (o->signal_at_ns > timeout_ns) {
				/* Past the deadline: ANY expires at once
				 * (kernel: sub-budget 0 probe finds it
				 * unsignaled). */
				dev->mock_now_ns = timeout_ns;
				return -ETIME;
			}
			dev->mock_now_ns = o->signal_at_ns;
			o->signaled = 1;
			if (first_signaled)
				*first_signaled = i;
			return 0;
		}
		/* All signaled already (or fired at/past the current
		 * instant with a zero budget): success without moving. */
		{
			int any_pending = 0;
			uint32_t k;

			for (k = 0; k < count; k++) {
				struct pf7580_mock_syncobj *o =
					mock_syncobj_find(dev, handles[k]);

				if (!o->signaled &&
				    o->signal_at_ns > dev->mock_now_ns)
					any_pending = 1;
			}
			if (!any_pending) {
				if (first_signaled)
					*first_signaled = 0;
				return 0;
			}
		}
	}
	{
		struct pf7580_syncobj_wait w;

		memset(&w, 0, sizeof(w));
		w.handles = (uint64_t)(uintptr_t)handles;
		w.timeout_nsec = timeout_ns;
		w.count_handles = count;
		w.flags = flags;
		if (ioctl(dev->fd, PF7580_IOCTL_SYNCOBJ_WAIT, &w))
			return -errno;
		if (first_signaled)
			*first_signaled = w.first_signaled;
		return 0;
	}
}

/* Last real-backend mmap request (diagnostics for the selftest). */
int pf7580_cpu_map_errno;
uint64_t pf7580_cpu_map_off;
size_t pf7580_cpu_map_len;
/* DEBUG-ONLY (gpu31-gpuwrite): kernel BO word readback. Fops-routed
 * IOWR(0xCF) 16 B payload {handle, offset, value OUT, pad}. Mock
 * reads the shadow directly. */
struct pf7580_debug_read_bo {
	uint32_t handle;
	uint32_t offset;
	uint32_t value;
	uint32_t pad;
};
#define PF7580_IOCTL_DEBUG_READ_BO \
	DRM_IOWR(0xCF, struct pf7580_debug_read_bo)
int pf7580_debug_read_bo(struct pf7580_dev *dev, uint32_t handle,
			 uint32_t offset, uint32_t *value)
{
	if (!dev || !value)
		return -EINVAL;
	if (offset & 3u)
		return -EINVAL;
	if (dev->is_mock) {
		struct pf7580_mock_bo *bo = mock_find(dev, handle);

		if (!bo || offset + 4 > bo->size)
			return -ENOENT;
		*value = *(uint32_t *)((char *)bo->shadow + offset);
		return 0;
	}
	{
		struct pf7580_debug_read_bo r;

		memset(&r, 0, sizeof(r));
		r.handle = handle;
		r.offset = offset;
		if (ioctl(dev->fd, PF7580_IOCTL_DEBUG_READ_BO, &r))
			return -errno;
		*value = r.value;
		return 0;
	}
}


void *pf7580_cpu_map(struct pf7580_dev *dev, uint32_t handle,
		     uint64_t map_offset, size_t size)
{
	void *addr;

	if (!dev || !size)
		return NULL;
	if (dev->is_mock) {
		struct pf7580_mock_bo *bo = mock_find(dev, handle);

		if (!bo || size > bo->size) {
			pf7580_cpu_map_errno = EINVAL;
			pf7580_cpu_map_off = map_offset;
			pf7580_cpu_map_len = size;
			return NULL;
		}
		if (map_offset != bo->map_offset) {
			pf7580_cpu_map_errno = EINVAL;
			pf7580_cpu_map_off = map_offset;
			pf7580_cpu_map_len = size;
			return NULL;
		}
		pf7580_cpu_map_errno = 0;
		return bo->shadow;
	}
	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    dev->fd, (off_t)map_offset);
	if (addr == MAP_FAILED) {
		pf7580_cpu_map_errno = errno;
		pf7580_cpu_map_off = map_offset;
		pf7580_cpu_map_len = size;
		return NULL;
	}
	pf7580_cpu_map_errno = 0;
	pf7580_cpu_map_off = map_offset;
	pf7580_cpu_map_len = size;
	(void)handle;
	return addr;
}

int pf7580_cpu_unmap(struct pf7580_dev *dev, void *addr, size_t size)
{
	size_t len;
	unsigned i;

	if (!dev || !addr || !size)
		return -EINVAL;
	if (dev->is_mock) {
		/* Mock mappings are persistent shadows; just validate. */
		for (i = 0; i < PF7580_MOCK_MAX_BOS; i++) {
			if (!dev->bos[i].used)
				continue;
			len = dev->bos[i].size;
			if (addr >= dev->bos[i].shadow &&
			    (char *)addr + size <=
			    (char *)dev->bos[i].shadow + len)
				return 0;
		}
		return -EINVAL;
	}
	if (munmap(addr, size))
		return -errno;
	return 0;
}

const char *pf7580_strerror(int err)
{
	if (err < 0)
		err = -err;
	switch (err) {
	case 0: return "OK";
	case EINVAL: return "EINVAL";
	case ENOENT: return "ENOENT";
	case ENOMEM: return "ENOMEM";
	case ENOSPC: return "ENOSPC";
	case ENODEV: return "ENODEV";
	case EACCES: return "EACCES";
	case EBUSY: return "EBUSY";
	case ETIMEDOUT: return "ETIMEDOUT";
	case EIO: return "EIO";
	case EFAULT: return "EFAULT";
	default: return strerror(err);
	}
}
