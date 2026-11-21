/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_syncobj.c - generic DRM syncobj ioctl bodies for the
 * T720 port (Exynos 7580, Nura 3.10.108, j7elte).
 *
 * Context (see the header): Mesa 20.3.5 panfrost asserts on
 * drmSyncobjCreate (pan_context.c:1632-1633). The 3.10 core exposes
 * no core SYNCOBJ rows, and the driver-private table only serves
 * nr < DRM_COMMAND_END (0xA0); the generic syncobj numbers
 * (0xBF-0xC5) always fall through to the core table, where a NULL
 * func returns -EINVAL (drm_drv.c:401-435). So this file registers
 * NOTHING in the driver's ioctl table. Instead the driver's fops
 * .unlocked_ioctl/.compat_ioctl point at panfrost_syncobj_ioctl,
 * which routes the seven syncobj numbers here and forwards
 * everything else to drm_ioctl() unchanged.
 *
 * errno contract (what Mesa/libdrm expect; same as mainline):
 *  - CREATE: unknown flags bits -> -EINVAL (mainline rejects
 *    anything but CREATE_SIGNALED at once: the check is
 *    flags & ~DRM_SYNCOBJ_CREATE_SIGNALED).
 *  - CREATE_SIGNALED: installs a signaled fence so a first WAIT
 *    returns 0 instead of blocking forever. 3.10 has no pre-signaled
 *    fence type, so an already-signaled private fence is used
 *    (dma_fence_init + dma_fence_signal, released on replace).
 *  - DESTROY of an unknown handle -> -ENOENT (0003 shim contract;
 *    matches mainline ENOENT-on-stale-handle).
 *  - WAIT: unknown flag bits -> -EINVAL; timeout is ABSOLUTE
 *    CLOCK_MONOTONIC ns (drm_syncobj_wait in the 20.x libdrm wrapper
 *    passes the absolute stamp straight through). Past/zero stamp is
 *    non-blocking (0 jiffies). count==0 or NULL handles -> -EINVAL.
 *    WAIT_ALL(all fences signaled -> 0) and WAIT_ANY(first signaled
 *    index in first_signaled, still 0; none signaled in budget ->
 *    -ETIME, matching the shim's dma_fence_wait_timeout -ETIME).
 *    WAIT_FOR_SUBMIT/WAIT_AVAILABLE need the timeline/eventfd code
 *    the 0003 shim deliberately omits -> -EINVAL (never silently
 *    ignored: Mesa would hang on an un-waited submit fence).
 *  - RESET of an unknown handle -> -ENOENT; NULL fence is already
 *    the signaled state, so reset keeps fence NULL and returns 0.
 *  - SIGNAL: unknown handle -> -ENOENT; the installed fence is a
 *    private already-signaled fence (see CREATE_SIGNALED). Signaling
 *    an already-signaled (or empty) syncobj is a no-op 0 -- Mesa
 *    re-signals without checking.
 *  - HANDLE_TO_FD/FD_TO_HANDLE tolerate bit 0 (libdrm 2.4.134 sets
 *    EXPORT/IMPORT_SYNC_FILE unconditionally; no sync_file.c on 3.10
 *    but the plain-fence anon-inode fd satisfies Mesa's snapshot use)
 *    and reject all other unknown bits (TIMELINE bit 1 included).
 *  - copy_from_user/copy_to_user faults -> -EFAULT (the 3.10 core
 *    copies the fixed struct, NOT the pointed-to handle array; the
 *    array is copied here).
 *  - no driver_priv (open did not run) -> -ENODEV.
 *
 * Locking: the 0003 manager serialises the idr; each syncobj
 * serialises its own fence pointer. WAIT snapshots fence refs under
 * the manager then sleeps unlocked. The anon-inode export holds a
 * fence ref for the file's lifetime (released in syncobj_file_release).
 *
 * SHIM-CALLSITES: 0003 (manager create/destroy/find, replace_fence,
 * find_fence). 0001 (dma_fence_init/signal/wait_timeout/get/put via
 * the 0003 wrappers). No 0002/0004 use.
 */

#include <linux/anon_inodes.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/file.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/uaccess.h>

#include <drm/drmP.h>
#include <drm/drm.h>

#include <linux/dma-fence.h>		/* 0001 (staged by Makefile) */
#include "drm_syncobj.h"		/* 0003 (staged by Makefile) */
#include "panfrost7580_syncobj.h"

/* ---- UAPI payload mirrors (3.10-era sizes, byte-identical wire) ---- */
/* v5.2/20.x drm.h: SYNCOBJ ioctls are core numbers, NOT driver-private:
 * CREATE 0xBF, DESTROY 0xC0, HANDLE_TO_FD 0xC1, FD_TO_HANDLE 0xC2,
 * WAIT 0xC3, RESET 0xC4, SIGNAL 0xC5 (DRM_IOWR). The _IOC_NR (low 8
 * bits) is what drm_ioctl dispatches on; the core table has no rows
 * there, so our fops dispatcher catches them first. */
#define P7580_SYNCOBJ_NR_CREATE		0xBFu
#define P7580_SYNCOBJ_NR_DESTROY	0xC0u
#define P7580_SYNCOBJ_NR_HANDLE_TO_FD	0xC1u
#define P7580_SYNCOBJ_NR_FD_TO_HANDLE	0xC2u
#define P7580_SYNCOBJ_NR_WAIT		0xC3u
#define P7580_SYNCOBJ_NR_RESET		0xC4u
#define P7580_SYNCOBJ_NR_SIGNAL		0xC5u
/* DEBUG-ONLY (gpu31-gpuwrite, strip before cutover): kernel BO word
 * readback route. 0xCF sits above the syncobj 0xBF-0xC5 block and
 * below DRM_COMMAND_BASE 0x40-space driver numbers: no collision
 * with either table; the six-ioctl driver table is untouched. */
#define P7580_DEBUG_READ_BO_NR		0xCFu

#define P7580_SYNCOBJ_CREATE_SIGNALED	(1u << 0)

#define P7580_SYNCOBJ_WAIT_ALL		(1u << 0)
#define P7580_SYNCOBJ_WAIT_FOR_SUBMIT	(1u << 1)
#define P7580_SYNCOBJ_WAIT_AVAILABLE	(1u << 2)
/* Bits the 20.x WAIT struct defines but this binary subset cannot
 * honour: deadline hint (needs fence deadlines, absent in 0001). */
#define P7580_SYNCOBJ_WAIT_DEADLINE	(1u << 3)

struct p7580_syncobj_create {
	__u32 handle;
	__u32 flags;
};

struct p7580_syncobj_destroy {
	__u32 handle;
	__u32 pad;
};

struct p7580_syncobj_handle {
	__u32 handle;
	__u32 flags;
	__s32 fd;
	__u32 pad;
};

struct p7580_syncobj_wait {
	__u64 handles;
	__s64 timeout_nsec;	/* absolute CLOCK_MONOTONIC */
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled;
	__u32 pad;
};

struct p7580_syncobj_array {
	__u64 handles;
	__u32 count_handles;
	__u32 pad;
};
/* DEBUG-ONLY (gpu31-gpuwrite, strip before cutover): 16 B payload
 * mirror of struct p7580_debug_read_bo (ioctl.c): handle + byte
 * offset + OUT value + zero pad. Same 16 B shape discipline as the
 * syncobj structs. */
struct p7580_debug_read_bo {
	__u32 handle;
	__u32 offset;
	__u32 value;
	__u32 pad;
};
int panfrost_debug_read_bo(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);

/* ABI gate (gpu31-abialign, both TUs): Mesa 20.3 bundles the 20.x
 * drm.h syncobj shapes (verified on-device 2026-11-20 from
 * /data/mesabuild/mesa20/include/drm-uapi/drm.h + disassembled
 * libdrm.so.2.134.0 wrappers):
 *   CREATE 8 B  IOWR(0xBF), DESTROY 8 B (v5.2-era: IOWR, current is
 *   IOWR too -- the old IOW comment in pf7580.c predates the check),
 *   HANDLE 16 B IOWR(0xC1/0xC2, NO point field),
 *   WAIT   32 B IOWR(0xC3, NO deadline_nsec tail),
 *   ARRAY  16 B IOWR(0xC4/0xC5).
 * The 3.10 core keys copy_from/to_user on OUR asize (the dispatcher's
 * asize table below), so the struct MUST stay 16/32 B: a 24/40 B
 * current-header struct would over-copy into the stack buffer. The
 * dispatcher ALSO keys on _IOC_NR only (not _IOC_SIZE), so a 24 B
 * HANDLE_TO_FD from libdrm 2.4.134 lands here with usize=24; the
 * handler reads only the first 16 B and ignores the point tail.
 * ioctl direction (client pf7580.c asserts the same): CREATE+WAIT are
 * IOWR, DESTROY is IOWR on the wire the kernel serves (20.x drm.h says
 * DRM_IOWR for all three; the legacy IOW DESTROY encoding aliases the
 * same _IOC_NR so dispatch is unaffected, but new code MUST use IOWR).
 * Flag contract: SYNC_FILE bits (HTOF/FTOH bit 0) need sync_file.c
 * (absent on 3.10) and are the ONLY rejected HTOF/FTOH bits; WAIT
 * rejects FOR_SUBMIT/AVAILABLE/DEADLINE. See the handle_to_fd body for
 * why SYNC_FILE is silently tolerated on plain-fence exports. */
#include <linux/bug.h>
/* Bit 0 doubles as the only valid HTOF/FTOH flag on BOTH the 20.x
 * header (EXPORT/IMPORT_SYNC_FILE) and mainline (TIMELINE is bit 1). */
#define P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE	(1u << 0)
#define P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE	(1u << 0)
static inline void p7580_syncobj_abi_check(void)
{
	BUILD_BUG_ON(sizeof(struct p7580_syncobj_create) != 8);
	BUILD_BUG_ON(sizeof(struct p7580_syncobj_destroy) != 8);
	BUILD_BUG_ON(sizeof(struct p7580_syncobj_handle) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_handle,
					handle) != 0);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_handle,
					flags) != 4);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_handle,
					fd) != 8);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_handle,
					pad) != 12);
	BUILD_BUG_ON(sizeof(struct p7580_syncobj_wait) != 32);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_wait,
					handles) != 0);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_wait,
					timeout_nsec) != 8);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_wait,
					count_handles) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_wait,
					flags) != 20);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_wait,
					first_signaled) != 24);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_syncobj_wait,
					pad) != 28);
	BUILD_BUG_ON(sizeof(struct p7580_syncobj_array) != 16);
	BUILD_BUG_ON(P7580_SYNCOBJ_CREATE_SIGNALED !=
		     (1u << 0));
	BUILD_BUG_ON(P7580_SYNCOBJ_WAIT_ALL != (1u << 0));
	BUILD_BUG_ON(P7580_SYNCOBJ_WAIT_FOR_SUBMIT != (1u << 1));
	BUILD_BUG_ON(P7580_SYNCOBJ_WAIT_AVAILABLE != (1u << 2));
	BUILD_BUG_ON(P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE != (1u << 0));
	BUILD_BUG_ON(P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE != (1u << 0));
	/* DEBUG-ONLY (gpu31-gpuwrite): 16 B readback payload pin. */
	BUILD_BUG_ON(sizeof(struct p7580_debug_read_bo) != 16);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_debug_read_bo,
					handle) != 0);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_debug_read_bo,
					offset) != 4);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_debug_read_bo,
					value) != 8);
	BUILD_BUG_ON(__builtin_offsetof(struct p7580_debug_read_bo,
					pad) != 12);
	BUILD_BUG_ON(P7580_DEBUG_READ_BO_NR != 0xCFu);
}

