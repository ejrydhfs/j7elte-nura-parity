/*
 * kmsflip - deterministic KMS flip / test-pattern checker for the
 *           Exynos 7580 DECON KMS bring-up (j7elte).
 *
 * Two modes:
 *
 *   kmsflip --headless
 *       No card0 needed. Opens the render node ($GBM_NODE, default
 *       /dev/dri/renderD128), allocates + maps a GBM buffer, then renders a
 *       deterministic pattern N times through EGL (surfaceless) + GLES2 and
 *       checksums the readback. This exercises the *userspace* half (Mesa
 *       GBM/EGL/GL, panfrost, dma-buf allocation) before a KMS device exists.
 *
 *   kmsflip (default, KMS)
 *       Opens card0, picks a connected connector + preferred mode, creates
 *       two CPU-filled dumb buffers (test pattern), drmModeSetCrtc()s one and
 *       then page-flips N times. Every flip must complete before --timeout-ms
 *       and the mapped buffer must still carry the exact pattern that was
 *       written before the flip (FNV-1a checksum). This is the M1/M2
 *       acceptance test: dumb-buffer scanout + vblank/page-flip plumbing.
 *
 * The tool does not read dmesg itself (root/permissions vary); the wrapper
 * scripts snapshot dmesg counters before/after and fold them into PASS/FAIL.
 *
 * Build (on device, native):
 *   make                    # builds kmsflip (EGL/GBM/GLES2 path enabled)
 *   make NO_EGL=1           # KMS-only build (no EGL/GLES headers needed)
 *
 * Exit status: 0 PASS, 1 FAIL, 2 usage, 3 environment (missing node/headers).
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

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
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

enum pattern { PAT_BARS, PAT_CHECKER, PAT_GRADIENT, PAT_NOISE };

static int parse_pattern(const char *s)
{
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

/*
 * Fill one XRGB8888 buffer. `frame` XORs a per-frame constant into every
 * pixel so consecutive frames have different checksums (proves the flip
 * actually presented new content) while the visual layout stays readable.
 */
static void fill_pattern(uint32_t *px, int w, int h, int kind, int frame)
{
	static const uint32_t bars[8] = {
		0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
		0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000,
	};
	uint32_t f = (uint32_t)frame * 2654435761u;
	int x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint32_t v;

			switch (kind) {
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
				uint64_t s = (uint64_t)(uint32_t)y << 32 |
					     (uint32_t)x;
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
			/* frame reference bar: 0..w/2 px wide */
			if (y < h / 64 && x < (int)((uint32_t)w * (uint32_t)(frame % 61) / 120u))
				v = 0xff7f7f7f;
			px[y * w + x] = v ^ f;
		}
	}
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

/* ------------------------------------------------------------------ KMS */

struct flip_event {
	int done;
};

static void page_flip_handler(int fd, unsigned int seq, unsigned int sec,
			      unsigned int usec, void *data)
{
	struct flip_event *ev = data;

	(void)fd;
	(void)seq;
	(void)sec;
	(void)usec;
	ev->done = 1;
}

struct kms_buf {
	uint32_t handle;
	uint32_t fb;
	uint32_t pitch;
	uint64_t size;
	void *map;
	uint64_t csum;
};

struct kms {
	int fd;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	drmModeCrtc *saved;
	drmModeModeInfo mode;
	uint32_t crtc_id;
	uint32_t conn_id;
	struct kms_buf buf[2];
};

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

static void kms_cleanup(struct kms *k)
{
	int i;

	if (k->fd < 0)
		return;
	if (k->saved && k->crtc_id) {
		if (k->saved->buffer_id) {
			drmModeSetCrtc(k->fd, k->crtc_id, k->saved->buffer_id,
				       k->saved->x, k->saved->y, &k->conn_id, 1,
				       &k->saved->mode);
		} else {
			drmModeSetCrtc(k->fd, k->crtc_id, 0, 0, 0, NULL, 0, NULL);
		}
	}
	for (i = 0; i < 2; i++) {
		if (k->buf[i].fb)
			drmModeRmFB(k->fd, k->buf[i].fb);
		if (k->buf[i].map)
			munmap(k->buf[i].map, k->buf[i].size);
		if (k->buf[i].handle)
			drmModeDestroyDumbBuffer(k->fd, k->buf[i].handle);
	}
	if (k->saved)
		drmModeFreeCrtc(k->saved);
	if (k->enc)
		drmModeFreeEncoder(k->enc);
	if (k->conn)
		drmModeFreeConnector(k->conn);
	if (k->res)
		drmModeFreeResources(k->res);
	drmDropMaster(k->fd);
	close(k->fd);
	memset(k, 0, sizeof(*k));
	k->fd = -1;
}

