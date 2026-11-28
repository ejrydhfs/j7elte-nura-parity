/*
 * mesa203-smoke.c - minimal EGL + GLES2 probe for the j7elte panfrost stack.
 *
 *   ./mesa203-smoke [surfaceless|gbm|both] [node]
 *
 * Prints EGL/GL strings and then runs a REAL fragment job: a full-screen
 * triangle (vertex + fragment shader) into a 720x1280 RGBA FBO, glFinish(),
 * then glReadPixels of a small window with an FNV-1a checksum. This is the
 * path that never retires on the newer Mesa for this driver, so a returned
 * checksum is the meaningful "the GPU job completed" signal.
 *
 * Built on the device (musl aarch64) against the system EGL/GLES2/gbm
 * headers; at runtime LD_LIBRARY_PATH selects the Mesa 20.3.5 libraries.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static int W = 720;
static int H = 1280;

static PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;

static const char *vs_src =
	"attribute vec2 p;\n"
	"varying vec2 v;\n"
	"void main() { v = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *fs_src =
	"precision mediump float;\n"
	"varying vec2 v;\n"
	"void main() { gl_FragColor = vec4(v, fract(v.x + v.y), 1.0); }\n";

static int verify_fail;

static const char *vs_verify =
	"attribute vec2 p;\n"
	"void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *fs_verify =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(0.5, 0.25, 0.75, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "SMOKE: shader compile failed: %s\n", log);
		return 0;
	}
	return s;
}

static uint64_t render_once(void)
{
	static const GLfloat tri[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
	GLuint fbo = 0, rb = 0, prog, vb, vs, fs;
	GLint loc;
	unsigned char px[16 * 16 * 4];
	uint64_t h = 1469598103934665603ULL;
	GLenum err;

	if (getenv("SMOKE_VERIFY")) {
		vs = compile(GL_VERTEX_SHADER, vs_verify);
		fs = compile(GL_FRAGMENT_SHADER, fs_verify);
	} else {
		vs = compile(GL_VERTEX_SHADER, vs_src);
		fs = compile(GL_FRAGMENT_SHADER, fs_src);
	}
	if (!vs || !fs)
		return 0;
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	loc = glGetAttribLocation(prog, "p");

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "SMOKE: FBO incomplete\n");
		return 0;
	}
	glViewport(0, 0, W, H);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	glGenBuffers(1, &vb);
	glBindBuffer(GL_ARRAY_BUFFER, vb);
	glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
	glEnableVertexAttribArray(loc);
	glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	if (!getenv("SMOKE_NODRAW")) {
		int n = getenv("SMOKE_DRAWS") ? atoi(getenv("SMOKE_DRAWS")) : 1;
		for (int i = 0; i < n; i++) {
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glFinish();
			printf("SMOKE:   dispatch %d/%d done\n", i + 1, n);
			if (i + 1 < n) {
				/* SMOKE_GAP_MS: idle time between dispatches, used to
				 * test whether the consumer's re-read of a freed
				 * descriptor page depends on the gap. */
				const char *g = getenv("SMOKE_GAP_MS");
				if (g && atoi(g) > 0)
					usleep((useconds_t)atoi(g) * 1000);
			}
		}
	} else {
		printf("SMOKE:   (SMOKE_NODRAW: draw skipped)\n");
		glFinish();
	}

	while ((err = glGetError()) != GL_NO_ERROR)
		fprintf(stderr, "SMOKE: gl error 0x%x\n", err);

	glReadPixels(W / 2 - 8, H / 2 - 8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
	if (getenv("SMOKE_VERIFY")) {
		/* Content check: the whole window must hold the constant colour.
		 * A stalled or dropped job leaves the clear colour or mixed
		 * garbage here, which a fence wait cannot distinguish (the
		 * driver signals the fence from the recovery path). */
		unsigned lo[4] = { 255, 255, 255, 255 }, hi[4] = { 0, 0, 0, 0 };
		for (size_t i = 0; i < sizeof px; i += 4)
			for (int c = 0; c < 4; c++) {
				if (px[i + c] < lo[c]) lo[c] = px[i + c];
				if (px[i + c] > hi[c]) hi[c] = px[i + c];
			}
		int uniform = 1, nonzero = 0;
		for (int c = 0; c < 4; c++)
			if (hi[c] - lo[c] > 0x11) uniform = 0;
		if (px[0] || px[1] || px[2]) nonzero = 1;
		printf("SMOKE:   verify uniform=%d nonzero=%d rgba=%u,%u,%u,%u "
		       "spread=%u/%u/%u/%u %s\n", uniform, nonzero,
		       px[0], px[1], px[2], px[3],
		       hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2], hi[3]-lo[3],
		       (uniform && nonzero) ? "OK" : "MISMATCH");
		if (!uniform || !nonzero)
			verify_fail = 1;
	}
	for (size_t i = 0; i < sizeof px; i++) {
		h ^= px[i];
		h *= 1099511628211ULL;
	}
	{
		/* Second identical readback after a pause: if this one is stable
		 * across processes while the first is not, the first read raced
		 * the job (fence not really waited). */
		const char *d = getenv("SMOKE_DELAY_MS");
		uint64_t h2 = 0;
		if (d && atoi(d) > 0) {
			unsigned char p2[16 * 16 * 4];
			usleep((useconds_t)atoi(d) * 1000);
			glReadPixels(W / 2 - 8, H / 2 - 8, 16, 16, GL_RGBA,
				     GL_UNSIGNED_BYTE, p2);
			for (size_t i = 0; i < sizeof p2; i++) {
				h2 ^= p2[i];
				h2 *= 1099511628211ULL;
			}
			printf("SMOKE:   delayed(%sms) csum2=0x%016llx %s\n", d,
			       (unsigned long long)h2,
			       h2 == h ? "same" : "DIFFERS");
		}
	}
	return h;
}