/* The manager lives first in driver_priv (GEM p7580_file_priv and the
 * driver's p7580_drv_file share the layout: mgr, ...). */
static struct drm_syncobj_manager *p7580_sync_mgr(struct drm_file *fp)
{
	struct drm_syncobj_manager *mgr;

	if (!fp)
		return NULL;
	mgr = (struct drm_syncobj_manager *)fp->driver_priv;
	return mgr;
}

/* A pre-signaled fence: 0001 has no timeline/signaled constructor, so
 * init a private fence and signal it once. Ops mirror the submit-job
 * fence backend contract (release via release_base). The table is
 * writable static (NOT const): dma_fence_init BUGs on a NULL release,
 * so the hook must be assignable at definition time, and a const
 * table would take a .rodata write on first CREATE_SIGNALED/SIGNAL. */
static void p7580_sync_signaled_release(struct dma_fence *fence)
{
	dma_fence_release_base(fence);
	kfree(fence);
}

static struct dma_fence_ops p7580_sync_signaled_ops = {
	.get_driver_name = NULL,
	.get_timeline_name = NULL,
	.enable_signaling = NULL,
	.signaled = NULL,
	.wait = NULL,
	.release = p7580_sync_signaled_release,
};

static struct dma_fence *p7580_sync_signaled_fence(void)
{
	struct dma_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	dma_fence_init(f, &p7580_sync_signaled_ops,
		       dma_fence_context_alloc(1), 1);
	dma_fence_signal(f);
	return f;
}

