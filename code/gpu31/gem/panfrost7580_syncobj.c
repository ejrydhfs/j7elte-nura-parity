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
 *  - HANDLE_TO_FD/FD_TO_HANDLE with flags != 0 -> -EINVAL (only
 *    plain fence transport exists: no sync_file.c in 3.10).
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
 * fence backend contract (release via release_base). */
static const struct dma_fence_ops p7580_sync_signaled_ops = {
	.get_driver_name = NULL,
	.get_timeline_name = NULL,
	.enable_signaling = NULL,
	.signaled = NULL,
	.wait = NULL,
	.release = NULL,	/* filled at first use (no static fn) */
};

static void p7580_sync_signaled_release(struct dma_fence *fence)
{
	dma_fence_release_base(fence);
	kfree(fence);
}

static struct dma_fence *p7580_sync_signaled_fence(void)
{
	struct dma_fence *f;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	/* release hook is per-instance (static ops has no .release
	 * until set here; init BUGs on NULL release). */
	((struct dma_fence_ops *)&p7580_sync_signaled_ops)->release =
		p7580_sync_signaled_release;
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
 * flags must be 0: *_EXPORT_SYNC_FILE needs the sync_file layer. */

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
	if (!mgr)
		return -ENODEV;
	if (args->flags)
		return -EINVAL;
	if (args->pad)
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
	if (args->flags)
		return -EINVAL;
	if (args->pad)
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
			if (r >= 0 || !r) {
				/* signaled (r>=0 remaining) or 0-budget
				 * hit exactly at signal: re-test. */
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
	default:
		return -ENOENT;	/* not a syncobj number: caller forwards */
	}
}

/* Same copy discipline as drm_ioctl (drm_drv.c:444-490): fixed-size
 * struct is kernel-copied; only the pointed-to handle arrays are
 * copied in the handlers. asize>usize zero-pads (older libdrm with a
 * shorter WAIT struct keeps working). */
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
		if (copy_from_user(kdata, (void __user __force *)arg,
				   usize) != 0) {
			ret = -EFAULT;
			goto out;
		}
	} else {
		memset(kdata, 0, usize);
	}
	ret = p7580_syncobj_dispatch(dev, kdata, file_priv, nr);
	if (ret == -ENOENT)
		ret = drm_ioctl(filp, cmd, arg);
	else if ((cmd & IOC_OUT) && ret == 0) {
		if (copy_to_user((void __user __force *)arg, kdata,
				 usize) != 0)
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
