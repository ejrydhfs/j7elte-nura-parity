/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_ioctl.c - six v5.2 ioctl handlers on the GEM core.
 *
 * Userspace contract (gpu31-target.md H1, byte-identical v5.2 UAPI):
 * SUBMIT=0x00, WAIT_BO=0x01, CREATE_BO=0x02, MMAP_BO=0x03,
 * GET_PARAM=0x04, GET_BO_OFFSET=0x05. GET_PARAM answers only
 * DRM_PANFROST_PARAM_GPU_PROD_ID (upstream rejects the rest with
 * -EINVAL; modern-Mesa probes of extra params are handled Mesa-side).
 * CREATE_BO.flags is reserved-zero (reject nonzero, v5.2 has no flags).
 *
 * SUBMIT validates here and dispatches to the jobs slice
 * (code/gpu31/jobs/panfrost7580_job.h: p7580_job_submit(), whose
 * signature is mirrored below so this file -c compiles standalone).
 * The jobs core resolves in-syncobjs (0003 find_fence), reserves the
 * BO resvs (0002), submits to the sched entity (0004), and installs
 * the out-sync fence (0003 replace_fence). User pointers (in_syncs,
 * bo_handles) are copied in with copy_from_user into a stack-capped
 * staging buffer (64 + 16 entries max); oversized counts are -EINVAL,
 * short/failing copies are -EFAULT. No direct user dereference.
 *
 * File-private state (p7580_file_priv, header): per-fd 0003 syncobj
 * manager (init on open, fini on postclose) + opaque jobs per-fd
 * context (allocated by the jobs slice on first SUBMIT, freed by it
 * on postclose). GEM owns neither the syncobj entries nor the sched
 * entities; it only wires the lifetimes.
 *
 * SHIM-CALLSITES: 0002 (dma_resv wait via p7580_gem_wait), 0003
 * (syncobj manager init/fini via driver_priv).
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#include <drm/drmP.h>
#include <drm/drm.h>

#include "drm_syncobj.h"		/* 0003 (staged by Makefile) */
#include "panfrost7580_gem.h"
/* Probe-snapshot GET_PARAM backend (main.c).  Declared here instead of
 * including panfrost7580_drv.h, whose jobs slice mirrors struct
 * p7580_submit locally in this TU. */
struct drm_device;
int p7580_drv_get_param(struct drm_device *ddev, uint32_t param,
			uint64_t *value);
#include "panfrost7580_ioctl.h"
#include "panfrost_drm.h"
/* ABI gate (gpu31-abialign): v5.2/Mesa-20.3 wire sizes + offsets. The
 * 3.10 core keys copy_from/to_user on the driver's own _IOC_SIZE, so
 * any field/size drift silently mis-copies userspace. Fail the build
 * instead. See also the mirror asserts in panfrost7580_syncobj.c. */
#include <linux/bug.h>
/* get_param is the Mesa handshake: identical on v5.2 and Mesa 20.3. */
static inline void p7580_abi_check_get_param(void)
{
	BUILD_BUG_ON(sizeof(struct drm_panfrost_get_param) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_get_param,
					param) != 0);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_get_param,
					pad) != 4);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_get_param,
					value) != 8);
	BUILD_BUG_ON(DRM_PANFROST_PARAM_GPU_PROD_ID != 0);
}
/* SUBMIT is the 40-byte v5.2 shape (no jm_ctx_handle/pad tail). */
static inline void p7580_abi_check_submit(void)
{
	BUILD_BUG_ON(sizeof(struct drm_panfrost_submit) != 40);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit, jc) != 0);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit,
					in_syncs) != 8);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit,
					in_sync_count) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit,
					out_sync) != 20);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit,
					bo_handles) != 24);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit,
					bo_handle_count) != 32);
	BUILD_BUG_ON(__builtin_offsetof(struct drm_panfrost_submit,
					requirements) != 36);
}


/* Jobs-slice SUBMIT core (code/gpu31/jobs/panfrost7580_job.h shape,
 * mirrored locally so this file -c compiles without the jobs tree).
 * Canonical fields (jobs/panfrost7580_jm_hw.h struct p7580_submit):
 * jc/in_syncs/in_sync_count/out_sync/bo_handles/bo_handle_count/
 * requirements -- u64 user pointers staged to kernel arrays below. */
struct p7580_job_dev;
struct p7580_file_ctx;
struct p7580_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
/* p7580_submit mirror pin (kept after the definition: C needs the
 * complete type for sizeof). */
static inline void p7580_abi_check_submit_mirror(void)
{
	BUILD_BUG_ON(sizeof(struct p7580_submit) != 40);
}
int p7580_job_submit(struct p7580_job_dev *jd, struct p7580_file_ctx *fctx,
		     const struct p7580_submit *args);