int panfrost_syncobj_create(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct p7580_syncobj_create *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);
	struct drm_syncobj *s;
	u32 handle;
	int ret;

	(void)dev;
	p7580_syncobj_abi_check();
	if (!mgr)
		return -ENODEV;
	/* Mainline rejects any unknown bit at once (flags & ~SIGNALED). */
	if (args->flags & ~P7580_SYNCOBJ_CREATE_SIGNALED)
		return -EINVAL;
	ret = drm_syncobj_create(mgr, &handle);
	if (ret)
		return ret;
	if (args->flags & P7580_SYNCOBJ_CREATE_SIGNALED) {
		struct dma_fence *f = p7580_sync_signaled_fence();

		if (!f) {
			drm_syncobj_destroy(mgr, handle);
			return -ENOMEM;
		}
		s = drm_syncobj_find(mgr, handle);
		if (!s) {
			dma_fence_put(f);
			drm_syncobj_destroy(mgr, handle);
			return -ENOENT;
		}
		ret = drm_syncobj_replace_fence(s, f);
		drm_syncobj_put(s);
		/* replace takes its own ref. */
		dma_fence_put(f);
		if (ret) {
			drm_syncobj_destroy(mgr, handle);
			return ret;
		}
	}
	args->handle = handle;
	return 0;
}

