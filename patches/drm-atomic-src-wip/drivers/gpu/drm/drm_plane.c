/*
 * Copyright (c) 2006-2008 Intel Corporation
 * Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
 * Copyright (c) 2008 Red Hat Inc.
 *
 * DRM core plane functions.  Split out of drm_crtc.c (upstream did the same
 * split in 4.9) so that the atomic backport does not have to grow that file
 * further.  Function bodies are the 4.0/4.9 versions adjusted for 3.10:
 *  - drm_plane_init() keeps the 3.10 "bool priv" argument and private-plane
 *    behaviour, drivers in this tree (exynos) call it with both values.
 *  - the "universal planes" userspace capability does not exist here; all
 *    non-private planes are listed as before.
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

#include <drm/drmP.h>
#include <drm/drm_crtc.h>

#include "drm_crtc_internal.h"

static int drm_plane_init_common(struct drm_device *dev,
				 struct drm_plane *plane,
				 unsigned long possible_crtcs,
				 const struct drm_plane_funcs *funcs,
				 const uint32_t *formats,
				 uint32_t format_count,
				 enum drm_plane_type type,
				 bool priv)
{
	struct drm_mode_config *config = &dev->mode_config;
	int ret;

	ret = drm_mode_object_get(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
	if (ret)
		return ret;

	drm_modeset_lock_init(&plane->modeset_lock);
	INIT_LIST_HEAD(&plane->head);

	plane->base.properties = &plane->properties;
	plane->dev = dev;
	plane->funcs = funcs;
	plane->type = type;
	plane->format_types = kmalloc(sizeof(uint32_t) * format_count,
				      GFP_KERNEL);
	if (!plane->format_types) {
		DRM_DEBUG_KMS("out of memory when allocating plane\n");
		drm_mode_object_put(dev, &plane->base);
		return -ENOMEM;
	}

	memcpy(plane->format_types, formats, format_count * sizeof(uint32_t));
	plane->format_count = format_count;
	plane->possible_crtcs = possible_crtcs;

	/*
	 * Private planes are not exposed to userspace but are still fully
	 * usable internally by the driver.
	 */
	if (priv)
		return 0;

	list_add_tail(&plane->head, &config->plane_list);
	config->num_total_plane++;
	if (plane->type == DRM_PLANE_TYPE_OVERLAY)
		config->num_overlay_plane++;

	if (config->plane_type_property)
		drm_object_attach_property(&plane->base,
					   config->plane_type_property,
					   plane->type);

	if (drm_core_check_feature(dev, DRIVER_ATOMIC)) {
		drm_object_attach_property(&plane->base, config->prop_fb_id, 0);
		drm_object_attach_property(&plane->base, config->prop_crtc_id, 0);
		drm_object_attach_property(&plane->base, config->prop_crtc_x, 0);
		drm_object_attach_property(&plane->base, config->prop_crtc_y, 0);
		drm_object_attach_property(&plane->base, config->prop_crtc_w, 0);
		drm_object_attach_property(&plane->base, config->prop_crtc_h, 0);
		drm_object_attach_property(&plane->base, config->prop_src_x, 0);
		drm_object_attach_property(&plane->base, config->prop_src_y, 0);
		drm_object_attach_property(&plane->base, config->prop_src_w, 0);
		drm_object_attach_property(&plane->base, config->prop_src_h, 0);
	}

	return 0;
}

/**
 * drm_universal_plane_init - Initialize a new universal plane object
 * @dev: DRM device
 * @plane: plane object to init
 * @possible_crtcs: bitmask of possible CRTCs
 * @funcs: callbacks for the new plane
 * @formats: array of supported formats (%DRM_FORMAT_*)
 * @format_count: number of elements in @formats
 * @type: type of plane (overlay, primary, cursor)
 *
 * Initializes a plane object of type @type.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drm_universal_plane_init(struct drm_device *dev, struct drm_plane *plane,
			     unsigned long possible_crtcs,
			     const struct drm_plane_funcs *funcs,
			     const uint32_t *formats, uint32_t format_count,
			     enum drm_plane_type type)
{
	int ret;

	if (WARN_ON(!funcs->destroy))
		return -EINVAL;

	drm_modeset_lock_all(dev);
	ret = drm_plane_init_common(dev, plane, possible_crtcs, funcs,
				    formats, format_count, type, false);
	drm_modeset_unlock_all(dev);

	return ret;
}
EXPORT_SYMBOL(drm_universal_plane_init);

/**
 * drm_plane_init - Initialize a legacy plane
 * @dev: DRM device
 * @plane: plane object to init
 * @possible_crtcs: bitmask of possible CRTCs
 * @funcs: callbacks for the new plane
 * @formats: array of supported formats (%DRM_FORMAT_*)
 * @format_count: number of elements in @formats
 * @priv: private plane (not exposed to userspace)
 *
 * Legacy 3.10 API.  New drivers should call drm_universal_plane_init()
 * instead.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drm_plane_init(struct drm_device *dev, struct drm_plane *plane,
		   unsigned long possible_crtcs,
		   const struct drm_plane_funcs *funcs,
		   const uint32_t *formats, uint32_t format_count,
		   bool priv)
{
	int ret;

	drm_modeset_lock_all(dev);
	ret = drm_plane_init_common(dev, plane, possible_crtcs, funcs,
				    formats, format_count,
				    DRM_PLANE_TYPE_OVERLAY, priv);
	drm_modeset_unlock_all(dev);

	return ret;
}
EXPORT_SYMBOL(drm_plane_init);

/**
 * drm_plane_cleanup - Clean up the core plane usage
 * @plane: plane to cleanup
 *
 * This function cleans up @plane and removes it from the DRM mode setting
 * core. Note that the function does *not* free the plane structure itself,
 * this is the responsibility of the caller.
 */
void drm_plane_cleanup(struct drm_plane *plane)
{
	struct drm_device *dev = plane->dev;

	drm_modeset_lock_all(dev);
	kfree(plane->format_types);
	drm_mode_object_put(dev, &plane->base);

	/* if not added to the list, it must be a private plane */
	if (!list_empty(&plane->head)) {
		list_del(&plane->head);
		dev->mode_config.num_total_plane--;
		if (plane->type == DRM_PLANE_TYPE_OVERLAY)
			dev->mode_config.num_overlay_plane--;
	}
	drm_modeset_unlock_all(dev);

	WARN_ON(plane->state && !plane->funcs->atomic_destroy_state);
	if (plane->state && plane->funcs->atomic_destroy_state)
		plane->funcs->atomic_destroy_state(plane, plane->state);

	memset(plane, 0, sizeof(*plane));
}
EXPORT_SYMBOL(drm_plane_cleanup);

/**
 * drm_plane_index - find the index of a registered plane
 * @plane: plane to find index for
 *
 * Given a registered plane, return the index of that plane within a DRM
 * device's list of planes.
 */
unsigned int drm_plane_index(struct drm_plane *plane)
{
	unsigned int index = 0;
	struct drm_plane *tmp;

	list_for_each_entry(tmp, &plane->dev->mode_config.plane_list, head) {
		if (tmp == plane)
			return index;

		index++;
	}

	BUG();
}
EXPORT_SYMBOL(drm_plane_index);
