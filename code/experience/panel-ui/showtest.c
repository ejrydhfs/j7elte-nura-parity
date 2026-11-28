#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR|O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("resources"); return 1; }
    printf("crtcs=%d connectors=%d\n", res->count_crtcs, res->count_connectors);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no connected connector\n"); return 1; }
    drmModeModeInfo mode = conn->modes[0];
    printf("connector %u mode %dx%d\n", conn->connector_id, mode.hdisplay, mode.vdisplay);

    struct drm_mode_create_dumb cd; memset(&cd,0,sizeof cd);
    cd.width = mode.hdisplay; cd.height = mode.vdisplay; cd.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 1; }
    struct drm_mode_map_dumb md; memset(&md,0,sizeof md); md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return 1; }
    unsigned int *p = mmap(0, cd.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    for (unsigned y = 0; y < cd.height; y++)
        for (unsigned x = 0; x < cd.width; x++)
            p[y*cd.width + x] = 0xffff00ff; /* magenta */

    uint32_t fb = 0;
    uint32_t handles[4] = { cd.handle,0,0,0 }, pitches[4] = { cd.pitch,0,0,0 }, offsets[4] = {0};
    if (drmModeAddFB2(fd, cd.width, cd.height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0)) {
        perror("addfb2"); return 1;
    }
    msync(p, cd.size, MS_SYNC);
    drmModeDirtyFB(fd, fb, NULL, 0);
    uint32_t crtc = res->crtcs[0];
    if (drmModeSetCrtc(fd, crtc, fb, 0, 0, &conn->connector_id, 1, &mode)) {
        perror("setcrtc");
    } else {
        printf("setcrtc ok fb=%u\n", fb);
    }
    sleep(6);
    return 0;
}
