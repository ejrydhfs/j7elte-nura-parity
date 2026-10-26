/*
 * kbase.h - minimal userspace client for the r15p0 kbase UK 10.6 UAPI.
 *
 * This is not a GL driver.  It is the userspace half of the kernel interface
 * (open/handshake/memory/job/event) that any GL or compute driver on this
 * kernel would have to be built on top of.  It compiles for aarch64 and for
 * 32-bit ARM (compat) and needs no libc beyond open/mmap/ioctl/read.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef KBASE_H
#define KBASE_H

#include "kbase_uapi.h"
#include <stddef.h>

struct kbase_dev {
	int fd;
	unsigned uk_major;
	unsigned uk_minor;
	char ddk_version[KBASE_GET_VERSION_BUFFER_SIZE + 1];
	int ctx_id;
	void *mtp;	/* memory tracking page mapping (must outlive all BOs) */
};

struct kbase_bo {
	void *cpu;		/* CPU mapping; == GPU VA for SAME_VA */
	uint64_t gpu_va;	/* address to put in atoms */
	uint64_t cookie;	/* raw MEM_ALLOC return value */
	uint64_t va_pages;
	uint64_t flags;
};

/* Open /dev/maliN, perform VERSION_CHECK -> SET_FLAGS -> map the memory
 * tracking page -> GET_VERSION -> GET_CONTEXT_ID.  The tracking page is
 * mandatory: kbase_mmap() rejects every other mapping while
 * kctx->process_mm is NULL (mali_kbase_mem_linux.c:2160-2168), and only the
 * PFN_DOWN(BASE_MEM_MAP_TRACKING_HANDLE) mapping installs it
 * (kbase_tracking_page_setup(), :2512-2536).  Returns 0 or -errno; on
 * failure 'err' gets a reason. */
int kbase_open_dev(struct kbase_dev *dev, const char *path,
		   char *err, size_t errlen);
void kbase_close_dev(struct kbase_dev *dev);

/* GET_GPU_PROPS_REG_DUMP.  'props' must point at >= 528 bytes. */
int kbase_get_props(struct kbase_dev *dev, void *props,
		    char *err, size_t errlen);

/* MEM_ALLOC + mmap.  On 64-bit clients the kernel forces BASE_MEM_SAME_VA;
 * the returned cookie is mmap'd and the resulting CPU address is the GPU VA
 * (kbasep_reg_mmap uses vma->vm_start as the GPU address). */
int kbase_alloc(struct kbase_dev *dev, uint64_t pages, uint64_t flags,
		struct kbase_bo *bo, char *err, size_t errlen);

/* munmap for SAME_VA regions (the kernel frees them on unmap), MEM_FREE
 * otherwise. */
int kbase_free(struct kbase_dev *dev, struct kbase_bo *bo);

/* KBASE_FUNC_JOB_SUBMIT with stride = sizeof(base_jd_atom_v2). */
int kbase_submit_atoms(struct kbase_dev *dev, struct base_jd_atom_v2 *atoms,
		       uint32_t nr_atoms);
int kbase_read_event(struct kbase_dev *dev, struct base_jd_event_v2 *ev);

/* Submit one BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME atom writing its result to
 * 'bo' (which must be GPU-mapped and writable) and wait for completion.
 * Fills 'dump' with what the kernel wrote.  Returns the event code or
 * -errno. */
int kbase_soft_dump_time(struct kbase_dev *dev, struct kbase_bo *bo,
			 struct base_dump_cpu_gpu_counters *dump);

const char *kbase_event_name(unsigned code);

#endif /* KBASE_H */