int panfrost_syncobj_destroy(struct drm_device *dev, void *data,
			     struct drm_file *file_priv)
{
	struct p7580_syncobj_destroy *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);

	(void)dev;
	if (!mgr)
		return -ENODEV;
	if (args->pad)
		return -EINVAL;
	/* Unknown handle -> -ENOENT (0003 contract, mainline parity). */
	return drm_syncobj_destroy(mgr, args->handle);
}

/* ---- FD export/import: plain-fence anon_inode transport ---- */
/* 3.10 has no sync_file.c, so HANDLE_TO_FD exports the CURRENT fence
 * (snapshot ref at export time; later replaces do not follow the fd).
 * Bit 0 (EXPORT/IMPORT_SYNC_FILE) is tolerated (see handler bodies);
 * all other flag bits are -EINVAL. */

struct p7580_syncobj_export {
	struct dma_fence *fence;	/* ref held for the file */
};

static int p7580_syncobj_file_release(struct inode *inode, struct file *filp)
{
	struct p7580_syncobj_export *ex = filp->private_data;

	(void)inode;
	if (ex) {
		dma_fence_put(ex->fence);
		kfree(ex);
	}
	return 0;
}

static const struct file_operations p7580_syncobj_file_fops = {
	.owner = THIS_MODULE,
	.release = p7580_syncobj_file_release,
};

