#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
	int f; struct fb_var_screeninfo v; struct fb_fix_screeninfo fx; void *m;
	unsigned long i, nz = 0, n; FILE *o;
	if (argc < 2) { fprintf(stderr, "usage: %s out\n", argv[0]); return 2; }
	f = open("/dev/fb0", O_RDWR);
	if (f < 0) { perror("open"); return 1; }
	if (ioctl(f, FBIOGET_VSCREENINFO, &v) || ioctl(f, FBIOGET_FSCREENINFO, &fx)) {
		perror("ioctl"); return 1;
	}
	printf("vis %ux%u virt %ux%u bpp %u line %u smem %#lx len %u\n",
		v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel,
		fx.line_length, (unsigned long)fx.smem_start, fx.smem_len);
	m = mmap(NULL, fx.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	n = fx.smem_len / 4;
	for (i = 0; i < n; i++) if (((unsigned int *)m)[i]) nz++;
	printf("words %lu nonzero %lu\n", n, nz);
	o = fopen(argv[1], "wb");
	if (!o) { perror("fopen"); return 1; }
	fwrite(m, 1, fx.smem_len, o);
	fclose(o);
	return 0;
}
