/*
 * kmsflip - atomic KMS flip test for the Exynos 7580 DECON bring-up
 *           (j7elte) and the render-node-only bring-up images.
 *
 * Three modes:
 *
 *   kmsflip (default, KMS)
 *       Opens card0, picks a connected connector + preferred mode, and
 *       drives the *same* interface a compositor (KWin) uses:
 *
 *         phase dumb    two dumb buffers, AddFB2(XRGB8888), an atomic
 *                       modeset (MODE_ID/ACTIVE/plane FB_ID), then N
 *                       non-blocking atomic page flips with
 *                       DRM_MODE_PAGE_FLIP_EVENT.
 *         phase dmabuf  imports dma-bufs into card0 and flips those.  The
 *                       source is, in order of preference: a GBM/linear
 *                       buffer allocated on the panfrost render node, a
 *                       dumb buffer on the render node exported with
 *                       PRIME, or a card0 dumb buffer exported and
 *                       re-imported (our PRIME export round-trip).
 *
 *       Every commit is asynchronous and every completion is waited for
 *       with a poll() deadline; a per-run SIGALRM watchdog backs up any
 *       ioctl that would otherwise block forever.  Completion events are
 *       counted per phase, and each phase prints one counter line.  The
 *       final line is a single PASS/FAIL/SKIP summary.
 *
 *   kmsflip --probe
 *       Render-node-only images (int49-class: /dev/dri/renderD128 but no
 *       card0).  Allocates two linear buffers on the render node (GBM when
 *       built with EGL/GBM, otherwise dumb), exports them with PRIME and
 *       round-trips the fd back to a handle when the kernel allows it.
 *       Proves the dma-buf *source* half of phase dmabuf without needing
 *       a KMS device.  Exit 0 = source OK, 3 = no render node.
 *
 *   kmsflip --headless
 *       GBM + EGL surfaceless + GLES2 render/readback checksum test used
 *       by run-headless.sh; unchanged.
 *
 * Exit status: 0 PASS, 1 FAIL, 2 usage, 3 environment/skip (missing
 * /dev/dri/card0 on a render-only kernel, or missing render node).
 *
 * Build (on device, native):  make            (KMS + GBM dma-buf source)
 *                             make NO_EGL=1   (libdrm only)
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#if HAVE_EGL
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#if !defined(EGL_PLATFORM_SURFACELESS_MESA)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#endif

/* libdrm exposes this through drm_fourcc.h on some installs only. */
#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888	0x34325258u	/* fourcc_code('X','R','2','4') */
#endif
#ifndef GBM_FORMAT_XRGB8888
#define GBM_FORMAT_XRGB8888	DRM_FORMAT_XRGB8888
#endif

#define PLANE_SRC_FULL		0	/* 16.16 fixed point origin */

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_alarm_hits;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/*
 * First SIGALRM asks cooperating code to stop.  If a libdrm call is stuck
 * in an EINTR-retry loop, the repeating timer fires again and the process
 * exits instead of wedging.  124 is the conventional timeout status.
 */
static void on_alarm(int sig)
{
	(void)sig;
	g_stop = 1;
	if (++g_alarm_hits > 1)
		_exit(124);
}

static void watchdog_arm(int ms)
{
	struct itimerval it;

	if (ms <= 0)
		return;
	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = ms / 1000;
	it.it_value.tv_usec = (ms % 1000) * 1000;
	if (!it.it_value.tv_sec && !it.it_value.tv_usec)
		it.it_value.tv_usec = 1000;
	it.it_interval.tv_sec = 2;	/* second hit is fatal */
	g_alarm_hits = 0;
	setitimer(ITIMER_REAL, &it, NULL);
}

static void watchdog_disarm(void)
{
	struct itimerval it;

	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
	g_alarm_hits = 0;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int err_from(int ret)
{
	int e = errno;

	if (e)
		return e;
	return ret < 0 ? -ret : EIO;
}

static uint64_t fnv1a(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint64_t h = 0xcbf29ce484222325ULL;
	size_t i;

	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ULL;
	}
	return h;
}

/*
 * Checksum a 2D buffer row-by-row so stride padding (which the tool never
 * writes) cannot influence the result.
 */
static uint64_t fnv1a_rows(const void *p, size_t pitch, size_t rowbytes,
			   int h)
{
	const uint8_t *b = p;
	uint64_t hash = 0xcbf29ce484222325ULL;
	int y;

	for (y = 0; y < h; y++) {
		hash ^= fnv1a(b + (size_t)y * pitch, rowbytes);
		hash *= 1099511628211ULL;
	}
	return hash;
}

enum pattern { PAT_SOLID, PAT_BARS, PAT_CHECKER, PAT_GRADIENT, PAT_NOISE };

static int parse_pattern(const char *s)
{
	if (!strcmp(s, "solid"))
		return PAT_SOLID;
	if (!strcmp(s, "bars"))
		return PAT_BARS;
	if (!strcmp(s, "checker"))
		return PAT_CHECKER;
	if (!strcmp(s, "gradient"))
		return PAT_GRADIENT;
	if (!strcmp(s, "noise"))
		return PAT_NOISE;
	return -1;
}

static uint32_t pattern_pixel(int kind, int frame, int x, int y, int w, int h)
{
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	/* Solid colours: consecutive frames are always different. */
	static const uint32_t solid[6] = {
		0xffff0000, 0xff00ff00, 0xff0000ff,
		0xffffffff, 0xffffff00, 0xff00ffff,
	};
	uint32_t v;
	uint32_t f = (uint32_t)frame * 2654435761u;

	switch (kind) {
	case PAT_SOLID:
		v = solid[(unsigned)frame % (sizeof(solid) / sizeof(solid[0]))];
		break;
	case PAT_CHECKER:
		v = ((((x >> 4) ^ (y >> 4)) & 1) ? 0xffffffff : 0xff000000);
		break;
	case PAT_GRADIENT:
		v = 0xff000000 |
		    ((uint32_t)(x * 255 / (w - 1)) << 16) |
		    ((uint32_t)(y * 255 / (h - 1)) << 8) |
		    ((uint32_t)((x ^ y) & 0xff));
		break;
	case PAT_NOISE: {
		uint64_t s = (uint64_t)(uint32_t)y << 32 | (uint32_t)x;

		s ^= (uint64_t)frame * 0x9e3779b97f4a7c15ULL;
		s ^= s >> 33;
		s *= 0xff51afd7ed558ccdULL;
		s ^= s >> 33;
		v = 0xff000000 | (uint32_t)s;
		break;
	}
	case PAT_BARS:
	default:
		v = bars[(x * 8) / w];
		break;
	}

	if (kind == PAT_SOLID)
		return v;	/* stay solid: no XOR and no progress bar */

	v ^= f;
	/* frame reference bar: 0..w/2 px wide */
	if (y < h / 64 &&
	    x < (int)((uint32_t)w * (uint32_t)(frame % 61) / 120u))
		v = 0xff7f7f7f;
	return v;
}

/* Fill an XRGB8888 buffer honouring its real pitch (dumb or GBM stride). */
static void fill_buffer(void *map, uint32_t pitch, int w, int h, int kind,
			int frame)
{
	uint32_t *px = map;
	uint32_t stride = pitch / 4;
	int x, y;

	if (!px || stride < (uint32_t)w)
		return;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			px[(size_t)y * stride + x] =
				pattern_pixel(kind, frame, x, y, w, h);
}

static int dump_ppm(const char *path, const uint32_t *px, uint32_t pitch,
		    int w, int h)
{
	FILE *fp = fopen(path, "wb");
	int x, y;

	if (!fp) {
		fprintf(stderr, "kmsflip: %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(fp, "P6\n%d %d\n255\n", w, h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint32_t v = px[(size_t)y * (pitch / 4) + x];
			unsigned char rgb[3] = { (v >> 16) & 0xff,
						 (v >> 8) & 0xff,
						 v & 0xff };

			fwrite(rgb, 1, 3, fp);
		}
	fclose(fp);
	return 0;
}

/* ------------------------------------------------------------ atomic KMS */

struct flip_stats {
	const char *phase;
	unsigned commits;	/* atomic commit ioctls, including retries */
	unsigned commit_ok;
	unsigned commit_fail;
	unsigned events;	/* completion events for this phase */
	unsigned flips;		/* page flips that retired */
	unsigned modeset;	/* 1 if this phase carried the modeset */
	unsigned timeouts;
	unsigned ebusy;
	unsigned einval;
	unsigned other;
	unsigned checksum_bad;
	unsigned setup_fail;
	unsigned addfb_fallback;
	double elapsed_ms;
};

/*
 * Event completion.  Tokens are monotonically increasing per commit and the
 * user_data is the token value itself, never a pointer to stack: a late
 * event from an earlier phase can therefore never dereference dead memory.
 * Only tokens >= the current phase base are counted for that phase.
 */
static unsigned long g_token_base = 1;
static unsigned long g_next_token = 1;
static unsigned long g_done_token;
static struct flip_stats *g_cur_stats;
static drmEventContext g_ectx;

static void flip_handler(int fd, unsigned int seq, unsigned int sec,
			 unsigned int usec, void *data)
{
	unsigned long tok = (unsigned long)(uintptr_t)data;

	(void)fd;
	(void)seq;
	(void)sec;
	(void)usec;
	if (tok >= g_token_base && g_cur_stats)
		g_cur_stats->events++;
	g_done_token = tok;
}

static void event_ctx_init(void)
{
	memset(&g_ectx, 0, sizeof(g_ectx));
	g_ectx.version = 2;
	g_ectx.page_flip_handler = flip_handler;
}

struct kms_ctx {
	int fd;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	drmModeCrtc *saved;
	drmModeModeInfo mode;
	uint32_t crtc_id;
	uint32_t conn_id;
	uint32_t crtc_index;
	drmModePlane *plane;
	drmModeObjectProperties *conn_props;
	drmModeObjectProperties *crtc_props;
	drmModeObjectProperties *plane_props;
	uint32_t conn_crtc_id;
	uint32_t crtc_active;
	uint32_t crtc_mode_id;
	uint32_t plane_fb_id;
	uint32_t plane_crtc_id;
	uint32_t plane_src_x, plane_src_y, plane_src_w, plane_src_h;
	uint32_t plane_crtc_x, plane_crtc_y, plane_crtc_w, plane_crtc_h;
	uint32_t mode_blob;
	int did_modeset;
};

/*
 * One display buffer.  dumb:  mapped dumb buffer + FB.
 *                      prime: dumb/GBM source, PRIME fd, imported handle, FB.
 */
struct flip_buf {
	uint32_t fb;
	void *map;
	uint32_t pitch;
	uint64_t size;
	int src_fd;		/* fd that owns dumb_handle (borrowed) */
	uint32_t dumb_handle;
	int prime_fd;		/* exported dma-buf fd */
	uint32_t import_handle;	/* handle in card0 */
	int imported;
	uint64_t csum;
	/* GBM source (only when HAVE_EGL) */
	void *gbm_bo;
	void *gbm_map_data;
};

struct dmabuf_src {
	int fd;			/* dumb source fd, -1 if GBM/none */
	int owned;		/* fd is ours and must be closed */
	int gbm_fd;		/* render node fd backing gbm_dev */
	void *gbm_dev;
	const char *label;
	char path[160];
};

static int prop_lookup(int fd, drmModeObjectProperties *props,
		       const char *name, uint32_t *pid, uint64_t *val)
{
	unsigned int i;

	if (!props)
		return -1;
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		int match;

		if (!p)
			continue;
		match = !strcmp(p->name, name);
		drmModeFreeProperty(p);
		if (match) {
			if (pid)
				*pid = props->props[i];
			if (val)
				*val = props->prop_values[i];
			return 0;
		}
	}
	return -1;
}