int panfrost_syncobj_handle_to_fd(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	struct p7580_syncobj_handle *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);
	struct drm_syncobj *s;
	struct dma_fence *f;
	struct p7580_syncobj_export *ex;
	int fd;

	(void)dev;
	p7580_syncobj_abi_check();
	if (!mgr)
		return -ENODEV;
	if (args->pad)
		return -EINVAL;
	/* Mesa 20.3 libdrm 2.4.134 unconditionally sets bit 0
	 * (EXPORT_SYNC_FILE) in drmSyncobjExportSyncFile/HandleToFD
	 * (disassembled: stp w1=handle,w2=-1 + mov w1=#1 for the flag
	 * word; pan_screen.c panfrost_fence_create calls ExportSyncFile
	 * so EVERY fence snapshot -- and therefore every glFinish/
	 * ReadPixels flush -- goes through here). There is no sync_file
	 * layer on 3.10, but the plain-fence anon-inode fd we install
	 * satisfies the only thing Mesa does with it: import the fence
	 * into a fresh syncobj. Rejecting bit 0 broke all draws (3x
	 * EINVAL per flush, SUBMIT never reached). So: tolerate bit 0,
	 * reject every OTHER unknown bit (TIMELINE bit 1 included --
	 * the 0003 shim has no timeline points, and silently dropping
	 * them would hang Mesa on a fence we never signalled). */
	if (args->flags & ~P7580_SYNCOBJ_HTOF_EXPORT_SYNC_FILE)
		return -EINVAL;
	s = drm_syncobj_find(mgr, args->handle);
	if (!s)
		return -ENOENT;
	f = drm_syncobj_find_fence(s);
	drm_syncobj_put(s);
	if (!f)
		return -EINVAL;	/* NULL fence: nothing to export */
	ex = kzalloc(sizeof(*ex), GFP_KERNEL);
	if (!ex) {
		dma_fence_put(f);
		return -ENOMEM;
	}
	ex->fence = f;	/* fd owns this ref now */
	fd = anon_inode_getfd("syncobj", &p7580_syncobj_file_fops, ex,
			      O_CLOEXEC);
	if (fd < 0) {
		dma_fence_put(f);
		kfree(ex);
		return fd;
	}
	args->fd = fd;
	return 0;
}

int panfrost_syncobj_fd_to_handle(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	struct p7580_syncobj_handle *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);
	struct file *filp;
	struct p7580_syncobj_export *ex;
	struct drm_syncobj *s;
	u32 handle;
	int ret;

	(void)dev;
	if (!mgr)
		return -ENODEV;
	if (args->pad)
		return -EINVAL;
	/* Mirror HANDLE_TO_FD: libdrm drmSyncobjImportSyncFile sets bit 0
	 * (IMPORT_SYNC_FILE); tolerate it, reject the rest (TIMELINE bit 1
	 * included). A foreign sync_file fd (real sync_file.c file) still
	 * fails the f_op check below -- we only snapshot fences from fds
	 * this driver exported. */
	if (args->flags & ~P7580_SYNCOBJ_FTOH_IMPORT_SYNC_FILE)
		return -EINVAL;
	if (args->fd < 0)
		return -EINVAL;
	filp = fget((unsigned int)args->fd);
	if (!filp)
		return -EINVAL;
	/* Only fds this driver exported carry a fence; anything else
	 * (eventfd, pipe, foreign sync_file) is -EINVAL, never a
	 * blind pointer dereference. */
	if (filp->f_op != &p7580_syncobj_file_fops) {
		fput(filp);
		return -EINVAL;
	}
	ex = filp->private_data;
	if (!ex || !ex->fence) {
		fput(filp);
		return -EINVAL;
	}
	ret = drm_syncobj_create(mgr, &handle);
	if (ret) {
		fput(filp);
		return ret;
	}
	s = drm_syncobj_find(mgr, handle);
	if (!s) {
		fput(filp);
		drm_syncobj_destroy(mgr, handle);
		return -ENOENT;
	}
	/* Import snapshots the CURRENT fence of the fd (same non-sync_file
	 * limit as export: later fence replaces on the source syncobj do
	 * not propagate through the fd). */
	ret = drm_syncobj_replace_fence(s, ex->fence);
	drm_syncobj_put(s);
	fput(filp);
	if (ret) {
		drm_syncobj_destroy(mgr, handle);
		return ret;
	}
	args->handle = handle;
	return 0;
}

