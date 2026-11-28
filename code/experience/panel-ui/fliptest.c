#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/mman.h>

static int flip_done;
static void handler(int fd, unsigned int seq, unsigned int tv_sec, unsigned int tv_usec, void *data)
{ flip_done = 1; printf("PAGE_FLIP event seq=%u\n", seq); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    printf("opened fd=%d\n", fd);
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("res"); return 1; }
    printf("crtcs=%d connectors=%d\n", res->count_crtcs, res->count_connectors);
    drmModeConnector *c = NULL; drmModeEncoder *enc = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *t = drmModeGetConnector(fd, res->connectors[i]);
        printf("conn %u status=%d modes=%d\n", t->connector_id, t->connection, t->count_modes);
        if (t->connection == DRM_MODE_CONNECTED && t->count_modes && !c) c = t; else drmModeFreeConnector(t);
    }
    printf("connectors scanned\n");
    if (!c) { printf("no connected connector\n"); return 1; }
    enc = drmModeGetEncoder(fd, c->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : res->crtcs[0];
    drmModeModeInfo mode = c->modes[0];
    printf("using crtc=%u mode=%dx%d\n", crtc_id, mode.hdisplay, mode.vdisplay);
    struct drm_mode_create_dumb dumb = {0};
    dumb.width = mode.hdisplay; dumb.height = mode.vdisplay; dumb.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb)) { perror("dumb"); return 1; }
    uint32_t fb;
    if (drmModeAddFB(fd, dumb.width, dumb.height, 24, 32, dumb.pitch, dumb.handle, &fb)) { perror("addfb"); return 1; }
    uint32_t *map; struct drm_mode_map_dumb md = { .handle = dumb.handle };
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    map = mmap(0, dumb.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);
    for (unsigned i = 0; i < dumb.size/4; i++) map[i] = 0xff00ff00;
    if (drmModeSetCrtc(fd, crtc_id, fb, 0, 0, &c->connector_id, 1, &mode)) { perror("setcrtc"); return 1; }
    printf("setcrtc ok\n");
    drmEventContext ev = { .version = 2, .page_flip_handler = handler };
    for (int i = 0; i < 3; i++) {
        if (drmModePageFlip(fd, crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL)) { perror("pageflip"); return 1; }
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, 3000);
        printf("poll=%d revents=%x\n", r, p.revents);
        if (r > 0) drmHandleEvent(fd, &ev);
        printf("flip_done=%d\n", flip_done);
    }
    return 0;
}