static int need_prop(int fd, drmModeObjectProperties *props,
		     const char *obj, const char *name, uint32_t *pid)
{
	if (prop_lookup(fd, props, name, pid, NULL) < 0) {
		fprintf(stderr, "kmsflip: %s has no \"%s\" property "
			"(atomic KMS incomplete?)\n", obj, name);
		return -1;
	}
	return 0;
}

static uint32_t pick_crtc_for_encoder(drmModeRes *res, drmModeEncoder *enc)
{
	int i;

	if (enc->crtc_id)
		return enc->crtc_id;
	for (i = 0; i < res->count_crtcs; i++)
		if (enc->possible_crtcs & (1u << i))
			return res->crtcs[i];
	return 0;
}

static int find_primary_plane(struct kms_ctx *k)
{
	drmModePlaneRes *pr;
	int i;

	pr = drmModeGetPlaneResources(k->fd);
	if (!pr) {
		fprintf(stderr, "kmsflip: drmModeGetPlaneResources: %s\n",
			strerror(errno));
		return -1;
	}
	for (i = 0; i < (int)pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(k->fd, pr->planes[i]);
		drmModeObjectProperties *props;
		uint64_t type = 0;

		if (!p)
			continue;
		if (!(p->possible_crtcs & (1u << k->crtc_index))) {
			drmModeFreePlane(p);
			continue;
		}
		props = drmModeObjectGetProperties(k->fd, p->plane_id,
						   DRM_MODE_OBJECT_PLANE);
		if (!props || prop_lookup(k->fd, props, "type", NULL, &type) < 0 ||
		    type != DRM_PLANE_TYPE_PRIMARY) {
			if (props)
				drmModeFreeObjectProperties(props);
			drmModeFreePlane(p);
			continue;
		}
		k->plane = p;
		k->plane_props = props;
		break;
	}
	drmModeFreePlaneResources(pr);
	if (!k->plane) {
		fprintf(stderr, "kmsflip: no primary plane for CRTC %u\n",
			k->crtc_id);
		return -1;
	}
	return 0;
}

static int kms_setup(struct kms_ctx *k, const char *dev, int conn_id,
		     int crtc_id, const char *mode_str)
{
	int i;

	memset(k, 0, sizeof(*k));
	k->fd = -1;

	k->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (k->fd < 0) {
		fprintf(stderr, "kmsflip: open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (drmSetMaster(k->fd) < 0)
		fprintf(stderr, "kmsflip: warning: drmSetMaster: %s\n",
			strerror(errno));

	if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
		fprintf(stderr,
			"kmsflip: DRM_CLIENT_CAP_ATOMIC unsupported: %s\n",
			strerror(errno));
		return 1;
	}
	if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
		fprintf(stderr, "kmsflip: universal planes cap: %s\n",
			strerror(errno));

	k->res = drmModeGetResources(k->fd);
	if (!k->res) {
		fprintf(stderr, "kmsflip: drmModeGetResources: %s\n",
			strerror(errno));
		return 1;
	}

	if (conn_id > 0) {
		for (i = 0; i < k->res->count_connectors; i++)
			if (k->res->connectors[i] == (uint32_t)conn_id)
				break;
		if (i == k->res->count_connectors) {
			fprintf(stderr, "kmsflip: connector %d not found\n",
				conn_id);
			return 1;
		}
		k->conn = drmModeGetConnector(k->fd, conn_id);
	} else {
		for (i = 0; i < k->res->count_connectors; i++) {
			drmModeConnector *c =
				drmModeGetConnector(k->fd, k->res->connectors[i]);

			if (!c)
				continue;
			if (c->connection == DRM_MODE_CONNECTED &&
			    c->count_modes > 0) {
				k->conn = c;
				break;
			}
			drmModeFreeConnector(c);
		}
	}
	if (!k->conn) {
		fprintf(stderr, "kmsflip: no connected connector with modes\n");
		return 1;
	}
	k->conn_id = k->conn->connector_id;

	if (mode_str) {
		int w, h;
		int found = 0;

		if (sscanf(mode_str, "%dx%d", &w, &h) != 2) {
			fprintf(stderr, "kmsflip: --mode wants WxH (got %s)\n",
				mode_str);
			return 1;
		}
		for (i = 0; i < k->conn->count_modes; i++) {
			drmModeModeInfo *m = &k->conn->modes[i];

			if (m->hdisplay == w && m->vdisplay == h &&
			    !(m->type & DRM_MODE_TYPE_PREFERRED)) {
				k->mode = *m;
				found = 1;
			}
		}
		if (!found)
			for (i = 0; i < k->conn->count_modes; i++) {
				drmModeModeInfo *m = &k->conn->modes[i];

				if (m->hdisplay == w && m->vdisplay == h) {
					k->mode = *m;
					found = 1;
					break;
				}
			}
		if (!found) {
			fprintf(stderr, "kmsflip: no mode %s on connector %u\n",
				mode_str, k->conn_id);
			return 1;
		}
	} else {
		int found = 0;

		for (i = 0; i < k->conn->count_modes; i++)
			if (k->conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				k->mode = k->conn->modes[i];
				found = 1;
				break;
			}
		if (!found && k->conn->count_modes) {
			k->mode = k->conn->modes[0];
			found = 1;
		}
		if (!found) {
			fprintf(stderr, "kmsflip: connector %u has no modes\n",
				k->conn_id);
			return 1;
		}
	}

	if (k->conn->encoder_id)
		k->enc = drmModeGetEncoder(k->fd, k->conn->encoder_id);
	if (!k->enc)
		for (i = 0; i < k->conn->count_encoders && !k->enc; i++)
			k->enc = drmModeGetEncoder(k->fd, k->conn->encoders[i]);
	if (!k->enc)
		for (i = 0; i < k->res->count_encoders; i++) {
			drmModeEncoder *e =
				drmModeGetEncoder(k->fd, k->res->encoders[i]);

			if (!e)
				continue;
			if (e->possible_crtcs) {
				k->enc = e;
				break;
			}
			drmModeFreeEncoder(e);
		}
	if (!k->enc) {
		fprintf(stderr, "kmsflip: no encoder for connector %u\n",
			k->conn_id);
		return 1;
	}

	k->crtc_id = crtc_id > 0 ? (uint32_t)crtc_id :
				   pick_crtc_for_encoder(k->res, k->enc);
	if (!k->crtc_id) {
		fprintf(stderr, "kmsflip: no usable CRTC\n");
		return 1;
	}
	for (i = 0; i < k->res->count_crtcs; i++)
		if (k->res->crtcs[i] == k->crtc_id) {
			k->crtc_index = (uint32_t)i;
			break;
		}
	k->saved = drmModeGetCrtc(k->fd, k->crtc_id);

	if (find_primary_plane(k) < 0)
		return 1;

	k->conn_props = drmModeObjectGetProperties(k->fd, k->conn_id,
						   DRM_MODE_OBJECT_CONNECTOR);
	k->crtc_props = drmModeObjectGetProperties(k->fd, k->crtc_id,
						   DRM_MODE_OBJECT_CRTC);
	if (need_prop(k->fd, k->conn_props, "connector", "CRTC_ID",
		      &k->conn_crtc_id) < 0 ||
	    need_prop(k->fd, k->crtc_props, "crtc", "ACTIVE",
		      &k->crtc_active) < 0 ||
	    need_prop(k->fd, k->crtc_props, "crtc", "MODE_ID",
		      &k->crtc_mode_id) < 0)
		return 1;
	if (need_prop(k->fd, k->plane_props, "primary plane", "FB_ID",
		      &k->plane_fb_id) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "CRTC_ID",
		      &k->plane_crtc_id) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "SRC_X",
		      &k->plane_src_x) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "SRC_Y",
		      &k->plane_src_y) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "SRC_W",
		      &k->plane_src_w) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "SRC_H",
		      &k->plane_src_h) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "CRTC_X",
		      &k->plane_crtc_x) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "CRTC_Y",
		      &k->plane_crtc_y) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "CRTC_W",
		      &k->plane_crtc_w) < 0 ||
	    need_prop(k->fd, k->plane_props, "primary plane", "CRTC_H",
		      &k->plane_crtc_h) < 0)
		return 1;

	if (drmModeCreatePropertyBlob(k->fd, &k->mode, sizeof(k->mode),
				      &k->mode_blob) < 0) {
		fprintf(stderr, "kmsflip: create MODE_ID blob: %s\n",
			strerror(errno));
		return 1;
	}

	printf("KMSFLIP: dev=%s connector=%u crtc=%u plane=%u mode=%s "
	       "(%dx%d@%d) atomic=1 saved_fb=%u\n", dev, k->conn_id,
	       k->crtc_id, k->plane->plane_id, k->mode.name,
	       k->mode.hdisplay, k->mode.vdisplay, k->mode.vrefresh,
	       k->saved ? k->saved->buffer_id : 0);
	return 0;
}

