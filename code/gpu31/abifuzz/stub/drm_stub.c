/* Linkable stub definitions for the fops/compat smoke test.
 * Compiled into the smoke binary only. NEVER in a kernel build. */
#include <drm/drmP.h>

unsigned long jiffies = 0;

int drm_open(void) { return 0; }
int drm_release(void) { return 0; }
long drm_ioctl(void) { return 0; }
long drm_compat_ioctl(void) { return 0; }
int drm_gem_cma_mmap(void) { return 0; }
unsigned int drm_poll(void) { return 0; }
long drm_read(void) { return 0; }
int drm_platform_init(void) { return 0; }
int drm_platform_exit(void) { return 0; }
long noop_llseek(void) { return 0; }
