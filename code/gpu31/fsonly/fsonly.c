/* fsonly.c - fragment-path probe: does the fragment job's FS write pixels?
 *
 * A GLES2 offscreen pbuffer is cleared to blue, then a full-screen quad is
 * drawn with the simplest possible fragment shader: a constant magenta.
 * The vertex shader only passes positions through (no varyings, no texture,
 * no uniforms), so any non-clear pixel in the readback can only have been
 * written by the fragment shader - not by the clear, not by geometry.
 *
 * On this GPU the fragment job only rasterises polygons produced by the
 * tiler, so with the VS->tiler handoff broken the quad bins nothing and the
 * readback is all clear.  code/gpu31/polylist/polycensus.c (LD_PRELOAD)
 * supplies the tiler's position buffer from the CPU (`PC2_PREFILL`,
 * `PC2_PREFILL_BIG`) to give the fragment job real coverage without relying
 * on the app's geometry leg.  `PAN_FBD_PRESLEEP_US` (diagnostic Mesa) makes
 * the FS ISA page survive, and `PC2_REFUSE_FRAG` suppresses the fragment
 * submit as an attribution control.
 *
 * Output is line-buffered and stage lines are printed before the blocking
 * glFinish, so a wedged fragment fence still leaves the last stage on the
 * wire.  Exit status is 0 whenever the GL work completed; the verdict line
 * `FS_WROTE=0|1` is what the harness parses.
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define DEF_W 256
#define DEF_H 256

static const char *VS_SRC =
	"attribute vec2 pos;\n"
	"void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_SRC =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";

/* Full-screen quad, two triangles, NDC. */
static const GLfloat QUAD[] = {
	-1.0f, -1.0f,   1.0f, -1.0f,  -1.0f,  1.0f,
	-1.0f,  1.0f,   1.0f, -1.0f,   1.0f,  1.0f,
};

static const EGLint CTX_ATT[] = {
	EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
};

static int g_w = DEF_W, g_h = DEF_H;

static void die_egl(const char *what)
{
	EGLint e = eglGetError();
	fprintf(stderr, "fsonly: %s (eglGetError 0x%04x)\n", what, e);
	exit(1);
}

static void check_gl(const char *what)
{
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		fprintf(stderr, "fsonly: GL error 0x%04x after %s\n", e, what);
		exit(1);
	}
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "fsonly: shader compile failed: %s\n", log);
		exit(1);
	}
	return sh;
}

static void setup_scene(void)
{
	GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
	GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
	GLuint prog = glCreateProgram();
	GLint linked = 0;

	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		fprintf(stderr, "fsonly: program link failed: %s\n", log);
		exit(1);
	}
	glUseProgram(prog);

	glViewport(0, 0, g_w, g_h);
	glClearColor(0.0f, 0.0f, 1.0f, 1.0f);	/* blue */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat),
			      QUAD);
	glEnableVertexAttribArray(0);
	check_gl("scene setup");
}

static void draw_once(void)
{
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	check_gl("draw");
}

static int is_magenta(const unsigned char *p)
{
	return p[0] >= 200 && p[2] >= 200 && p[1] <= 64;
}

static int is_clear(const unsigned char *p)
{
	return p[0] <= 48 && p[1] <= 48 && p[2] >= 176;
}

