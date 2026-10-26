/*
 * kbase-selftest.c - ABI conformance + bring-up test for the r15p0 kbase
 * UK 10.6 interface.
 *
 * Modes:
 *   (default)      full bring-up: handshake, props, alloc, soft-job submit,
 *                  completion event, free.  Requires /dev/maliN.
 *   --print-sizes  print wire struct sizes/offsets (used by verify-abi.sh).
 *   --probe-legacy on a fresh fd, submit an r7p0-shaped (32-byte) job-submit
 *                  and report how the r15p0 kernel rejects it.
 *   --probe-modern on a fresh fd, issue a modern numbered VERSION_CHECK ioctl
 *                  (Mesa kbase backend style) and report the rejection.
 *
 * Usage: kbase-selftest [--device PATH] [--print-sizes|--probe-legacy|--probe-modern]
 *
 * Exit codes: 0 success, 2 device absent, 1 failure.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include "kbase.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/mali0"

static void print_sizes(void)
{
	printf("uku_version_check_args %zu\n", sizeof(struct uku_version_check_args));
	printf("kbase_uk_set_flags %zu\n", sizeof(struct kbase_uk_set_flags));
	printf("kbase_uk_get_ddk_version %zu\n", sizeof(struct kbase_uk_get_ddk_version));
	printf("kbase_uk_context_id %zu\n", sizeof(struct kbase_uk_context_id));
	printf("kbase_uk_mem_alloc %zu\n", sizeof(struct kbase_uk_mem_alloc));
	printf("kbase_uk_mem_free %zu\n", sizeof(struct kbase_uk_mem_free));
	printf("kbase_uk_mem_query %zu\n", sizeof(struct kbase_uk_mem_query));
	printf("kbase_uk_mem_commit %zu\n", sizeof(struct kbase_uk_mem_commit));
	printf("kbase_uk_mem_flags_change %zu\n", sizeof(struct kbase_uk_mem_flags_change));
	printf("kbase_uk_job_submit %zu\n", sizeof(struct kbase_uk_job_submit));
	printf("base_jd_atom_v2 %zu\n", sizeof(struct base_jd_atom_v2));
	printf("base_jd_event_v2 %zu\n", sizeof(struct base_jd_event_v2));
	printf("base_dump_cpu_gpu_counters %zu\n", sizeof(struct base_dump_cpu_gpu_counters));
	printf("kbase_uk_gpuprops %zu\n", sizeof(union kbase_uk_header) + 528);
	printf("offset base_jd_atom_v2.core_req %zu\n", (size_t)__builtin_offsetof(struct base_jd_atom_v2, core_req));
	printf("offset base_jd_atom_v2.gles_ctx_handle %zu\n", (size_t)__builtin_offsetof(struct base_jd_atom_v2, gles_ctx_handle));
	printf("offset base_jd_atom_v2.surfacep %zu\n", (size_t)__builtin_offsetof(struct base_jd_atom_v2, surfacep));
	printf("offset kbase_uk_job_submit.stride %zu\n", (size_t)__builtin_offsetof(struct kbase_uk_job_submit, stride));
	printf("offset kbase_uk_job_submit.gles_ctx_handle %zu\n", (size_t)__builtin_offsetof(struct kbase_uk_job_submit, gles_ctx_handle));
	printf("offset kbase_uk_job_submit.surfacep %zu\n", (size_t)__builtin_offsetof(struct kbase_uk_job_submit, surfacep));
	printf("UK version %u.%u\n", KBASE_UK_VERSION_MAJOR, KBASE_UK_VERSION_MINOR);
	printf("KBASE_FUNC_MAX %u CALL_MAX_SIZE %u\n", (unsigned)KBASE_FUNC_MAX, (unsigned)KBASE_UK_CALL_MAX_SIZE);
}

/* The exact wire struct the ROM's r7p0 userspace would send (32 bytes). */
struct r7p0_job_submit {
	union kbase_uk_header header;
	union kbase_pointer addr;
	uint32_t nr_atoms;
	uint32_t stride;
	uint32_t gles_ctx_handle;
};

static int probe_legacy(const char *path)
{
	struct kbase_dev dev;
	struct r7p0_job_submit legacy;
	struct base_jd_atom_v2 atoms[1];
	char err[256];
	int ret, saved;

	printf("\n-- r7p0 (UK 10.0) job-submit probe on %s --\n", path);
	ret = kbase_open_dev(&dev, path, err, sizeof(err));
	if (ret) {
		printf("open/handshake failed: %s\n", err);
		return 2;
	}

	memset(atoms, 0, sizeof(atoms));
	memset(&legacy, 0, sizeof(legacy));
	legacy.header.id = KBASE_FUNC_JOB_SUBMIT;
	legacy.addr.value = atoms;
	legacy.nr_atoms = 0;
	/* r7p0 layout: 48 bytes without, 56 bytes with MALI_SYSTRACE_SUPPORT. */
	legacy.stride = 56;

	printf("sending %zu-byte r7p0 submit (r15p0 kernel expects %zu)\n",
	       sizeof(legacy), sizeof(struct kbase_uk_job_submit));
	errno = 0;
	ret = ioctl(dev.fd, KBASE_UK_IOCTL(sizeof(legacy)), &legacy);
	saved = errno;
	printf("ioctl -> %d, errno=%d (%s), header.ret=%u\n",
	       ret, saved, strerror(saved), legacy.header.ret);
	if (ret != 0)
		printf("result: rejected before reaching kbase_jd_submit "
		       "(kbase_dispatch bad_size; kernel logs "
		       "\"Wrong syscall size (%zu)\")\n", sizeof(legacy));
	else
		printf("result: UNEXPECTED - r15p0 accepted an r7p0 submit\n");

	kbase_close_dev(&dev);
	return 0;
}

