import re
base = "/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch-k94/drivers/gpu/drm/exynos7580/"
# 1) add the sampler to dmabuf.c
p = base + "exynos_drm7580_dmabuf.c"
s = open(p).read()
if "exynos7580_dump_scanout" not in s:
    s = s.replace('#include "exynos_drm7580.h"',
'''#include "exynos_drm7580.h"
#include <linux/dma-buf.h>

unsigned int exynos7580_dump_fb;
module_param(exynos7580_dump_fb, uint, 0644);
MODULE_PARM_DESC(exynos7580_dump_fb,
	"1 = log three scanout pixels of the next committed frame, then clear");

/*
 * Sample the pixels the DECON is about to scan out.  The buffer is an
 * imported dma-buf (panfrost GBM), so map it through the exporter instead of
 * assuming any CPU address.
 */
void exynos7580_dump_scanout(struct drm_framebuffer *fb)
{
	struct drm_gem_object *obj;
	void *map;
	const u32 ys[3] = { 100, 640, 1200 };
	unsigned int pitch, i;

	if (!fb || !fb->pitches[0])
		return;
	obj = drm_fb_cma_get_gem_obj(fb, 0);
	if (!obj || !obj->import_attach || !obj->import_attach->dmabuf) {
		pr_info("scanout-dump: fb has no imported dmabuf\\n");
		return;
	}
	map = dma_buf_vmap(obj->import_attach->dmabuf);
	if (!map) {
		pr_info("scanout-dump: vmap failed\\n");
		return;
	}
	pitch = fb->pitches[0];
	for (i = 0; i < 3; i++) {
		unsigned int y = ys[i] < fb->height ? ys[i] : fb->height - 1;
		u32 px = ((const u32 *)((const u8 *)map + y * pitch))[360];
		pr_info("scanout-dump: %ux%u y=%u px=%08x\\n",
			fb->width, fb->height, y, px);
	}
	dma_buf_vunmap(obj->import_attach->dmabuf, map);
	exynos7580_dump_fb = 0;
}
EXPORT_SYMBOL_GPL(exynos7580_dump_scanout);''', 1)
    open(p, "w").write(s)
    print("dmabuf.c patched")
# 2) declare + call from plane commit (after decon_kms_commit succeeded)
h = base + "exynos_drm7580.h"
hs = open(h).read()
if "exynos7580_dump_fb" not in hs:
    hs = hs.rstrip() + "\n\nextern unsigned int exynos7580_dump_fb;\nvoid exynos7580_dump_scanout(struct drm_framebuffer *fb);\n"
    open(h, "w").write(hs)
    print("header patched")
pl = base + "exynos_drm7580_plane.c"
ps = open(pl).read()
if "exynos7580_dump_scanout" not in ps:
    anchor = "\tret = decon_kms_commit(ex->decon, wins, nplanes);"
    ps = ps.replace(anchor, "\tif (exynos7580_dump_fb && plane->state && plane->state->fb)\n\t\texynos7580_dump_scanout(plane->state->fb);\n\n" + anchor, 1)
    open(pl, "w").write(ps)
    print("plane.c patched")
