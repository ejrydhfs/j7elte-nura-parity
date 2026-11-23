/*
 * Internal interfaces shared between the DRM core files (drm_crtc.c,
 * drm_plane.c, drm_framebuffer.c).  Upstream introduced drm_crtc_internal.h
 * with the drm_crtc.c split; this is the 3.10 subset needed by the atomic
 * backport.
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

#ifndef __DRM_CRTC_INTERNAL_H__
#define __DRM_CRTC_INTERNAL_H__

int drm_mode_object_get(struct drm_device *dev,
			struct drm_mode_object *obj, uint32_t obj_type);
void drm_mode_object_put(struct drm_device *dev,
			 struct drm_mode_object *object);

#endif /* __DRM_CRTC_INTERNAL_H__ */
