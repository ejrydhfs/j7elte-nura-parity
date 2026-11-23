/*
 * Copyright (c) 2006-2008 Intel Corporation
 * Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2008 Red Hat Inc.
 *
 * DRM core framebuffer object functions.  Moved verbatim out of drm_crtc.c
 * (upstream split drm_crtc.c into drm_plane.c/drm_framebuffer.c in 4.9) so
 * the atomic backport can live in new files.  No behaviour change.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>

#include "drm_crtc_internal.h"

/**
 * drm_framebuffer_init - initialize a framebuffer
 * @dev: DRM device
 * @fb: framebuffer to be initialized
 * @funcs: ... with these functions
 *
 * Allocates an ID for the framebuffer's parent mode object, sets its mode
 * functions & device file and adds it to the master fd list.
 *
 * IMPORTANT:
 * This functions publishes the fb and makes it available for concurrent access
 * by other users. Which means by this point the fb _must_ be fully set up -
 * since all the fb attributes are invariant over its lifetime, no further
 * locking but only correct reference counting is required.
 *
 * RETURNS:
 * Zero on success, error code on failure.
 */
int drm_framebuffer_init(struct drm_device *dev, struct drm_framebuffer *fb,
			 const struct drm_framebuffer_funcs *funcs)
{
	int ret;

	mutex_lock(&dev->mode_config.fb_lock);
	kref_init(&fb->refcount);
	INIT_LIST_HEAD(&fb->filp_head);
	fb->dev = dev;
	fb->funcs = funcs;

	ret = drm_mode_object_get(dev, &fb->base, DRM_MODE_OBJECT_FB);
	if (ret)
		goto out;

	/* Grab the idr reference. */
	drm_framebuffer_reference(fb);

	dev->mode_config.num_fb++;
	list_add(&fb->head, &dev->mode_config.fb_list);
out:
	mutex_unlock(&dev->mode_config.fb_lock);

	return 0;
}
EXPORT_SYMBOL(drm_framebuffer_init);

static void drm_framebuffer_free(struct kref *kref)
{
	struct drm_framebuffer *fb =
			container_of(kref, struct drm_framebuffer, refcount);
	fb->funcs->destroy(fb);
}

static struct drm_framebuffer *__drm_framebuffer_lookup(struct drm_device *dev,
							uint32_t id)
{
	struct drm_mode_object *obj = NULL;
	struct drm_framebuffer *fb;

	mutex_lock(&dev->mode_config.idr_mutex);
	obj = idr_find(&dev->mode_config.crtc_idr, id);
	if (!obj || (obj->type != DRM_MODE_OBJECT_FB) || (obj->id != id))
		fb = NULL;
	else
		fb = obj_to_fb(obj);
	mutex_unlock(&dev->mode_config.idr_mutex);

	return fb;
}

/**
 * drm_framebuffer_lookup - look up a drm framebuffer and grab a reference
 * @dev: drm device
 * @id: id of the fb object
 *
 * If successful, this grabs an additional reference to the framebuffer -
 * callers need to make sure to eventually unreference the returned framebuffer
 * again.
 */
struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev,
					       uint32_t id)
{
	struct drm_framebuffer *fb;

	mutex_lock(&dev->mode_config.fb_lock);
	fb = __drm_framebuffer_lookup(dev, id);
	if (fb)
		drm_framebuffer_reference(fb);
	mutex_unlock(&dev->mode_config.fb_lock);

	return fb;
}
EXPORT_SYMBOL(drm_framebuffer_lookup);

/**
 * drm_framebuffer_unreference - unref a framebuffer
 * @fb: framebuffer to unref
 *
 * This functions decrements the fb's refcount and frees it if it drops to zero.
 */
void drm_framebuffer_unreference(struct drm_framebuffer *fb)
{
	DRM_DEBUG("FB ID: %d\n", fb->base.id);
	kref_put(&fb->refcount, drm_framebuffer_free);
}
EXPORT_SYMBOL(drm_framebuffer_unreference);

/**
 * drm_framebuffer_reference - incr the fb refcnt
 * @fb: framebuffer
 */
void drm_framebuffer_reference(struct drm_framebuffer *fb)
{
	DRM_DEBUG("FB ID: %d\n", fb->base.id);
	kref_get(&fb->refcount);
}
EXPORT_SYMBOL(drm_framebuffer_reference);

static void drm_framebuffer_free_bug(struct kref *kref)
{
	BUG();
}

static void __drm_framebuffer_unreference(struct drm_framebuffer *fb)
{
	DRM_DEBUG("FB ID: %d\n", fb->base.id);
	kref_put(&fb->refcount, drm_framebuffer_free_bug);
}

