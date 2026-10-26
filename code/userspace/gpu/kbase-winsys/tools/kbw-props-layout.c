/*
 * kbw-props-layout.c - dump the layout of struct mali_base_gpu_props as the
 * r15p0 kernel compiles it.
 *
 *   cc -I$K/drivers/gpu/arm/t7xx/r15p0 tools/kbw-props-layout.c -o props-layout
 *   ./props-layout
 *
 * The output is what include/kbase_winsys_gpuprops.h hard-codes in its
 * _Static_asserts; re-run this after any kernel header change.  The stub
 * defines are the same ones kbase/tools/kbase-layout.c needs because
 * mali_base_kernel.h expects a kernel build environment.
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

#define P(t) printf("size %-38s %zu\n", #t, sizeof(t))
#define O(t, f) printf("offset %-36s %zu\n", #t "." #f, offsetof(t, f))

int main(void)
{
	P(struct mali_base_gpu_props);
	P(struct mali_base_gpu_core_props);
	P(struct mali_base_gpu_l2_cache_props);
	P(struct mali_base_gpu_tiler_props);
	P(struct mali_base_gpu_thread_props);
	P(struct gpu_raw_gpu_props);
	P(struct mali_base_gpu_coherent_group_info);
	P(struct mali_base_gpu_coherent_group);

	O(struct mali_base_gpu_props, core_props);
	O(struct mali_base_gpu_props, l2_props);
	O(struct mali_base_gpu_props, tiler_props);
	O(struct mali_base_gpu_props, thread_props);
	O(struct mali_base_gpu_props, raw_props);
	O(struct mali_base_gpu_props, coherency_info);

	O(struct mali_base_gpu_core_props, product_id);
	O(struct mali_base_gpu_core_props, version_status);
	O(struct mali_base_gpu_core_props, minor_revision);
	O(struct mali_base_gpu_core_props, major_revision);
	O(struct mali_base_gpu_core_props, gpu_speed_mhz);
	O(struct mali_base_gpu_core_props, gpu_freq_khz_max);
	O(struct mali_base_gpu_core_props, gpu_freq_khz_min);
	O(struct mali_base_gpu_core_props, log2_program_counter_size);
	O(struct mali_base_gpu_core_props, texture_features);
	O(struct mali_base_gpu_core_props, gpu_available_memory_size);

	O(struct gpu_raw_gpu_props, shader_present);
	O(struct gpu_raw_gpu_props, tiler_present);
	O(struct gpu_raw_gpu_props, l2_present);
	O(struct gpu_raw_gpu_props, stack_present);
	O(struct gpu_raw_gpu_props, l2_features);
	O(struct gpu_raw_gpu_props, suspend_size);
	O(struct gpu_raw_gpu_props, mem_features);
	O(struct gpu_raw_gpu_props, mmu_features);
	O(struct gpu_raw_gpu_props, as_present);
	O(struct gpu_raw_gpu_props, js_present);
	O(struct gpu_raw_gpu_props, js_features);
	O(struct gpu_raw_gpu_props, tiler_features);
	O(struct gpu_raw_gpu_props, texture_features);
	O(struct gpu_raw_gpu_props, gpu_id);
	O(struct gpu_raw_gpu_props, thread_max_threads);
	O(struct gpu_raw_gpu_props, thread_max_workgroup_size);
	O(struct gpu_raw_gpu_props, thread_max_barrier_size);
	O(struct gpu_raw_gpu_props, thread_features);
	O(struct gpu_raw_gpu_props, coherency_mode);

	O(struct mali_base_gpu_coherent_group_info, num_groups);
	O(struct mali_base_gpu_coherent_group_info, num_core_groups);
	O(struct mali_base_gpu_coherent_group_info, coherency);
	O(struct mali_base_gpu_coherent_group_info, group);

	O(struct mali_base_gpu_thread_props, max_threads);
	O(struct mali_base_gpu_thread_props, max_workgroup_size);
	O(struct mali_base_gpu_thread_props, max_barrier_size);
	O(struct mali_base_gpu_thread_props, max_registers);
	O(struct mali_base_gpu_thread_props, max_task_queue);
	O(struct mali_base_gpu_thread_props, max_thread_group_split);
	O(struct mali_base_gpu_thread_props, impl_tech);

	O(struct mali_base_gpu_l2_cache_props, log2_line_size);
	O(struct mali_base_gpu_l2_cache_props, log2_cache_size);
	O(struct mali_base_gpu_l2_cache_props, num_l2_slices);

	O(struct mali_base_gpu_tiler_props, bin_size_bytes);
	O(struct mali_base_gpu_tiler_props, max_active_levels);

	printf("GPU_MAX_JOB_SLOTS %d\n", GPU_MAX_JOB_SLOTS);
	printf("BASE_MAX_COHERENT_GROUPS %d\n", BASE_MAX_COHERENT_GROUPS);
	printf("BASE_GPU_NUM_TEXTURE_FEATURES_REGISTERS %d\n",
	       BASE_GPU_NUM_TEXTURE_FEATURES_REGISTERS);
	return 0;
}