static int req_add(drmModeAtomicReq *req, uint32_t obj, uint32_t prop,
		   uint64_t val, const char *what, struct flip_stats *st)
{
	if (!prop) {
		fprintf(stderr, "kmsflip: %s: missing property id for %s\n",
			st && st->phase ? st->phase : "kms", what);
		if (st)
			st->other++;
		return -1;
	}
	if (drmModeAtomicAddProperty(req, obj, prop, val) < 0) {
		fprintf(stderr, "kmsflip: %s: add %s: %s\n",
			st && st->phase ? st->phase : "kms", what,
			strerror(errno));
		if (st)
			st->other++;
		return -1;
	}
	return 0;
}

/*
 * Non-blocking atomic commit.  Retries EBUSY (previous flip still pending)
 * only inside the caller's timeout.  EINVAL is counted separately because
 * it is the signature of a property/format rejection and never benefits
 * from a retry.
 */
static int atomic_commit(struct kms_ctx *k, uint32_t fb, int modeset,
			 unsigned long token, int timeout_ms,
			 struct flip_stats *st)
{
	drmModeAtomicReq *req;
	uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
	double end;
	int alt_flags = 0;	/* modeset EINVAL retried with NONBLOCK */
	int rc = 0;

	req = drmModeAtomicAlloc();
	if (!req) {
		st->other++;
		return -1;
	}

	if (req_add(req, k->conn_id, k->conn_crtc_id, k->crtc_id,
		    "connector CRTC_ID", st) ||
	    req_add(req, k->crtc_id, k->crtc_active, 1, "CRTC ACTIVE", st) ||
	    req_add(req, k->plane->plane_id, k->plane_crtc_id, k->crtc_id,
		    "plane CRTC_ID", st) ||
	    req_add(req, k->plane->plane_id, k->plane_fb_id, fb,
		    "plane FB_ID", st) ||
	    req_add(req, k->plane->plane_id, k->plane_src_x, PLANE_SRC_FULL,
		    "plane SRC_X", st) ||
	    req_add(req, k->plane->plane_id, k->plane_src_y, PLANE_SRC_FULL,
		    "plane SRC_Y", st) ||
	    req_add(req, k->plane->plane_id, k->plane_src_w,
		    (uint64_t)k->mode.hdisplay << 16, "plane SRC_W", st) ||
	    req_add(req, k->plane->plane_id, k->plane_src_h,
		    (uint64_t)k->mode.vdisplay << 16, "plane SRC_H", st) ||
	    req_add(req, k->plane->plane_id, k->plane_crtc_x, 0,
		    "plane CRTC_X", st) ||
	    req_add(req, k->plane->plane_id, k->plane_crtc_y, 0,
		    "plane CRTC_Y", st) ||
	    req_add(req, k->plane->plane_id, k->plane_crtc_w,
		    k->mode.hdisplay, "plane CRTC_W", st) ||
	    req_add(req, k->plane->plane_id, k->plane_crtc_h,
		    k->mode.vdisplay, "plane CRTC_H", st)) {
		drmModeAtomicFree(req);
		return -1;
	}
	if (modeset) {
		if (req_add(req, k->crtc_id, k->crtc_mode_id, k->mode_blob,
			    "CRTC MODE_ID", st)) {
			drmModeAtomicFree(req);
			return -1;
		}
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	} else {
		flags |= DRM_MODE_ATOMIC_NONBLOCK;
	}

	end = now_ms() + timeout_ms;
	for (;;) {
		int e;

		st->commits++;
		errno = 0;
		if (drmModeAtomicCommit(k->fd, req, flags,
					(void *)(uintptr_t)token) == 0) {
			st->commit_ok++;
			break;
		}
		e = err_from(-1);
		if (e == EBUSY) {
			st->ebusy++;
			if (now_ms() < end) {
				usleep(1000);
				continue;
			}
			st->commit_fail++;
			fprintf(stderr, "kmsflip: %s: atomic commit EBUSY "
				"for %d ms\n", st->phase, timeout_ms);
			rc = -EBUSY;
			break;
		}
		if (e == EINVAL && modeset && !alt_flags) {
			/* Some 3.10-era backports reject a blocking
			 * modeset; try the NONBLOCK variant before
			 * declaring a property rejection. */
			flags |= DRM_MODE_ATOMIC_NONBLOCK;
			alt_flags = 1;
			fprintf(stderr, "kmsflip: %s: atomic modeset EINVAL, "
				"retrying with NONBLOCK\n", st->phase);
			continue;
		}
		if (e == EINVAL)
			st->einval++;
		else
			st->other++;
		st->commit_fail++;
		fprintf(stderr, "kmsflip: %s: atomic commit: %s\n",
			st->phase, strerror(e));
		rc = -e;
		break;
	}
	drmModeAtomicFree(req);
	return rc;
}

static int wait_flip(struct kms_ctx *k, unsigned long token, int timeout_ms,
		     struct flip_stats *st)
{
	double end = now_ms() + timeout_ms;

	while (!g_stop) {
		double left = end - now_ms();
		struct pollfd pfd;
		int pr;

		if (g_done_token == token)
			return 0;
		if (left <= 0)
			return -ETIMEDOUT;
		pfd.fd = k->fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		pr = poll(&pfd, 1, (int)(left > 20.0 ? 20.0 : left < 1.0 ? 1.0 : left));
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "kmsflip: %s: poll: %s\n", st->phase,
				strerror(errno));
			return -errno;
		}
		if (pr == 0)
			continue;
		if (pfd.revents & (POLLIN | POLLERR | POLLHUP)) {
			if (drmHandleEvent(k->fd, &g_ectx) < 0) {
				fprintf(stderr, "kmsflip: %s: drmHandleEvent: "
					"%s\n", st->phase, strerror(errno));
				return -EIO;
			}
		}
	}
	return -EINTR;
}

/* Consume any event that arrived after a timeout so it cannot leak. */
static void drain_events(struct kms_ctx *k)
{
	for (;;) {
		struct pollfd pfd = { k->fd, POLLIN, 0 };

		if (poll(&pfd, 1, 0) <= 0)
			break;
		if (drmHandleEvent(k->fd, &g_ectx) < 0)
			break;
	}
}

/* ------------------------------------------------------- buffer handling */

static void buf_zero(struct flip_buf *b)
{
	memset(b, 0, sizeof(*b));
	b->src_fd = -1;
	b->prime_fd = -1;
}

static void buf_destroy(struct kms_ctx *k, struct flip_buf *b)
{
	int i;

	if (!k || k->fd < 0 || !b)
		return;

	if (b->fb) {
		for (i = 0; i < 25; i++) {
			errno = 0;
			if (drmModeRmFB(k->fd, b->fb) == 0) {
				b->fb = 0;
				break;
			}
			if (errno != EBUSY)
				break;
			usleep(20000);
		}
		if (b->fb)
			fprintf(stderr, "kmsflip: RmFB %u stayed busy; "
				"leaving it to the kernel\n", b->fb);
	}
	if (b->imported) {
		if (b->import_handle && b->import_handle != b->dumb_handle)
			drmCloseBufferHandle(k->fd, b->import_handle);
		b->import_handle = 0;
		b->imported = 0;
	}
	if (b->prime_fd >= 0) {
		close(b->prime_fd);
		b->prime_fd = -1;
	}
#if HAVE_EGL
	if (b->gbm_bo) {
		if (b->gbm_map_data)
			gbm_bo_unmap(b->gbm_bo, b->gbm_map_data);
		gbm_bo_destroy(b->gbm_bo);
		b->gbm_bo = NULL;
		b->gbm_map_data = NULL;
		b->map = NULL;
	}
#else
	(void)i;
#endif
	if (b->map) {
		if (b->size)
			munmap(b->map, b->size);
		b->map = NULL;
	}
	if (b->dumb_handle && b->src_fd >= 0)
		drmModeDestroyDumbBuffer(b->src_fd, b->dumb_handle);
	b->dumb_handle = 0;
	b->src_fd = -1;
}

