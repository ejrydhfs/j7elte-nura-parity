/*
 * kbase_uapi.h - ARM kbase "UK" userspace ABI, DDK r15p0 / UK 10.6.
 *
 * Wire layout transcribed from
 *   drivers/gpu/arm/t7xx/r15p0/mali_uk.h
 *   drivers/gpu/arm/t7xx/r15p0/mali_kbase_uku.h
 *   drivers/gpu/arm/t7xx/r15p0/mali_base_kernel.h
 *   drivers/gpu/arm/t7xx/r15p0/mali_base_mem_priv.h
 *
 * This is the ABI the Nura kernel actually builds (CONFIG_MALI_T7XX=y,
 * CONFIG_MALI_R15P0=y).  All sizes/offsets below were verified against the
 * kernel headers by tools/kbase-layout.c (see verify-abi.sh); the static
 * asserts fail the build if anything drifts.
 *
 * The kernel dispatches on a u32 function id *inside* the argument buffer:
 * the ioctl cmd is only used for _IOC_SIZE(cmd), its type/number are ignored
 * (mali_kbase_core_linux.c:kbase_ioctl).  Use KBASE_UK_IOCTL(sizeof(args)).
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef KBASE_UAPI_H
#define KBASE_UAPI_H

#include <stdint.h>
#include <sys/ioctl.h>

#define KBASE_UK_VERSION_MAJOR 10
#define KBASE_UK_VERSION_MINOR 6

/* mali_uk.h: internal UK function, not a client function */
#define UKP_FUNC_ID_CHECK_VERSION 0
#define UK_FUNC_ID 512

/* mali_kbase_core_linux.c: the kernel only reads _IOC_SIZE(cmd). */
#define KBASE_UK_IOCTL(size) _IOC(_IOC_WRITE, 0, 0, (size))
/* largest accepted argument buffer: CALL_MAX_SIZE = KBASE_FUNC_MAX - 1 */
#define KBASE_UK_CALL_MAX_SIZE 568

union kbase_uk_header {
	uint32_t id;	/* in: function id */
	uint32_t ret;	/* out: mal_error */
	uint64_t sizer;	/* force 8-byte alignment */
};

struct uku_version_check_args {
	union kbase_uk_header header;
	uint16_t major;	/* in: user major, out: kernel major */
	uint16_t minor;	/* in: user minor, out: kernel minor */
	uint8_t padding[4];
};

/* 32/64-bit neutral pointer: 8 bytes in every ABI.  Native 64-bit clients
 * use .value; 32-bit clients (CONFIG_COMPAT) use .compat_value. */
union kbase_pointer {
	void *value;
	uint32_t compat_value;
	uint64_t sizer;
};

struct kbase_uk_set_flags {
	union kbase_uk_header header;
	uint32_t create_flags;
	uint32_t padding;
};

#define KBASE_GET_VERSION_BUFFER_SIZE 64
struct kbase_uk_get_ddk_version {
	union kbase_uk_header header;
	char version_buffer[KBASE_GET_VERSION_BUFFER_SIZE];
	uint32_t version_string_size;
	uint32_t padding;
};

struct kbase_uk_context_id {
	union kbase_uk_header header;
	int id;
};

struct kbase_uk_mem_alloc {
	union kbase_uk_header header;
	uint64_t va_pages;	/* in */
	uint64_t commit_pages;	/* in */
	uint64_t extent;	/* in */
	uint64_t flags;		/* in/out */
	uint64_t gpu_va;	/* out: real GPU VA, or a SAME_VA cookie */
	uint16_t va_alignment;
	uint8_t padding[6];
};

struct kbase_uk_mem_free {
	union kbase_uk_header header;
	uint64_t gpu_addr;
};

struct kbase_uk_mem_query {
	union kbase_uk_header header;
	uint64_t gpu_addr;
	uint64_t query;
	uint64_t value;		/* out */
};

struct kbase_uk_mem_commit {
	union kbase_uk_header header;
	uint64_t gpu_addr;
	uint64_t pages;
	uint32_t result_subcode; /* out */
	uint32_t padding;
};

struct kbase_uk_mem_flags_change {
	union kbase_uk_header header;
	uint64_t gpu_va;
	uint64_t flags;
	uint64_t mask;
};

