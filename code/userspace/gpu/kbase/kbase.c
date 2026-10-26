/*
 * kbase.c - userspace client for the r15p0 kbase UK 10.6 UAPI.
 *
 * Kernel reference points (drivers/gpu/arm/t7xx/r15p0):
 *   mali_kbase_core_linux.c:kbase_ioctl()     - cmd only supplies _IOC_SIZE
 *   mali_kbase_core_linux.c:kbase_dispatch()  - in-buffer u32 function id,
 *       VERSION_CHECK must come first, then SET_FLAGS
 *   mali_kbase_core_linux.c:kbase_api_handshake()
 *   mali_kbase_jd.c:kbase_jd_submit()         - stride == sizeof(atom)
 *   mali_kbase_mem_linux.c:kbase_mmap()       - SAME_VA cookie -> mapping
 *   mali_kbase_mem_linux.c:kbasep_reg_mmap()  - GPU VA = vma->vm_start
 *   mali_kbase_softjobs.c:kbase_dump_cpu_gpu_time()
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include "kbase.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int uk_call(int fd, void *args, size_t size)
{
	if (size > KBASE_UK_CALL_MAX_SIZE)
		return -E2BIG;
	if (ioctl(fd, KBASE_UK_IOCTL(size), args) != 0)
		return -errno;
	return 0;
}

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (!err || !errlen)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

int kbase_open_dev(struct kbase_dev *dev, const char *path,
		   char *err, size_t errlen)
{
	struct uku_version_check_args ver;
	struct kbase_uk_set_flags flags;
	struct kbase_uk_get_ddk_version version;
	struct kbase_uk_context_id ctx;
	int ret;

	memset(dev, 0, sizeof(*dev));
	dev->fd = -1;
	dev->ctx_id = -1;
	dev->mtp = MAP_FAILED;

	dev->fd = open(path, O_RDWR | O_CLOEXEC);
	if (dev->fd < 0) {
		set_err(err, errlen, "open(%s): %s", path, strerror(errno));
		return -errno;
	}

	/* Handshake.  The kernel stores the negotiated version per context and
	 * refuses every other call until this succeeds. */
	memset(&ver, 0, sizeof(ver));
	ver.header.id = UKP_FUNC_ID_CHECK_VERSION;
	ver.major = KBASE_UK_VERSION_MAJOR;
	ver.minor = KBASE_UK_VERSION_MINOR;
	ret = uk_call(dev->fd, &ver, sizeof(ver));
	if (ret) {
		set_err(err, errlen, "VERSION_CHECK ioctl: %s", strerror(-ret));
		goto fail;
	}
	if (ver.header.ret != 0) {
		set_err(err, errlen, "VERSION_CHECK rejected: ret=%u",
			ver.header.ret);
		ret = -EPROTO;
		goto fail;
	}
	dev->uk_major = ver.major;
	dev->uk_minor = ver.minor;
	if (!(ver.major == 10 && ver.minor >= 3) &&
	    !(ver.major > 10)) {
		set_err(err, errlen,
			"kernel speaks UK %u.%u; this client implements 10.6",
			ver.major, ver.minor);
		ret = -EPROTO;
		goto fail;
	}

	/* SET_FLAGS is the mandatory first post-handshake call. */
	memset(&flags, 0, sizeof(flags));
	flags.header.id = KBASE_FUNC_SET_FLAGS;
	flags.create_flags = 0;	/* BASE_CONTEXT_CREATE_FLAG_NONE */
	ret = uk_call(dev->fd, &flags, sizeof(flags));
	if (ret) {
		set_err(err, errlen, "SET_FLAGS ioctl: %s", strerror(-ret));
		goto fail;
	}
	if (flags.header.ret != 0) {
		set_err(err, errlen, "SET_FLAGS rejected: ret=%u",
			flags.header.ret);
		ret = -EPROTO;
		goto fail;
	}

	/* Map the memory tracking page.  Every other mmap on this fd fails with
	 * -EINVAL until this installs kctx->process_mm. */
	dev->mtp = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, dev->fd,
			(off_t)BASE_MEM_MAP_TRACKING_HANDLE);
	if (dev->mtp == MAP_FAILED) {
		set_err(err, errlen, "mmap(tracking page): %s", strerror(errno));
		ret = -errno;
		goto fail;
	}

	memset(&version, 0, sizeof(version));
	version.header.id = KBASE_FUNC_GET_VERSION;
	ret = uk_call(dev->fd, &version, sizeof(version));
	if (ret) {
		set_err(err, errlen, "GET_VERSION ioctl: %s", strerror(-ret));
		goto fail;
	}
	if (version.header.ret != 0) {
		set_err(err, errlen, "GET_VERSION rejected: ret=%u",
			version.header.ret);
		ret = -EPROTO;
		goto fail;
	}
	version.version_buffer[KBASE_GET_VERSION_BUFFER_SIZE - 1] = '\0';
	memcpy(dev->ddk_version, version.version_buffer,
	       KBASE_GET_VERSION_BUFFER_SIZE);
	dev->ddk_version[KBASE_GET_VERSION_BUFFER_SIZE] = '\0';

	memset(&ctx, 0, sizeof(ctx));
	ctx.header.id = KBASE_FUNC_GET_CONTEXT_ID;
	ret = uk_call(dev->fd, &ctx, sizeof(ctx));
	if (ret == 0 && ctx.header.ret == 0)
		dev->ctx_id = ctx.id;

	return 0;

