/* Host-only stand-in for 3.10 include/drm/drm_gem_cma_helper.h.
 * NEVER in a kernel build. */
#ifndef _ABIFUZZ_DRM_GEM_CMA_HELPER_H_
#define _ABIFUZZ_DRM_GEM_CMA_HELPER_H_

int drm_gem_cma_mmap(void);

#endif /* _ABIFUZZ_DRM_GEM_CMA_HELPER_H_ */
