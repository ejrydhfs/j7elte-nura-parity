/* dumbprobe.c - test DRM_IOCTL_MODE_CREATE_DUMB on a KMS node. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
	struct { unsigned w, h; unsigned bpp; } cases[] = {
		{ 16, 16, 32 }, { 128, 128, 32 }, { 720, 1280, 32 }, { 720, 1280, 16 },
	};
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }
	printf("dumbprobe: node=%s fd=%d\n", node, fd);
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct drm_mode_create_dumb c = {
			.width = cases[i].w, .height = cases[i].h, .bpp = cases[i].bpp,
		};
		int ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c);
		printf("CREATE_DUMB %ux%u bpp=%u: ret=%d errno=%d (%s)",
		       cases[i].w, cases[i].h, cases[i].bpp, ret, ret ? errno : 0,
		       ret ? strerror(errno) : "ok");
		if (ret)
			printf("\n");
		else {
			struct drm_mode_map_dumb m = { .handle = c.handle };
			int mr = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m);
			printf(" handle=%u pitch=%u size=%u map: ret=%d errno=%d\n",
			       c.handle, c.pitch, c.size, mr, mr ? errno : 0);
			struct drm_mode_destroy_dumb d = { .handle = c.handle };
			drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
		}
	}
	{
		drmVersion *v = drmGetVersion(fd);
		if (v) printf("driver=%s %d.%d.%d\n", v->name, v->version_major,
			      v->version_minor, v->version_patchlevel);
	}
	close(fd);
	return 0;
}
