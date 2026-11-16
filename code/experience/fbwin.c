/* DECON window-config probe for Samsung j7elte (Exynos 7580).
 * Allocates an ION buffer, fills it, commits it as window 0 through
 * S3CFB_WIN_CONFIG and flips. Prints every failure with errno.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define ION_IOC_MAGIC 'I'
struct ion_allocation_data {
	size_t len, align;
	unsigned int heap_id_mask, flags;
	int handle;
};
struct ion_fd_data { int handle; int fd; };
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_MAP _IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)

typedef unsigned int u32;

struct decon_win_rect { int x, y; u32 w, h; };
struct decon_frame { int x, y; u32 w, h, f_w, f_h; };
struct vpp_params { unsigned long long addr[3]; int rot; int eq_mode; };

struct decon_win_config {
	int state;			/* 0 disabled, 2 buffer */
	union {
		u32 color;
		struct {
			int fd_idma[3];
			int fence_fd;
			int plane_alpha;
			int blending;
			int idma_type;
			int format;
			struct vpp_params vpp_parm;
			struct decon_win_rect block_area;
			struct decon_win_rect transparent_area;
			struct decon_win_rect opaque_area;
			struct decon_frame src;
		} b;
	} u;
	struct decon_frame dst;
	unsigned char protection;
};

struct decon_win_config_data {
	int fence;
	int fd_odma;
	struct decon_win_config config[8];	/* MAX_DECON_WIN + 1 */
};

#define S3CFB_WIN_CONFIG _IOW('F', 209, struct decon_win_config_data)

static int pan_only(void)
{
	int fbfd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo var;
	if (fbfd < 0) { perror("open fb"); return 1; }
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0) { perror("gvi"); return 1; }
	var.yoffset = 0; var.xoffset = 0; var.activate = FB_ACTIVATE_NOW;
	if (ioctl(fbfd, FBIOPAN_DISPLAY, &var) < 0) { perror("pan"); return 1; }
	printf("pan ok\n");
	return 0;
}

