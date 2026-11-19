/* Host-only stand-in for 3.10 include/drm/drmP.h (subset used by the
 * drmcore compat headers). Encodes the REAL 3.10 gate values
 * (DRM_AUTH 0x1, DRM_UNLOCKED 0x10) and the legacy DRM_IOCTL_DEF_DRV
 * row shape so flag-contract checks are meaningful. The fops struct
 * carries the real 3.10 linux/fs.h entry used by both compat macros
 * (compat_ioctl), so the smoke test can verify which macro wires it.
 * NEVER in a kernel build. */
#ifndef _ABIFUZZ_DRM_DRM_P_H_
#define _ABIFUZZ_DRM_DRM_P_H_

#include <drm/drm.h>

struct drm_driver {
	int num_ioctls;
};

struct drm_device {
	struct drm_driver *driver;
};

struct drm_file {
	int dummy;
};

struct platform_device {
	int dummy;
};

#define THIS_MODULE	((void *)0)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#endif

/* 3.10 drivers/gpu/drm/drmP.h gate bits (no RENDER_ALLOW until 3.12). */
#define DRM_AUTH		0x1
#define DRM_MASTER		0x2
#define DRM_ROOT_ONLY		0x4
#define DRM_CONTROL_ALLOW	0x8
#define DRM_UNLOCKED		0x10

#ifndef DRM_RENDER_ALLOW
#define DRM_RENDER_ALLOW	DRM_UNLOCKED
#endif

struct drm_ioctl_desc {
	unsigned int cmd;
	int (*func)(struct drm_device *, void *, struct drm_file *);
	unsigned int flags;
	unsigned int cmd_drv;
};

/* Legacy 3.10 constructor shape: one row per driver ioctl. */
#define DRM_IOCTL_DEF_DRV(ioctl, _func, _flags) \
	{ .cmd = DRM_IOCTL_##ioctl, .func = _func, \
	  .flags = _flags, .cmd_drv = DRM_##ioctl }

struct file_operations {
	void *owner;
	int (*open)(void);
	int (*release)(void);
	long (*unlocked_ioctl)(void);
	long (*compat_ioctl)(void);
	int (*mmap)(void);
	unsigned int (*poll)(void);
	long (*read)(void);
	long (*llseek)(void);
};

int drm_open(void);
int drm_release(void);
long drm_ioctl(void);
long drm_compat_ioctl(void);
int drm_gem_cma_mmap(void);
unsigned int drm_poll(void);
long drm_read(void);
long noop_llseek(void);
int drm_platform_init(void);
int drm_platform_exit(void);

#endif /* _ABIFUZZ_DRM_DRM_P_H_ */
