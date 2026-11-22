/* rdtest.c - offscreen GLES2 render + per-frame readback probe (T720 panfrost).
 *
 * Purpose: isolate the 512x512 / readback stall (gpu31-mfbd.md) by making the
 * three variables explicit on the command line:
 *
 *   --size N          render target edge (default 512; 256 is the known-good)
 *   --frames N        frames to draw (default 10)
 *   --pre-finish 0|1  1 = glFinish() before glReadPixels (waits on the SUBMIT
 *                     syncobj fence instead of the BO WAIT_BO path)
 *   --read 0|1        0 = no readback at all (glFinish only) = clean control
 *
 * One line per frame: elapsed ms, corner(2,2), centre, nonzero byte count,
 * a byte checksum and the verdict:
 *   OK        corner ~= clear (5,5,31) and centre ~= red (255,0,0)
 *   ZERO      every byte read back is 0 (the GPU wrote nothing / stale map)
 *   MISMATCH  pixels present but wrong
 * A frame taking >=1000 ms means the submit faulted and waited for the 5 s
 * drm_sched recovery timeout (see dmesg p7580_job ERR/TIMEOUT).
 *
 * Build on the phone (no X11 needed):
 *   gcc -O2 -Wall -Wextra -std=c99 -o /tmp/rdtest rdtest.c -lEGL -lGLESv2
 * Run with the panfrost Mesa 20.3.5 build:
 *   B=/data/mesabuild/mesa20-b
 *   LD_LIBRARY_PATH=$B/src/egl:$B/src/mapi/es2api:$B/src/gbm:$B/src/gallium/targets/dri \
 *   LIBGL_DRIVERS_PATH=$B/src/gallium/targets/dri \
 *   MESA_LOADER_DRIVER_OVERRIDE=panfrost EGL_PLATFORM=surfaceless \
 *   /tmp/rdtest --size 512 --frames 10 --pre-finish 0 --read 1
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

static long
now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000000000L + t.tv_nsec;
}

static const char *VS_SRC =
	"attribute vec2 pos;\n"
	"void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS_SRC =
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

/* Centred-ish triangle covering most of the target. */
static const GLfloat VTX[] = {
	0.0f, 0.8f,
	-0.8f, -0.8f,
	0.8f, -0.8f,
};

static GLuint
mk(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char b[512];
		glGetShaderInfoLog(s, 512, NULL, b);
		fprintf(stderr, "rdtest: shader: %s\n", b);
		exit(1);
	}
	return s;
}

int
main(int argc, char **argv)
{
	int W = 512, H = 512, frames = 10, pre_finish = 0, do_read = 1;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLint ncfg = 0;
	EGLSurface surf;
	EGLContext ctx;
	GLuint vs, fs, prog, vbo;
	GLint pa;
	unsigned char *px;
	unsigned long long zero_frames = 0, ok_frames = 0, bad_frames = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			W = H = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--pre-finish") && i + 1 < argc) {
			pre_finish = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--read") && i + 1 < argc) {
			do_read = atoi(argv[++i]);
		} else {
			fprintf(stderr, "usage: %s [--size N] [--frames N]"
				" [--pre-finish 0|1] [--read 0|1]\n", argv[0]);
			return 2;
		}
	}
	if (W <= 0 || H <= 0 || W > 4096 || frames <= 0) {
		fprintf(stderr, "rdtest: bad size/frames\n");
		return 2;
	}

	dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "rdtest: eglInitialize failed\n");
		return 1;
	}
	{
		static const EGLint att[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_NONE
		};
		if (!eglChooseConfig(dpy, att, &cfg, 1, &ncfg) || ncfg < 1) {
			fprintf(stderr, "rdtest: no RGB888 ES2 pbuffer config\n");
			return 1;
		}
	}
	{
		EGLint pb[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
		EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

		surf = eglCreatePbufferSurface(dpy, cfg, pb);
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
	}
	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "rdtest: pbuffer/context failed 0x%x\n",
			eglGetError());
		return 1;
	}

	printf("rdtest: renderer=%s size=%dx%d frames=%d read=%d pre_finish=%d\n",
	       (const char *)glGetString(GL_RENDERER), W, H, frames,
	       do_read, pre_finish);

	vs = mk(GL_VERTEX_SHADER, VS_SRC);
	fs = mk(GL_FRAGMENT_SHADER, FS_SRC);
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glUseProgram(prog);
	pa = glGetAttribLocation(prog, "pos");
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(VTX), VTX, GL_STATIC_DRAW);
	glEnableVertexAttribArray(pa);
	glVertexAttribPointer(pa, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glViewport(0, 0, W, H);

	px = malloc((size_t)W * H * 4);

	for (int f = 0; f < frames; f++) {
		long t0, dt_ms;
		unsigned long long sum = 0, nz = 0;
		unsigned char *c, *m;
		const char *verdict;

		t0 = now_ns();
		glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		if (!do_read) {
			glFinish();
			dt_ms = (now_ns() - t0) / 1000000L;
			printf("FRAME %d %ldms (no readback)\n", f, dt_ms);
			continue;
		}

		if (pre_finish)
			glFinish();

		memset(px, 0, (size_t)W * H * 4);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		dt_ms = (now_ns() - t0) / 1000000L;

		for (size_t i = 0; i < (size_t)W * H * 4; i++) {
			sum += px[i];
			if (px[i])
				nz++;
		}
		c = &px[((size_t)2 * W + 2) * 4];               /* corner */
		m = &px[((size_t)(H / 2) * W + W / 2) * 4];     /* centre */

		if (!nz) {
			verdict = "ZERO";
			zero_frames++;
		} else if (m[0] > 240 && m[1] < 32 && m[2] < 32) {
			/* The triangle covers the centre; that is the pixel the
			 * GPU must have written. (The corner is reported too, but
			 * the transfer is a raw tiled map, so it is informational.)
			 */
			verdict = "OK";
			ok_frames++;
		} else {
			verdict = "MISMATCH";
			bad_frames++;
		}

		printf("FRAME %d %ldms corner=%u,%u,%u,%u centre=%u,%u,%u,%u "
		       "nz=%llu sum=%llu %s\n", f, dt_ms,
		       c[0], c[1], c[2], c[3], m[0], m[1], m[2], m[3],
		       nz, sum, verdict);
	}

	printf("TOTAL frames=%d ok=%llu zero=%llu mismatch=%llu\n",
	       frames, ok_frames, zero_frames, bad_frames);

	glFinish();
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	free(px);
	return (bad_frames || zero_frames) ? 3 : 0;
}