static int probe(const char *name, EGLDisplay dpy, struct gbm_device *gbm)
{
	EGLint major, minor, n = 0;
	EGLConfig cfg[1];
	EGLContext ctx;
	EGLSurface surf = EGL_NO_SURFACE;
	struct gbm_surface *gs = NULL;
	const EGLint cfg_attr_surf[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
		EGL_NONE
	};
	const EGLint cfg_attr_win[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE
	};
	const EGLint *cfg_attr = gbm ? cfg_attr_win : cfg_attr_surf;
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

	printf("SMOKE: [%s] dpy=%p\n", name, (void *)dpy);
	if (dpy == EGL_NO_DISPLAY) {
		printf("SMOKE: [%s] eglGetPlatformDisplay failed (0x%x)\n",
		       name, eglGetError());
		return 1;
	}
	if (!eglInitialize(dpy, &major, &minor)) {
		printf("SMOKE: [%s] eglInitialize failed (0x%x)\n",
		       name, eglGetError());
		return 1;
	}
	printf("SMOKE: [%s] EGL_VERSION=%s EGL_VENDOR=%s\n", name,
	       eglQueryString(dpy, EGL_VERSION),
	       eglQueryString(dpy, EGL_VENDOR));
	eglBindAPI(EGL_OPENGL_ES_API);
	if (!eglChooseConfig(dpy, cfg_attr, cfg, 1, &n) || n < 1) {
		printf("SMOKE: [%s] no ES2 config (0x%x)\n", name, eglGetError());
		return 1;
	}
	ctx = eglCreateContext(dpy, cfg[0], EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT) {
		printf("SMOKE: [%s] eglCreateContext failed (0x%x)\n",
		       name, eglGetError());
		return 1;
	}
	if (gbm) {
		gs = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888,
					GBM_BO_USE_RENDERING);
		if (!gs) {
			printf("SMOKE: [%s] gbm_surface_create failed\n", name);
			return 1;
		}
		surf = eglCreateWindowSurface(dpy, cfg[0],
					   (EGLNativeWindowType)(uintptr_t)gs, NULL);
		if (surf == EGL_NO_SURFACE) {
			printf("SMOKE: [%s] eglCreateWindowSurface failed (0x%x)\n",
			       name, eglGetError());
			return 1;
		}
		printf("SMOKE: [%s] gbm_surface %dx%d + EGL window surface created\n",
		       name, W, H);
	}
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
		printf("SMOKE: [%s] eglMakeCurrent failed (0x%x)\n",
		       name, eglGetError());
		return 1;
	}
	printf("SMOKE: [%s] GL_VENDOR=\"%s\"\n", name, glGetString(GL_VENDOR));
	printf("SMOKE: [%s] GL_RENDERER=\"%s\"\n", name, glGetString(GL_RENDERER));
	printf("SMOKE: [%s] GL_VERSION=\"%s\"\n", name, glGetString(GL_VERSION));
	printf("SMOKE: [%s] GL_SHADING_LANGUAGE_VERSION=\"%s\"\n", name,
	       glGetString(GL_SHADING_LANGUAGE_VERSION));
	{
		uint64_t h = render_once();
		printf("SMOKE: [%s] fragment-job csum=0x%016llx %s\n", name,
		       (unsigned long long)h, h ? "RENDERED" : "FAILED");
		if (!h)
			return 1;
	}
	if (surf != EGL_NO_SURFACE) {
		printf("SMOKE: [%s] eglSwapBuffers %s\n", name,
		       eglSwapBuffers(dpy, surf) ? "ok" : "FAILED");
		eglDestroySurface(dpy, surf);
		gbm_surface_destroy(gs);
	}
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "both";
	const char *node = argc > 2 ? argv[2] : "/dev/dri/renderD128";
	int rc = 0;

	if (getenv("SMOKE_W")) W = atoi(getenv("SMOKE_W"));
	if (getenv("SMOKE_H")) H = atoi(getenv("SMOKE_H"));
	get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform_display) {
		fprintf(stderr, "SMOKE: eglGetPlatformDisplayEXT missing\n");
		return 2;
	}
	{
		const char *v = eglQueryString(EGL_NO_DISPLAY, EGL_VERSION);
		printf("SMOKE: client EGL_VERSION=%s\n", v ? v : "(query needs a display)");
	}

	if (!strcmp(mode, "surfaceless") || !strcmp(mode, "both")) {
		/* SMOKE_PAD=<bytes>: allocate a dummy GBM BO after context setup
		 * is impossible here (context lives inside probe), so allocate it
		 * up front and keep it alive across the draw. Used to test whether
		 * the MMU fault addresses track the top of the IOVA space. */
		const char *pad_s = getenv("SMOKE_PAD");
		struct gbm_device *pad_dev = NULL;
		struct gbm_bo *pad_bo = NULL;
		int pad_fd = -1;
		if (pad_s && atoi(pad_s) > 0) {
			unsigned side = (unsigned)atoi(pad_s);
			pad_fd = open(node, O_RDWR | O_CLOEXEC);
			if (pad_fd >= 0)
				pad_dev = gbm_create_device(pad_fd);
			if (pad_dev)
				pad_bo = gbm_bo_create(pad_dev, side, side,
						       GBM_FORMAT_XRGB8888,
						       GBM_BO_USE_RENDERING);
			printf("SMOKE: pad %ux%u bo=%p\n", side, side, (void *)pad_bo);
		}
		EGLDisplay dpy = get_platform_display(
			EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
		rc |= probe("surfaceless", dpy, NULL);
		if (pad_bo)
			gbm_bo_destroy(pad_bo);
		if (pad_dev)
			gbm_device_destroy(pad_dev);
		if (pad_fd >= 0)
			close(pad_fd);
	}
	if (!strcmp(mode, "gbm") || !strcmp(mode, "both")) {
		int fd = open(node, O_RDWR | O_CLOEXEC);
		struct gbm_device *gbm;
		EGLDisplay dpy;
		if (fd < 0) {
			perror("SMOKE: open node");
			return 1;
		}
		gbm = gbm_create_device(fd);
		printf("SMOKE: [gbm] node=%s gbm=%p backend=%s\n", node,
		       (void *)gbm, gbm ? gbm_device_get_backend_name(gbm) : "-");
		if (!gbm) {
			close(fd);
			return 1;
		}
		dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
		rc |= probe("gbm", dpy, gbm);
		gbm_device_destroy(gbm);
		close(fd);
	}
	rc |= verify_fail;
	printf("SMOKE: RESULT %s\n", rc ? "FAIL" : "PASS");
	return rc;
}
