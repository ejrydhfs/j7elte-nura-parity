/*
 * kms-atomic-probe - check that a DRM card provides the API KWin 6 requires.
 *
 * KWin 6 refuses to start unless the card supports atomic modesetting and
 * exposes universal planes.  This helper is the session gate: it opens the
 * card, requests DRM_CLIENT_CAP_ATOMIC + DRM_CLIENT_CAP_UNIVERSAL_PLANES and
 * reports the resource counts.
 *
 * Build on the phone (libdrm-dev + gcc are installed in the current image):
 *
 *   cc -O2 -Wall -Wextra -o /usr/local/sbin/kms-atomic-probe \
 *       kms-atomic-probe.c $(pkg-config --cflags --libs libdrm 2>/dev/null \
 *                            || echo -I/usr/include/libdrm -ldrm)
 *
 * Usage: kms-atomic-probe [/dev/dri/card0]
 *
 * Exit status:
 *   0  atomic supported, >=1 CRTC, connector and plane
 *   1  card opened but is not usable for atomic KMS
 *   2  card could not be opened
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/dri/card0";
    drmModeRes *res = NULL;
    drmModePlaneRes *planes = NULL;
    drmVersionPtr ver = NULL;
    int fd;
    int ret = 1;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "kms-atomic-probe: %s: %s\n", path, strerror(errno));
        return 2;
    }

    ver = drmGetVersion(fd);
    if (ver) {
        fprintf(stderr, "kms-atomic-probe: %s: driver %s %d.%d.%d (%s)\n",
                path, ver->name ? ver->name : "?",
                ver->version_major, ver->version_minor,
                ver->version_patchlevel,
                ver->date ? ver->date : "?");
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "kms-atomic-probe: %s: atomic=0 (%s)\n",
                path, strerror(errno));
        goto out;
    }

    /* Universal planes are required for the primary/overlay model KWin uses. */
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0)
        fprintf(stderr, "kms-atomic-probe: warning: universal planes: %s\n",
                strerror(errno));

    res = drmModeGetResources(fd);
    planes = drmModeGetPlaneResources(fd);

    printf("%s atomic=1 crtcs=%d connectors=%d planes=%d\n",
           path,
           res ? res->count_crtcs : 0,
           res ? res->count_connectors : 0,
           planes ? (int)planes->count_planes : 0);

    if (res && planes && res->count_crtcs > 0 && res->count_connectors > 0 &&
        planes->count_planes > 0)
        ret = 0;

out:
    if (res)
        drmModeFreeResources(res);
    if (planes)
        drmModeFreePlaneResources(planes);
    if (ver)
        drmFreeVersion(ver);
    close(fd);
    return ret;
}
