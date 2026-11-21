/* microdraw.c - minimal attribute-less triangle: constant gl_Position, solid red.
 * Offscreen 256x256 pbuffer, clear + 1 draw + glFinish, readback 3 pixels.
 * ES 3.0 (#version 300 es) so gl_VertexID selects the vertex constant.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define W 256
#define H 256

static const char *VS_SRC =
	"#version 300 es\n"
	"void main() {\n"
	"  vec2 p = vec2(-0.5, -0.5);\n"
	"  if (gl_VertexID == 1) p = vec2(0.5, -0.5);\n"
	"  else if (gl_VertexID == 2) p = vec2(0.0, 0.5);\n"
	"  gl_Position = vec4(p, 0.0, 1.0);\n"
	"}\n";
static const char *FS_SRC =
	"#version 300 es\n"
	"precision mediump float;\n"
	"out vec4 col;\n"
	"void main() { col = vec4(1.0, 0.0, 0.0, 1.0); }\n";

static void die(const char *m) { fprintf(stderr, "microdraw: %s\n", m); exit(1); }
static void ck(const char *m) { GLenum e = glGetError(); if (e) { fprintf(stderr, "microdraw: GL err %s: 0x%x\n", m, e); exit(1); } }
static GLuint sh(GLenum t, const char *s) {
	GLuint v = glCreateShader(t);
	glShaderSource(v, 1, &s, 0); glCompileShader(v);
	GLint ok = 0; glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
	if (!ok) { char l[1024]; glGetShaderInfoLog(v, sizeof l, 0, l); fprintf(stderr, "microdraw: shader fail: %s\n", l); exit(1); }
	return v;
}

int main(void) {
	EGLDisplay ed = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (ed == EGL_NO_DISPLAY) die("eglGetDisplay");
	EGLint mj, mn;
	if (!eglInitialize(ed, &mj, &mn)) die("eglInitialize");
	if (!eglBindAPI(EGL_OPENGL_ES_API)) die("eglBindAPI");
	printf("microdraw: EGL %d.%d\n", mj, mn);
	static const EGLint ca[] = { EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
		EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE };
	static const EGLint pa[] = { EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE };
	static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE };
	EGLConfig c; EGLint n = 0;
	if (!eglChooseConfig(ed, ca, &c, 1, &n) || !n) die("chooseConfig");
	EGLSurface s = eglCreatePbufferSurface(ed, c, pa);
	if (s == EGL_NO_SURFACE) die("pbuffer");
	EGLContext x = eglCreateContext(ed, c, EGL_NO_CONTEXT, xa);
	if (x == EGL_NO_CONTEXT) die("context");
	if (!eglMakeCurrent(ed, s, s, x)) die("makeCurrent");
	printf("microdraw: GL vendor='%s' renderer='%s' version='%s'\n",
		glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

	GLuint vs = sh(GL_VERTEX_SHADER, VS_SRC), fs = sh(GL_FRAGMENT_SHADER, FS_SRC);
	GLuint p = glCreateProgram();
	glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
	GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) { char l[1024]; glGetProgramInfoLog(p, sizeof l, 0, l); fprintf(stderr, "microdraw: link fail: %s\n", l); exit(1); }
	glUseProgram(p);
	glViewport(0, 0, W, H); ck("setup");
	glClearColor(0.1f, 0.1f, 0.4f, 1.0f);

	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	ck("draw");
	glFinish(); ck("finish");

	unsigned char px[4];
	int pass = 1;
	glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); ck("read");
	printf("microdraw: centre(128,128) RGBA=%u,%u,%u,%u (expect ~255,0,0)\n", px[0],px[1],px[2],px[3]);
	if (!(px[0] > 200 && px[1] < 80 && px[2] < 80)) pass = 0;
	glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); ck("read2");
	printf("microdraw: corner(2,2) RGBA=%u,%u,%u,%u (expect ~26,26,102)\n", px[0],px[1],px[2],px[3]);
	if (!(abs((int)px[0]-26) < 32 && abs((int)px[1]-26) < 32 && abs((int)px[2]-102) < 32)) pass = 0;
	glReadPixels(128, 200, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); ck("read3");
	printf("microdraw: top(128,200) RGBA=%u,%u,%u,%u (expect ~26,26,102)\n", px[0],px[1],px[2],px[3]);
	if (!(abs((int)px[0]-26) < 32 && abs((int)px[1]-26) < 32 && abs((int)px[2]-102) < 32)) pass = 0;

	printf("microdraw: RENDER_%s\n", pass ? "OK" : "MISMATCH");
	eglMakeCurrent(ed, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(ed, x); eglDestroySurface(ed, s); eglTerminate(ed);
	printf("microdraw: %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 3;
}