static int buf_addfb(struct kms_ctx *k, struct flip_buf *b, uint32_t handle,
		     int w, int h, struct flip_stats *st)
{
	uint32_t handles[4] = { handle, 0, 0, 0 };
	uint32_t pitches[4] = { b->pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	int ret, e;

	errno = 0;
	ret = drmModeAddFB2(k->fd, (uint32_t)w, (uint32_t)h,
			    DRM_FORMAT_XRGB8888, handles, pitches, offsets,
			    &b->fb, 0);
	if (ret == 0)
		return 0;
	e = err_from(ret);
	fprintf(stderr, "kmsflip: %s: AddFB2: %s, retrying legacy AddFB\n",
		st->phase, strerror(e));
	st->addfb_fallback++;

	errno = 0;
	ret = drmModeAddFB(k->fd, (uint32_t)w, (uint32_t)h, 24, 32, b->pitch,
			   handle, &b->fb);
	if (ret == 0)
		return 0;
	e = err_from(ret);
	if (e == EINVAL)
		st->einval++;
	else
		st->other++;
	fprintf(stderr, "kmsflip: %s: AddFB: %s\n", st->phase, strerror(e));
	return -1;
}

static int buf_dumb_create(struct kms_ctx *k, struct flip_buf *b, int w,
			   int h, int fd, struct flip_stats *st)
{
	uint64_t off = 0;
	int ret, e;

	buf_zero(b);
	errno = 0;
	ret = drmModeCreateDumbBuffer(fd, (uint32_t)w, (uint32_t)h, 32, 0,
				      &b->dumb_handle, &b->pitch, &b->size);
	if (ret < 0) {
		e = err_from(ret);
		fprintf(stderr, "kmsflip: %s: dumb_create %dx%d on fd %d: %s\n",
			st->phase, w, h, fd, strerror(e));
		if (e == EINVAL)
			st->einval++;
		else
			st->other++;
		return -1;
	}
	b->src_fd = fd;		/* own cleanup from here on */
	errno = 0;
	if (drmModeMapDumbBuffer(fd, b->dumb_handle, &off) < 0) {
		e = err_from(-1);
		fprintf(stderr, "kmsflip: %s: map dumb: %s\n", st->phase,
			strerror(e));
		st->other++;
		buf_destroy(k, b);
		return -1;
	}
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		      (off_t)off);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		e = errno;
		fprintf(stderr, "kmsflip: %s: mmap dumb: %s\n", st->phase,
			strerror(e));
		st->other++;
		buf_destroy(k, b);
		return -1;
	}
	return 0;
}

/* card0 dumb buffer -> FB (phase dumb). */
static int buf_dumb_fb(struct kms_ctx *k, struct flip_buf *b, int w, int h,
		       struct flip_stats *st)
{
	if (buf_dumb_create(k, b, w, h, k->fd, st) < 0)
		return -1;
	if (buf_addfb(k, b, b->dumb_handle, w, h, st) < 0) {
		buf_destroy(k, b);
		return -1;
	}
	return 0;
}

/* dumb buffer on @srcfd -> PRIME export -> import into card0 -> FB. */
static int buf_prime_fb(struct kms_ctx *k, struct flip_buf *b, int w, int h,
			int srcfd, struct flip_stats *st)
{
	int ret, e;

	if (buf_dumb_create(k, b, w, h, srcfd, st) < 0)
		return -1;
	errno = 0;
	ret = drmPrimeHandleToFD(srcfd, b->dumb_handle,
				 DRM_CLOEXEC | DRM_RDWR, &b->prime_fd);
	if (ret < 0) {
		e = err_from(ret);
		fprintf(stderr, "kmsflip: %s: PRIME export handle %u: %s\n",
			st->phase, b->dumb_handle, strerror(e));
		if (e == EINVAL)
			st->einval++;
		else
			st->other++;
		buf_destroy(k, b);
		return -1;
	}
	errno = 0;
	ret = drmPrimeFDToHandle(k->fd, b->prime_fd, &b->import_handle);
	if (ret < 0) {
		e = err_from(ret);
		fprintf(stderr, "kmsflip: %s: PRIME import fd %d: %s\n",
			st->phase, b->prime_fd, strerror(e));
		if (e == EINVAL)
			st->einval++;
		else
			st->other++;
		buf_destroy(k, b);
		return -1;
	}
	b->imported = 1;
	if (buf_addfb(k, b, b->import_handle, w, h, st) < 0) {
		buf_destroy(k, b);
		return -1;
	}
	return 0;
}

#if HAVE_EGL
/* GBM linear BO on the render node -> PRIME export -> import -> FB. */
static int buf_gbm_fb(struct kms_ctx *k, struct flip_buf *b, int w, int h,
		      struct dmabuf_src *src, struct flip_stats *st)
{
	struct gbm_bo *bo;
	uint32_t stride = 0;
	void *map_data = NULL, *p;
	int ret, e;

	buf_zero(b);
	bo = gbm_bo_create(src->gbm_dev, (uint32_t)w, (uint32_t)h,
			   GBM_FORMAT_XRGB8888,
			   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	if (!bo)
		bo = gbm_bo_create(src->gbm_dev, (uint32_t)w, (uint32_t)h,
				   GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR);
	if (!bo) {
		fprintf(stderr, "kmsflip: %s: gbm_bo_create %dx%d failed\n",
			st->phase, w, h);
		st->other++;
		return -1;
	}
	if (gbm_bo_get_modifier(bo) != 0) {
		fprintf(stderr, "kmsflip: %s: GBM gave modifier 0x%llx, "
			"need linear\n", st->phase,
			(unsigned long long)gbm_bo_get_modifier(bo));
		gbm_bo_destroy(bo);
		st->other++;
		return -1;
	}
	p = gbm_bo_map(bo, 0, 0, (uint32_t)w, (uint32_t)h,
		       GBM_BO_TRANSFER_WRITE, &stride, &map_data);
	if (!p) {
		fprintf(stderr, "kmsflip: %s: gbm_bo_map failed\n",
			st->phase);
		gbm_bo_destroy(bo);
		st->other++;
		return -1;
	}
	b->gbm_bo = bo;
	b->gbm_map_data = map_data;
	b->map = p;
	b->pitch = stride ? stride : (uint32_t)w * 4;
	b->size = (uint64_t)b->pitch * (uint64_t)h;
	b->prime_fd = gbm_bo_get_fd(bo);
	if (b->prime_fd < 0) {
		b->prime_fd = -1;
		fprintf(stderr, "kmsflip: %s: gbm_bo_get_fd failed\n",
			st->phase);
		st->other++;
		buf_destroy(k, b);
		return -1;
	}
	errno = 0;
	ret = drmPrimeFDToHandle(k->fd, b->prime_fd, &b->import_handle);
	if (ret < 0) {
		e = err_from(ret);
		fprintf(stderr, "kmsflip: %s: PRIME import GBM fd: %s\n",
			st->phase, strerror(e));
		if (e == EINVAL)
			st->einval++;
		else
			st->other++;
		buf_destroy(k, b);
		return -1;
	}
	b->imported = 1;
	if (buf_addfb(k, b, b->import_handle, w, h, st) < 0) {
		buf_destroy(k, b);
		return -1;
	}
	return 0;
}
#endif

/* ------------------------------------------------------------- phases */

static int run_flip_phase(struct kms_ctx *k, const char *name,
			  const char *source, struct flip_buf *b, int nbuf,
			  int frames, int timeout_ms, int kind,
			  const char *dump, struct flip_stats *st)
{
	int w = k->mode.hdisplay, h = k->mode.vdisplay;
	unsigned long token;
	int need_modeset = !k->did_modeset;
	int i, cur = 0, rc = 0;
	double t0;

	memset(st, 0, sizeof(*st));
	st->phase = name;
	g_token_base = ++g_next_token;	/* first token of this phase */
	g_done_token = 0;
	g_cur_stats = st;
	t0 = now_ms();

	fill_buffer(b[0].map, b[0].pitch, w, h, kind, 0);
	b[0].csum = fnv1a_rows(b[0].map, b[0].pitch, (size_t)w * 4, h);

	token = g_next_token++;
	if (atomic_commit(k, b[0].fb, need_modeset, token, timeout_ms, st)) {
		rc = -1;
		goto out;
	}
	if (wait_flip(k, token, timeout_ms, st)) {
		st->timeouts++;
		fprintf(stderr, "kmsflip: %s: initial commit timed out\n",
			name);
		drain_events(k);
		rc = -1;
		goto out;
	}
	k->did_modeset = 1;
	st->modeset = need_modeset ? 1 : 0;
	if (fnv1a_rows(b[0].map, b[0].pitch, (size_t)w * 4, h) != b[0].csum) {
		st->checksum_bad++;
		fprintf(stderr, "kmsflip: %s: modeset buffer checksum "
			"changed\n", name);
	}

	for (i = 0; i < frames && !g_stop; i++) {
		int nxt = (cur + 1) % nbuf;

		fill_buffer(b[nxt].map, b[nxt].pitch, w, h, kind, i + 1);
		b[nxt].csum = fnv1a_rows(b[nxt].map, b[nxt].pitch,
					 (size_t)w * 4, h);
		token = g_next_token++;
		if (atomic_commit(k, b[nxt].fb, 0, token, timeout_ms, st)) {
			rc = -1;
			break;
		}
		if (wait_flip(k, token, timeout_ms, st)) {
			st->timeouts++;
			fprintf(stderr, "kmsflip: %s: flip %d timed out\n",
				name, i);
			drain_events(k);
			rc = -1;
			break;
		}
		st->flips++;
		cur = nxt;
		if (fnv1a_rows(b[nxt].map, b[nxt].pitch, (size_t)w * 4, h) !=
		    b[nxt].csum) {
			st->checksum_bad++;
			fprintf(stderr, "kmsflip: %s: flip %d checksum "
				"mismatch (buffer changed under scanout)\n",
				name, i);
		}
	}

out:
	st->elapsed_ms = now_ms() - t0;
	if (g_stop && rc == 0) {
		fprintf(stderr, "kmsflip: %s: aborted by signal\n", name);
		rc = -1;
	}
	if (rc == 0 && st->flips != (unsigned)frames)
		rc = -1;
	if (rc == 0 && st->events != (unsigned)frames + 1) {
		fprintf(stderr, "kmsflip: %s: %u completion events, "
			"expected %d\n", name, st->events, frames + 1);
		rc = -1;
	}
	if (rc == 0 && (st->timeouts || st->einval || st->other ||
			st->checksum_bad || st->commit_fail))
		rc = -1;

	if (dump && b[cur].map) {
		if (dump_ppm(dump, b[cur].map, b[cur].pitch, w, h) == 0)
			printf("KMSFLIP: phase=%s dumped=%s\n", name, dump);
	}

	printf("KMSFLIP: phase=%s source=%s modeset=%u flips=%u/%d "
	       "commits=%u commit_ok=%u events=%u timeouts=%u ebusy=%u "
	       "einval=%u other=%u checksum_bad=%u addfb_fallback=%u "
	       "elapsed_ms=%.0f %s\n", name, source, st->modeset, st->flips,
	       frames, st->commits, st->commit_ok, st->events, st->timeouts,
	       st->ebusy, st->einval, st->other, st->checksum_bad,
	       st->addfb_fallback, st->elapsed_ms, rc ? "FAIL" : "PASS");
	g_cur_stats = NULL;
	return rc;
}

static int phase_dumb(struct kms_ctx *k, int frames, int timeout_ms,
		      int kind, const char *dump, struct flip_stats *st,
		      struct flip_buf b[2])
{
	int i, rc;