static int kms_setup(struct kms *k, const char *dev, int conn_id, int crtc_id,
		     const char *mode_str, int *out_w, int *out_h)
{
	int i;

	memset(k, 0, sizeof(*k));
	k->fd = -1;

	k->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (k->fd < 0) {
		fprintf(stderr, "kmsflip: open %s: %s\n", dev, strerror(errno));
		return 3;
	}
	if (drmSetMaster(k->fd) < 0)
		fprintf(stderr, "kmsflip: warning: drmSetMaster: %s\n",
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
			fprintf(stderr, "kmsflip: connector %d not found\n", conn_id);
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
		int w, h, r;
		int found = 0;

		r = sscanf(mode_str, "%dx%d", &w, &h);
		if (r != 2) {
			fprintf(stderr, "kmsflip: --mode wants WxH (got %s)\n",
				mode_str);
			return 2;
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
			fprintf(stderr,
				"kmsflip: no mode %s on connector %u\n",
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
	if (!k->enc) {
		/* Prefer an encoder the connector itself advertises. */
		int j;

		for (j = 0; j < k->conn->count_encoders && !k->enc; j++)
			k->enc = drmModeGetEncoder(k->fd,
						   k->conn->encoders[j]);
	}
	if (!k->enc)
		for (i = 0; i < k->res->count_encoders; i++) {
			drmModeEncoder *e = drmModeGetEncoder(k->fd,
							     k->res->encoders[i]);
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
	k->saved = drmModeGetCrtc(k->fd, k->crtc_id);

	*out_w = k->mode.hdisplay;
	*out_h = k->mode.vdisplay;
	printf("KMSFLIP: dev=%s connector=%u crtc=%u mode=%s (%dx%d@%d)\n",
	       dev, k->conn_id, k->crtc_id, k->mode.name, k->mode.hdisplay,
	       k->mode.vdisplay, k->mode.vrefresh);
	return 0;
}

static int kms_buf_create(struct kms *k, struct kms_buf *b, int w, int h,
			  const char *phase)
{
	uint64_t off = 0;
	int ret;

	ret = drmModeCreateDumbBuffer(k->fd, w, h, 32, 0, &b->handle,
				      &b->pitch, &b->size);
	if (ret < 0) {
		fprintf(stderr,
			"kmsflip: %s: drmModeCreateDumbBuffer %dx%d: %s\n",
			phase, w, h, strerror(errno));
		fprintf(stderr,
			"kmsflip: (M1 acceptance needs a dumb_create path in the "
			"DECON KMS driver)\n");
		return -1;
	}
	if (drmModeMapDumbBuffer(k->fd, b->handle, &off) < 0) {
		fprintf(stderr, "kmsflip: %s: map dumb: %s\n", phase,
			strerror(errno));
		return -1;
	}
	b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      k->fd, (off_t)off);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		fprintf(stderr, "kmsflip: %s: mmap dumb: %s\n", phase,
			strerror(errno));
		return -1;
	}
	if (drmModeAddFB(k->fd, w, h, 24, 32, b->pitch, b->handle, &b->fb) < 0) {
		fprintf(stderr, "kmsflip: %s: drmModeAddFB: %s\n", phase,
			strerror(errno));
		return -1;
	}
	return 0;
}

static int run_kms(struct kms *k, const char *dev, int conn_id, int crtc_id,
		   const char *mode_str, int frames, int timeout_ms,
		   int kind, const char *dump, int hold_ms)
{
	struct flip_event ev = { 0 };
	drmEventContext ectx;
	double t0, elapsed;
	struct rusage ru;
	int w, h, i, rc;
	int ok_flips = 0, timeouts = 0, stale = 0, bad_csum = 0, flip_err = 0;
	uint64_t csum_first = 0, csum_last = 0;
	long maxrss = 0;

	rc = kms_setup(k, dev, conn_id, crtc_id, mode_str, &w, &h);
	if (rc)
		return rc;

	if (kms_buf_create(k, &k->buf[0], w, h, "buf0") ||
	    kms_buf_create(k, &k->buf[1], w, h, "buf1")) {
		kms_cleanup(k);
		return 1;
	}

	fill_pattern(k->buf[0].map, w, h, kind, 0);
	fill_pattern(k->buf[1].map, w, h, kind, 1);

	rc = drmModeSetCrtc(k->fd, k->crtc_id, k->buf[0].fb, 0, 0,
			    &k->conn_id, 1, &k->mode);
	if (rc < 0) {
		fprintf(stderr, "kmsflip: drmModeSetCrtc: %s\n", strerror(errno));
		kms_cleanup(k);
		return 1;
	}
	printf("KMSFLIP: crtc set, starting %d flips (timeout %d ms)\n",
	       frames, timeout_ms);

	memset(&ectx, 0, sizeof(ectx));
	ectx.version = 2;
	ectx.page_flip_handler = page_flip_handler;

	t0 = now_ms();
	for (i = 0; i < frames && !g_stop; i++) {
		struct kms_buf *b = &k->buf[(i + 1) & 1];
		double ft0, ft1;
		double wait;
		int pr;

		fill_pattern(b->map, w, h, kind, i + 2);
		b->csum = fnv1a_rows(b->map, b->pitch,
				     (size_t)w * 4, h);

		ev.done = 0;
		if (drmModePageFlip(k->fd, k->crtc_id, b->fb,
				    DRM_MODE_PAGE_FLIP_EVENT, &ev) < 0) {
			fprintf(stderr, "kmsflip: flip %d: drmModePageFlip: %s\n",
				i, strerror(errno));
			flip_err++;
			break;
		}

		ft0 = now_ms();
		wait = (double)timeout_ms;
		while (!ev.done && wait > 0 && !g_stop) {
			struct pollfd pfd = { k->fd, POLLIN, 0 };
			int slice = (int)(wait > 20.0 ? 20.0 : wait);

			pr = poll(&pfd, 1, slice);
			if (pr < 0) {
				if (errno == EINTR)
					continue;
				fprintf(stderr, "kmsflip: poll: %s\n",
					strerror(errno));
				flip_err++;
				break;
			}
			if (pr == 0) {
				wait -= slice;
				continue;
			}
			if (drmHandleEvent(k->fd, &ectx) < 0) {
				fprintf(stderr, "kmsflip: drmHandleEvent: %s\n",
					strerror(errno));
				flip_err++;
				break;
			}
		}
		ft1 = now_ms();
		if (!ev.done) {
			timeouts++;
			fprintf(stderr,
				"kmsflip: flip %d TIMEOUT after %.1f ms\n", i,
				ft1 - ft0);
			/* drain any late event so the next flip is clean */
			while (poll(&(struct pollfd){ k->fd, POLLIN, 0 }, 1, 0) > 0)
				drmHandleEvent(k->fd, &ectx);
			continue;
		}
		ok_flips++;

		if (fnv1a_rows(b->map, b->pitch, (size_t)w * 4, h) !=
		    b->csum) {
			bad_csum++;
			fprintf(stderr,
				"kmsflip: flip %d CHECKSUM MISMATCH "
				"(buffer changed under scanout)\n", i);
		}
		if (i == 0)
			csum_first = b->csum;
		csum_last = b->csum;
		if ((i + 1) % 60 == 0 || i + 1 == frames)
			printf("KMSFLIP: flip %d/%d t=%.0f ms%s\n", i + 1,
			       frames, ft1 - t0,
			       (i + 1) % 60 ? "" : "");
	}
	elapsed = now_ms() - t0;

	if (dump && k->buf[(frames - 1) & 1].map) {
		if (!dump_ppm(dump, k->buf[(frames - 1) & 1].map,
			      k->buf[(frames - 1) & 1].pitch, w, h))
			printf("KMSFLIP: pattern dumped to %s\n", dump);
	}
	if (hold_ms > 0)
		usleep((useconds_t)hold_ms * 1000);

	getrusage(RUSAGE_SELF, &ru);
	maxrss = ru.ru_maxrss;

	rc = (ok_flips == frames && timeouts == 0 && bad_csum == 0 &&
	      flip_err == 0) ? 0 : 1;
	printf("KMSFLIP: frames=%d ok=%d timeout=%d checksum_bad=%d "
	       "flip_err=%d stale=%d elapsed=%.0fms fps=%.1f "
	       "csum_first=0x%016llx csum_last=0x%016llx maxrss=%ldkB %s\n",
	       frames, ok_flips, timeouts, bad_csum, flip_err, stale, elapsed,
	       elapsed > 0 ? frames * 1000.0 / elapsed : 0.0,
	       (unsigned long long)csum_first, (unsigned long long)csum_last,
	       maxrss, rc ? "FAIL" : "PASS");

	kms_cleanup(k);
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
		"usage: kmsflip [--headless] [options]\n"
		"  --device PATH      KMS device (default /dev/dri/card0)\n"
		"  --headless         GBM + EGL surfaceless test, no card0\n"
		"  --node PATH        render node for --headless "
		"(default /dev/dri/renderD128)\n"
		"  --connector ID     connector id (default: first connected)\n"
		"  --crtc ID          crtc id (default: encoder's crtc)\n"
		"  --mode WxH         mode (default: preferred)\n"
		"  --pattern NAME     bars|checker|gradient|noise (default bars)\n"
		"  --frames N         frames/flips (default 10; soak uses 300)\n"
		"  --timeout-ms MS    per-flip completion timeout (default 250)\n"
		"  --hold-ms MS       keep the last frame up before restoring\n"
		"  --dump PATH.ppm    write the last frame as a PPM\n"
		"  --expect-renderer S  fail unless GL_RENDERER contains S\n"
		"  --list             list connectors/modes and exit\n"
		"  -h, --help         this text\n");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/dri/card0";
	const char *node = "/dev/dri/renderD128";
	const char *mode = NULL;
	const char *pattern = "bars";
	const char *dump = NULL;
	const char *expect = NULL;
	int headless = 0, list = 0;
	int conn_id = 0, crtc_id = 0;
	int frames = 10, timeout_ms = 250, hold_ms = 0;
	int w = 720, h = 1280, kind;
	int rc;

	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "headless", no_argument, NULL, 'H' },
		{ "node", required_argument, NULL, 'n' },
		{ "connector", required_argument, NULL, 'c' },
		{ "crtc", required_argument, NULL, 'r' },
		{ "mode", required_argument, NULL, 'm' },
		{ "pattern", required_argument, NULL, 'p' },
		{ "frames", required_argument, NULL, 'f' },
		{ "timeout-ms", required_argument, NULL, 't' },
		{ "hold-ms", required_argument, NULL, 'k' },
		{ "dump", required_argument, NULL, 'o' },
		{ "expect-renderer", required_argument, NULL, 'E' },
		{ "list", no_argument, NULL, 'l' },
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
		case 'k':
			hold_ms = atoi(optarg);
			break;
		case 'o':
			dump = optarg;
			break;
		case 'E':
			expect = optarg;
			break;
		case 'l':
			list = 1;
			break;
		case 'h':
		default:
			usage();
			return rc == 'h' ? 0 : 2;
		}
	}
	if (frames <= 0)
		frames = 10;
	if (timeout_ms <= 0)
		timeout_ms = 250;
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

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (list) {
		kms_list(dev);
		return 0;
	}
	if (headless)
		return run_headless(node, w, h, frames, expect, dump);
	{
		struct kms k;

		memset(&k, 0, sizeof(k));
		k.fd = -1;
		rc = run_kms(&k, dev, conn_id, crtc_id, mode, frames,
			     timeout_ms, kind, dump, hold_ms);
	}
	return rc;
}
