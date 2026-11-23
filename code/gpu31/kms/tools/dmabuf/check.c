/*
 * Host-side unit test for the DECON KMS framebuffer format/pitch validation.
 *
 * The function under test is not re-implemented here: the Makefile extracts
 * the block delimited by the BEGIN/END exynos7580_fb_check markers from
 * ../../exynos_drm7580_dmabuf.c into exynos7580_fb_check.inc and this file
 * includes it.  A change to the kernel code therefore changes the test.
 *
 *   make        build + run (native)
 *   make clean
 *
 * Exit status 0 on success; any failing case prints the case name and exits 1.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "exynos7580_fb_check.inc"

#define FMT_XRGB8888	0x34325258u
#define FMT_ARGB8888	0x34325241u
#define FMT_XBGR8888	0x34324258u
#define FMT_RGB565	0x36314752u
#define FMT_RGBA5551	0x35314152u
#define FMT_NV12	0x3231564eu	/* planar, must be rejected */
#define FMT_RGB888	0x33424752u	/* packed 24bpp, DECON window is not 24bpp RGB */
#define FMT_UNKNOWN	0x00000000u

#define FB_W		720u
#define FB_H		1280u
#define FB_PITCH	(FB_W * 4)
#define FB_SIZE		((unsigned long long)FB_PITCH * FB_H)

static int failures;

static void base(struct exy7580_fb_desc *d)
{
	memset(d, 0, sizeof(*d));
	d->pixel_format = FMT_XRGB8888;
	d->width = FB_W;
	d->height = FB_H;
	d->handles[0] = 1;
	d->pitches[0] = FB_PITCH;
	d->offsets[0] = 0;
	d->size = FB_SIZE;
	d->max_width = 4096;
	d->max_height = 4096;
}

static void expect(const char *name, struct exy7580_fb_desc *d, int want)
{
	int ret = exy7580_fb_check(d);

	if (ret != want) {
		printf("FAIL %-42s got %d want %d\n", name, ret, want);
		failures++;
	} else {
		printf("ok   %-42s (%d)\n", name, ret);
	}
}

int main(void)
{
	struct exy7580_fb_desc d;

	/* --- accepted layouts ------------------------------------------- */
	base(&d);
	expect("XRGB8888 720x1280", &d, 0);

	base(&d);
	d.pixel_format = FMT_ARGB8888;
	expect("ARGB8888 720x1280", &d, 0);

	base(&d);
	d.pixel_format = FMT_XBGR8888;
	expect("XBGR8888 720x1280", &d, 0);

	base(&d);
	d.pixel_format = FMT_RGB565;
	d.pitches[0] = FB_W * 2;
	d.size = (unsigned long long)FB_W * 2 * FB_H;
	expect("RGB565 720x1280", &d, 0);

	base(&d);
	d.pixel_format = FMT_RGBA5551;
	d.pitches[0] = FB_W * 2;
	d.size = (unsigned long long)FB_W * 2 * FB_H;
	expect("RGBA5551 720x1280", &d, 0);

	base(&d);
	d.width = 32;			/* exactly 128 bytes at 32bpp */
	d.pitches[0] = 128;
	d.size = 128ull * FB_H;
	expect("minimum window width 128 bytes", &d, 0);

	base(&d);
	d.pitches[0] = FB_PITCH + 256;	/* aligned padding stride */
	d.size = (unsigned long long)d.pitches[0] * FB_H;
	expect("padded stride", &d, 0);

	/* --- rejected layouts ------------------------------------------- */
	base(&d);
	d.flags = 1;
	expect("nonzero flags (modifier/interlace)", &d, -EINVAL);

	base(&d);
	d.pixel_format = FMT_NV12;
	d.pitches[1] = FB_W;
	d.offsets[1] = FB_PITCH * FB_H;
	expect("NV12 planar", &d, -EINVAL);

	base(&d);
	d.pixel_format = FMT_RGB888;
	d.pitches[0] = FB_W * 3;
	expect("RGB888 (not a DECON RGB mode)", &d, -EINVAL);

	base(&d);
	d.pixel_format = FMT_UNKNOWN;
	expect("unknown fourcc", &d, -EINVAL);

	base(&d);
	d.handles[0] = 0;
	expect("missing handle", &d, -EINVAL);

	base(&d);
	d.handles[1] = 2;
	expect("second handle (multi-plane)", &d, -EINVAL);

	base(&d);
	d.pitches[1] = FB_W;
	expect("second pitch", &d, -EINVAL);

	base(&d);
	d.width = 0;
	expect("zero width", &d, -EINVAL);

	base(&d);
	d.height = 0;
	expect("zero height", &d, -EINVAL);

	base(&d);
	d.width = 4097;
	expect("width over max", &d, -EINVAL);

	base(&d);
	d.height = 4097;
	expect("height over max", &d, -EINVAL);

	base(&d);
	d.max_width = 0;
	expect("unset max width", &d, -EINVAL);

	base(&d);
	d.width = 31;			/* 124 bytes at 32bpp: < 128 */
	d.pitches[0] = 124;
	expect("window narrower than 128 bytes", &d, -EINVAL);

	base(&d);
	d.pitches[0] = FB_PITCH - 4;
	expect("pitch below width*cpp", &d, -EINVAL);

	base(&d);
	d.pitches[0] = FB_PITCH + 2;	/* not a whole pixel */
	expect("pitch not pixel aligned", &d, -EINVAL);

	base(&d);
	d.pitches[0] = 0;
	expect("zero pitch", &d, -EINVAL);

	base(&d);
	d.offsets[0] = 2;		/* not a whole pixel */
	expect("offset not pixel aligned", &d, -EINVAL);

	base(&d);
	d.offsets[0] = FB_SIZE;		/* starts past the buffer */
	expect("offset at end of buffer", &d, -EINVAL);

	base(&d);
	d.size = FB_SIZE - 4;		/* last row does not fit */
	expect("buffer one pixel short", &d, -EINVAL);

	base(&d);
	d.size = 0;
	expect("zero buffer size", &d, -EINVAL);

	base(&d);
	d.height = 0xffffffffu;		/* u64 arithmetic must not wrap */
	expect("huge height", &d, -EINVAL);

	base(&d);
	d.offsets[0] = 0xffffff00u;
	expect("huge offset", &d, -EINVAL);

	expect("NULL descriptor", NULL, -EINVAL);

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all format/pitch validation cases passed\n");
	return 0;
}
