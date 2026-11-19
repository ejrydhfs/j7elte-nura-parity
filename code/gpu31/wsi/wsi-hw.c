/* SPDX-License-Identifier: MIT */
/*
 * wsi-hw.c - hardware WSI path probe for a render-only node.
 *
 * Prepares the present path Mesa will use once panfrost registers
 * /dev/dri/renderD128 (Mode B, G31_Kms):
 *
 *   1. node identity: DRM_IOCTL_VERSION driver name + GET_PARAM GPU_PROD_ID
 *      (v5.2 UAPI mirror: code/gpu31/gem/panfrost_drm.h; H1 iff driver is
 *      "panfrost", T720 iff prod id == 0x0720).
 *   2. GBM device creation on the render fd: gbm_create_device() + a small
 *      SCANOUT|RENDERING test BO (stride readback) + dma-buf export
 *      (gbm_bo_get_fd) + CPU map/unmap round trip.
 *   3. X11 server caps: MIT-SHM (XShmQueryExtension) and DRI3/Present
 *      (XQueryExtension) — decides pixmap import vs shm fallback.
 *
 * Decision (pure function wsi_decide(), unit-covered by --selftest):
 *
 *   h1 && t720 && bo && dma_export && dri3 -> WSI_GBM_PIXMAP
 *     render on the node, present via DRI3 pixmap-from-buffers.
 *   h1 && t720 && bo && shm              -> WSI_GBM_SHM
 *     render on the node, present via XShmPutImage fallback
 *     (expected on the fbdev X: xf86-video-fbdev has no DRI3).
 *   otherwise                            -> WSI_NONE (no HW present path;
 *     SW EGL-X11/llvmpipe via tri-x11 stays the working path).
 *
 * Usage:
 *   wsi-hw [--node PATH] [--display DPY]   probe + decision + knobs
 *   wsi-hw --selftest                       mock decision matrix + live
 *                                           /dev/null negative (no HW needed)
 *
 * Exits: 0 = HW present path found (GBM_*), 3 = none, 2 = no device,
 * 1 = usage error. Mirrors render-node-probe's exit scheme.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm/drm.h>

#include <gbm.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

/* ---- v5.2 UAPI minimum (source of truth: code/gpu31/gem/panfrost_drm.h,
 * byte-identical to Linux v5.2 include/uapi/drm/panfrost_drm.h). Only
 * GET_PARAM is needed here; SUBMIT/WAIT_BO/CREATE_BO are exercised by
 * code/gpu31/client/pf7580. ---- */
#define WSI_PANFROST_PARAM_GPU_PROD_ID	0u
#define WSI_T720_PROD_ID		0x0720u

struct wsi_get_param {
	uint32_t param;
	uint32_t pad;		/* must be zero */
	uint64_t value;
};

#define WSI_IOCTL_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + 0x04, struct wsi_get_param)

_Static_assert(sizeof(struct wsi_get_param) == 16, "get_param wire size");

enum wsi_path {
	WSI_GBM_PIXMAP = 0,	/* GBM render + DRI3 pixmap import */
	WSI_GBM_SHM = 1,	/* GBM render + XShm fallback */
	WSI_NONE = 2		/* no HW present path */
};

static const char *wsi_path_name(enum wsi_path p)
{
	switch (p) {
	case WSI_GBM_PIXMAP:
		return "GBM_PIXMAP";
	case WSI_GBM_SHM:
		return "GBM_SHM";
	default:
		return "NONE";
	}
}

/* Pure decision logic: every input is a probed boolean. */
static enum wsi_path wsi_decide(int h1, int t720, int gbm_bo,
				int gbm_export, int dri3, int shm)
{
	if (h1 && t720 && gbm_bo) {
		if (gbm_export && dri3)
			return WSI_GBM_PIXMAP;
		if (shm)
			return WSI_GBM_SHM;
	}
	return WSI_NONE;
}

/* ---- live probes ---- */

struct wsi_node {
	int h1;			/* driver name == "panfrost" */
	int t720;		/* GET_PARAM prod id == 0x0720 */
	char driver[64];
};

static int wsi_probe_node(int fd, struct wsi_node *n)
{
	struct drm_version ver;
	struct wsi_get_param p;

	memset(n, 0, sizeof(*n));
	memset(&ver, 0, sizeof(ver));
	ver.name = n->driver;
	ver.name_len = sizeof(n->driver) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &ver) != 0)
		return -errno;
	n->h1 = !strcmp(n->driver, "panfrost");
	if (!n->h1)
		return 0;
	memset(&p, 0, sizeof(p));
	p.param = WSI_PANFROST_PARAM_GPU_PROD_ID;
	if (ioctl(fd, WSI_IOCTL_GET_PARAM, &p) != 0)
		return 0;	/* DRM node, but prod id unreadable: H1, no T720 */
	n->t720 = ((uint32_t)p.value == WSI_T720_PROD_ID);
	return 0;
}

