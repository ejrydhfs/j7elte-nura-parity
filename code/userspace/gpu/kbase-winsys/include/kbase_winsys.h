/*
 * kbase_winsys.h - kbase (UK 10.6) winsys for the Panfrost Gallium driver.
 *
 * The Nura 3.10 kernel exposes ARM kbase r15p0 (/dev/mali0), not DRM.  This
 * library is the userspace KMD layer a Gallium driver needs: one kbase
 * context per fd, SAME_VA memory, atom submission with explicit
 * dependencies, and fd-backed fences (kbase sync stream + soft fence
 * trigger atoms).  It is the exact shape of Mesa's src/panfrost/lib/kmod
 * backend interface, so the Mesa glue is a thin translation layer.
 *
 * It does not compile shaders and does not draw.  It is not a GL driver.
 *
 * ABI source of truth: parity/userspace/gpu/kbase/ (kbase_uapi.h, verified
 * against the kernel's r15p0 headers by tools/verify-abi.sh).
 * Kernel reference points (drivers/gpu/arm/t7xx/r15p0):
 *   mali_kbase_core_linux.c:kbase_api_handshake()  - UK version check
 *   mali_kbase_core_linux.c:kbase_dispatch()       - STREAM_CREATE (id 536)
 *   mali_kbase_mem_linux.c:kbase_mmap()            - SAME_VA cookie mmap
 *   mali_kbase_jd.c:kbase_jd_submit()              - atom stride 64
 *   mali_kbase_softjobs.c:kbase_prepare_soft_job() - fence trigger/wait
 *   mali_kbase_event.c:kbase_event_post()          - one event per atom
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef KBASE_WINSYS_H
#define KBASE_WINSYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbase.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KBW_MAX_ATOMS 256 /* BASE_JD_ATOM_COUNT */

struct kbw_device;
struct kbw_screen;
struct kbw_context;
struct kbw_bo;
struct kbw_fence;

/* GPU capabilities, parsed from the 528-byte mali_base_gpu_props blob that
 * KBASE_FUNC_GPU_PROPS_REG_DUMP returns.  Offsets are static-asserted in
 * kbase_winsys.c against tools/kbw-props-layout.c output. */
struct kbw_caps {
   char ddk_version[KBASE_GET_VERSION_BUFFER_SIZE + 1];
   unsigned uk_major;
   unsigned uk_minor;

   uint32_t gpu_id; /* raw GPU_ID register: product in bits 16..31 */
   uint32_t product_id;
   uint16_t major_revision;
   uint16_t minor_revision;
   uint16_t version_status;

   uint64_t shader_present;
   uint64_t tiler_present;
   uint64_t l2_present;
   uint64_t stack_present;
   uint32_t as_present;
   uint32_t js_present;
   uint32_t js_features[16];

   uint32_t l2_features;
   uint32_t mem_features;
   uint32_t mmu_features;
   uint32_t tiler_features;
   uint32_t texture_features[4];
   uint32_t thread_features;
   uint32_t thread_max_threads;
   uint32_t thread_max_workgroup_size;
   uint32_t thread_max_barrier_size;
   uint32_t coherency_mode;
   uint32_t num_groups;
   uint32_t num_core_groups;

   /* Derived. */
   uint32_t va_bits;
   uint32_t core_count;
   uint32_t core_id_range;
   uint32_t max_job_slots;
   bool io_coherent;
};

/* ------------------------------------------------------------------ */
/* Device: exactly one kbase context (one open of /dev/maliN). */

/* Open and handshake.  Returns 0 or -errno; on failure 'err' is filled. */
int kbw_device_open(const char *path, struct kbw_device **out, char *err,
                    size_t errlen);

/* Adopt an already-open /dev/maliN fd (used by the Mesa backend, which gets
 * the fd from pipe_screen creation).  On success the device owns 'fd'. */
int kbw_device_adopt_fd(int fd, struct kbw_device **out, char *err,
                        size_t errlen);

void kbw_device_destroy(struct kbw_device *dev);

const struct kbw_caps *kbw_device_caps(const struct kbw_device *dev);
int kbw_device_fd(const struct kbw_device *dev);

/* Screen: a device plus a VA/BO policy.  There is one per pipe_screen. */
int kbw_screen_create(struct kbw_device *dev, struct kbw_screen **out);
void kbw_screen_destroy(struct kbw_screen *screen);
struct kbw_device *kbw_screen_device(struct kbw_screen *screen);

/* ------------------------------------------------------------------ */
/* Context: atom slots, event reader, fence timeline.
 *
 * kbase creates a context per open(); two contexts therefore need two fds.
 * KBW_CONTEXT_SHARE_SCREEN_FD reuses the screen's fd (what Mesa needs, since
 * BOs and the VM belong to the screen), KBW_CONTEXT_NEW_FD opens a second
 * kbase context (only valid for standalone use, no BO sharing). */

