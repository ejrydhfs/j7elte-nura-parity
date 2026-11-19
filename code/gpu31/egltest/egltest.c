/* egltest.c - EGL/GLES2 textured-triangle smoke test.
 *
 * Offscreen: 256x256 EGL pbuffer, textured triangle, glReadPixels check,
 *   optional PPM dump (--dump FILE).
 * Onscreen: X11 window surface, N swapped frames, same readback check.
 *
 * Prints EGL + GL vendor/renderer/version strings first.
 * Exit: 0 = every executed check passed (onscreen skipped with a note
 *         when no DISPLAY is set),
 *       1 = EGL/GL setup or API error,
 *       2 = usage error,
 *       3 = pixel self-check mismatch.
 *
 * The texture is a 2x2 RGBA checker (bottom row red|green, top row
 * blue|white), NEAREST filtered. Triangle UVs are chosen so the centre
 * pixel samples texel (0,0) = red: weights at NDC (0,0) are a=b=0.25,
 * c=0.5, giving uv=(0.25,0.25), the centre of that texel.
 *
 * Force software GL for the llvmpipe baseline:
 *   GALLIUM_DRIVER=llvmpipe ./egltest
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define PBUF_W 256
#define PBUF_H 256
#define OFF_FRAMES 60

/* 2x2 RGBA texture, row 0 (v = 0, bottom) first. */
static const GLubyte TEX_RGBA[2 * 2 * 4] = {
	/* bottom row: red, green */
	255, 0, 0, 255,
	0, 255, 0, 255,
	/* top row: blue, white */
	0, 0, 255, 255,
	255, 255, 255, 255,
};

static const char *VS_SRC =
	"attribute vec2 pos;\n"
	"attribute vec2 uv;\n"
	"varying vec2 vuv;\n"
	"void main() { vuv = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_SRC =
	"precision mediump float;\n"
	"varying vec2 vuv;\n"
	"uniform sampler2D tex;\n"
	"void main() { gl_FragColor = texture2D(tex, vuv); }\n";

/* NDC triangle + UVs (see file comment for the centre-pixel math). */
static const GLfloat VTX[] = {
	/*   x       y       u      v   */
	-0.8f, -0.8f,  0.0f, 0.0f,	/* A */
	 0.8f, -0.8f,  1.0f, 0.0f,	/* B */
	 0.0f,  0.8f,  0.0f, 0.5f,	/* C */
};

static const EGLint CTX_ATT[] = {
	EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
};

static void die_egl(const char *what)
{
	EGLint e = eglGetError();
	fprintf(stderr, "egltest: %s (eglGetError 0x%04x)\n", what, e);
	exit(1);
}

static void check_gl(const char *what)
{
	GLenum e = glGetError();
	if (e != GL_NO_ERROR) {
		fprintf(stderr, "egltest: GL error 0x%04x after %s\n", e, what);
		exit(1);
	}
}

static long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static const char *qstr(EGLDisplay ed, EGLint name)
{
	const char *s = eglQueryString(ed, name);
	return s ? s : "(null)";
}

static void print_egl_info(EGLDisplay ed, EGLint major, EGLint minor)
{
	printf("egltest: EGL vendor='%s' version='%s' (%d.%d)\n",
	       qstr(ed, EGL_VENDOR), qstr(ed, EGL_VERSION),
	       (int)major, (int)minor);
	printf("egltest: EGL client APIs='%s'\n", qstr(ed, EGL_CLIENT_APIS));
}

static void print_gl_info(const char *tag)
{
	printf("egltest: [%s] GL vendor='%s' renderer='%s' version='%s'\n",
	       tag, glGetString(GL_VENDOR), glGetString(GL_RENDERER),
	       glGetString(GL_VERSION));
	printf("egltest: [%s] GL shading language='%s'\n",
	       tag, glGetString(GL_SHADING_LANGUAGE_VERSION));
	check_gl("reading GL strings");
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;
	check_gl("glCreateShader");
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "egltest: shader compile failed: %s\n", log);
		exit(1);
	}
	return sh;
}

/* Compile/link/texture/viewport for the current context. */
static void setup_scene(int ww, int wh)
{
	GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
	GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
	GLuint prog = glCreateProgram();
	GLuint tex;
	GLint linked = 0, loc;
	check_gl("glCreateProgram");
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "uv");
	glLinkProgram(prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof(log), NULL, log);
		fprintf(stderr, "egltest: program link failed: %s\n", log);
		exit(1);
	}
	glUseProgram(prog);
	loc = glGetUniformLocation(prog, "tex");
	glUniform1i(loc, 0);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
			GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
			GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0,
		     GL_RGBA, GL_UNSIGNED_BYTE, TEX_RGBA);
	check_gl("texture upload");

	glViewport(0, 0, ww, wh);
	glClearColor(0.1f, 0.1f, 0.4f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
			      4 * sizeof(GLfloat), VTX);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
			      4 * sizeof(GLfloat), VTX + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	check_gl("scene setup");
}