struct kbase_uk_job_submit {
	union kbase_uk_header header;
	union kbase_pointer addr;	/* array of base_jd_atom_v2 */
	uint32_t nr_atoms;
	uint32_t stride;		/* must equal sizeof(base_jd_atom_v2) */
	uint32_t gles_ctx_handle;	/* Samsung systrace extension */
	uint32_t frame_number;		/* Samsung systrace extension */
	void *surfacep;			/* Samsung systrace extension */
};

struct base_jd_udata {
	uint64_t blob[2];
};

struct base_dependency {
	uint8_t atom_id;
	uint8_t dependency_type;
};

#define BASE_JD_DEP_TYPE_INVALID 0
#define BASE_JD_DEP_TYPE_DATA 1
#define BASE_JD_DEP_TYPE_ORDER 2

/* base_jd_atom_v2 as compiled by the r15p0 kernel: 64 bytes. */
struct base_jd_atom_v2 {
	uint64_t jc;				/* 0  job-chain GPU address */
	struct base_jd_udata udata;		/* 8  opaque user data */
	union kbase_pointer extres_list;	/* 24 */
	uint16_t nr_extres;			/* 32 */
	uint16_t compat_core_req;		/* 34 UK<=10.2 only */
	struct base_dependency pre_dep[2];	/* 36 */
	uint8_t atom_number;			/* 40 0..255 */
	uint8_t prio;				/* 41 */
	uint8_t device_nr;			/* 42 */
	uint8_t padding[1];			/* 43 */
	uint32_t core_req;			/* 44 (UK >= 10.3) */
	uint32_t gles_ctx_handle;		/* 48 Samsung */
	uint32_t frame_number;			/* 52 Samsung */
	void *surfacep;				/* 56 Samsung */
};

/* Completion event read from /dev/maliN: 24 bytes. */
typedef enum base_jd_event_code {
	BASE_JD_EVENT_DONE = 0x01,
	BASE_JD_EVENT_TERMINATED = 0x04,
	BASE_JD_EVENT_JOB_CONFIG_FAULT = 0x40,
	BASE_JD_EVENT_JOB_POWER_FAULT = 0x41,
	BASE_JD_EVENT_JOB_READ_FAULT = 0x42,
	BASE_JD_EVENT_JOB_WRITE_FAULT = 0x43,
	BASE_JD_EVENT_JOB_AFFINITY_FAULT = 0x44,
	BASE_JD_EVENT_JOB_BUS_FAULT = 0x48,
	BASE_JD_EVENT_INSTR_INVALID_PC = 0x50,
	BASE_JD_EVENT_INSTR_INVALID_ENC = 0x51,
	BASE_JD_EVENT_INSTR_TYPE_MISMATCH = 0x52,
	BASE_JD_EVENT_INSTR_OPERAND_FAULT = 0x53,
	BASE_JD_EVENT_INSTR_TLS_FAULT = 0x54,
	BASE_JD_EVENT_INSTR_BARRIER_FAULT = 0x55,
	BASE_JD_EVENT_INSTR_ALIGN_FAULT = 0x56,
	BASE_JD_EVENT_TILE_RANGE_FAULT = 0x59,
	BASE_JD_EVENT_STATE_FAULT = 0x5a,
	BASE_JD_EVENT_OUT_OF_MEMORY = 0x60,
	BASE_JD_EVENT_UNKNOWN = 0x7f,
	BASE_JD_EVENT_ACTIVE = 0x08,
	BASE_JD_EVENT_JOB_CANCELLED = 0x4002,
	BASE_JD_EVENT_PM_EVENT = 0x4004,
	BASE_JD_EVENT_BAG_DONE = 0x6800,
	BASE_JD_EVENT_DRV_TERMINATED = 0x7000,
	BASE_JD_EVENT_REMOVED_FROM_NEXT = 0xc000
} base_jd_event_code;

struct base_jd_event_v2 {
	base_jd_event_code event_code;	/* 0 */
	uint8_t atom_number;		/* 4 */
	struct base_jd_udata udata;	/* 8 */
};

/* Memory allocation flags (mali_base_kernel.h enum base_mem_alloc_flags). */
#define BASE_MEM_PROT_CPU_RD (1u << 0)
#define BASE_MEM_PROT_CPU_WR (1u << 1)
#define BASE_MEM_PROT_GPU_RD (1u << 2)
#define BASE_MEM_PROT_GPU_WR (1u << 3)
#define BASE_MEM_COHERENT_SYSTEM (1u << 10)
#define BASE_MEM_COHERENT_LOCAL (1u << 11)
#define BASE_MEM_CACHED_CPU (1u << 12)
#define BASE_MEM_SAME_VA (1u << 13)
#define BASE_MEM_NEED_MMAP (1u << 14)
#define BASE_MEM_FLAGS_NR_BITS 19

