/*
 * drm_compat_310_test.c — compile-only consumer proving both drmcore
 * headers parse against the real 3.10.108 tree headers (host-side
 * -fsyntax-only + -c checks; never part of the kernel build).
 */
#include "drm_compat_310.h"
#include "panfrost_ioctls_310.h"

/* Handlers live in the panfrost driver (G31_PanfrostTarget/G31_Gem); declare
 * the 3.10 drm_ioctl_t-shaped prototypes here so the table rows typecheck. */
static int panfrost_ioctl_submit(struct drm_device *dev, void *data,
				 struct drm_file *file_priv);
static int panfrost_ioctl_wait_bo(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);
static int panfrost_ioctl_create_bo(struct drm_device *dev, void *data,
				    struct drm_file *file_priv);
static int panfrost_ioctl_mmap_bo(struct drm_device *dev, void *data,
				  struct drm_file *file_priv);
static int panfrost_ioctl_get_param(struct drm_device *dev, void *data,
				    struct drm_file *file_priv);
static int panfrost_ioctl_get_bo_offset(struct drm_device *dev, void *data,
					 struct drm_file *file_priv);

static int panfrost_ioctl_submit(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	(void)dev; (void)data; (void)file_priv;
	return 0;
}

static int panfrost_ioctl_wait_bo(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	(void)dev; (void)data; (void)file_priv;
	return 0;
}

static int panfrost_ioctl_create_bo(struct drm_device *dev, void *data,
				    struct drm_file *file_priv)
{
	(void)dev; (void)data; (void)file_priv;
	return 0;
}

static int panfrost_ioctl_mmap_bo(struct drm_device *dev, void *data,
				  struct drm_file *file_priv)
{
	(void)dev; (void)data; (void)file_priv;
	return 0;
}

static int panfrost_ioctl_get_param(struct drm_device *dev, void *data,
				    struct drm_file *file_priv)
{
	(void)dev; (void)data; (void)file_priv;
	return 0;
}

static int panfrost_ioctl_get_bo_offset(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	(void)dev; (void)data; (void)file_priv;
	return 0;
}

static struct drm_ioctl_desc panfrost_test_ioctls[] = {
	PANFROST_IOCTLS_310
};

/* CMA-backed GEM fops, same wiring the panfrost driver will use. */
DEFINE_DRM_GEM_FOPS(panfrost_test_fops, drm_gem_cma_mmap);

/* Referenced from panfrost_test_ref() below so -c keeps it (avoids
 * -Wunused-function while still proving the APIs resolve). */
static void panfrost_test_touch(struct drm_device *dev,
				struct platform_device *pdev)
{
	unsigned int before = ARRAY_SIZE(panfrost_test_ioctls);
	void __iomem *reg = (void __iomem *)0x11400000UL;
	u32 val = 0;

	(void)before;
	(void)panfrost_test_fops.owner;

	/* Poll shape used for GPU_ID / JS_PRESENT / PWR ready polling. */
	(void)readl_poll_timeout(reg, val, (val != 0), 10, 100000);

	/* Registration shape (exynos_drm_platform_probe pattern). */
	dev->driver->num_ioctls = (int)before;
	(void)drm_platform_init;
	(void)drm_platform_exit;
	(void)pdev;
}
void (*panfrost_test_ref)(struct drm_device *,
			  struct platform_device *) = &panfrost_test_touch;
