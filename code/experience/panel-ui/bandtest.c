#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    drmModeRes *res = drmModeGetResources(fd);
    drmModeConnector *c = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *t = drmModeGetConnector(fd, res->connectors[i]);
        if (t->connection == DRM_MODE_CONNECTED && t->count_modes && !c) c = t; else drmModeFreeConnector(t);
    }
    if (!c) { printf("no connector\n"); return 1; }
    drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];
    drmModeModeInfo mode = c->modes[0];
    struct drm_mode_create_dumb dumb = {0};
    dumb.width = mode.hdisplay; dumb.height = mode.vdisplay; dumb.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) { perror("dumb"); return 1; }
    uint32_t fb;
    if (drmModeAddFB(fd, dumb.width, dumb.height, 24, 32, dumb.pitch, dumb.handle, &fb)) { perror("addfb"); return 1; }
    struct drm_mode_map_dumb md = { .handle = dumb.handle };
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    uint8_t *map = mmap(0, dumb.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);
    /* three vertical bands: red | green | blue (DRM XRGB8888: bytes B,G,R,X) */
    for (unsigned y = 0; y < dumb.height; y++) {
        uint32_t *row = (uint32_t *)(map + y * dumb.pitch);
        for (unsigned x = 0; x < dumb.width; x++) {
            unsigned third = dumb.width / 3;
            uint32_t v;
            if (x < third)            v = 0x00FF0000; /* red  */
            else if (x < 2 * third)   v = 0x0000FF00; /* green */
            else                      v = 0x000000FF; /* blue */
            row[x] = v;
        }
    }
    if (drmModeSetCrtc(fd, crtc_id, fb, 0, 0, &c->connector_id, 1, &mode)) { perror("setcrtc"); return 1; }
    printf("bands set, holding\n");
    fflush(stdout);
    sleep(120);
    return 0;
}
