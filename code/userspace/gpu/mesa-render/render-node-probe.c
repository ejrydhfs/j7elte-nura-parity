/*
 * render-node-probe.c - classify a DRM render node before any Mesa work.
 *
 * Given a device node path (default /dev/dri/renderD128), print:
 *   - DRM_IOCTL_VERSION driver name (H1 signal: "panfrost")
 *   - DRM_IOCTL_PANFROST_GET_PARAM answers for GPU_PROD_ID, REVISION,
 *     SHADER/TILER/L2_PRESENT, MMU_FEATURES, SELECTED_COHERENCY (H1 confirm)
 *   - kbase UKP_FUNC_ID_CHECK_VERSION 10.6 handshake result (H2 signal;
 *     on a real DRM fd the kernel answers ENOTTY, harmless)
 * and a final verdict line: H1 (panfrost UAPI) / H2 (kbase UK shim) /
 * NEITHER.
 *
 * Self-contained C99: only <drm/drm.h> + <drm/panfrost_drm.h> + libc, no
 * libdrm needed, so it cross-builds with plain aarch64-linux-gnu-gcc.
 *
 * Exit: 0 classified, 2 no such device, 1 error.
 *
 * SPDX-License-Identifier: MIT
 */
/* Expose O_CLOEXEC under strict -std=c99; must precede any header. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/panfrost_drm.h>

/* kbase UK 10.6 version-check wire ABI (kbase_uapi.h shape; the ioctl cmd is
 * only used for _IOC_SIZE(cmd), type/number ignored by kbase_ioctl). */
#define UKP_FUNC_ID_CHECK_VERSION 0
#define KBASE_UK_IOCTL(size) _IOC(_IOC_WRITE, 0, 0, (size))

struct uk_version_check {
	union { uint32_t id; uint32_t ret; uint64_t sizer; } header;
	uint16_t major;
	uint16_t minor;
	uint8_t padding[4];
};

static int panfrost_param(int fd, uint32_t param, uint64_t *val)
{
	struct drm_panfrost_get_param p;

	memset(&p, 0, sizeof(p));
	p.param = param;
	if (ioctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &p))
		return -errno;
	*val = p.value;
	return 0;
}

static void show_param(int fd, uint32_t param, const char *name)
{
	uint64_t v = 0;
	int r = panfrost_param(fd, param, &v);

	if (r)
		printf("  %-22s: ioctl failed (%s)\n", name, strerror(-r));
	else
		printf("  %-22s: 0x%llx (%llu)\n", name,
		       (unsigned long long)v, (unsigned long long)v);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	struct drm_version ver;
	char name[64] = { 0 }, date[64] = { 0 }, desc[256] = { 0 };
	struct uk_version_check uk;
	int fd, h1 = 0, h2 = 0, rc = 0;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", argv[0], path,
			strerror(errno));
		return 2;
	}

	/* DRM identity first: harmless on any DRM node. */
	memset(&ver, 0, sizeof(ver));
	ver.name = (char *)name;
	ver.name_len = sizeof(name) - 1;
	ver.date = date;
	ver.date_len = sizeof(date) - 1;
	ver.desc = desc;
	ver.desc_len = sizeof(desc) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0) {
		printf("drm driver: %s (%s)\n", name, desc);
		h1 = !strcmp(name, "panfrost");
	} else {
		printf("DRM_IOCTL_VERSION failed: %s\n", strerror(errno));
	}

	if (h1) {
		printf("GET_PARAM:\n");
		show_param(fd, DRM_PANFROST_PARAM_GPU_PROD_ID, "GPU_PROD_ID");
		show_param(fd, DRM_PANFROST_PARAM_GPU_REVISION, "GPU_REVISION");
		show_param(fd, DRM_PANFROST_PARAM_SHADER_PRESENT,
			   "SHADER_PRESENT");
		show_param(fd, DRM_PANFROST_PARAM_TILER_PRESENT,
			   "TILER_PRESENT");
		show_param(fd, DRM_PANFROST_PARAM_L2_PRESENT, "L2_PRESENT");
		show_param(fd, DRM_PANFROST_PARAM_AS_PRESENT, "AS_PRESENT");
		show_param(fd, DRM_PANFROST_PARAM_JS_PRESENT, "JS_PRESENT");
		show_param(fd, DRM_PANFROST_PARAM_MMU_FEATURES, "MMU_FEATURES");
		show_param(fd, DRM_PANFROST_PARAM_SELECTED_COHERENCY,
			   "SELECTED_COHERENCY");
		printf("H1 check: panfrost UAPI answers\n");
	} else {
		printf("not a panfrost DRM driver; GET_PARAM skipped\n");
	}

	/* kbase UK handshake: version query only, no state change. A real DRM
	 * driver rejects the unknown ioctl with ENOTTY; a kbase shim answers
	 * UK 10.6. Do this after the DRM probes so a shim that also serves
	 * DRM ioctls is still fully characterised. */
	memset(&uk, 0, sizeof(uk));
	uk.header.id = UKP_FUNC_ID_CHECK_VERSION;
	uk.major = 10;
	uk.minor = 6;
	if (ioctl(fd, KBASE_UK_IOCTL(sizeof(uk)), &uk) == 0 &&
	    uk.header.ret == 0) {
		printf("kbase handshake: ok, kernel UK %u.%u\n",
		       uk.major, uk.minor);
		if ((uk.major == 10 && uk.minor >= 3) || uk.major > 10)
			h2 = 1;
		else
			printf("kbase handshake: UK version too old\n");
	} else if (errno == ENOTTY || errno == EINVAL) {
		printf("kbase handshake: not answered (%s)\n",
		       strerror(errno));
	} else {
		printf("kbase handshake: failed (%s), ret=%u\n",
		       strerror(errno), uk.header.ret);
	}

	if (h1 && h2)
		printf("verdict: H1+H2 (hybrid node)\n");
	else if (h1)
		printf("verdict: H1 (panfrost UAPI: use stock panfrost_dri.so)\n");
	else if (h2)
		printf("verdict: H2 (kbase UK shim: kbase_kmod + adapt patch)\n");
	else
		printf("verdict: NEITHER (no panfrost UAPI, no UK handshake)\n");

	close(fd);
	return rc;
}