int panfrost_syncobj_wait(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct p7580_syncobj_wait *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);
	struct drm_syncobj **syncs = NULL;
	struct dma_fence **fences = NULL;
	uint32_t count, i;
	struct timespec now;
	int64_t remain_ns;
	long timeout;
	int ret = 0;

	(void)dev;
	if (!mgr)
		return -ENODEV;
	count = args->count_handles;
	if (!count || !args->handles)
		return -EINVAL;
	/* WAIT_FOR_SUBMIT/WAIT_AVAILABLE need the timeline/eventfd code
	 * the 0003 shim omits; deadline needs 0001 fence deadlines.
	 * Reject (never ignore): Mesa would hang on a fence we never
	 * waited for. WAIT_ALL vs ANY is honoured below. */
	if (args->flags & ~(P7580_SYNCOBJ_WAIT_ALL))
		return -EINVAL;
	/* Absolute -> relative: same shape as WAIT_BO (ktime_get_ts is
	 * the 3.10 spelling). Past stamp stays non-blocking. */
	ktime_get_ts(&now);
	remain_ns = (int64_t)args->timeout_nsec -
		    ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
	if (remain_ns <= 0)
		timeout = 0;
	else if ((uint64_t)remain_ns > (uint64_t)LONG_MAX)
		timeout = LONG_MAX;
	else
		timeout = nsecs_to_jiffies((uint64_t)remain_ns);

	/* Snapshot syncobj + fence refs under the manager, sleep after.
	 * Caps mirror SUBMIT (64 BOs / 16 in-syncs): 64 handles max. */
	if (count > 64)
		return -EINVAL;
	syncs = kcalloc(count, sizeof(*syncs), GFP_KERNEL);
	if (!syncs)
		return -ENOMEM;
	fences = kcalloc(count, sizeof(*fences), GFP_KERNEL);
	if (!fences) {
		kfree(syncs);
		return -ENOMEM;
	}
	for (i = 0; i < count; i++) {
		uint32_t h;

		if (copy_from_user(&h,
				   (void __user *)(uintptr_t)args->handles +
				   i * sizeof(h), sizeof(h))) {
			ret = -EFAULT;
			count = i;
			goto out_put_syncs;
		}
		syncs[i] = drm_syncobj_find(mgr, h);
		if (!syncs[i]) {
			ret = -ENOENT;
			count = i;
			goto out_put_syncs;
		}
		/* NULL fence == already signaled (0003 contract): no
		 * fence ref, counted as signaled below. */
		fences[i] = drm_syncobj_find_fence(syncs[i]);
	}
	if (args->flags & P7580_SYNCOBJ_WAIT_ALL) {
		for (i = 0; i < args->count_handles; i++) {
			long r;

			if (!fences[i])
				continue;
			r = dma_fence_wait_timeout(fences[i], true,
						   timeout);
			if (r < 0) {
				ret = (int)r;
				goto out_put_all;
			}
			/* r == 0 is ambiguous: non-blocking probe on a
			 * pending fence (timeout == 0) also yields 0
			 * (0001 maps helper-0 to 0 when no budget was
			 * given). Only a signaled fence counts: re-test
			 * and time out otherwise, or a WAIT_ALL poll on
			 * a pending fence would falsely report success. */
			if (!dma_fence_is_signaled(fences[i])) {
				ret = -ETIME;
				goto out_put_all;
			}
			/* Chained budget: each fence consumes from what
			 * the previous one left (0001 returns remaining;
			 * 0 with budget left means signaled now). */
			timeout = r;
		}
		args->first_signaled = 0;
	} else {
		/* ANY: poll without consuming the whole budget on the
		 * first unsignaled fence. Zero-timeout test first, then
		 * bounded wait on each in turn with the remaining
		 * budget split evenly (binary-subset approximation). */
		uint32_t n = args->count_handles;

		for (i = 0; i < n; i++) {
			if (!fences[i] ||
			    dma_fence_is_signaled(fences[i])) {
				args->first_signaled = i;
				goto out_put_all;
			}
		}
		if (timeout == 0) {
			ret = -ETIME;
			goto out_put_all;
		}
		for (i = 0; i < n; i++) {
			long r = dma_fence_wait_timeout(fences[i], true,
							timeout / (n - i));

			if (r < 0 && r != -ETIME) {
				ret = (int)r;
				goto out_put_all;
			}
			/* r >= 0: signaled with remaining budget (or a
			 * 0-budget hit exactly at signal). r == 0 with an
			 * unsignaled fence is the non-blocking probe
			 * outcome: fall through to the -ETIME below. */
			if (r >= 0) {
				if (!fences[i] ||
				    dma_fence_is_signaled(fences[i])) {
					args->first_signaled = i;
					ret = 0;
					goto out_put_all;
				}
			}
		}
		ret = -ETIME;
	}
out_put_all:
	count = args->count_handles;
out_put_syncs:
	for (i = 0; i < count; i++) {
		dma_fence_put(fences[i]);
		if (syncs[i])
			drm_syncobj_put(syncs[i]);
	}
	kfree(fences);
	kfree(syncs);
	return ret;
}