fail:
	kbase_close_dev(dev);
	return ret;
}

void kbase_close_dev(struct kbase_dev *dev)
{
	if (dev->mtp && dev->mtp != MAP_FAILED) {
		munmap(dev->mtp, 4096);
		dev->mtp = NULL;
	}
	if (dev->fd >= 0)
		close(dev->fd);
	dev->fd = -1;
}

int kbase_get_props(struct kbase_dev *dev, void *props,
		    char *err, size_t errlen)
{
	/* 8-byte header followed by struct mali_base_gpu_props (528 bytes). */
	struct {
		union kbase_uk_header header;
		uint8_t props[528];
	} msg;
	int ret;

	memset(&msg, 0, sizeof(msg));
	msg.header.id = KBASE_FUNC_GPU_PROPS_REG_DUMP;
	ret = uk_call(dev->fd, &msg, sizeof(msg));
	if (ret) {
		set_err(err, errlen, "GPU_PROPS ioctl: %s", strerror(-ret));
		return ret;
	}
	if (msg.header.ret != 0) {
		set_err(err, errlen, "GPU_PROPS rejected: ret=%u",
			msg.header.ret);
		return -EPROTO;
	}
	memcpy(props, msg.props, sizeof(msg.props));
	return 0;
}

int kbase_alloc(struct kbase_dev *dev, uint64_t pages, uint64_t flags,
		struct kbase_bo *bo, char *err, size_t errlen)
{
	struct kbase_uk_mem_alloc alloc;
	void *cpu = MAP_FAILED;
	int ret;

	memset(bo, 0, sizeof(*bo));
	memset(&alloc, 0, sizeof(alloc));
	alloc.header.id = KBASE_FUNC_MEM_ALLOC;
	alloc.va_pages = pages;
	alloc.commit_pages = pages;
	alloc.extent = 0;
	alloc.flags = flags;

	ret = uk_call(dev->fd, &alloc, sizeof(alloc));
	if (ret) {
		set_err(err, errlen, "MEM_ALLOC ioctl: %s", strerror(-ret));
		return ret;
	}
	if (alloc.header.ret != 0) {
		set_err(err, errlen, "MEM_ALLOC rejected: ret=%u",
			alloc.header.ret);
		return -EPROTO;
	}

	bo->cookie = alloc.gpu_va;
	bo->va_pages = pages;
	bo->flags = alloc.flags;

	cpu = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
		   dev->fd, (off_t)alloc.gpu_va);
	if (cpu == MAP_FAILED) {
		set_err(err, errlen, "mmap(cookie 0x%llx): %s",
			(unsigned long long)alloc.gpu_va, strerror(errno));
		ret = -errno;
		/* Do not MEM_FREE: the region is owned by the cookie and is
		 * reclaimed when the context closes. */
		memset(bo, 0, sizeof(*bo));
		return ret;
	}

	bo->cpu = cpu;
	/* For SAME_VA the mapping address *is* the GPU address
	 * (kbasep_reg_mmap passes vma->vm_start to kbase_gpu_mmap). */
	bo->gpu_va = (uint64_t)(uintptr_t)cpu;
	return 0;
}

int kbase_free(struct kbase_dev *dev, struct kbase_bo *bo)
{
	struct kbase_uk_mem_free mem;
	int ret = 0;

	if (!bo->cpu)
		return 0;

	if (bo->flags & BASE_MEM_SAME_VA) {
		if (munmap(bo->cpu, bo->va_pages * 4096) != 0)
			return -errno;
	} else {
		memset(&mem, 0, sizeof(mem));
		mem.header.id = KBASE_FUNC_MEM_FREE;
		mem.gpu_addr = bo->cookie;
		ret = uk_call(dev->fd, &mem, sizeof(mem));
		if (ret == 0 && mem.header.ret != 0)
			ret = -EPROTO;
	}
	memset(bo, 0, sizeof(*bo));
	return ret;
}

