/* tri-x11.c - WSI triangle demo: EGL/GLES2 on X11 (fbdev X + Mesa swrast).
 *
 * Path proven here: X11 window surface + eglSwapBuffers. With no DRM node on
 * this kernel the swap goes through the swrast DRI2 path (MIT-SHM/Present
 * X extensions, both advertised by the running server).
 *
 * Proves: (1) EGL window-surface creation on :0, (2) GLES2 draw executes
 * (glReadPixels check of known pixels), (3) present reaches the screen
 * (window stays mapped for --hold seconds for xwd capture).
 *
 * Usage: tri-x11 [--frames N] [--hold SECS] [--size WxH]
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

static void die(const char *msg)
{
	fprintf(stderr, "tri-x11: %s\n", msg);
	exit(1);
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
		fprintf(stderr, "tri-x11: shader compile failed: %s\n", log);
		exit(1);
	}
	return sh;
}

static long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	int frames = 120, hold = 8, ww = 320, wh = 240;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hold") && i + 1 < argc)
			hold = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &ww, &wh) != 2)
				die("bad --size, want WxH");
		} else {
			fprintf(stderr, "usage: %s [--frames N] [--hold S] [--size WxH]\n",
				argv[0]);
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	Display *dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("XOpenDisplay failed");
	int scr = DefaultScreen(dpy);
	Window root = RootWindow(dpy, scr);
	Window win = XCreateSimpleWindow(dpy, root, 200, 500, ww, wh, 1,
					 BlackPixel(dpy, scr),
					 WhitePixel(dpy, scr));
	XStoreName(dpy, win, "wsi-triangle");
	XSelectInput(dpy, win, ExposureMask | StructureNotifyMask);
	XMapWindow(dpy, win);
	/* Wait until mapped so the surface has a real drawable. */
	for (;;) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (ev.type == MapNotify)
			break;
	}

	EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)dpy);
	if (ed == EGL_NO_DISPLAY)
		die("eglGetDisplay failed");
	EGLint major, minor;
	if (!eglInitialize(ed, &major, &minor))
		die("eglInitialize failed");
	printf("tri-x11: EGL vendor='%s' version='%s'\n",
	       eglQueryString(ed, EGL_VENDOR),
	       eglQueryString(ed, EGL_VERSION));
	printf("tri-x11: EGL client APIs='%s'\n",
	       eglQueryString(ed, EGL_CLIENT_APIS));

	static const EGLint cfg_att[] = {
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint ncfg = 0;
	if (!eglChooseConfig(ed, cfg_att, &cfg, 1, &ncfg) || ncfg < 1)
		die("eglChooseConfig found no RGB888 ES2 window config");

	EGLSurface surf =
		eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)win, NULL);
	if (surf == EGL_NO_SURFACE)
		die("eglCreateWindowSurface failed");
	static const EGLint ctx_att[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
	};
	EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, ctx_att);
	if (ctx == EGL_NO_CONTEXT)
		die("eglCreateContext failed");
	if (!eglMakeCurrent(ed, surf, surf, ctx))
		die("eglMakeCurrent failed");
	printf("tri-x11: GL vendor='%s' renderer='%s' version='%s'\n",
	       glGetString(GL_VENDOR), glGetString(GL_RENDERER),
	       glGetString(GL_VERSION));

	static const char *vs_src =
		"attribute vec2 pos;\n"
		"attribute vec3 col;\n"
		"varying vec3 vcol;\n"
		"void main() { vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char *fs_src =
		"precision mediump float;\n"
		"varying vec3 vcol;\n"
		"void main() { gl_FragColor = vec4(vcol, 1.0); }\n";
	GLuint prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "col");
	glLinkProgram(prog);
	GLint linked = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked)
		die("program link failed");
	glUseProgram(prog);

	/* R, G, B corners; NDC coords. */
	static const GLfloat vtx[] = {
		-0.8f, -0.8f, 1.0f, 0.0f, 0.0f,
		 0.8f, -0.8f, 0.0f, 1.0f, 0.0f,
		 0.0f,  0.8f, 0.0f, 0.0f, 1.0f,
	};
	glViewport(0, 0, ww, wh);
	glClearColor(0.1f, 0.1f, 0.4f, 1.0f);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
			      vtx);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
			      vtx + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	long t0 = now_ns();
	for (int f = 0; f < frames; f++) {
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		if (!eglSwapBuffers(ed, surf)) {
			fprintf(stderr, "tri-x11: eglSwapBuffers failed at frame %d\n",
				f);
			return 1;
		}
	}
	long t1 = now_ns();
	double secs = (t1 - t0) / 1e9;
	printf("tri-x11: %d frames in %.3f s = %.1f FPS (%dx%d)\n",
	       frames, secs, frames / secs, ww, wh);

	/* In-render verification: redraw once, read known pixels back. */
	/* Corner (2,2): outside triangle -> clear color ~(26,26,102). */
	/* Center NDC (0,0): barycentric a=b=0.25 (R,G), c=0.5 (B) -> (64,64,128). */
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	unsigned char px[4];
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("tri-x11: readback corner(2,2) RGBA=%u,%u,%u,%u (expect ~26,26,102)\n",
	       px[0], px[1], px[2], px[3]);
	glReadPixels(ww / 2, wh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("tri-x11: readback center(%d,%d) RGBA=%u,%u,%u,%u (expect ~64,64,128)\n",
	       ww / 2, wh / 2, px[0], px[1], px[2], px[3]);
	int ok = 1;
	unsigned char c[4];
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
	if (abs(c[0] - 26) > 12 || abs(c[1] - 26) > 12 ||
	    abs(c[2] - 102) > 12)
		ok = 0;
	glReadPixels(ww / 2, wh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
	if (abs(c[0] - 64) > 16 || abs(c[1] - 64) > 16 ||
	    abs(c[2] - 128) > 16)
		ok = 0;
	printf("tri-x11: RENDER_%s\n", ok ? "OK" : "MISMATCH");
	eglSwapBuffers(ed, surf);

	printf("tri-x11: holding window %d s for capture...\n", hold);
	long deadline = now_ns() + (long)hold * 1000000000L;
	while (now_ns() < deadline) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
		}
		{ struct timespec rq = { 0, 50000000L }; nanosleep(&rq, NULL); }
	}

	eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(ed, ctx);
	eglDestroySurface(ed, surf);
	eglTerminate(ed);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	printf("tri-x11: done\n");
	return ok ? 0 : 3;
}
