#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

int main(void){
    int fd = open("/dev/dri/card0", O_RDWR|O_CLOEXEC);
    if (fd < 0){ perror("open"); return 1; }
    struct drm_mode_create_dumb cd; memset(&cd,0,sizeof cd);
    cd.width=720; cd.height=1280; cd.bpp=32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)){ perror("create_dumb"); return 1; }
    printf("dumb handle=%u pitch=%u size=%llu\n", cd.handle, cd.pitch, (unsigned long long)cd.size);
    struct drm_mode_map_dumb md; memset(&md,0,sizeof md); md.handle=cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)){ perror("map_dumb"); return 1; }
    void *p = mmap(0, cd.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p==MAP_FAILED){ perror("mmap"); return 1; }
    memset(p, 0x40, cd.size);
    unsigned int fourccs[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    for (int i=0;i<2;i++){
        uint32_t h=0;
        uint32_t handles[4]={cd.handle,0,0,0}, pitches[4]={cd.pitch,0,0,0}, offsets[4]={0};
        int r = drmModeAddFB2(fd, cd.width, cd.height, fourccs[i], handles, pitches, offsets, &h, 0);
        printf("addfb2 %s -> r=%d errno=%d(%s) fb=%u\n",
               fourccs[i]==DRM_FORMAT_XRGB8888?"XRGB8888":"ARGB8888",
               r, errno, strerror(errno), h);
        if (!r) drmModeRmFB(fd, h);
    }
    drmModeAddFB2WithModifiers(fd, cd.width, cd.height, DRM_FORMAT_ARGB8888,
        (uint32_t[]){cd.handle,0,0,0}, (uint32_t[]){cd.pitch,0,0,0}, (uint32_t[]){0},
        (uint64_t[]){0}, &(uint32_t){0}, DRM_MODE_FB_MODIFIERS);
    printf("modifier-linear rc errno=%d(%s)\n", errno, strerror(errno));
    return 0;
}