/* dev->mode_config.fb_lock must be held! */
static void __drm_framebuffer_unregister(struct drm_device *dev,
					 struct drm_framebuffer *fb)
{
	mutex_lock(&dev->mode_config.idr_mutex);
	idr_remove(&dev->mode_config.crtc_idr, fb->base.id);
	mutex_unlock(&dev->mode_config.idr_mutex);

	fb->base.id = 0;

	__drm_framebuffer_unreference(fb);
}

/**
 * drm_framebuffer_unregister_private - unregister a private fb from the lookup idr
 * @fb: fb to unregister
 *
 * Drivers need to call this when cleaning up driver-private framebuffers, e.g.
 * those used for fbdev. Note that the caller must hold a reference of it's own,
 * i.e. the object may not be destroyed through this call (since it'll lead to a
 * locking inversion).
 */
void drm_framebuffer_unregister_private(struct drm_framebuffer *fb)
{
	struct drm_device *dev = fb->dev;

	mutex_lock(&dev->mode_config.fb_lock);
	/* Mark fb as reaped and drop idr ref. */
	__drm_framebuffer_unregister(dev, fb);
	mutex_unlock(&dev->mode_config.fb_lock);
}
EXPORT_SYMBOL(drm_framebuffer_unregister_private);

/**
 * drm_framebuffer_cleanup - remove a framebuffer object
 * @fb: framebuffer to remove
 *
 * Cleanup references to a user-created framebuffer. This function is intended
 * to be used from the drivers ->destroy callback.
 *
 * Note that this function does not remove the fb from active usuage - if it is
 * still used anywhere, hilarity can ensue since userspace could call getfb on
 * the id and get back -EINVAL. Obviously no concern at driver unload time.
 *
 * Also, the framebuffer will not be removed from the lookup idr - for
 * user-created framebuffers this will happen in in the rmfb ioctl. For
 * driver-private objects (e.g. for fbdev) drivers need to explicitly call
 * drm_framebuffer_unregister_private.
 */
void drm_framebuffer_cleanup(struct drm_framebuffer *fb)
{
	struct drm_device *dev = fb->dev;

	mutex_lock(&dev->mode_config.fb_lock);
	list_del(&fb->head);
	dev->mode_config.num_fb--;
	mutex_unlock(&dev->mode_config.fb_lock);
}
EXPORT_SYMBOL(drm_framebuffer_cleanup);

/**
 * drm_framebuffer_remove - remove and unreference a framebuffer object
 * @fb: framebuffer to remove
 *
 * Scans all the CRTCs and planes in @dev's mode_config.  If they're
 * using @fb, removes it, setting it to NULL. Then drops the reference to the
 * passed-in framebuffer. Might take the modeset locks.
 *
 * Note that this function optimizes the cleanup away if the caller holds the
 * last reference to the framebuffer. It is also guaranteed to not take the
 * modeset locks in this case.
 */
void drm_framebuffer_remove(struct drm_framebuffer *fb)
{
	struct drm_device *dev = fb->dev;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	struct drm_mode_set set;
	int ret;

	WARN_ON(!list_empty(&fb->filp_head));

	/*
	 * drm ABI mandates that we remove any deleted framebuffers from active
	 * useage. But since most sane clients only remove framebuffers they no
	 * longer need, try to optimize this away.
	 *
	 * Since we're holding a reference ourselves, observing a refcount of 1
	 * means that we're the last holder and can skip it. Also, the refcount
	 * can never increase from 1 again, so we don't need any barriers or
	 * locks.
	 *
	 * Note that userspace could try to race with use and instate a new
	 * usage _after_ we've cleared all current ones. End result will be an
	 * in-use fb with fb-id == 0. Userspace is allowed to shoot its own foot
	 * in this manner.
	 */
	if (atomic_read(&fb->refcount.refcount) > 1) {
		drm_modeset_lock_all(dev);
		/* remove from any CRTC */
		list_for_each_entry(crtc, &dev->mode_config.crtc_list, head) {
			if (crtc->fb == fb) {
				/* should turn off the crtc */
				memset(&set, 0, sizeof(struct drm_mode_set));
				set.crtc = crtc;
				set.fb = NULL;
				ret = drm_mode_set_config_internal(&set);
				if (ret)
					DRM_ERROR("failed to reset crtc %p when fb was deleted\n", crtc);
			}
		}

		list_for_each_entry(plane, &dev->mode_config.plane_list, head) {
			if (plane->fb == fb) {
				/* should turn off the crtc */
				ret = plane->funcs->disable_plane(plane);
				if (ret)
					DRM_ERROR("failed to disable plane with busy fb\n");
				/* disconnect the plane from the fb and crtc: */
				__drm_framebuffer_unreference(plane->fb);
				plane->fb = NULL;
				plane->crtc = NULL;
			}
		}
		drm_modeset_unlock_all(dev);
	}

	drm_framebuffer_unreference(fb);
}
EXPORT_SYMBOL(drm_framebuffer_remove);