	memset(st, 0, sizeof(*st));
	st->phase = "dumb";
	for (i = 0; i < 2; i++)
		buf_zero(&b[i]);
	for (i = 0; i < 2; i++) {
		if (buf_dumb_fb(k, &b[i], k->mode.hdisplay,
				k->mode.vdisplay, st) < 0) {
			st->setup_fail = 1;
			while (i-- > 0)
				buf_destroy(k, &b[i]);
			printf("KMSFLIP: phase=dumb source=dumb setup_fail=1 "
			       "commits=%u commit_ok=%u events=%u timeouts=%u "
			       "ebusy=%u einval=%u other=%u FAIL\n",
			       st->commits, st->commit_ok, st->events,
			       st->timeouts, st->ebusy, st->einval, st->other);
			return 1;
		}
	}
	rc = run_flip_phase(k, "dumb", "dumb", b, 2, frames, timeout_ms,
			    kind, dump, st);
	return rc;
}

/*
 * Pick the dma-buf source and create both buffers.  Order:
 *   panfrost-gbm  (GBM linear BO on the render node, built with EGL/GBM)
 *   panfrost-dumb (dumb buffer on the render node + PRIME export)
 *   card-prime    (card0 dumb buffer exported and re-imported)
 */
static int phase_dmabuf(struct kms_ctx *k, struct dmabuf_src *src,
			int frames, int timeout_ms, int kind,
			const char *dump, const char *render_node,
			int force_self, struct flip_stats *st,
			struct flip_buf b[2])
{
	int have = 0, i, rc;

	memset(st, 0, sizeof(*st));
	st->phase = "dmabuf";
	memset(src, 0, sizeof(*src));
	src->fd = -1;
	src->gbm_fd = -1;
	src->label = "none";
	for (i = 0; i < 2; i++)
		buf_zero(&b[i]);

	if (!force_self && render_node && render_node[0] && !g_stop) {
#if HAVE_EGL
		src->gbm_fd = open(render_node, O_RDWR | O_CLOEXEC);
		if (src->gbm_fd >= 0)
			src->gbm_dev = gbm_create_device(src->gbm_fd);
		if (src->gbm_dev) {
			src->label = "panfrost-gbm";
			snprintf(src->path, sizeof(src->path), "gbm:%s",
				 render_node);
			for (i = 0; i < 2; i++)
				if (buf_gbm_fb(k, &b[i], k->mode.hdisplay,
					       k->mode.vdisplay, src,
					       st) < 0)
					break;
			if (i == 2) {
				have = 1;
			} else {
				while (i-- > 0)
					buf_destroy(k, &b[i]);
			}
		}
		if (!have) {
			if (src->gbm_dev) {
				gbm_device_destroy(src->gbm_dev);
				src->gbm_dev = NULL;
			}
			if (src->gbm_fd >= 0) {
				close(src->gbm_fd);
				src->gbm_fd = -1;
			}
		}
#endif
		if (!have) {
			src->fd = open(render_node, O_RDWR | O_CLOEXEC);
			if (src->fd >= 0) {
				src->owned = 1;
				src->label = "panfrost-dumb";
				snprintf(src->path, sizeof(src->path), "%s",
					 render_node);
				for (i = 0; i < 2; i++)
					if (buf_prime_fb(k, &b[i],
							 k->mode.hdisplay,
							 k->mode.vdisplay,
							 src->fd, st) < 0)
						break;
				if (i == 2) {
					have = 1;
				} else {
					while (i-- > 0)
						buf_destroy(k, &b[i]);
					close(src->fd);
					src->fd = -1;
					src->owned = 0;
				}
			}
		}
	}
	if (!have) {
		src->fd = k->fd;
		src->owned = 0;
		src->label = "card-prime";
		snprintf(src->path, sizeof(src->path), "card:%d", k->fd);
		for (i = 0; i < 2; i++)
			if (buf_prime_fb(k, &b[i], k->mode.hdisplay,
					 k->mode.vdisplay, src->fd, st) < 0)
				break;
		if (i == 2) {
			have = 1;
		} else {
			while (i-- > 0)
				buf_destroy(k, &b[i]);
			src->fd = -1;
		}
	}

	if (!have) {
		st->setup_fail = 1;
		printf("KMSFLIP: phase=dmabuf source=%s setup_fail=1 "
		       "commits=%u commit_ok=%u events=%u timeouts=%u "
		       "ebusy=%u einval=%u other=%u FAIL\n", src->label,
		       st->commits, st->commit_ok, st->events, st->timeouts,
		       st->ebusy, st->einval, st->other);
#if HAVE_EGL
		if (src->gbm_dev) {
			gbm_device_destroy(src->gbm_dev);
			src->gbm_dev = NULL;
		}
#endif
		if (src->gbm_fd >= 0) {
			close(src->gbm_fd);
			src->gbm_fd = -1;
		}
		if (src->fd >= 0 && src->owned) {
			close(src->fd);
			src->fd = -1;
			src->owned = 0;
		}
		return 1;
	}