static void draw_once(void)
{
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Corner (2,2) is outside the triangle -> clear color ~(26,26,102).
 * Centre samples texel (0,0) -> red ~(255,0,0). */
static int verify_pixels(int ww, int wh, const char *tag)
{
	unsigned char px[4];
	int ok = 1;

	glFinish();
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("egltest: [%s] readback corner(2,2) RGBA=%u,%u,%u,%u"
	       " (expect ~26,26,102)\n",
	       tag, px[0], px[1], px[2], px[3]);
	if (abs((int)px[0] - 26) > 24 || abs((int)px[1] - 26) > 24 ||
	    abs((int)px[2] - 102) > 24)
		ok = 0;

	glReadPixels(ww / 2, wh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("egltest: [%s] readback center(%d,%d) RGBA=%u,%u,%u,%u"
	       " (expect ~255,0,0)\n",
	       tag, ww / 2, wh / 2, px[0], px[1], px[2], px[3]);
	if (abs((int)px[0] - 255) > 32 || px[1] > 32 || px[2] > 32)
		ok = 0;
	check_gl("readback");

	printf("egltest: [%s] RENDER_%s\n", tag, ok ? "OK" : "MISMATCH");
	return ok;
}

static void dump_ppm(const char *path, int ww, int wh)
{
	size_t n = (size_t)ww * (size_t)wh * 4;
	unsigned char *buf = malloc(n);
	FILE *f;
	int x, y;
	if (!buf) {
		fprintf(stderr, "egltest: out of memory for dump\n");
		exit(1);
	}
	glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
	check_gl("dump readback");
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "egltest: cannot open dump '%s'\n", path);
		free(buf);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", ww, wh);
	for (y = wh - 1; y >= 0; y--) {
		for (x = 0; x < ww; x++) {
			unsigned char *p = buf + ((size_t)y * (size_t)ww +
						  (size_t)x) * 4;
			fputc(p[0], f);
			fputc(p[1], f);
			fputc(p[2], f);
		}
	}
	fclose(f);
	free(buf);
	printf("egltest: [offscreen] dumped %s (%dx%d PPM)\n", path, ww, wh);
}

static int run_offscreen(EGLDisplay ed, const char *dump)
{
	static const EGLint cfg_att[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_NONE
	};
	static const EGLint pb_att[] = {
		EGL_WIDTH, PBUF_W, EGL_HEIGHT, PBUF_H, EGL_NONE
	};
	EGLConfig cfg;
	EGLint ncfg = 0;
	EGLSurface surf;
	EGLContext ctx;
	long t0, t1;
	double secs;
	int f, ok;

	if (!eglChooseConfig(ed, cfg_att, &cfg, 1, &ncfg) || ncfg < 1)
		die_egl("offscreen eglChooseConfig found no RGB888 ES2 pbuffer"
			" config");
	surf = eglCreatePbufferSurface(ed, cfg, pb_att);
	if (surf == EGL_NO_SURFACE)
		die_egl("eglCreatePbufferSurface failed");
	ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, CTX_ATT);
	if (ctx == EGL_NO_CONTEXT)
		die_egl("offscreen eglCreateContext failed");
	if (!eglMakeCurrent(ed, surf, surf, ctx))
		die_egl("offscreen eglMakeCurrent failed");

	print_gl_info("offscreen");
	setup_scene(PBUF_W, PBUF_H);
	t0 = now_ns();
	for (f = 0; f < OFF_FRAMES; f++)
		draw_once();
	glFinish();
	check_gl("offscreen draws");
	t1 = now_ns();
	secs = (t1 - t0) / 1e9;
	printf("egltest: [offscreen] %d draws in %.3f s = %.1f FPS (%dx%d)\n",
	       OFF_FRAMES, secs, OFF_FRAMES / secs, PBUF_W, PBUF_H);

	draw_once();
	ok = verify_pixels(PBUF_W, PBUF_H, "offscreen");
	if (dump)
		dump_ppm(dump, PBUF_W, PBUF_H);

	eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(ed, ctx);
	eglDestroySurface(ed, surf);
	return ok;
}