int panfrost_syncobj_reset(struct drm_device *dev, void *data,
			   struct drm_file *file_priv)
{
	struct p7580_syncobj_array *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);
	uint32_t i, count = args->count_handles;

	(void)dev;
	if (!mgr)
		return -ENODEV;
	if (!count || !args->handles)
		return -EINVAL;
	if (count > 64)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		uint32_t h;
		struct drm_syncobj *s;

		if (copy_from_user(&h,
				   (void __user *)(uintptr_t)args->handles +
				   i * sizeof(h), sizeof(h)))
			return -EFAULT;
		s = drm_syncobj_find(mgr, h);
		if (!s)
			return -ENOENT;
		/* Binary reset: drop the fence (NULL == signaled in the
		 * 0003 contract, but Mesa only resets after WAIT, so the
		 * next SUBMIT re-installs via replace_fence anyway). */
		drm_syncobj_replace_fence(s, NULL);
		drm_syncobj_put(s);
	}
	return 0;
}

int panfrost_syncobj_signal(struct drm_device *dev, void *data,
			    struct drm_file *file_priv)
{
	struct p7580_syncobj_array *args = data;
	struct drm_syncobj_manager *mgr = p7580_sync_mgr(file_priv);
	uint32_t i, count = args->count_handles;

	(void)dev;
	if (!mgr)
		return -ENODEV;
	if (!count || !args->handles)
		return -EINVAL;
	if (count > 64)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		uint32_t h;
		struct drm_syncobj *s;
		struct dma_fence *f;
		int ret;

		if (copy_from_user(&h,
				   (void __user *)(uintptr_t)args->handles +
				   i * sizeof(h), sizeof(h)))
			return -EFAULT;
		s = drm_syncobj_find(mgr, h);
		if (!s)
			return -ENOENT;
		/* Already-signaled (or empty) is a no-op 0. */
		f = drm_syncobj_find_fence(s);
		if (!f || dma_fence_is_signaled(f)) {
			dma_fence_put(f);
			drm_syncobj_put(s);
			continue;
		}
		dma_fence_put(f);
		f = p7580_sync_signaled_fence();
		if (!f) {
			drm_syncobj_put(s);
			return -ENOMEM;
		}
		ret = drm_syncobj_replace_fence(s, f);
		dma_fence_put(f);
		drm_syncobj_put(s);
		if (ret)
			return ret;
	}
	return 0;
}

/* ---- fops dispatcher (main.c wires .unlocked_ioctl here) ---- */

static long p7580_syncobj_dispatch(struct drm_device *dev, void *kdata,
				   struct drm_file *file_priv,
				   unsigned int nr)
{
	switch (nr) {
	case P7580_SYNCOBJ_NR_CREATE:
		return panfrost_syncobj_create(dev, kdata, file_priv);
	case P7580_SYNCOBJ_NR_DESTROY:
		return panfrost_syncobj_destroy(dev, kdata, file_priv);
	case P7580_SYNCOBJ_NR_HANDLE_TO_FD:
		return panfrost_syncobj_handle_to_fd(dev, kdata, file_priv);
	case P7580_SYNCOBJ_NR_FD_TO_HANDLE:
		return panfrost_syncobj_fd_to_handle(dev, kdata, file_priv);
	case P7580_SYNCOBJ_NR_WAIT:
		return panfrost_syncobj_wait(dev, kdata, file_priv);
	case P7580_SYNCOBJ_NR_RESET:
		return panfrost_syncobj_reset(dev, kdata, file_priv);
	case P7580_SYNCOBJ_NR_SIGNAL:
		return panfrost_syncobj_signal(dev, kdata, file_priv);
	case P7580_DEBUG_READ_BO_NR:
		/* DEBUG-ONLY (gpu31-gpuwrite): kernel BO word readback.
		 * Not a syncobj number and not a driver-table number:
		 * handled here at the fops layer only. */
		return panfrost_debug_read_bo(dev, kdata, file_priv);
	default:
		return -ENOENT;	/* not a syncobj number: caller forwards */
	}
}