/* Read the whole target, classify pixels, print a verdict line. */
static void classify(const char *tag)
{
	size_t n = (size_t)g_w * (size_t)g_h * 4;
	unsigned char *buf = malloc(n);
	unsigned long magenta = 0, clear = 0, other = 0;
	int x, y, x0 = g_w, y0 = g_h, x1 = -1, y1 = -1;
	int fx = -1, fy = -1;
	int ox0 = g_w, oy0 = g_h, ox1 = -1, oy1 = -1;
	const unsigned char *s;

	if (!buf) {
		fprintf(stderr, "fsonly: out of memory\n");
		exit(1);
	}
	printf("fsonly: [%s] readback %dx%d ...\n", tag, g_w, g_h);
	fflush(stdout);
	glFinish();
	check_gl("glFinish");
	glReadPixels(0, 0, g_w, g_h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	check_gl("glReadPixels");

	for (y = 0; y < g_h; y++) {
		for (x = 0; x < g_w; x++) {
			const unsigned char *p =
				buf + ((size_t)y * (size_t)g_w + (size_t)x) * 4;
			if (is_magenta(p)) {
				magenta++;
				if (fx < 0) {
					fx = x;
					fy = y;
				}
				if (x < x0) x0 = x;
				if (y < y0) y0 = y;
				if (x > x1) x1 = x;
				if (y > y1) y1 = y;
			} else if (is_clear(p)) {
				clear++;
			} else {
				other++;
				if (x < ox0) ox0 = x;
				if (y < oy0) oy0 = y;
				if (x > ox1) ox1 = x;
				if (y > oy1) oy1 = y;
			}
		}
	}

	printf("fsonly: [%s] pixels total=%d clear=%lu magenta=%lu other=%lu\n",
	       tag, g_w * g_h, clear, magenta, other);
	if (magenta) {
		printf("fsonly: [%s] FS bbox x=%d..%d y=%d..%d first=(%d,%d)\n",
		       tag, x0, x1, y0, y1, fx, fy);
	}
	if (other) {
		printf("fsonly: [%s] other bbox x=%d..%d y=%d..%d\n",
		       tag, ox0, ox1, oy0, oy1);
	}

	{
		static const int pt[3][2] = { {2, 2}, {DEF_W / 2, DEF_H / 2},
					      {0, 0} };
		int i;
		for (i = 0; i < 3; i++) {
			x = pt[i][0] * g_w / DEF_W;
			y = pt[i][1] * g_h / DEF_H;
			s = buf + ((size_t)y * (size_t)g_w + (size_t)x) * 4;
			printf("fsonly: [%s] sample(%d,%d)=%u,%u,%u,%u\n",
			       tag, x, y, s[0], s[1], s[2], s[3]);
		}
	}
	printf("fsonly: [%s] FS_WROTE=%d\n", tag, magenta != 0);
	fflush(stdout);
	free(buf);
}

int main(int argc, char **argv)
{
	EGLDisplay ed;
	EGLConfig cfg;
	EGLint ncfg = 0, major = 0, minor = 0;
	EGLSurface surf;
	EGLContext ctx;
	static const EGLint cfg_att[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_NONE
	};
	EGLint pb_att[] = { EGL_WIDTH, DEF_W, EGL_HEIGHT, DEF_H, EGL_NONE };
	const char *tag = "fsonly";
	int frames = 1, f;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	for (f = 1; f < argc; f++) {
		if (!strcmp(argv[f], "--frames") && f + 1 < argc) {
			frames = atoi(argv[++f]);
		} else if (!strcmp(argv[f], "--size") && f + 1 < argc) {
			if (sscanf(argv[++f], "%dx%d", &g_w, &g_h) != 2 ||
			    g_w < 16 || g_h < 16 || g_w > 4096 || g_h > 4096) {
				fprintf(stderr, "fsonly: bad --size\n");
				return 2;
			}
		} else if (!strcmp(argv[f], "--tag") && f + 1 < argc) {
			tag = argv[++f];
		} else {
			fprintf(stderr, "usage: %s [--frames N] [--size WxH]"
				" [--tag NAME]\n", argv[0]);
			return 2;
		}
	}
	if (frames < 1)
		frames = 1;

	ed = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (ed == EGL_NO_DISPLAY)
		die_egl("eglGetDisplay");
	if (!eglInitialize(ed, &major, &minor))
		die_egl("eglInitialize");
	printf("fsonly: EGL %d.%d vendor='%s' version='%s'\n", major, minor,
	       eglQueryString(ed, EGL_VENDOR), eglQueryString(ed, EGL_VERSION));

	if (!eglChooseConfig(ed, cfg_att, &cfg, 1, &ncfg) || ncfg < 1)
		die_egl("eglChooseConfig found no RGB888 ES2 pbuffer config");
	pb_att[1] = g_w;
	pb_att[3] = g_h;
	surf = eglCreatePbufferSurface(ed, cfg, pb_att);
	if (surf == EGL_NO_SURFACE)
		die_egl("eglCreatePbufferSurface failed");
	ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, CTX_ATT);
	if (ctx == EGL_NO_CONTEXT)
		die_egl("eglCreateContext failed");
	if (!eglMakeCurrent(ed, surf, surf, ctx))
		die_egl("eglMakeCurrent failed");

	printf("fsonly: [%s] GL renderer='%s'\n", tag,
	       glGetString(GL_RENDERER));
	setup_scene();

	for (f = 0; f < frames; f++) {
		printf("fsonly: [%s] draw %d/%d\n", tag, f + 1, frames);
		fflush(stdout);
		draw_once();
		glFinish();
		check_gl("frame glFinish");
		printf("fsonly: [%s] draw %d/%d finished\n", tag, f + 1, frames);
		fflush(stdout);
	}

	classify(tag);

	eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(ed, ctx);
	eglDestroySurface(ed, surf);
	eglTerminate(ed);
	return 0;
}
