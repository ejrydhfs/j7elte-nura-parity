#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int do_addfb(int fd, uint32_t w, uint32_t h, uint32_t fmt,
                    const uint32_t hd[4], const uint32_t pt[4],
                    const uint32_t of[4], uint32_t *id, const char *tag)
{
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.width = w; f.height = h; f.pixel_format = fmt; f.flags = 0;
    for (int i = 0; i < 4; i++) {
        f.handles[i] = hd ? hd[i] : 0;
        f.pitches[i] = pt ? pt[i] : 0;
        f.offsets[i] = of ? of[i] : 0;
    }
    int r = drmIoctl(fd, DRM_IOCTL_MODE_ADDFB2, &f);
    fprintf(stderr, "[shim] %s %ux%u fmt=%08x h=%u p=%u o=%u rc=%d errno=%d(%s) fb=%u\n",
            tag, w, h, fmt, hd ? hd[0] : 0, pt ? pt[0] : 0, of ? of[0] : 0,
            r, errno, strerror(errno), f.fb_id);
    fflush(stderr);
    if (r) return -1;
    if (id) *id = f.fb_id;
    return 0;
}

int drmModeAddFB2(int fd, uint32_t w, uint32_t h, uint32_t fmt,
                  const uint32_t hd[4], const uint32_t pt[4], const uint32_t of[4],
                  uint32_t *id, uint32_t flags)
{
    (void)flags;
    return do_addfb(fd, w, h, fmt, hd, pt, of, id, "AddFB2");
}

int drmModeAddFB2WithModifiers(int fd, uint32_t w, uint32_t h, uint32_t fmt,
                               const uint32_t hd[4], const uint32_t pt[4],
                               const uint32_t of[4], const uint64_t mod[4],
                               uint32_t *id, uint32_t flags)
{
    (void)mod; (void)flags;
    return do_addfb(fd, w, h, fmt, hd, pt, of, id, "AddFB2Mod");
}