struct wsi_gbm {
	int bo;			/* SCANOUT|RENDERING BO created, stride read */
	int exp;		/* dma-buf export fd obtained */
	int map;		/* CPU map/unmap round trip worked */
	uint32_t stride;
};
/* CPU mapping is informational only (never feeds the decision): some
 * GBM drivers only map LINEAR BOs, so wsi_probe_gbm retries on a
 * LINEAR sibling before reporting map=0. */
static int wsi_try_map(struct gbm_bo *bo)
{
	void *map, *map_data = NULL;
	uint32_t stride = 0;

	map = gbm_bo_map(bo, 0, 0, 64, 64, GBM_BO_TRANSFER_READ,
			 &stride, &map_data);
	if (map) {
		gbm_bo_unmap(bo, map_data);
		return 1;
	}
	return 0;
}


static int wsi_probe_gbm(int fd, struct wsi_gbm *g)
{
	struct gbm_device *dev;
	struct gbm_bo *bo;
	int dmafd = -1;

	memset(g, 0, sizeof(*g));
	dev = gbm_create_device(fd);
	if (!dev)
		return -errno;
	bo = gbm_bo_create(dev, 64, 64, GBM_FORMAT_XRGB8888,
			   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!bo) {
		int e = errno ? errno : ENODEV;

		gbm_device_destroy(dev);
		return -e;
	}
	g->bo = 1;
	g->stride = gbm_bo_get_stride(bo);
	dmafd = gbm_bo_get_fd(bo);
	if (dmafd >= 0) {
		g->exp = 1;
		close(dmafd);
	}
	if (wsi_try_map(bo)) {
		g->map = 1;
	} else {
		struct gbm_bo *lin =
			gbm_bo_create(dev, 64, 64, GBM_FORMAT_XRGB8888,
				      GBM_BO_USE_LINEAR);

		if (lin) {
			if (wsi_try_map(lin))
				g->map = 1;
			gbm_bo_destroy(lin);
		}
	}
	gbm_bo_destroy(bo);
	gbm_device_destroy(dev);
	return 0;
}

struct wsi_x11 {
	int open;		/* display opened */
	int shm;		/* MIT-SHM present */
	int dri3;		/* DRI3 present (pixmap import needs it) */
	int present;		/* Present present */
};

static int wsi_probe_x11(const char *dpyname, struct wsi_x11 *x)
{
	Display *dpy;
	int ev, err;

	memset(x, 0, sizeof(*x));
	dpy = XOpenDisplay(dpyname);
	if (!dpy)
		return -errno;
	x->open = 1;
	x->shm = XShmQueryExtension(dpy) ? 1 : 0;
	x->dri3 = XQueryExtension(dpy, "DRI3", &ev, &err, &err) ? 1 : 0;
	x->present = XQueryExtension(dpy, "Present", &ev, &err, &err) ? 1 : 0;
	XCloseDisplay(dpy);
	return 0;
}

/* ---- knobs ---- */

static void wsi_print_knobs(enum wsi_path p)
{
	printf("knobs:\n");
	printf("  unset LIBGL_ALWAYS_SOFTWARE"
	       "  # shipped =1 by j7elte-gpu-soft; HW GL needs it unset\n");
	printf("  export MESA_LOADER_DRIVER_OVERRIDE=panfrost"
	       "  # pin DRI driver for the render fd\n");
	printf("  unset DRI_PRIME  # single GPU, no offload\n");
	printf("  unset PAN_MESA_DEBUG  # first-HW-run validation only:"
	       " PAN_MESA_DEBUG=trace,sync\n");
	printf("  unset EGL_LOG_LEVEL LIBGL_DEBUG"
	       "  # debug runs only: EGL_LOG_LEVEL=debug LIBGL_DEBUG=verbose\n");
	if (p == WSI_GBM_PIXMAP)
		printf("  EGL client: eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,"
		       " gbm_dev) + DRI3 pixmap import\n");
	else if (p == WSI_GBM_SHM)
		printf("  EGL client: eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,"
		       " gbm_dev) + XShmPutImage fallback (no DRI3 on fbdev X)\n");
	else
		printf("  no HW present path: keep tri-x11"
		       " (EGL-X11 window + llvmpipe) until panfrost registers\n");
	printf("  Mesa needs T720-capable panfrost (Alpine mesa-dri-gallium"
	       " ships panfrost_dri.so; G31_Mesa 0005 compat absorbs\n");
	printf("  modern-Mesa extra-param/MADVISE probes: kernel answers"
	       " -EINVAL, Mesa falls back — no knob, never widen the UAPI\n");
}

/* ---- mock decision matrix (no device needed) ---- */

struct wsi_case {
	int h1, t720, bo, exp, dri3, shm;
	enum wsi_path want;
	const char *name;
};