/* Job requirements. */
#define BASE_JD_REQ_DEP 0u
#define BASE_JD_REQ_FS (1u << 0)
#define BASE_JD_REQ_CS (1u << 1)
#define BASE_JD_REQ_T (1u << 2)
#define BASE_JD_REQ_CF (1u << 3)
#define BASE_JD_REQ_V (1u << 4)
#define BASE_JD_REQ_SOFT_JOB (1u << 9)
#define BASE_JD_REQ_SOFT_JOB_TYPE (BASE_JD_REQ_SOFT_JOB | 0x1f)
#define BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME (BASE_JD_REQ_SOFT_JOB | 0x1)
#define BASE_JD_REQ_SOFT_FENCE_TRIGGER (BASE_JD_REQ_SOFT_JOB | 0x2)
#define BASE_JD_REQ_SOFT_FENCE_WAIT (BASE_JD_REQ_SOFT_JOB | 0x3)
#define BASE_JD_REQ_SOFT_EVENT_WAIT (BASE_JD_REQ_SOFT_JOB | 0x5)
#define BASE_JD_REQ_SOFT_EVENT_SET (BASE_JD_REQ_SOFT_JOB | 0x6)
#define BASE_JD_REQ_SOFT_JOB_TYPE_MASK (BASE_JD_REQ_SOFT_JOB | 0x1f)

#define BASE_JD_PRIO_MEDIUM 0
#define BASE_JD_PRIO_HIGH 1
#define BASE_JD_PRIO_LOW 2

/* Result written by BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME into atom->jc. */
struct base_dump_cpu_gpu_counters {
	uint64_t system_time;	/* 0 */
	uint64_t cycle_counter;	/* 8 */
	uint64_t sec;		/* 16 */
	uint64_t usec;		/* 24 */
	uint8_t padding[32];	/* to 64 bytes (cache line) */
};

/* Function ids (mali_kbase_uku.h enum kbase_uk_function_id). */
#define KBASE_FUNC_MEM_ALLOC (UK_FUNC_ID + 0)
#define KBASE_FUNC_MEM_IMPORT (UK_FUNC_ID + 1)
#define KBASE_FUNC_MEM_COMMIT (UK_FUNC_ID + 2)
#define KBASE_FUNC_MEM_QUERY (UK_FUNC_ID + 3)
#define KBASE_FUNC_MEM_FREE (UK_FUNC_ID + 4)
#define KBASE_FUNC_MEM_FLAGS_CHANGE (UK_FUNC_ID + 5)
#define KBASE_FUNC_MEM_ALIAS (UK_FUNC_ID + 6)
#define KBASE_FUNC_SYNC (UK_FUNC_ID + 8)
#define KBASE_FUNC_POST_TERM (UK_FUNC_ID + 9)
#define KBASE_FUNC_GPU_PROPS_REG_DUMP (UK_FUNC_ID + 14)
#define KBASE_FUNC_GET_VERSION (UK_FUNC_ID + 16)
#define KBASE_FUNC_SET_FLAGS (UK_FUNC_ID + 18)
#define KBASE_FUNC_JOB_SUBMIT (UK_FUNC_ID + 28)
#define KBASE_FUNC_DISJOINT_QUERY (UK_FUNC_ID + 29)
#define KBASE_FUNC_GET_CONTEXT_ID (UK_FUNC_ID + 31)
#define KBASE_FUNC_TLSTREAM_ACQUIRE_V10_4 (UK_FUNC_ID + 32)
#define KBASE_FUNC_TLSTREAM_FLUSH (UK_FUNC_ID + 35)
#define KBASE_FUNC_HWCNT_READER_SETUP (UK_FUNC_ID + 36)
#define KBASE_FUNC_SOFT_EVENT_UPDATE (UK_FUNC_ID + 38)
#define KBASE_FUNC_MEM_JIT_INIT (UK_FUNC_ID + 39)
#define KBASE_FUNC_TLSTREAM_ACQUIRE (UK_FUNC_ID + 40)
/* Samsung vendor block (CONFIG_MALI_SEC_HWCNT / secure rendering). */
#define KBASE_FUNC_HWCNT_UTIL_SETUP (UK_FUNC_ID + 41)
#define KBASE_FUNC_HWCNT_GPR_DUMP (UK_FUNC_ID + 42)
#define KBASE_FUNC_VSYNC_SKIP (UK_FUNC_ID + 43)
#define KBASE_FUNC_CREATE_SURFACE (UK_FUNC_ID + 44)
#define KBASE_FUNC_DESTROY_SURFACE (UK_FUNC_ID + 45)
#define KBASE_FUNC_SET_MIN_LOCK (UK_FUNC_ID + 46)
#define KBASE_FUNC_UNSET_MIN_LOCK (UK_FUNC_ID + 47)
#define KBASE_FUNC_TMU_SKIP (UK_FUNC_ID + 48)
#define KBASE_FUNC_SECURE_WORLD_RENDERING (UK_FUNC_ID + 55)
#define KBASE_FUNC_NON_SECURE_WORLD_RENDERING (UK_FUNC_ID + 56)
#define KBASE_FUNC_MAX (UK_FUNC_ID + 57) /* 0x239 */