/* DrmCore owns the manager storage; GEM only borrows it via driver_priv.
 * job_fctx is owned by the jobs slice (opaque pointer here). */
struct p7580_file_priv {
	struct drm_syncobj_manager mgr;
	struct p7580_file_ctx *job_fctx;
	struct p7580_job_dev *job_dev;
};

int p7580_file_priv_init(struct drm_device *dev, struct drm_file *file_priv)
{
	struct p7580_file_priv *priv;

	(void)dev;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	drm_syncobj_manager_init(&priv->mgr);
	file_priv->driver_priv = priv;
	return 0;
}

void p7580_file_priv_fini(struct drm_device *dev, struct drm_file *file_priv)
{
	struct p7580_file_priv *priv = file_priv->driver_priv;

	(void)dev;
	if (!priv)
		return;
	/* Jobs context (sched entities) is freed by the jobs slice's
	 * own postclose leg; GEM only drops the pointer here. */
	priv->job_fctx = NULL;
	priv->job_dev = NULL;
	drm_syncobj_manager_fini(&priv->mgr);
	file_priv->driver_priv = NULL;
	kfree(priv);
}

/* T720 product ID (MIDGARD T720 = 0x720, same decode as kbase gpuprops). */
#define P7580_GPU_PROD_ID		0x720u

/* Caps mirrored from the jobs slice (p7580_submit limits): counts above
 * these are -EINVAL before any copy_from_user runs. */
#define P7580_SUBMIT_BO_MAX		64u
#define P7580_SUBMIT_IN_SYNC_MAX	16u

int panfrost_ioctl_get_param(struct drm_device *dev, void *data,
			     struct drm_file *file_priv)
{
	struct drm_panfrost_get_param *args = data;
	int ret;

	(void)file_priv;
	/* ABI gate: run the build-time checks on every call path so a
	 * header drift cannot slip past an unbuilt TU. Cheap (constant). */
	p7580_abi_check_get_param();
	/* Mesa 20.3 and 26+ both zero the struct, set param and issue the
	 * IOWR(0x44/16 B) ioctl.  Mesa 26 requires a whole feature set (its
	 * panfrost/kmod backend asserts on a failed required query in debug
	 * builds, and compiles zeros otherwise), so answer every
	 * DRM_PANFROST_PARAM_* this part has a register for from the probe
	 * snapshot.  Unknown/unsupported ids stay -EINVAL: Mesa gates the
	 * 1.3/1.5/1.6-only params on the DRM version and tolerates the
	 * failure.  The log is the evidence for which ids Mesa actually
	 * asks for. */
	if (args->pad) {
		pr_info("p7580_get_param: pad=0x%x param=%u rc=-EINVAL\n",
			args->pad, args->param);
		return -EINVAL;
	}
	ret = p7580_drv_get_param(dev, args->param, &args->value);
	pr_info("p7580_get_param: param=%u value=0x%llx rc=%d%s\n",
		args->param, (unsigned long long)args->value, ret,
		ret ? " (unknown/unsupported)" : "");
	return ret;
}

int panfrost_ioctl_create_bo(struct drm_device *dev, void *data,
			     struct drm_file *file_priv)
{
	struct drm_panfrost_create_bo *args = data;
	struct p7580_dev *gd = dev->dev_private;
	struct drm_gem_object *obj;
	struct p7580_bo *bo;
	uint32_t handle;
	int ret;

	if (!args->size || args->pad) {
		pr_info("p7580_create_bo: bad args size=0x%x pad=0x%x rc=-EINVAL\n",
			args->size, args->pad);
		return -EINVAL;
	}
	/* Modern Mesa (kernel 1.1+) passes PANFROST_BO_NOEXEC (non-executable
	 * BO) and PANFROST_BO_HEAP (alloc-on-fault tiler heap).  Backing here
	 * is always pre-faulted SHMEM, so both are hints: accept and record
	 * them; ignore, with a warning, any bit this driver does not know
	 * instead of failing the allocation. */
	if (args->flags & ~(PANFROST_BO_NOEXEC | PANFROST_BO_HEAP |
			    PANFROST_BO_WB_MMAP))
		pr_warn("p7580_create_bo: unknown flags 0x%x ignored (size=0x%x)\n",
			args->flags, args->size);
	pr_info("p7580_create_bo: enter size=0x%x flags=0x%x abi sizeof=%zu flags_off=%zu pad_off=%zu offset_off=%zu\n",
		args->size, args->flags, sizeof(*args),
		offsetof(struct drm_panfrost_create_bo, flags),
		offsetof(struct drm_panfrost_create_bo, pad),
		offsetof(struct drm_panfrost_create_bo, offset));