static int run_onscreen(Display *dpy, EGLDisplay ed, int frames,
			int ww, int wh, int hold)
{
	static const EGLint cfg_att[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE
	};
	int scr = DefaultScreen(dpy);
	Window root = RootWindow(dpy, scr);
	Window win;
	EGLConfig cfg;
	EGLint ncfg = 0;
	EGLSurface surf;
	EGLContext ctx;
	long t0, t1;
	double secs;
	int f, ok;

	win = XCreateSimpleWindow(dpy, root, 200, 500, (unsigned)ww,
				  (unsigned)wh, 1,
				  BlackPixel(dpy, scr), WhitePixel(dpy, scr));
	XStoreName(dpy, win, "egltest");
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
	XMapWindow(dpy, win);
	for (;;) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (ev.type == MapNotify)
			break;
	}

	if (!eglChooseConfig(ed, cfg_att, &cfg, 1, &ncfg) || ncfg < 1)
		die_egl("onscreen eglChooseConfig found no RGB888 ES2 window"
			" config");
	surf = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)win,
				      NULL);
	if (surf == EGL_NO_SURFACE)
		die_egl("eglCreateWindowSurface failed");
	ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, CTX_ATT);
	if (ctx == EGL_NO_CONTEXT)
		die_egl("onscreen eglCreateContext failed");
	if (!eglMakeCurrent(ed, surf, surf, ctx))
		die_egl("onscreen eglMakeCurrent failed");

	print_gl_info("onscreen");
	setup_scene(ww, wh);
	t0 = now_ns();
	for (f = 0; f < frames; f++) {
		draw_once();
		if (!eglSwapBuffers(ed, surf)) {
			fprintf(stderr,
				"egltest: eglSwapBuffers failed at frame %d\n",
				f);
			exit(1);
		}
	}
	t1 = now_ns();
	secs = (t1 - t0) / 1e9;
	printf("egltest: [onscreen] %d frames in %.3f s = %.1f FPS (%dx%d)\n",
	       frames, secs, frames / secs, ww, wh);

	draw_once();
	glFinish();
	ok = verify_pixels(ww, wh, "onscreen");
	eglSwapBuffers(ed, surf);

	if (hold > 0) {
		long deadline;
		printf("egltest: holding window %d s for capture...\n", hold);
		deadline = now_ns() + (long)hold * 1000000000L;
		while (now_ns() < deadline) {
			while (XPending(dpy)) {
				XEvent ev;
				XNextEvent(dpy, &ev);
			}
			{
				struct timespec rq = { 0, 50000000L };
				nanosleep(&rq, NULL);
			}
		}
	}

	eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(ed, ctx);
	eglDestroySurface(ed, surf);
	XDestroyWindow(dpy, win);
	return ok;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--offscreen-only] [--onscreen-only] [--frames N]\n"
		"            [--hold SECS] [--size WxH] [--dump FILE.ppm]\n",
		prog);
}

int main(int argc, char **argv)
{
	int off_only = 0, on_only = 0;
	int frames = 60, hold = 1, ww = 320, wh = 240;
	const char *dump = NULL;
	const char *xname;
	Display *dpy = NULL;
	EGLDisplay ed;
	EGLint major = 0, minor = 0;
	int want_on, ok = 1, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--offscreen-only")) {
			off_only = 1;
		} else if (!strcmp(argv[i], "--onscreen-only")) {
			on_only = 1;
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--hold") && i + 1 < argc) {
			hold = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &ww, &wh) != 2 ||
			    ww <= 0 || wh <= 0) {
				fprintf(stderr, "egltest: bad --size, want"
					" WxH\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			dump = argv[++i];
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if ((off_only && on_only) || frames < 0 || hold < 0) {
		usage(argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	xname = getenv("DISPLAY");
	want_on = !off_only;
	if (on_only && !xname) {
		fprintf(stderr, "egltest: --onscreen-only needs DISPLAY\n");
		return 1;
	}
	if (want_on && !xname) {
		printf("egltest: no DISPLAY, onscreen skipped\n");
		want_on = 0;
	}

	if (want_on) {
		dpy = XOpenDisplay(NULL);
		if (!dpy) {
			fprintf(stderr, "egltest: XOpenDisplay failed\n");
			return 1;
		}
		ed = eglGetDisplay((EGLNativeDisplayType)dpy);
	} else {
		ed = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	}
	if (ed == EGL_NO_DISPLAY)
		die_egl("eglGetDisplay failed");
	if (!eglInitialize(ed, &major, &minor))
		die_egl("eglInitialize failed");
	print_egl_info(ed, major, minor);
	if (!eglBindAPI(EGL_OPENGL_ES_API))
		die_egl("eglBindAPI(OPENGL_ES) failed");

	if (!on_only)
		ok &= run_offscreen(ed, dump);
	if (want_on)
		ok &= run_onscreen(dpy, ed, frames, ww, wh, hold);

	eglTerminate(ed);
	if (dpy)
		XCloseDisplay(dpy);
	printf("egltest: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 3;
}