/* Same copy discipline as drm_ioctl (drm_drv.c:444-490): fixed-size
 * struct is kernel-copied; only the pointed-to handle arrays are
 * copied in the handlers. asize>usize zero-pads (older libdrm with a
 * shorter WAIT struct keeps working); usize>asize truncates (newer
 * libdrm with a LONGER struct: 24 B HANDLE with point tail, 40 B WAIT
 * with deadline_nsec -- the extra tail bytes are NOT copied in, so
 * the handler never sees them, and the copy_to_user writes back only
 * usize... see below). Copy-back is capped at asize (never usize):
 * with a 24 B userspace HANDLE, blindly writing 24 B back would leak
 * 8 B of stack past our 16 B struct into userspace. */
static long p7580_syncobj_do(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev;
	unsigned int nr = _IOC_NR(cmd);
	unsigned int usize = _IOC_SIZE(cmd);
	char stack_kdata[128];
	char *kdata = stack_kdata;
	unsigned int asize;
	long ret;

	if (!file_priv)
		return -EINVAL;
	dev = file_priv->minor->dev;
	if (!dev)
		return -ENODEV;
	if (drm_device_is_unplugged(dev))
		return -ENODEV;
	switch (nr) {
	case P7580_SYNCOBJ_NR_CREATE:
		asize = sizeof(struct p7580_syncobj_create);
		break;
	case P7580_SYNCOBJ_NR_DESTROY:
		asize = sizeof(struct p7580_syncobj_destroy);
		break;
	case P7580_SYNCOBJ_NR_HANDLE_TO_FD:
	case P7580_SYNCOBJ_NR_FD_TO_HANDLE:
		asize = sizeof(struct p7580_syncobj_handle);
		break;
	case P7580_SYNCOBJ_NR_WAIT:
		asize = sizeof(struct p7580_syncobj_wait);
		break;
	case P7580_SYNCOBJ_NR_RESET:
	case P7580_SYNCOBJ_NR_SIGNAL:
		asize = sizeof(struct p7580_syncobj_array);
		break;
	case P7580_DEBUG_READ_BO_NR:
		asize = sizeof(struct p7580_debug_read_bo);
		break;
	default:
		return drm_ioctl(filp, cmd, arg);
	}
	if (asize > sizeof(stack_kdata)) {
		kdata = kmalloc(asize, GFP_KERNEL);
		if (!kdata)
			return -ENOMEM;
	}
	if (asize > usize)
		memset(kdata + usize, 0, asize - usize);
	if (cmd & IOC_IN) {
		/* Truncate: copy at most asize (our struct); a longer
		 * userspace struct (24 B HANDLE, 40 B WAIT) leaves its
		 * tail in userspace, untouched. */
		unsigned int cpin = usize < asize ? usize : asize;

		if (copy_from_user(kdata, (void __user __force *)arg,
				   cpin) != 0) {
			ret = -EFAULT;
			goto out;
		}
		if (cpin < asize)
			memset(kdata + cpin, 0, asize - cpin);
	} else {
		memset(kdata, 0, asize);
	}
	ret = p7580_syncobj_dispatch(dev, kdata, file_priv, nr);
	if (ret == -ENOENT)
		ret = drm_ioctl(filp, cmd, arg);
	else if ((cmd & IOC_OUT) && ret == 0) {
		/* Cap the write-back at asize: never leak stack past our
		 * struct into a longer userspace struct's tail. */
		unsigned int cpout = usize < asize ? usize : asize;

		if (copy_to_user((void __user __force *)arg, kdata,
				 cpout) != 0)
			ret = -EFAULT;
	}
out:
	if (kdata != stack_kdata)
		kfree(kdata);
	return ret;
}

long panfrost_syncobj_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	unsigned int nr = _IOC_NR(cmd);

	/* Fast path: driver-private numbers never reach us with extra
	 * work -- straight to the core. */
	if ((nr >= DRM_COMMAND_BASE) && (nr < DRM_COMMAND_END))
		return drm_ioctl(filp, cmd, arg);
	return p7580_syncobj_do(filp, cmd, arg);
}

#ifdef CONFIG_COMPAT
long panfrost_syncobj_compat_ioctl(struct file *filp, unsigned int cmd,
				   unsigned long arg)
{
	/* Payload structs are width-clean (no pointers, only u32/__u64/
	 * __s32); compat32 handling is identical. */
	return panfrost_syncobj_ioctl(filp, cmd, arg);
}
#endif