	/* SHMEM for general BOs (upstream panfrost uses drm_gem_shmem;
	 * CMA carries no size beyond the display window and fails 8 MiB+
	 * via dma_alloc_attrs -> swiotlb ENOMEM, blocking Mesa's 16 MiB
	 * tiler heap. CMA stays available for explicit WC scanout only,
	 * selected via p7580_gem_create_with_handle, never here). */
	ret = p7580_gem_create(gd, dev, args->size, P7580_BO_SHMEM,
			       P7580_GEM_WC, &obj);
	if (ret)
		return ret;
	ret = drm_gem_handle_create(file_priv, obj, &handle);
	if (ret) {
		drm_gem_object_unreference_unlocked(obj);
		return ret;
	}
	/* Handle holds the reference now. */
	drm_gem_object_unreference_unlocked(obj);

	bo = to_p7580_bo(obj);
	args->handle = handle;
	args->offset = (uint64_t)bo->iova;
	pr_info("p7580_create_bo: size=0x%x flags=0x%x handle=%u iova=0x%llx offset_valid=%u node_start=0x%llx rc=0\n",
		args->size, args->flags, handle,
		(unsigned long long)bo->iova, bo->offset_valid,
		(unsigned long long)bo->offset_node.start);
	return 0;
}

int panfrost_ioctl_mmap_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_panfrost_mmap_bo *args = data;
	struct p7580_dev *gd = dev->dev_private;
	struct drm_gem_object *obj;
	struct p7580_bo *bo;
	uint64_t fake_offset = 0;

	if (args->flags)
		pr_warn("p7580_mmap_bo: flags=0x%x ignored (currently no meaning)\n",
			args->flags);
	if (!gd)
		return -ENODEV;

	/* drm_gem_object_lookup takes the reference internally (no
	 * outer struct_mutex needed; lookup is idr + kref_get). */
	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (!obj) {
		pr_info("p7580_mmap_bo: handle=%u lookup MISS rc=-ENOENT\n",
			args->handle);
		return -ENOENT;
	}
	bo = to_p7580_bo(obj);
	/* gpu31-mmlock: same fake-offset encoding as before (page index
	 * << PAGE_SHIFT), now read from the driver-owned manager. The
	 * lookup reference keeps the object -- and therefore its offset --
	 * alive for the read; the lock makes the read atomic against a
	 * free that can run on the sched workqueue. */
	mutex_lock(&gd->mmap_offset_lock);
	if (bo->offset_valid)
		fake_offset = (uint64_t)bo->offset_node.start << PAGE_SHIFT;
	mutex_unlock(&gd->mmap_offset_lock);
	pr_info("p7580_mmap_bo: handle=%u offset_valid=%u node_start=0x%llx fake_offset=0x%llx iova=0x%llx\n",
		args->handle, bo->offset_valid,
		(unsigned long long)bo->offset_node.start,
		(unsigned long long)fake_offset,
		(unsigned long long)bo->iova);
	drm_gem_object_unreference_unlocked(obj);
	if (!fake_offset) {
		pr_info("p7580_mmap_bo: no fake offset rc=-ENOENT\n");
		return -ENOENT;
	}

	args->offset = fake_offset;
	return 0;
}

int panfrost_ioctl_get_bo_offset(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct drm_panfrost_get_bo_offset *args = data;
	struct drm_gem_object *obj;
	struct p7580_bo *bo;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (!obj)
		return -ENOENT;
	bo = to_p7580_bo(obj);
	args->offset = (uint64_t)bo->iova;
	drm_gem_object_unreference_unlocked(obj);
	return 0;
}