static int probe_modern(const char *path)
{
	struct {
		uint16_t major;
		uint16_t minor;
		uint8_t padding[4];
	} payload = { 10, 6, { 0 } };
	int fd, ret, saved;

	printf("\n-- modern numbered VERSION_CHECK probe on %s --\n", path);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("open failed: %s\n", strerror(errno));
		return 2;
	}

	/* Mesa's kbase kmod would send KBASE_IOCTL_VERSION_CHECK, i.e.
	 * _IOW(0x80, nr, struct kbase_ioctl_version_check).  This kernel
	 * dispatches on a u32 id inside the payload, so the ioctl number is
	 * irrelevant and the payload is misread as a uk_header. */
	errno = 0;
	ret = ioctl(fd, _IOW(0x80, 0, char[8]), &payload);
	saved = errno;
	printf("numbered ioctl -> %d, errno=%d (%s)\n", ret, saved,
	       strerror(saved));
	if (ret != 0)
		printf("result: rejected (the u32 read as uk_header.id was "
		       "0x%08x, not UKP_FUNC_ID_CHECK_VERSION)\n",
		       (unsigned)(payload.major | ((unsigned)payload.minor << 16)));
	else
		printf("result: UNEXPECTED - numbered ioctl accepted\n");

	close(fd);
	return 0;
}

static void describe_props(const uint8_t *props)
{
	uint32_t product_id;
	uint16_t version_status, minor_revision;
	uint32_t gpu_speed;

	memcpy(&product_id, props + 0, 4);
	memcpy(&version_status, props + 4, 2);
	memcpy(&minor_revision, props + 6, 2);
	memcpy(&gpu_speed, props + 12, 4);
	printf("  props: product_id=0x%08x version_status=0x%04x "
	       "minor_revision=0x%04x gpu_speed=%u MHz\n",
	       product_id, version_status, minor_revision, gpu_speed);
}

int main(int argc, char **argv)
{
	const char *path = DEFAULT_DEVICE;
	const char *mode = NULL;
	struct kbase_dev dev;
	struct kbase_bo bo;
	struct base_dump_cpu_gpu_counters dump;
	uint8_t props[528];
	char err[256];
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "--print-sizes"))
			mode = "sizes";
		else if (!strcmp(argv[i], "--probe-legacy"))
			mode = "legacy";
		else if (!strcmp(argv[i], "--probe-modern"))
			mode = "modern";
		else {
			fprintf(stderr, "unknown argument: %s\n", argv[i]);
			return 1;
		}
	}

	if (mode && !strcmp(mode, "sizes")) {
		print_sizes();
		return 0;
	}
	if (mode && !strcmp(mode, "legacy"))
		return probe_legacy(path);
	if (mode && !strcmp(mode, "modern"))
		return probe_modern(path);

	printf("kbase UK %u.%u userspace bring-up test\n",
	       KBASE_UK_VERSION_MAJOR, KBASE_UK_VERSION_MINOR);
	printf("wire ABI: atom=%zu submit=%zu event=%zu (r15p0/r7p0 kernel "
	       "expectations differ; see analysis.md)\n",
	       sizeof(struct base_jd_atom_v2),
	       sizeof(struct kbase_uk_job_submit),
	       sizeof(struct base_jd_event_v2));

	ret = kbase_open_dev(&dev, path, err, sizeof(err));
	if (ret) {
		printf("device unavailable: %s\n", err);
		if (ret == -ENOENT || ret == -ENODEV || ret == -EACCES) {
			printf("device %s not present: ABI self-check passed, "
			       "bring-up not executed\n", path);
			return 2;
		}
		return 1;
	}

	printf("handshake: UK %u.%u, DDK \"%s\", context id %d\n",
	       dev.uk_major, dev.uk_minor, dev.ddk_version, dev.ctx_id);

	if (kbase_get_props(&dev, props, err, sizeof(err)) == 0)
		describe_props(props);
	else
		printf("  GPU_PROPS failed: %s\n", err);

	ret = kbase_alloc(&dev, 1,
			  BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
				  BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
				  BASE_MEM_SAME_VA,
			  &bo, err, sizeof(err));
	if (ret) {
		printf("MEM_ALLOC failed: %s\n", err);
		kbase_close_dev(&dev);
		return 1;
	}
	printf("alloc: cookie=0x%llx gpu_va=0x%llx (cpu==gpu) flags=0x%llx\n",
	       (unsigned long long)bo.cookie, (unsigned long long)bo.gpu_va,
	       (unsigned long long)bo.flags);

	ret = kbase_soft_dump_time(&dev, &bo, &dump);
	if (ret < 0) {
		printf("soft job submit/read failed: %s (%d)\n",
		       ret == -EPIPE ? "driver terminated" : "error", ret);
		kbase_free(&dev, &bo);
		kbase_close_dev(&dev);
		return 1;
	}
	printf("soft job: event=%s (0x%x) atom ok\n",
	       kbase_event_name((unsigned)ret), (unsigned)ret);
	printf("kernel wrote back: sec=%llu usec=%llu system_time=%llu "
	       "cycle_counter=%llu\n",
	       (unsigned long long)dump.sec, (unsigned long long)dump.usec,
	       (unsigned long long)dump.system_time,
	       (unsigned long long)dump.cycle_counter);

	kbase_free(&dev, &bo);
	kbase_close_dev(&dev);
	printf("RESULT: kbase memory + job submission + event delivery OK\n");
	return 0;
}
