/* Host-test stub for <mali_base_kernel.h> (r15p0 field subset).
 *
 * ONLY for code/gpu31/shim/stride_test.  It carries exactly the fields the
 * r7p0 compat header reads, at the kernel's own offsets (measured from the
 * r15p0 headers with tools/kbase-layout.c):
 *
 *   base_jd_atom_v2: jc@0, udata@8, extres_list@24, nr_extres@32,
 *     compat_core_req@34, pre_dep@36, atom_number@40, prio@41, device_nr@42,
 *     core_req@44, gles_ctx_handle@48, frame_number@52, surfacep@56 (64 B).
 *   base_jd_atom_v2_r7p0 fields live in the compat header itself (56 B,
 *     core_req@34, pre_dep@36, gles@48); the 48-byte variant is the same
 *     layout minus the systrace gles word.
 *
 * The soft-job values match both revisions for types 1..4
 * (r7p0 mali_base_kernel.h:417-419,457 vs r15p0 :525-527,565); type 5 is
 * SOFT_DVFS in r7p0 but SOFT_EVENT_WAIT in r15p0, which is why the header
 * refuses it.
 */
#ifndef SHIM_MALI_BASE_KERNEL_H
#define SHIM_MALI_BASE_KERNEL_H

#include <linux/types.h>

#define BASE_JD_ATOM_COUNT 256

#define BASE_JD_REQ_SOFT_JOB (1u << 9)
#define BASE_JD_REQ_SOFT_JOB_TYPE (BASE_JD_REQ_SOFT_JOB | 0x1f)
#define BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME (BASE_JD_REQ_SOFT_JOB | 0x1)
#define BASE_JD_REQ_SOFT_FENCE_TRIGGER (BASE_JD_REQ_SOFT_JOB | 0x2)
#define BASE_JD_REQ_SOFT_FENCE_WAIT (BASE_JD_REQ_SOFT_JOB | 0x3)
#define BASE_JD_REQ_SOFT_REPLAY (BASE_JD_REQ_SOFT_JOB | 0x4)

/* Deliberately no SOFT_DVFS / SOFT_EVENT_WAIT: the header under test must
 * not depend on either spelling.  It only references types 1..4 plus the
 * BASE_JD_REQ_SOFT_JOB / _TYPE masks. */


typedef u8 base_atom_id;
typedef u8 base_jd_prio;

#endif /* SHIM_MALI_BASE_KERNEL_H */