static const struct wsi_case wsi_matrix[] = {
	{ 1, 1, 1, 1, 1, 1, WSI_GBM_PIXMAP, "full-stack->pixmap" },
	{ 1, 1, 1, 1, 1, 0, WSI_GBM_PIXMAP, "pixmap-needs-no-shm" },
	{ 1, 1, 1, 1, 0, 1, WSI_GBM_SHM, "no-dri3->shm" },
	{ 1, 1, 1, 0, 1, 1, WSI_GBM_SHM, "no-export->shm-despite-dri3" },
	{ 1, 1, 1, 0, 0, 1, WSI_GBM_SHM, "bo+shm-only->shm" },
	{ 1, 1, 1, 1, 0, 0, WSI_NONE, "bo-but-no-present->none" },
	{ 1, 1, 0, 0, 1, 1, WSI_NONE, "no-bo->none" },
	{ 1, 0, 1, 1, 1, 1, WSI_NONE, "non-t720-panfrost->none" },
	{ 0, 0, 1, 1, 1, 1, WSI_NONE, "non-panfrost-drm->none" },
	{ 0, 0, 0, 0, 1, 1, WSI_NONE, "sw-server-only->none" },
	{ 0, 0, 0, 0, 0, 0, WSI_NONE, "nothing->none" },
};

static int wsi_selftest(void)
{
	size_t i;
	int failures = 0;
	int fd;

	for (i = 0; i < sizeof(wsi_matrix) / sizeof(wsi_matrix[0]); i++) {
		const struct wsi_case *c = &wsi_matrix[i];
		enum wsi_path got =
			wsi_decide(c->h1, c->t720, c->bo, c->exp,
				   c->dri3, c->shm);

		if (got != c->want) {
			printf("FAIL matrix %s: got %s want %s\n", c->name,
			       wsi_path_name(got), wsi_path_name(c->want));
			failures++;
		}
	}
	/* Live negative: /dev/null is never a DRM node — must classify
	 * cleanly as not-panfrost, never crash, never claim H1. */
	fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("FAIL selftest: cannot open /dev/null: %s\n",
		       strerror(errno));
		failures++;
	} else {
		struct wsi_node n;

		if (wsi_probe_node(fd, &n) == 0) {
			if (n.h1 || n.t720) {
				printf("FAIL selftest: /dev/null claims H1/T720\n");
				failures++;
			}
		} else {
			/* ioctl failed as expected; h1/t720 stay 0. */
			if (n.h1 || n.t720) {
				printf("FAIL selftest: /dev/null error path claims H1\n");
				failures++;
			}
		}
		if (wsi_decide(n.h1, n.t720, 0, 0, 0, 0) != WSI_NONE) {
			printf("FAIL selftest: /dev/null decides non-NONE\n");
			failures++;
		}
		close(fd);
	}
	if (failures == 0)
		printf("WSI-HW SELFTEST: PASS (%zu matrix + /dev/null negative)\n",
		       sizeof(wsi_matrix) / sizeof(wsi_matrix[0]));
	else
		printf("WSI-HW SELFTEST: FAIL (%d)\n", failures);
	return failures ? 1 : 0;
}

/* ---- main ---- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--node PATH] [--display DPY] [--selftest]\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *node = "/dev/dri/renderD128";
	const char *dpyname = NULL;
	int i;
	struct stat st;
	int fd = -1;
	struct wsi_node n;
	struct wsi_gbm g;
	struct wsi_x11 x;
	enum wsi_path p;
	int gbm_rc, x11_rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--selftest"))
			return wsi_selftest();
		else if (!strcmp(argv[i], "--node") && i + 1 < argc)
			node = argv[++i];
		else if (!strcmp(argv[i], "--display") && i + 1 < argc)
			dpyname = argv[++i];
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (stat(node, &st) != 0) {
		printf("wsi-hw: node %s: %s -> NO_DEVICE\n", node,
		       strerror(errno));
		return 2;
	}
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("wsi-hw: open %s: %s -> NO_DEVICE\n", node,
		       strerror(errno));
		return 2;
	}

	memset(&n, 0, sizeof(n));
	memset(&g, 0, sizeof(g));
	memset(&x, 0, sizeof(x));

	if (wsi_probe_node(fd, &n) != 0)
		printf("wsi-hw: node %s: not a DRM node (driver query failed)\n",
		       node);
	else
		printf("wsi-hw: node %s: driver='%s' h1=%d t720=%d\n",
		       node, n.driver[0] ? n.driver : "(none)", n.h1,
		       n.t720);

	gbm_rc = wsi_probe_gbm(fd, &g);
	if (gbm_rc == 0)
		printf("wsi-hw: gbm: device ok, test bo ok"
		       " (stride=%u export=%d map=%d)\n",
		       g.stride, g.exp, g.map);
	else
		printf("wsi-hw: gbm: unavailable (%s)\n",
		       strerror(-gbm_rc));
	close(fd);

	x11_rc = wsi_probe_x11(dpyname, &x);
	if (x11_rc == 0 && x.open)
		printf("wsi-hw: x11: open ok shm=%d dri3=%d present=%d\n",
		       x.shm, x.dri3, x.present);
	else
		printf("wsi-hw: x11: no display (%s)\n",
		       strerror(-x11_rc));

	p = wsi_decide(n.h1, n.t720, g.bo, g.exp, x.dri3, x.shm);
	printf("wsi-hw: decision: %s\n", wsi_path_name(p));
	wsi_print_knobs(p);
	return (p == WSI_NONE) ? 3 : 0;
}