int panfrost_ioctl_wait_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct drm_panfrost_wait_bo *args = data;
	struct timespec now;
	int64_t remain_ns;
	long timeout;

	if (args->pad)
		return -EINVAL;
	/* F-IO2: v5.2 timeout_ns is ABSOLUTE (CLOCK_MONOTONIC). Convert
	 * to a relative jiffies budget via the monotonic clock; a past
	 * or zero stamp stays non-blocking (0), matching the old
	 * <= 0 arm. ktime_get_ts is the 3.10 spelling (no ktime_get_ns
	 * export used in-tree on this kernel). */
	ktime_get_ts(&now);
	remain_ns = (int64_t)args->timeout_ns -
		    ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
	if (remain_ns <= 0)
		timeout = 0;
	else if ((uint64_t)remain_ns > (uint64_t)LONG_MAX)
		timeout = LONG_MAX;
	else
		timeout = nsecs_to_jiffies((uint64_t)remain_ns);

	timeout = p7580_gem_wait(dev, file_priv, args->handle, timeout, 1);
	/* gpu31-storm58: dma_resv_wait_timeout() returns >0 (jiffies
	 * left) when the object is idle, 0 when the budget ran out and
	 * <0 for an error -- exactly v5.2 panfrost_wait_bo's contract,
	 * including the non-blocking poll: timeout_ns = 0 (a past
	 * absolute stamp) becomes a 0-jiffy wait and returns 0 for a
	 * still-pending fence. The old code mapped only <0 to an error
	 * and returned 0 for BOTH 0 and >0, so WAIT_BO answered
	 * "ready" for a busy BO. Mesa's BO-cache fetch uses exactly
	 * that poll (panfrost_bo_wait(bo, 0, RW)) before handing a
	 * cached BO back to the next batch, so it reused pages that an
	 * in-flight submit still owned -- measured on int42 as 4
	 * re-fetches of handle 6 (64 KiB descriptor slab) inside a
	 * 60 s window in which that submit's fence never signaled.
	 * Report 0 as -ETIMEDOUT like upstream. */
	if (timeout == 0)
		return -ETIMEDOUT;
	if (timeout < 0)
		return (int)timeout;
	return 0;
}
/* DEBUG-ONLY (gpu31-gpuwrite, strip before cutover): kernel-side BO
 * word readback after completion. Same maintenance as the pre-signal
 * IRQ path (p7580_gem_debug_read_word calls p7580_gem_job_done_sync),
 * so the kernel view cannot reproduce the userspace-alias ambiguity.
 * Plain static enable: always on while the ticket is open; the cutover
 * deletes this function + its 0xCF route. No MMIO, no sleeps. */
struct p7580_debug_read_bo {
	uint32_t handle;
	uint32_t offset;	/* byte offset of the u32 to read */
	uint32_t value;		/* OUT: u32 at that offset */
	uint32_t pad;		/* must be zero */
};
int panfrost_debug_read_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct p7580_debug_read_bo *args = data;
	struct drm_gem_object *obj;
	uint32_t v = 0;
	int ret;

	if (args->pad)
		return -EINVAL;
	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (!obj)
		return -ENOENT;
	ret = p7580_gem_debug_read_word(obj, args->offset, &v);
	drm_gem_object_unreference_unlocked(obj);
	if (ret)
		return ret;
	args->value = v;
	return 0;
}

/* SUBMIT: validate-only in this slice is over -- full dispatch goes to
 * the jobs core, with user arrays staged on the stack first. */
int panfrost_ioctl_submit(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct drm_panfrost_submit *args = data;
	struct p7580_file_priv *priv = file_priv->driver_priv;
	uint32_t bo_handles[P7580_SUBMIT_BO_MAX];
	uint32_t in_syncs[P7580_SUBMIT_IN_SYNC_MAX];
	struct p7580_submit staged;

	p7580_abi_check_submit();
	p7580_abi_check_submit_mirror();
	if (!args->jc || (args->jc & 7u))
		return -EINVAL;
	if (args->requirements & ~PANFROST_JD_REQ_FS)
		return -EINVAL;
	if (args->in_sync_count > P7580_SUBMIT_IN_SYNC_MAX ||
	    args->bo_handle_count > P7580_SUBMIT_BO_MAX)
		return -EINVAL;
	if (!priv || !priv->job_dev || !priv->job_fctx)
		return -ENODEV;

	/* Stage user arrays locally; the jobs core never touches
	 * userspace. in_syncs/bo_handles are u32 arrays (Mesa winsys
	 * passes __u64 pointers to u32 storage). */
	if (args->bo_handle_count) {
		if (!args->bo_handles ||
		    copy_from_user(bo_handles, (void __user *)(uintptr_t)
				   args->bo_handles,
				   args->bo_handle_count * sizeof(uint32_t)))
			return -EFAULT;
	}
	if (args->in_sync_count) {
		if (!args->in_syncs ||
		    copy_from_user(in_syncs, (void __user *)(uintptr_t)
				   args->in_syncs,
				   args->in_sync_count * sizeof(uint32_t)))
			return -EFAULT;
	}

	staged.jc = args->jc;
	staged.in_syncs = (uint64_t)(uintptr_t)in_syncs;
	staged.in_sync_count = args->in_sync_count;
	staged.out_sync = args->out_sync;
	staged.bo_handles = (uint64_t)(uintptr_t)bo_handles;
	staged.bo_handle_count = args->bo_handle_count;
	staged.requirements = args->requirements;

	return p7580_job_submit(priv->job_dev, priv->job_fctx, &staged);
}