/* mmap offsets (mali_base_kernel.h BASEP_MEM_* / BASE_MEM_*_HANDLE). */
#define BASEP_MEM_INVALID_HANDLE (0ull << 12)
#define BASE_MEM_COOKIE_BASE (64ull << 12)
#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12) /* mmap this page first */

/* ------------------------------------------------------------------ */
/* Compile-time ABI conformance (values measured from the r15p0 kernel
 * headers with tools/kbase-layout.c on the same compiler; see
 * abi-expected.txt).  Offsets are relative to the start of the struct. */
#define KBASE_ASSERT_SIZE(t, n) _Static_assert(sizeof(t) == (n), #t " size")
#define KBASE_ASSERT_OFF(t, f, n) \
	_Static_assert(__builtin_offsetof(t, f) == (n), #t "." #f " offset")

KBASE_ASSERT_SIZE(union kbase_uk_header, 8);
KBASE_ASSERT_SIZE(struct uku_version_check_args, 16);
KBASE_ASSERT_SIZE(struct kbase_uk_set_flags, 16);
KBASE_ASSERT_SIZE(struct kbase_uk_get_ddk_version, 80);
KBASE_ASSERT_SIZE(struct kbase_uk_context_id, 16);
KBASE_ASSERT_SIZE(struct kbase_uk_mem_alloc, 56);
KBASE_ASSERT_SIZE(struct kbase_uk_mem_free, 16);
KBASE_ASSERT_SIZE(struct kbase_uk_mem_query, 32);
KBASE_ASSERT_SIZE(struct kbase_uk_mem_commit, 32);
KBASE_ASSERT_SIZE(struct kbase_uk_mem_flags_change, 32);
KBASE_ASSERT_SIZE(struct kbase_uk_job_submit, 40);
KBASE_ASSERT_SIZE(struct base_jd_atom_v2, 64);
KBASE_ASSERT_SIZE(struct base_jd_event_v2, 24);
KBASE_ASSERT_SIZE(struct base_dump_cpu_gpu_counters, 64);

KBASE_ASSERT_OFF(struct kbase_uk_mem_alloc, gpu_va, 40);
KBASE_ASSERT_OFF(struct kbase_uk_job_submit, addr, 8);
KBASE_ASSERT_OFF(struct kbase_uk_job_submit, nr_atoms, 16);
KBASE_ASSERT_OFF(struct kbase_uk_job_submit, stride, 20);
KBASE_ASSERT_OFF(struct kbase_uk_job_submit, gles_ctx_handle, 24);
KBASE_ASSERT_OFF(struct kbase_uk_job_submit, frame_number, 28);
KBASE_ASSERT_OFF(struct kbase_uk_job_submit, surfacep, 32);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, udata, 8);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, extres_list, 24);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, nr_extres, 32);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, compat_core_req, 34);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, pre_dep, 36);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, atom_number, 40);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, prio, 41);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, device_nr, 42);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, core_req, 44);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, gles_ctx_handle, 48);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, frame_number, 52);
KBASE_ASSERT_OFF(struct base_jd_atom_v2, surfacep, 56);

#endif /* KBASE_UAPI_H */