	printf("KMSFLIP: dmabuf source=%s path=%s\n", src->label, src->path);
	rc = run_flip_phase(k, "dmabuf", src->label, b, 2, frames,
			    timeout_ms, kind, dump, st);
	return rc;
}

static void dmabuf_src_close(struct dmabuf_src *src)
{
#if HAVE_EGL
	if (src->gbm_dev) {
		gbm_device_destroy(src->gbm_dev);
		src->gbm_dev = NULL;
	}
#endif
	if (src->gbm_fd >= 0) {
		close(src->gbm_fd);
		src->gbm_fd = -1;
	}
	if (src->fd >= 0 && src->owned)
		close(src->fd);
	src->fd = -1;
	src->owned = 0;
}

/* --------------------------------------------------------- restore/teardown */

static int kms_restore(struct kms_ctx *k)
{
	struct flip_stats st = { .phase = "restore" };
	drmModeAtomicReq *req;
	uint32_t blob = 0;
	int ok = 0;

	if (!k || k->fd < 0)
		return -1;
	if (!k->did_modeset)
		return 0;

	req = drmModeAtomicAlloc();
	if (req) {
		int good = 1;

		good &= req_add(req, k->conn_id, k->conn_crtc_id, k->crtc_id,
				"connector CRTC_ID", &st) == 0;
		if (k->saved && k->saved->buffer_id && k->saved->mode_valid) {
			if (drmModeCreatePropertyBlob(k->fd, &k->saved->mode,
						      sizeof(k->saved->mode),
						      &blob) == 0) {
				good &= req_add(req, k->crtc_id,
						k->crtc_active, 1,
						"CRTC ACTIVE", &st) == 0;
				good &= req_add(req, k->crtc_id,
						k->crtc_mode_id, blob,
						"CRTC MODE_ID", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_crtc_id, k->crtc_id,
						"plane CRTC_ID", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_fb_id,
						k->saved->buffer_id,
						"plane FB_ID", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_src_x, 0,
						"plane SRC_X", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_src_y, 0,
						"plane SRC_Y", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_src_w,
						(uint64_t)k->saved->mode.hdisplay << 16,
						"plane SRC_W", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_src_h,
						(uint64_t)k->saved->mode.vdisplay << 16,
						"plane SRC_H", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_crtc_x, k->saved->x,
						"plane CRTC_X", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_crtc_y, k->saved->y,
						"plane CRTC_Y", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_crtc_w,
						k->saved->mode.hdisplay,
						"plane CRTC_W", &st) == 0;
				good &= req_add(req, k->plane->plane_id,
						k->plane_crtc_h,
						k->saved->mode.vdisplay,
						"plane CRTC_H", &st) == 0;
			} else {
				good = 0;
			}
		} else {
			good &= req_add(req, k->crtc_id, k->crtc_active, 0,
					"CRTC ACTIVE", &st) == 0;
			good &= req_add(req, k->conn_id, k->conn_crtc_id, 0,
					"connector CRTC_ID", &st) == 0;
			good &= req_add(req, k->plane->plane_id,
					k->plane_fb_id, 0,
					"plane FB_ID", &st) == 0;
			good &= req_add(req, k->plane->plane_id,
					k->plane_crtc_id, 0,
					"plane CRTC_ID", &st) == 0;
		}
		if (good) {
			errno = 0;
			if (drmModeAtomicCommit(k->fd, req,
						DRM_MODE_ATOMIC_ALLOW_MODESET,
						NULL) == 0)
				ok = 1;
			else
				fprintf(stderr, "kmsflip: restore atomic: "
					"%s\n", strerror(errno));
		}
		drmModeAtomicFree(req);
		if (blob)
			drmModeDestroyPropertyBlob(k->fd, blob);
	}

	if (!ok && k->saved) {
		errno = 0;
		if (k->saved->buffer_id) {
			if (drmModeSetCrtc(k->fd, k->crtc_id,
					   k->saved->buffer_id, k->saved->x,
					   k->saved->y, &k->conn_id, 1,
					   &k->saved->mode) == 0)
				ok = 2;
		} else if (drmModeSetCrtc(k->fd, k->crtc_id, 0, 0, 0, NULL,
					  0, NULL) == 0) {
			ok = 2;
		}
		if (!ok)
			fprintf(stderr, "kmsflip: restore legacy: %s\n",
				strerror(errno));
	}
	printf("KMSFLIP: restore=%s saved_fb=%u\n",
	       ok == 1 ? "atomic" : ok == 2 ? "legacy" : "fail",
	       k->saved ? k->saved->buffer_id : 0);
	return ok ? 0 : -1;
}

static void kms_teardown(struct kms_ctx *k)
{
	if (!k || k->fd < 0)
		return;
	if (k->mode_blob)
		drmModeDestroyPropertyBlob(k->fd, k->mode_blob);
	k->mode_blob = 0;
	if (k->conn_props)
		drmModeFreeObjectProperties(k->conn_props);
	if (k->crtc_props)
		drmModeFreeObjectProperties(k->crtc_props);
	if (k->plane_props)
		drmModeFreeObjectProperties(k->plane_props);
	k->conn_props = k->crtc_props = k->plane_props = NULL;
	if (k->plane)
		drmModeFreePlane(k->plane);
	k->plane = NULL;
	if (k->saved)
		drmModeFreeCrtc(k->saved);
	k->saved = NULL;
	if (k->enc)
		drmModeFreeEncoder(k->enc);
	k->enc = NULL;
	if (k->conn)
		drmModeFreeConnector(k->conn);
	k->conn = NULL;
	if (k->res)
		drmModeFreeResources(k->res);
	k->res = NULL;
	drmDropMaster(k->fd);
	close(k->fd);
	k->fd = -1;
}

/* ---------------------------------------------------------------- probe */

#if HAVE_EGL
static int probe_gbm_buf(void *gbm_dev, int fd, int w, int h,
			 unsigned *export_ok, unsigned *import_ok,
			 unsigned *import_err, unsigned *einval,
			 unsigned *other)
{
	struct gbm_bo *bo;
	uint32_t stride = 0, handle = 0;
	void *map_data = NULL, *p;
	int pfd, ret, e;

	bo = gbm_bo_create(gbm_dev, (uint32_t)w, (uint32_t)h,
			   GBM_FORMAT_XRGB8888,
			   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	if (!bo)
		bo = gbm_bo_create(gbm_dev, (uint32_t)w, (uint32_t)h,
				   GBM_FORMAT_XRGB8888, GBM_BO_USE_LINEAR);
	if (!bo) {
		fprintf(stderr, "kmsflip: probe: gbm_bo_create failed\n");
		(*other)++;
		return -1;
	}
	if (gbm_bo_get_modifier(bo) != 0) {
		gbm_bo_destroy(bo);
		(*other)++;
		return -1;
	}
	p = gbm_bo_map(bo, 0, 0, (uint32_t)w, (uint32_t)h,
		       GBM_BO_TRANSFER_WRITE, &stride, &map_data);
	if (!p) {
		gbm_bo_destroy(bo);
		(*other)++;
		return -1;
	}
	memset(p, 0x5a, (size_t)(stride ? stride : (uint32_t)w * 4) * h);
	pfd = gbm_bo_get_fd(bo);
	if (pfd < 0) {
		gbm_bo_unmap(bo, map_data);
		gbm_bo_destroy(bo);
		(*other)++;
		return -1;
	}
	(*export_ok)++;

	/* Round-trip is diagnostic on render nodes (AUTH may be denied). */
	errno = 0;
	ret = drmPrimeFDToHandle(fd, pfd, &handle);
	if (ret < 0) {
		e = err_from(ret);
		if (e == EINVAL)
			(*einval)++;
		else
			(*import_err)++;
		fprintf(stderr, "kmsflip: probe: PRIME import on render "
			"node: %s (source is still usable via card0)\n",
			strerror(e));
	} else {
		(*import_ok)++;
		if (handle)
			drmCloseBufferHandle(fd, handle);
	}
	close(pfd);
	gbm_bo_unmap(bo, map_data);
	gbm_bo_destroy(bo);
	return 0;
}
#endif

static int probe_dumb_buf(int fd, int w, int h, unsigned *export_ok,
			  unsigned *import_ok, unsigned *import_err,
			  unsigned *einval, unsigned *other)
{
	uint32_t handle = 0, handle2 = 0, pitch = 0;
	uint64_t size = 0, off = 0;
	void *map;
	int pfd = -1, ret, e;

	errno = 0;
	if (drmModeCreateDumbBuffer(fd, (uint32_t)w, (uint32_t)h, 32, 0,
				    &handle, &pitch, &size) < 0) {
		e = err_from(-1);
		if (e == EINVAL)
			(*einval)++;
		else
			(*other)++;
		fprintf(stderr, "kmsflip: probe: dumb_create: %s\n",
			strerror(e));
		return -1;
	}
	errno = 0;
	if (drmModeMapDumbBuffer(fd, handle, &off) < 0) {
		(*other)++;
		drmModeDestroyDumbBuffer(fd, handle);
		return -1;
	}
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   (off_t)off);
	if (map != MAP_FAILED) {
		memset(map, 0x5a, (size_t)size);
		munmap(map, size);
	}
	errno = 0;
	ret = drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC | DRM_RDWR, &pfd);
	if (ret < 0) {
		e = err_from(ret);
		if (e == EINVAL)
			(*einval)++;
		else
			(*other)++;
		fprintf(stderr, "kmsflip: probe: PRIME export: %s\n",
			strerror(e));
		drmModeDestroyDumbBuffer(fd, handle);
		return -1;
	}
	(*export_ok)++;

	errno = 0;
	ret = drmPrimeFDToHandle(fd, pfd, &handle2);
	if (ret < 0) {
		e = err_from(ret);
		if (e == EINVAL)
			(*einval)++;
		else
			(*import_err)++;
		fprintf(stderr, "kmsflip: probe: PRIME import on render "
			"node: %s\n", strerror(e));
	} else {
		(*import_ok)++;
		if (handle2 && handle2 != handle)
			drmCloseBufferHandle(fd, handle2);
	}
	close(pfd);
	drmModeDestroyDumbBuffer(fd, handle);
	return 0;
}

static int run_probe(const char *node, int hard_ms)
{
	drmVersionPtr ver;
	const char *driver = "?";
	const char *source = "none";
	int fd, i, rc = 1, gbm_ok = 0;
	unsigned export_ok = 0, import_ok = 0, import_err = 0;
	unsigned einval = 0, other = 0;
#if HAVE_EGL
	struct gbm_device *gbm_dev = NULL;
	int gbm_fd = -1;
#endif

	watchdog_arm(hard_ms > 0 ? hard_ms : 30000);
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("KMSFLIP: RESULT SKIP reason=no-render-node node=%s\n",
		       node);
		watchdog_disarm();
		return 3;
	}
	ver = drmGetVersion(fd);
	if (ver && ver->name)
		driver = ver->name;

#if HAVE_EGL
	gbm_fd = open(node, O_RDWR | O_CLOEXEC);
	if (gbm_fd >= 0)
		gbm_dev = gbm_create_device(gbm_fd);
	if (gbm_dev) {
		for (i = 0; i < 2; i++)
			if (probe_gbm_buf(gbm_dev, fd, 64, 64, &export_ok,
					  &import_ok, &import_err, &einval,
					  &other) < 0)
				break;
		if (i == 2) {
			gbm_ok = 1;
			source = "gbm";
		}
	}
#endif
	if (!gbm_ok) {
		export_ok = 0;
		for (i = 0; i < 2; i++)
			if (probe_dumb_buf(fd, 64, 64, &export_ok, &import_ok,
					   &import_err, &einval, &other) < 0)
				break;
		if (i == 2) {
			gbm_ok = 1;
			source = "dumb";
		}
	}

	if (!gbm_ok)
		printf("KMSFLIP: hint: this kernel's render node has no "
		       "dumb_create; source env.sh kms (Mesa 20 panfrost) "
		       "before --probe, or use the KMS path on card0\n");
	printf("KMSFLIP: probe node=%s driver=%s source=%s buffers=2 "
	       "prime_export=%u prime_import=%u import_err=%u\n", node,
	       driver, source, export_ok, import_ok, import_err);
	rc = gbm_ok ? 0 : 1;
	printf("KMSFLIP: RESULT %s probe node=%s driver=%s source=%s "
	       "buffers=2 prime_export=%u prime_import=%u import_err=%u "
	       "commits=0 events=0 timeouts=0 ebusy=0 einval=%u other=%u\n",
	       rc ? "FAIL" : "PASS", node, driver, source, export_ok,
	       import_ok, import_err, einval, other);

#if HAVE_EGL
	if (gbm_dev)
		gbm_device_destroy(gbm_dev);
	if (gbm_fd >= 0)
		close(gbm_fd);
#endif
	if (ver)
		drmFreeVersion(ver);
	close(fd);
	watchdog_disarm();
	return rc;
}

/* ------------------------------------------------------------ run_kms */

static int auto_hard_ms(int frames, int timeout_ms, int phases)
{
	long long v = 5000 + (long long)phases * (frames + 2) *
			      timeout_ms * 2;

	if (v > 1800000)
		v = 1800000;
	return (int)v;
}

