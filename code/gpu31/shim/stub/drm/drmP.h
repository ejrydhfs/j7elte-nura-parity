/* Kbuild-independent syntax check for the H2 shim C file.
 *
 * The shim patch (0001-drm-kbase-shim-render-node-over-kbase.patch) is
 * reviewed by inspection against the 3.10.108 tree excerpts quoted in
 * docs/reports/gpu31-shim.md section 3; only the Integrator compiles it
 * in-tree.  This stub lets the host compiler parse the SAME source file
 * for typos, brace balance, and obvious type errors without kbuild.
 *
 * What the stubs cover (values/numbers verified against the tree):
 *  - linux: module/platform/slab/uaccess minimal set
 *  - drm/drmP.h: drm_device/minor/file/ioctl_desc/driver fops subset,
 *    DRM_MINOR_RENDER=3, DRM_COMMAND_BASE=0x40, DRIVER_GEM=0x1000,
 *    DRM_UNLOCKED, DRM_IOCTL_NR/DRM_IOCTL_VERSION/DRM_IOCTL_DEF,
 *    core prototypes used by the shim
 *  - kbase externs: declared in the shim C file itself (not here), so a
 *    signature drift fails at Integrator build time, loudly.
 *
 * NOT a substitute for the in-tree build: 3.10 types (u64 ioctl args,
 * struct file/inode layouts, platform_driver shape) differ from the host
 * kernel headers, so PASS here means "parses + type-consistent against
 * the verified shapes", and the Integrator's `git apply --check` + kbuild
 * is the authoritative gate.
 */
#ifndef SHIMCHECK_DRM_DRMP_H
#define SHIMCHECK_DRM_DRMP_H

#include <linux/types.h>
#include <stddef.h>

#define DRM_MINOR_RENDER 3
#define DRM_COMMAND_BASE 0x40
#define DRIVER_GEM 0x1000
#define DRM_UNLOCKED 0x10

#define DRM_IOCTL_VERSION 0x00
#define DRM_IOCTL_NR(n) ((n) & 0xff)
#define DRM_IOCTL_DEF(ioctl, func, flags) \
	{ (unsigned)(ioctl), (flags), (int (*)(void))(func), 0, #ioctl }
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct drm_device;
struct drm_file;
struct drm_minor {
	int type;
	int index;
};
struct drm_file {
	void *driver_priv;
	struct file *filp;
	struct drm_minor *minor;
};
struct drm_ioctl_desc {
	unsigned int cmd;
	int flags;
	int (*func)(void);
	unsigned int cmd_drv;
	const char *name;
};
struct drm_driver {
	int driver_features;
	int (*open)(struct drm_device *, struct drm_file *);
	void (*preclose)(struct drm_device *, struct drm_file *);
	void (*postclose)(struct drm_device *, struct drm_file *);
	int (*dma_ioctl)(struct drm_device *, void *, struct drm_file *);
	struct drm_ioctl_desc *ioctls;
	int num_ioctls;
	struct file_operations *fops;
	const char *name;
	const char *desc;
	const char *date;
	int major;
	int minor;
	int patchlevel;
};
struct drm_device {
	struct platform_device *platformdev;
	struct device *dev;
	struct drm_minor *render;
	struct drm_driver *driver;
};
struct inode {
	unsigned long i_ino;
};
struct file {
	void *private_data;
	const struct file_operations *f_op;
};
struct file_operations {
	void *owner;
	void *open;
	void *read;
	void *poll;
	void *mmap;
	void *unlocked_ioctl;
	void *compat_ioctl;
	void *release;
};
struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned long vm_pgoff;
};
struct poll_table_struct;
typedef struct poll_table_struct poll_table;
struct device {
	void *driver_data;
};
struct platform_device {
	struct device dev;
};
#define __user
#define _IOC_NR(n) ((n) & 0xff)
#define POLLERR 0x08
struct platform_driver {
	int (*probe)(struct platform_device *);
	int (*remove)(struct platform_device *);
	struct {
		const char *name;
		void *owner;
	} driver;
};

struct kbase_device;
struct kbase_context;

int drm_get_minor(struct drm_device *dev, struct drm_minor **minor, int type);
int drm_put_minor(struct drm_minor **minor_p);
int drm_fill_in_dev(struct drm_device *dev, void *ent, void *driver);
int drm_put_dev(struct drm_device *dev);
int drm_open(struct inode *inode, struct file *filp);
long drm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
int drm_release(struct inode *inode, struct file *filp);
int drm_version(void);
int is_compat_task(void);
void *platform_get_drvdata(struct platform_device *pdev);
void platform_set_drvdata(struct platform_device *pdev, void *data);
struct platform_device *platform_device_register_simple(const char *name,
							int id, void *res,
							unsigned int n);
void platform_device_unregister(struct platform_device *pdev);
int platform_driver_probe(struct platform_driver *drv,
			  int (*probe)(struct platform_device *));
void platform_driver_unregister(struct platform_driver *drv);

#endif /* SHIMCHECK_DRM_DRMP_H */