static int fb_fill(void)
{
	int fbfd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	void *m;
	if (fbfd < 0) { perror("open fb"); return 1; }
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0) { perror("gvi"); return 1; }
	if (ioctl(fbfd, FBIOGET_FSCREENINFO, &fix) < 0) { perror("gfi"); return 1; }
	printf("fb %ux%u %ubpp line %u smem %#lx len %u\n", var.xres, var.yres,
		var.bits_per_pixel, fix.line_length, (unsigned long)fix.smem_start, fix.smem_len);
	m = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (m == MAP_FAILED) { perror("mmap fb"); return 1; }
	memset(m, 0x00, fix.smem_len);
	/* paint the top half white, bottom half red-ish (ARGB) */
	{
		unsigned int *p = m;
		unsigned int n = var.xres * var.yres, i;
		for (i = 0; i < n; i++)
			p[i] = (i < n / 2) ? 0xffffffffu : 0xffff0000u;
	}
	msync(m, fix.smem_len, MS_SYNC);
	var.yoffset = 0; var.xoffset = 0; var.activate = FB_ACTIVATE_NOW;
	if (ioctl(fbfd, FBIOPAN_DISPLAY, &var) < 0) perror("pan");
	else printf("pan ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "panonly"))
		return pan_only();
	if (argc > 1 && !strcmp(argv[1], "pand")) {
		int f = open("/dev/fb0", O_RDWR);
		struct fb_var_screeninfo v;
		if (f < 0) { perror("open"); return 1; }
		if (ioctl(f, FBIOGET_VSCREENINFO, &v)) { perror("gvi"); return 1; }
		v.xoffset = 0; v.yoffset = 0; v.activate = FB_ACTIVATE_NOW;
		for (;;) {
			ioctl(f, FBIOPAN_DISPLAY, &v);
			usleep(200000);
		}
	}
	if (argc > 1 && !strcmp(argv[1], "dump")) {
		int f = open("/dev/fb0", O_RDWR);
		struct fb_fix_screeninfo fx;
		unsigned int *p; unsigned long i, nz = 0, n;
		if (f < 0) { perror("open"); return 1; }
		if (ioctl(f, FBIOGET_FSCREENINFO, &fx)) { perror("gfi"); return 1; }
		p = mmap(NULL, fx.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, f, 0);
		if (p == MAP_FAILED) { perror("mmap"); return 1; }
		n = fx.smem_len / 4;
		for (i = 0; i < n; i++) if (p[i]) nz++;
		printf("fb words=%lu nonzero=%lu first=%08x\n", n, nz, p[0]);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "fbfill"))
		return fb_fill();
	if (argc > 1 && !strcmp(argv[1], "single")) {
		int f = open("/dev/fb0", O_RDWR);
		struct fb_var_screeninfo v;
		if (f < 0) { perror("open"); return 1; }
		if (ioctl(f, FBIOGET_VSCREENINFO, &v)) { perror("gvi"); return 1; }
		v.xres_virtual = v.xres;
		v.yres_virtual = v.yres;
		v.xoffset = 0; v.yoffset = 0;
		v.activate = FB_ACTIVATE_NOW;
		if (ioctl(f, FBIOPUT_VSCREENINFO, &v)) { perror("put vi"); return 1; }
		printf("single buffer %ux%u virt %ux%u\n", v.xres, v.yres,
			v.xres_virtual, v.yres_virtual);
		return 0;
	}
	int ion, fbfd, fd = -1, i;
	struct fb_var_screeninfo var;
	struct decon_win_config_data data;
	void *buf;
	unsigned int heaps[] = { 1 << 4, 1 << 0, 0xffffffff };
	size_t len = 720 * 1280 * 4;

	ion = open("/dev/ion", O_RDWR);
	if (ion < 0) { perror("open /dev/ion"); return 1; }

	for (i = 0; i < 3; i++) {
		struct ion_allocation_data ad = { .len = len, .align = 4096,
			.heap_id_mask = heaps[i], .flags = 0, .handle = 0 };
		struct ion_fd_data fdd = { .handle = 0, .fd = -1 };
		if (ioctl(ion, ION_IOC_ALLOC, &ad) < 0) {
			fprintf(stderr, "ION_IOC_ALLOC heap %#x: %s\n",
				heaps[i], strerror(errno));
			continue;
		}
		fdd.handle = ad.handle;
		if (ioctl(ion, ION_IOC_MAP, &fdd) < 0) {
			fprintf(stderr, "ION_IOC_MAP: %s\n", strerror(errno));
			continue;
		}
		fd = fdd.fd;
		break;
	}
	if (fd < 0) { fprintf(stderr, "no ion buffer\n"); return 1; }

	buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) { perror("mmap ion"); return 1; }
	memset(buf, 0xff, len);			/* white */
	msync(buf, len, MS_SYNC);

	fbfd = open("/dev/fb0", O_RDWR);
	if (fbfd < 0) { perror("open /dev/fb0"); return 1; }
	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("FBIOGET_VSCREENINFO");
		return 1;
	}
	printf("fb: %ux%u %ubpp\n", var.xres, var.yres, var.bits_per_pixel);

	memset(&data, 0, sizeof(data));
	data.config[0].state = 2;		/* DECON_WIN_STATE_BUFFER */
	data.config[0].u.b.fd_idma[0] = fd;
	data.config[0].u.b.fence_fd = -1;
	data.config[0].u.b.plane_alpha = 255;
	data.config[0].u.b.blending = 0;	/* none */
	data.config[0].u.b.idma_type = 0;	/* IDMA_G0 */
	data.config[0].u.b.format = 4;		/* XRGB_8888 */
	data.config[0].u.b.src = (struct decon_frame){ 0, 0,
		var.xres, var.yres, var.xres, var.yres };
	data.config[0].dst = (struct decon_frame){ 0, 0,
		var.xres, var.yres, var.xres, var.yres };
	data.config[0].protection = 0;

	if (ioctl(fbfd, S3CFB_WIN_CONFIG, &data) < 0) {
		fprintf(stderr, "S3CFB_WIN_CONFIG(size %zu, cmd %#lx): %s\n",
			sizeof(data), (unsigned long)S3CFB_WIN_CONFIG, strerror(errno));
		return 1;
	}
	printf("win config committed, fence %d\n", data.fence);

	if (!(argc > 1 && !strcmp(argv[1], "nopan"))) {
		var.yoffset = 0; var.xoffset = 0; var.activate = FB_ACTIVATE_NOW;
		ioctl(fbfd, FBIOPAN_DISPLAY, &var);
	}

	if (argc > 1 && !strcmp(argv[1], "hold"))
		sleep(60);
	return 0;
}