static int run_kms(const char *dev, const char *render_node, int conn_id,
		   int crtc_id, const char *mode_str, int frames,
		   int timeout_ms, int hard_ms, int kind, const char *dump,
		   int hold_ms, int no_dmabuf, int force_self)
{
	struct kms_ctx k;
	struct flip_stats sd, sdm, total;
	struct flip_buf dumb[2], dma[2];
	struct dmabuf_src dsrc;
	int f_dumb = 1, f_dma = 0, phases = 0, restore_rc = -1;
	int nphases = no_dmabuf ? 1 : 2;
	uint32_t plane_id = 0;
	int i, rc;
	double t0;

	memset(&k, 0, sizeof(k));
	k.fd = -1;
	memset(&sd, 0, sizeof(sd));
	memset(&sdm, 0, sizeof(sdm));
	memset(&total, 0, sizeof(total));
	memset(&dsrc, 0, sizeof(dsrc));
	dsrc.fd = -1;
	dsrc.gbm_fd = -1;
	dsrc.label = "none";
	for (i = 0; i < 2; i++) {
		buf_zero(&dumb[i]);
		buf_zero(&dma[i]);
	}

	if (hard_ms <= 0)
		hard_ms = auto_hard_ms(frames, timeout_ms, nphases);
	watchdog_arm(hard_ms);
	t0 = now_ms();

	if (kms_setup(&k, dev, conn_id, crtc_id, mode_str) != 0) {
		printf("KMSFLIP: RESULT FAIL reason=setup device=%s "
		       "commits=0 events=0 timeouts=0 ebusy=0 einval=0 "
		       "other=0\n", dev);
		kms_teardown(&k);
		watchdog_disarm();
		return 1;
	}

	plane_id = k.plane ? k.plane->plane_id : 0;
	f_dumb = phase_dumb(&k, frames, timeout_ms, kind, dump, &sd, dumb);
	phases++;
	if (!no_dmabuf && !g_stop) {
		f_dma = phase_dmabuf(&k, &dsrc, frames, timeout_ms, kind,
				     dump, render_node, force_self, &sdm, dma);
		phases++;
	}

	if (hold_ms > 0 && !g_stop) {
		double end = now_ms() + hold_ms;

		while (!g_stop && now_ms() < end)
			usleep(50000);
	}

	/* Restore before removing FBs: the saved fb must be active first. */
	watchdog_arm(5000);
	restore_rc = kms_restore(&k);
	watchdog_disarm();

	for (i = 0; i < 2; i++) {
		buf_destroy(&k, &dumb[i]);
		buf_destroy(&k, &dma[i]);
	}
	dmabuf_src_close(&dsrc);
	kms_teardown(&k);

	total.commits = sd.commits + sdm.commits;
	total.commit_ok = sd.commit_ok + sdm.commit_ok;
	total.commit_fail = sd.commit_fail + sdm.commit_fail;
	total.events = sd.events + sdm.events;
	total.flips = sd.flips + sdm.flips;
	total.timeouts = sd.timeouts + sdm.timeouts;
	total.ebusy = sd.ebusy + sdm.ebusy;
	total.einval = sd.einval + sdm.einval;
	total.other = sd.other + sdm.other;
	total.checksum_bad = sd.checksum_bad + sdm.checksum_bad;

	rc = (!f_dumb && !f_dma && !g_stop) ? 0 : 1;
	if (g_stop)
		fprintf(stderr, "kmsflip: run aborted (signal/watchdog)\n");
	printf("KMSFLIP: RESULT %s device=%s connector=%u crtc=%u plane=%u "
	       "mode=%s frames=%d phases=%d commits=%u commit_ok=%u "
	       "events=%u flips=%u/%d timeouts=%u ebusy=%u einval=%u "
	       "other=%u checksum_bad=%u restore=%s elapsed_ms=%.0f\n",
	       rc ? "FAIL" : "PASS", dev, k.conn_id ? k.conn_id : 0,
	       k.crtc_id, plane_id, k.mode.name,
	       frames, phases, total.commits, total.commit_ok, total.events,
	       total.flips, phases * frames, total.timeouts, total.ebusy,
	       total.einval, total.other, total.checksum_bad,
	       restore_rc == 0 ? "ok" : "fail", now_ms() - t0);
	return rc;
}

static void kms_list(const char *dev)
{
	drmModeRes *res;
	drmModeConnector *conn;
	int fd, i, j;

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "kmsflip: open %s: %s\n", dev, strerror(errno));
		exit(3);
	}
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "kmsflip: drmModeGetResources: %s\n",
			strerror(errno));
		close(fd);
		exit(1);
	}
	printf("KMSFLIP-LIST: dev=%s crtcs=%d connectors=%d encoders=%d\n", dev,
	       res->count_crtcs, res->count_connectors, res->count_encoders);
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;
		printf("CONNECTOR id=%u type=%d status=%s encoder=%u modes=%d\n",
		       conn->connector_id, conn->connector_type,
		       conn->connection == DRM_MODE_CONNECTED ? "connected" :
								 "disconnected",
		       conn->encoder_id, conn->count_modes);
		for (j = 0; j < conn->count_modes; j++)
			printf("  MODE %s %dx%d@%d type=0x%x\n",
			       conn->modes[j].name, conn->modes[j].hdisplay,
			       conn->modes[j].vdisplay,
			       conn->modes[j].vrefresh, conn->modes[j].type);
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);
	close(fd);
}

/* ------------------------------------------------------------- headless */

#if HAVE_EGL
static const char *vs_src =
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  v_uv = a_pos * 0.5 + 0.5;\n"
	"  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *fs_src =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"  float bar = mod(floor(v_uv.x * 8.0), 2.0);\n"
	"  vec3 c = vec3(v_uv.x, v_uv.y, bar);\n"
	"  if (v_uv.y < 0.05) c = vec3(1.0, 1.0, 1.0);\n"
	"  gl_FragColor = vec4(c, 1.0);\n"
	"}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		GLsizei n = 0;

		glGetShaderInfoLog(sh, sizeof(log), &n, log);
		fprintf(stderr, "kmsflip: shader: %.*s\n", (int)n, log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static int run_headless(const char *node, int w, int h, int frames,
			const char *expect, const char *dump)
{
	const char *renderer = NULL;
	const char *version = NULL;
	struct gbm_device *gbm_dev = NULL;
	struct gbm_bo *bo = NULL;
	EGLDisplay dpy = EGL_NO_DISPLAY;
	EGLContext ctx = EGL_NO_CONTEXT;
	EGLSurface surf = EGL_NO_SURFACE;
	EGLConfig cfg = NULL;
	EGLint ncfg = 0, major = 0, minor = 0;
	GLuint vs = 0, fs = 0, prog = 0;
	GLint pos_loc;
	unsigned char *pix = NULL;
	uint64_t csum_ref = 0;
	int fd = -1, i, rc = 1;
	int stable = 1, gbm_ok = 0, map_ok = 0;
	uint32_t stride = 0;
	void *map_data = NULL;
	uint64_t gbm_csum = 0;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
			"eglGetPlatformDisplayEXT");

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "kmsflip: open %s: %s\n", node, strerror(errno));
		return 3;
	}
	gbm_dev = gbm_create_device(fd);
	if (!gbm_dev) {
		fprintf(stderr, "kmsflip: gbm_create_device(%s) failed\n", node);
		close(fd);
		return 1;
	}
	if (!strcmp(gbm_device_get_backend_name(gbm_dev), "drm"))
		printf("KMSFLIP-HEADLESS: gbm backend=drm node=%s\n", node);

	bo = gbm_bo_create(gbm_dev, (uint32_t)w, (uint32_t)h,
			   GBM_FORMAT_XRGB8888,
			   GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
	if (!bo) {
		printf("KMSFLIP-HEADLESS: gbm linear BO unavailable, retrying "
		       "tiled-only\n");
		bo = gbm_bo_create(gbm_dev, (uint32_t)w, (uint32_t)h,
				   GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
	}
	if (bo) {
		gbm_ok = 1;
		stride = gbm_bo_get_stride(bo);
		{
			uint32_t st = 0;
			void *p = gbm_bo_map(bo, 0, 0, (uint32_t)w, (uint32_t)h,
					     GBM_BO_TRANSFER_WRITE, &st, &map_data);
			if (p) {
				uint32_t *px = p;
				int x, y;

				map_ok = 1;
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++)
						px[(size_t)y * (st / 4) + x] =
							0xff000000 |
							((uint32_t)(x ^ y) & 0xffffff);
				gbm_csum = fnv1a(p, (size_t)st * (size_t)h);
				gbm_bo_unmap(bo, map_data);
			} else {
				fprintf(stderr, "kmsflip: gbm_bo_map failed\n");
			}
		}
		printf("KMSFLIP-HEADLESS: gbm bo=%dx%d stride=%u csum=0x%016llx "
		       "map=%s\n", gbm_bo_get_width(bo), gbm_bo_get_height(bo),
		       stride, (unsigned long long)gbm_csum,
		       map_ok ? "ok" : "fail");
	} else {
		fprintf(stderr, "kmsflip: gbm_bo_create failed\n");
	}

	if (get_platform_display)
		dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
					   EGL_DEFAULT_DISPLAY, NULL);
	if (dpy == EGL_NO_DISPLAY)
		dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY) {
		fprintf(stderr, "kmsflip: no EGL display\n");
		goto out;
	}
	if (!eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "kmsflip: eglInitialize failed: 0x%x\n",
			eglGetError());
		goto out;
	}
	printf("KMSFLIP-HEADLESS: EGL %d.%d vendor=%s\n", major, minor,
	       eglQueryString(dpy, EGL_VENDOR));
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		fprintf(stderr, "kmsflip: eglBindAPI failed\n");
		goto out;
	}
	{
		EGLint attribs[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_NONE
		};
		if (!eglChooseConfig(dpy, attribs, &cfg, 1, &ncfg) || !ncfg) {
			fprintf(stderr, "kmsflip: no EGL config\n");
			goto out;
		}
	}
	{
		EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
	}
	if (ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "kmsflip: eglCreateContext failed\n");
		goto out;
	}
	{
		EGLint sattr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
		surf = eglCreatePbufferSurface(dpy, cfg, sattr);
	}
	if (surf == EGL_NO_SURFACE) {
		fprintf(stderr, "kmsflip: eglCreatePbufferSurface %dx%d failed\n",
			w, h);
		goto out;
	}
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "kmsflip: eglMakeCurrent failed\n");
		goto out;
	}
	renderer = (const char *)glGetString(GL_RENDERER);
	version = (const char *)glGetString(GL_VERSION);
	printf("KMSFLIP-HEADLESS: GL_RENDERER=\"%s\" GL_VERSION=\"%s\"\n",
	       renderer ? renderer : "?", version ? version : "?");

	if (expect && (!renderer || !strstr(renderer, expect))) {
		fprintf(stderr,
			"kmsflip: renderer does not contain \"%s\" (wanted HW "
			"panfrost)\n", expect);
		goto out;
	}

	vs = compile_shader(GL_VERTEX_SHADER, vs_src);
	fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
	if (!vs || !fs)
		goto out;
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	{
		GLint ok = 0;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[1024];
			GLsizei n = 0;
			glGetProgramInfoLog(prog, sizeof(log), &n, log);
			fprintf(stderr, "kmsflip: link: %.*s\n", (int)n, log);
			goto out;
		}
	}
	glUseProgram(prog);
	pos_loc = glGetAttribLocation(prog, "a_pos");
	if (pos_loc < 0) {
		fprintf(stderr, "kmsflip: a_pos attribute not found\n");
		goto out;
	}
	{
		static const GLfloat verts[] = {
			-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f
		};
		glVertexAttribPointer((GLuint)pos_loc, 2, GL_FLOAT, GL_FALSE, 0,
				      verts);
		glEnableVertexAttribArray((GLuint)pos_loc);
	}
	glViewport(0, 0, w, h);

	pix = malloc((size_t)w * (size_t)h * 4);
	if (!pix) {
		fprintf(stderr, "kmsflip: out of memory\n");
		goto out;
	}

	for (i = 0; i < frames && !g_stop; i++) {
		uint64_t csum;

		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
		if (glGetError() != GL_NO_ERROR) {
			fprintf(stderr, "kmsflip: GL error on frame %d\n", i);
			goto out;
		}
		csum = fnv1a(pix, (size_t)w * (size_t)h * 4);
		if (i == 0) {
			csum_ref = csum;
		} else if (csum != csum_ref) {
			stable = 0;
			fprintf(stderr,
				"kmsflip: frame %d checksum changed "
				"(0x%016llx != 0x%016llx)\n", i,
				(unsigned long long)csum,
				(unsigned long long)csum_ref);
		}
		if ((i + 1) % 30 == 0 || i + 1 == frames)
			printf("KMSFLIP-HEADLESS: frame %d/%d csum=0x%016llx\n",
			       i + 1, frames, (unsigned long long)csum);
	}
	if (dump && pix) {
		/* glReadPixels is bottom-up; flip into a PPM. */
		uint32_t *rows = malloc((size_t)w * (size_t)h * 4);
		if (rows) {
			int y;

			for (y = 0; y < h; y++)
				memcpy(&rows[(size_t)y * w],
				       &pix[(size_t)(h - 1 - y) * w * 4],
				       (size_t)w * 4);
			dump_ppm(dump, rows, (uint32_t)w * 4, w, h);
			printf("KMSFLIP-HEADLESS: pattern dumped to %s\n", dump);
			free(rows);
		}
	}

	rc = (stable && csum_ref != 0) ? 0 : 1;
	printf("KMSFLIP-HEADLESS: node=%s size=%dx%d frames=%d stable=%d "
	       "gbm=%d gbm_map=%d csum=0x%016llx %s\n", node, w, h, frames,
	       stable, gbm_ok, map_ok, (unsigned long long)csum_ref,
	       rc ? "FAIL" : "PASS");

