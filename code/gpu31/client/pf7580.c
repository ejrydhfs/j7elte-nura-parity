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

/* ------------------------------------------------------------------ */
/* Device handle. */

#define PF7580_MOCK_MAX_BOS 64
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

struct pf7580_dev {
	int fd;		/* real backend DRM fd, -1 for mock */
	int is_mock;
	int fail_next;	/* positive errno for one-shot fault injection */
	/* Mock state. */
	uint32_t next_handle;
	struct pf7580_mock_bo bos[PF7580_MOCK_MAX_BOS];
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

void *pf7580_cpu_map(struct pf7580_dev *dev, uint32_t handle,
		     uint64_t map_offset, size_t size)
{
	void *addr;

	if (!dev || !size)
		return NULL;
	if (dev->is_mock) {
		struct pf7580_mock_bo *bo = mock_find(dev, handle);

		if (!bo || size > bo->size)
			return NULL;
		if (map_offset != bo->map_offset)
			return NULL;
		return bo->shadow;
	}
	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    dev->fd, (off_t)map_offset);
	if (addr == MAP_FAILED)
		return NULL;
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