int kbase_submit_atoms(struct kbase_dev *dev, struct base_jd_atom_v2 *atoms,
		       uint32_t nr_atoms)
{
	struct kbase_uk_job_submit submit;
	int ret;

	memset(&submit, 0, sizeof(submit));
	submit.header.id = KBASE_FUNC_JOB_SUBMIT;
	submit.addr.value = atoms;
	submit.nr_atoms = nr_atoms;
	submit.stride = sizeof(struct base_jd_atom_v2);

	ret = uk_call(dev->fd, &submit, sizeof(submit));
	if (ret)
		return ret;
	if (submit.header.ret != 0)
		return -EPROTO;
	return 0;
}

int kbase_read_event(struct kbase_dev *dev, struct base_jd_event_v2 *ev)
{
	ssize_t n;

	memset(ev, 0, sizeof(*ev));
	n = read(dev->fd, ev, sizeof(*ev));
	if (n < 0)
		return -errno;
	if (n != (ssize_t)sizeof(*ev))
		return -EIO;
	if (ev->event_code == BASE_JD_EVENT_DRV_TERMINATED)
		return -EPIPE;
	return 0;
}

int kbase_soft_dump_time(struct kbase_dev *dev, struct kbase_bo *bo,
			 struct base_dump_cpu_gpu_counters *dump)
{
	struct base_jd_atom_v2 atom;
	struct base_jd_event_v2 ev;
	int ret;

	if (!bo->cpu)
		return -EINVAL;

	memset(&atom, 0, sizeof(atom));
	atom.jc = bo->gpu_va;
	atom.udata.blob[0] = 0x6b626173655f7465ull;	/* tag */
	atom.udata.blob[1] = bo->gpu_va;
	atom.nr_extres = 0;
	atom.compat_core_req = (uint16_t)(BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME & 0x7fff);
	atom.pre_dep[0].atom_id = 0;
	atom.pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_INVALID;
	atom.pre_dep[1].atom_id = 0;
	atom.pre_dep[1].dependency_type = BASE_JD_DEP_TYPE_INVALID;
	atom.atom_number = 1;
	atom.prio = BASE_JD_PRIO_MEDIUM;
	atom.device_nr = 0;
	atom.core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME;

	ret = kbase_submit_atoms(dev, &atom, 1);
	if (ret)
		return ret;

	ret = kbase_read_event(dev, &ev);
	if (ret)
		return ret;
	if (ev.atom_number != atom.atom_number)
		return -EIO;
	if (ev.event_code != BASE_JD_EVENT_DONE)
		return (int)ev.event_code;

	memcpy(dump, bo->cpu, sizeof(*dump));
	return (int)ev.event_code;
}

const char *kbase_event_name(unsigned code)
{
	switch (code) {
	case BASE_JD_EVENT_DONE: return "DONE";
	case BASE_JD_EVENT_TERMINATED: return "TERMINATED";
	case BASE_JD_EVENT_JOB_CONFIG_FAULT: return "JOB_CONFIG_FAULT";
	case BASE_JD_EVENT_JOB_POWER_FAULT: return "JOB_POWER_FAULT";
	case BASE_JD_EVENT_JOB_READ_FAULT: return "JOB_READ_FAULT";
	case BASE_JD_EVENT_JOB_WRITE_FAULT: return "JOB_WRITE_FAULT";
	case BASE_JD_EVENT_JOB_BUS_FAULT: return "JOB_BUS_FAULT";
	case BASE_JD_EVENT_INSTR_INVALID_PC: return "INSTR_INVALID_PC";
	case BASE_JD_EVENT_INSTR_INVALID_ENC: return "INSTR_INVALID_ENC";
	case BASE_JD_EVENT_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
	case BASE_JD_EVENT_JOB_CANCELLED: return "JOB_CANCELLED";
	case BASE_JD_EVENT_BAG_DONE: return "BAG_DONE";
	case BASE_JD_EVENT_DRV_TERMINATED: return "DRV_TERMINATED";
	case BASE_JD_EVENT_REMOVED_FROM_NEXT: return "REMOVED_FROM_NEXT";
	default: return "OTHER";
	}
}