out:
	free(pix);
	if (prog)
		glDeleteProgram(prog);
	if (vs)
		glDeleteShader(vs);
	if (fs)
		glDeleteShader(fs);
	if (dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			       EGL_NO_CONTEXT);
		if (surf != EGL_NO_SURFACE)
			eglDestroySurface(dpy, surf);
		if (ctx != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctx);
		eglTerminate(dpy);
	}
	if (bo)
		gbm_bo_destroy(bo);
	if (gbm_dev) {
		/* gbm owns its dup of fd; destroy first, then close ours. */
		gbm_device_destroy(gbm_dev);
	}
	if (fd >= 0)
		close(fd);
	return rc;
}
#else
static int run_headless(const char *node, int w, int h, int frames,
			const char *expect, const char *dump)
{
	(void)node;
	(void)w;
	(void)h;
	(void)frames;
	(void)expect;
	(void)dump;
	fprintf(stderr, "kmsflip: built without EGL/GBM support\n");
	return 3;
}
#endif

/* ---------------------------------------------------------------- main */

static void usage(void)
{
	fprintf(stderr,
		"usage: kmsflip [options]            atomic KMS flip test\n"
		"       kmsflip --probe             dma-buf source probe, no card0\n"
		"       kmsflip --headless          GBM+EGL render test, no card0\n"
		"  --device PATH        KMS device (default /dev/dri/card0)\n"
		"  --render-node PATH   dma-buf source (default "
		"/dev/dri/renderD128)\n"
		"  --connector ID       connector id (default: first connected)\n"
		"  --crtc ID            crtc id (default: encoder's crtc)\n"
		"  --mode WxH           mode (default: preferred)\n"
		"  --pattern NAME       solid|bars|checker|gradient|noise "
		"(default solid)\n"
		"  --frames N           page flips per phase (default 10)\n"
		"  --timeout-ms MS      per commit/event timeout (default 250)\n"
		"  --hard-timeout-ms MS overall watchdog (0 = automatic)\n"
		"  --hold-ms MS         keep the last frame up before restoring\n"
		"  --dump PATH.ppm      write the last frame as a PPM\n"
		"  --no-dmabuf          skip the dma-buf phase\n"
		"  --prime-self         force card0 PRIME round-trip source\n"
		"  --probe              render-node dma-buf source probe only\n"
		"  --list               list connectors/modes and exit\n"
		"  --headless           GBM + EGL surfaceless test, no card0\n"
		"  --node PATH          render node for --headless/--probe\n"
		"  --expect-renderer S  headless: fail unless GL_RENDERER has S\n"
		"  -h, --help           this text\n");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/dri/card0";
	const char *node = "/dev/dri/renderD128";
	const char *render_node = "/dev/dri/renderD128";
	const char *mode = NULL;
	const char *pattern = "solid";
	const char *dump = NULL;
	const char *expect = NULL;
	int headless = 0, list = 0, probe = 0, no_dmabuf = 0, prime_self = 0;
	int conn_id = 0, crtc_id = 0;
	int frames = 10, timeout_ms = 250, hard_ms = 0, hold_ms = 0;
	int w = 720, h = 1280, kind;
	int rc;

	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "headless", no_argument, NULL, 'H' },
		{ "node", required_argument, NULL, 'n' },
		{ "render-node", required_argument, NULL, 'R' },
		{ "connector", required_argument, NULL, 'c' },
		{ "crtc", required_argument, NULL, 'r' },
		{ "mode", required_argument, NULL, 'm' },
		{ "pattern", required_argument, NULL, 'p' },
		{ "frames", required_argument, NULL, 'f' },
		{ "timeout-ms", required_argument, NULL, 't' },
		{ "hard-timeout-ms", required_argument, NULL, 'T' },
		{ "hold-ms", required_argument, NULL, 'k' },
		{ "dump", required_argument, NULL, 'o' },
		{ "no-dmabuf", no_argument, NULL, 'D' },
		{ "prime-self", no_argument, NULL, 'P' },
		{ "probe", no_argument, NULL, 'q' },
		{ "list", no_argument, NULL, 'l' },
		{ "expect-renderer", required_argument, NULL, 'E' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while ((rc = getopt_long(argc, argv, "hH", opts, NULL)) != -1) {
		switch (rc) {
		case 'd':
			dev = optarg;
			break;
		case 'H':
			headless = 1;
			break;
		case 'n':
			node = optarg;
			render_node = optarg;
			break;
		case 'R':
			render_node = optarg;
			break;
		case 'c':
			conn_id = atoi(optarg);
			break;
		case 'r':
			crtc_id = atoi(optarg);
			break;
		case 'm':
			mode = optarg;
			break;
		case 'p':
			pattern = optarg;
			break;
		case 'f':
			frames = atoi(optarg);
			break;
		case 't':
			timeout_ms = atoi(optarg);
			break;
		case 'T':
			hard_ms = atoi(optarg);
			break;
		case 'k':
			hold_ms = atoi(optarg);
			break;
		case 'o':
			dump = optarg;
			break;
		case 'D':
			no_dmabuf = 1;
			break;
		case 'P':
			prime_self = 1;
			break;
		case 'q':
			probe = 1;
			break;
		case 'l':
			list = 1;
			break;
		case 'E':
			expect = optarg;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}
	if (frames <= 0 || frames > 100000)
		frames = 10;
	if (timeout_ms < 10)
		timeout_ms = 10;
	if (timeout_ms > 60000)
		timeout_ms = 60000;
	if (hold_ms < 0)
		hold_ms = 0;
	if (headless && mode) {
		int r = sscanf(mode, "%dx%d", &w, &h);

		if (r != 2) {
			fprintf(stderr, "kmsflip: --mode wants WxH\n");
			return 2;
		}
	}
	kind = parse_pattern(pattern);
	if (kind < 0) {
		fprintf(stderr, "kmsflip: unknown pattern %s\n", pattern);
		return 2;
	}

	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_signal;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sa.sa_handler = on_alarm;
		sigaction(SIGALRM, &sa, NULL);
	}
	event_ctx_init();

	if (list) {
		kms_list(dev);
		return 0;
	}
	if (headless) {
		int hard = hard_ms > 0 ? hard_ms : 5000 + frames * 2000;

		watchdog_arm(hard);
		rc = run_headless(node, w, h, frames, expect, dump);
		watchdog_disarm();
		return rc;
	}
	if (probe)
		return run_probe(render_node, hard_ms);

	/*
	 * Render-only bring-up kernels (int49-class) have no card0.  Do not
	 * fail, and do not touch the render node from the KMS path: state the
	 * skip and point at --probe, which is the meaningful check there.
	 */
	if (access(dev, F_OK) != 0) {
		printf("KMSFLIP: RESULT SKIP reason=no-card0 device=%s "
		       "hint=\"render-only kernel; run: kmsflip --probe "
		       "--node %s\" commits=0 events=0 timeouts=0 ebusy=0 "
		       "einval=0 other=0\n", dev, render_node);
		return 3;
	}

	return run_kms(dev, render_node, conn_id, crtc_id, mode, frames,
		       timeout_ms, hard_ms, kind, dump, hold_ms, no_dmabuf,
		       prime_self);
}
