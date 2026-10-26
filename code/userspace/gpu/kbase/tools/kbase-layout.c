/*
 * kbase-layout.c - dump the wire layout of the kernel's own kbase UK headers.
 *
 * Build it against a DDK revision's directory, e.g.
 *   gcc -I$K/drivers/gpu/arm/t7xx/r15p0 -DKBASE_LAYOUT_R15P0 kbase-layout.c
 *   gcc -I$K/drivers/gpu/arm/t72x/r7p0  kbase-layout.c
 * and compare the output with `kbase-selftest --print-sizes`.  This is the
 * evidence that kbase_uapi.h matches what the kernel actually compiles,
 * rather than a transcription that merely looks plausible.
 *
 * The two stub defines exist because mali_base_kernel.h expects a kernel
 * build environment (assert macro, PAGE_MASK, __user).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

#define __user
#define CDBG_ASSERT(x) ((void)0)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))

#include "mali_uk.h"
#include "mali_base_kernel.h"
#include "mali_kbase_gpuprops_types.h"
#include "mali_kbase_platform_uku.h"
#include "mali_kbase_uku.h"

#define P(t) printf("%-28s %zu\n", #t, sizeof(t))
#define O(t, f) printf("offset %-21s %zu\n", #t "." #f, offsetof(t, f))

int main(void)
{
	P(struct uku_version_check_args);
	P(struct kbase_uk_set_flags);
	P(struct kbase_uk_get_ddk_version);
	P(struct kbase_uk_context_id);
	P(struct kbase_uk_mem_alloc);
	P(struct kbase_uk_mem_free);
	P(struct kbase_uk_mem_query);
	P(struct kbase_uk_mem_commit);
	P(struct kbase_uk_mem_flags_change);
	P(struct kbase_uk_job_submit);
	P(base_jd_atom_v2);
	P(base_jd_event_v2);
	P(struct base_dump_cpu_gpu_counters);

	O(struct kbase_uk_job_submit, stride);
	O(struct base_jd_atom_v2, core_req);
#ifdef KBASE_LAYOUT_R15P0
	P(struct kbase_uk_gpuprops);
	O(struct base_jd_atom_v2, gles_ctx_handle);
	O(struct base_jd_atom_v2, surfacep);
	O(struct kbase_uk_job_submit, gles_ctx_handle);
	O(struct kbase_uk_job_submit, surfacep);
#endif
	printf("BASE_UK_VERSION                 %u.%u\n",
	       (unsigned)BASE_UK_VERSION_MAJOR, (unsigned)BASE_UK_VERSION_MINOR);
	printf("sizeof(base_jd_core_req)        %zu\n", sizeof(base_jd_core_req));
	printf("KBASE_FUNC_MAX                  %u\n", (unsigned)KBASE_FUNC_MAX);
	return 0;
}