#define KBW_CONTEXT_SHARE_SCREEN_FD 0u
#define KBW_CONTEXT_NEW_FD 1u

int kbw_context_create(struct kbw_screen *screen, uint32_t flags,
                       struct kbw_context **out, char *err, size_t errlen);
void kbw_context_destroy(struct kbw_context *ctx);

/* Wait until every submitted atom has completed.  timeout_ns < 0 waits
 * forever.  Returns 0, -ETIME or another -errno. */
int kbw_context_wait_idle(struct kbw_context *ctx, int64_t timeout_ns);

/* ------------------------------------------------------------------ */
/* Buffer objects.  On a 64-bit client kbase forces BASE_MEM_SAME_VA, so
 * CPU address == GPU address; the kernel only assigns the GPU VA at mmap
 * time, which is why kbw_bo_create() maps immediately. */

/* Default flags: CPU+GPU read/write, cached CPU, coherent system. */
#define KBW_BO_DEFAULT_FLAGS                                                 \
   (BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR | BASE_MEM_PROT_GPU_RD |     \
    BASE_MEM_PROT_GPU_WR | BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM)

struct kbw_bo {
   struct kbase_bo base;
   struct kbw_context *ctx;
   uint64_t size;
   uint64_t flags; /* flags as returned by the kernel */
};

int kbw_bo_create(struct kbw_context *ctx, uint64_t size, uint64_t flags,
                  struct kbw_bo **out, char *err, size_t errlen);
void kbw_bo_destroy(struct kbw_bo *bo);

void *kbw_bo_cpu(const struct kbw_bo *bo);
uint64_t kbw_bo_gpu_va(const struct kbw_bo *bo);
uint64_t kbw_bo_size(const struct kbw_bo *bo);
int kbw_bo_fd(const struct kbw_bo *bo); /* underlying kbase fd */

/* ------------------------------------------------------------------ */
/* Fences. */

struct kbw_fence {
   int fd;
   bool signaled;
   unsigned refcount;
};

/* Wrap a fence fd (takes ownership). */
int kbw_fence_from_fd(int fd, struct kbw_fence **out);
struct kbw_fence *kbw_fence_ref(struct kbw_fence *fence);
void kbw_fence_unref(struct kbw_fence *fence);

/* 0 = signaled, -ETIME = not yet, other -errno. timeout_ns < 0 = forever. */
int kbw_fence_wait(struct kbw_fence *fence, int64_t timeout_ns);
bool kbw_fence_is_signaled(struct kbw_fence *fence);
/* dup() for handing to another component (WSI, EGL). Returns -errno. */
int kbw_fence_dup_fd(struct kbw_fence *fence);

/* ------------------------------------------------------------------ */
/* Submission.  One kbw_submit_info is one BASE_JD_REQ batch (one
 * KBASE_FUNC_JOB_SUBMIT).  Dependencies are indexes into 'atoms'. */

struct kbw_atom {
   uint64_t jc;      /* GPU VA of a Midgard job-chain (JT header) */
   uint32_t core_req; /* BASE_JD_REQ_* (FS/T/CS/CF/V, or a SOFT_* job) */
   uint8_t prio;      /* BASE_JD_PRIO_* */
   uint8_t device_nr; /* coherent group, only with SPECIFIC_COHERENT_GROUP */
   int8_t dep[2];     /* index into atoms[] or -1 */
   uint8_t dep_type[2]; /* BASE_JD_DEP_TYPE_{DATA,ORDER} */
   uint64_t udata[2]; /* echoed back in the completion event */
};

struct kbw_submit_info {
   const struct kbw_atom *atoms;
   uint32_t atom_count;

   /* Optional sync file to wait on before any atom runs.  Consumed (not
    * closed) by the callee; the caller keeps ownership of the fd. */
   int in_fence_fd;

   /* Optional out fence.  On success *out_fence_fd receives a new fd that
    * signals when the last atom completes (kbase soft fence trigger atom,
    * so it is also usable with the Android sync framework). */
   int *out_fence_fd;
};

int kbw_submit(struct kbw_context *ctx, const struct kbw_submit_info *info);

/* ------------------------------------------------------------------ */
/* Soft-job round trip used by bring-up: BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME
 * writes base_dump_cpu_gpu_counters into 'bo' (GPU writable, 64-byte
 * aligned).  Returns BASE_JD_EVENT_DONE or -errno. */
int kbw_soft_dump_time(struct kbw_context *ctx, struct kbw_bo *bo,
                       struct base_dump_cpu_gpu_counters *dump);

const char *kbw_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* KBASE_WINSYS_H */
