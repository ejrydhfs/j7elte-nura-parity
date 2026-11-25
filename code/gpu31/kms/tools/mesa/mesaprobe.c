/* mesaprobe.c - which Gallium driver does Mesa/Gbm/EGL pick for a DRM node?
 *
 * Answers, for one node:
 *   --ver <node>       version + caps only (never touches modeset ioctls)
 *   --info <node>      kernel driver name (drmGetVersion), libdrm bus type,
 *                      DRM_CAP_DUMB_BUFFER/PRIME, every DRM device libdrm
 *                      knows, and whether the node is a KMS node (has a CRTC),
 *                      plus whether the plane advertises IN_FORMATS.
 *   --opens <node>     drmOpenWithType() for the names Mesa 20 kmsro asks for
 *                      ("panfrost" render, "exynos" card).  This is exactly
 *                      the lookup kmsro_drm_winsys.c does in Mesa 20.
 *   --gbm <node>       gbm_create_device() + EGL + GLES2, print gbm backend,
 *                      EGL vendor, GL_RENDERER (this is the kmscube path).
 *   --default          EGL_PLATFORM-driven display (surfaceless matrix cell);
 *                      honours $EGL_PLATFORM like kmscube does not.
 *
 * With --gbm --render it additionally creates a gbm *window* surface
 * (GBM_BO_USE_SCANOUT|RENDERING), draws one frame and eglSwapBuffers() so the
 * renderonly/kmsro buffer allocation path runs, then prints the front BO's
 * fd/handle/stride/modifier.  With --addfb it also feeds that BO back to the
 * KMS node (drmPrimeFDToHandle + drmModeAddFB2WithModifiers / drmModeAddFB2 /
 * drmModeAddFB) - the import path scanout needs.  Nothing commits a CRTC, so
 * this never touches the page-flip path.
 *
 * Output lines starting with "PROBE " are the machine-readable results.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

#define MAX_DEVS 32

static const char *node = NULL;
static bool opt_info, opt_ver, opt_opens, opt_gbm, opt_default, opt_render,
	opt_addfb;
static int opt_w = 720, opt_h = 1280;

static void usage(void)
{
	fprintf(stderr,
		"usage: mesaprobe --info|--ver|--opens|--gbm <node>|--default "
		"[--render] [--addfb] [--size WxH]\n");
	exit(2);
}

static void print_ver(const char *tag, int fd)
{
	drmVersionPtr v = drmGetVersion(fd);
	if (!v) {
		printf("PROBE %s: drmGetVersion failed: %s\n", tag, strerror(errno));
		return;
	}
	printf("PROBE %s: kernel_driver=%.*s desc=%.*s date=%.*s\n", tag,
	       v->name_len, v->name, v->desc_len, v->desc,
	       v->date_len, v->date);
	drmFreeVersion(v);
}

static void print_caps(int fd)
{
	uint64_t dumb = 0, prime = 0;

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb) == 0)
		printf("PROBE cap: DUMB_BUFFER=%llu\n", (unsigned long long)dumb);
	if (drmGetCap(fd, DRM_CAP_PRIME, &prime) == 0)
		printf("PROBE cap: PRIME=0x%llx (import=%d export=%d)\n",
		       (unsigned long long)prime,
		       !!(prime & DRM_PRIME_CAP_IMPORT), !!(prime & DRM_PRIME_CAP_EXPORT));
}

/* KMS resources + whether any plane carries an IN_FORMATS blob. */
static void print_modeset(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		printf("PROBE kms: no mode resources (%s) -> not a KMS node\n",
		       strerror(errno));
		return;
	}
	printf("PROBE kms: crtcs=%d connectors=%d encoders=%d fbs=%d\n",
	       res->count_crtcs, res->count_connectors, res->count_encoders,
	       res->count_fbs);

	drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
	if (pr) {
		int informats = 0;
		for (uint32_t i = 0; i < pr->count_planes; i++) {
			drmModeObjectProperties *props =
				drmModeObjectGetProperties(fd, pr->planes[i],
							   DRM_MODE_OBJECT_PLANE);
			if (!props)
				continue;
			for (uint32_t p = 0; p < props->count_props; p++) {
				drmModePropertyRes *prop =
					drmModeGetProperty(fd, props->props[p]);
				if (!prop)
					continue;
				if (!strcmp(prop->name, "IN_FORMATS"))
					informats++;
				drmModeFreeProperty(prop);
			}
			drmModeFreeObjectProperties(props);
		}
		printf("PROBE kms: planes=%u planes_with_IN_FORMATS=%d\n",
		       pr->count_planes, informats);
		drmModeFreePlaneResources(pr);
	}
	drmModeFreeResources(res);
}

static void print_devices(void)
{
	drmDevicePtr devs[MAX_DEVS];
	int n = drmGetDevices2(0, devs, MAX_DEVS);
	if (n <= 0) {
		printf("PROBE devices: drmGetDevices2=%d\n", n);
		return;
	}
	for (int i = 0; i < n; i++) {
		/* xf86drm.h: PCI=0, USB=1, PLATFORM=2, HOST1X=3, FAUX=4 */
		static const char *busname[] = { "pci", "usb", "platform",
						 "host1x", "faux" };
		const char *bus = (unsigned)devs[i]->bustype < 5 ?
				  busname[devs[i]->bustype] : "?";
		printf("PROBE device[%d]: bus=%s", i, bus);
		for (int t = 0; t < DRM_NODE_MAX; t++) {
			static const char *tname[] = { "primary", "control", "render" };
			if (!(devs[i]->available_nodes & (1 << t)))
				continue;
			printf(" %s=%s", tname[t], devs[i]->nodes[t]);
		}
		if (devs[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
			int fd = open(devs[i]->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				drmVersionPtr v = drmGetVersion(fd);
				printf(" render_driver=%s", v ? v->name : "?");
				drmFreeVersion(v);
				close(fd);
			}
		}
		printf("\n");
	}
	drmFreeDevices(devs, n);
}

/* The exact lookups Mesa 20's kmsro performs (drmOpenWithType("panfrost",
 * DRM_NODE_RENDER)), plus the pre-2d19f91 literal so a stale module/kernel
 * shows up instead of silently "not found". */
static void do_opens(void)
{
	struct { const char *name; int type; } tries[] = {
		{ "exynos",    DRM_NODE_PRIMARY },
		{ "exynos7580", DRM_NODE_PRIMARY },
		{ "panfrost",  DRM_NODE_RENDER },
		{ "panfrost7580", DRM_NODE_RENDER },
	};
	for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
		int fd = drmOpenWithType(tries[i].name, NULL, tries[i].type);
		if (fd >= 0)
			printf("PROBE openWithType: name=%s type=%s -> %d\n",
			       tries[i].name,
			       tries[i].type == DRM_NODE_RENDER ? "render" : "card",
			       fd);
		else
			printf("PROBE openWithType: name=%s type=%s -> -1 (%s)\n",
			       tries[i].name,
			       tries[i].type == DRM_NODE_RENDER ? "render" : "card",
			       strerror(errno));
		if (fd >= 0)
			close(fd);
	}
}

static EGLDisplay get_display(struct gbm_device *gbm)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = EGL_NO_DISPLAY;

	if (opt_default) {
		dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (dpy != EGL_NO_DISPLAY)
			return dpy;
		if (get_plat)
			dpy = get_plat(EGL_PLATFORM_SURFACELESS_MESA,
				       EGL_DEFAULT_DISPLAY, NULL);
		return dpy;
	}
	if (get_plat)
		dpy = get_plat(EGL_PLATFORM_GBM_KHR, (void *)gbm, NULL);
	if (dpy == EGL_NO_DISPLAY)
		dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
	return dpy;
}

static int run_egl(struct gbm_device *gbm, int kms_fd)
{
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLint ncfg = 0;
	EGLContext ctx;
	EGLSurface surf = EGL_NO_SURFACE;
	const char *r;

	dpy = get_display(gbm);
	if (dpy == EGL_NO_DISPLAY) {
		printf("PROBE egl: no display (0x%x)\n", eglGetError());
		return 1;
	}
	if (!eglInitialize(dpy, NULL, NULL)) {
		printf("PROBE egl: eglInitialize failed 0x%x\n", eglGetError());
		return 1;
	}
	printf("PROBE egl: vendor=%s version=%s\n",
	       eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		printf("PROBE egl: bindAPI failed\n");
		return 1;
	}
	{
		EGLint attr[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_NONE
		};
		/* Surfaceless/pbuffer-only platforms have no window config: fall
		 * back to a config without a surface-type constraint so the
		 * renderer string can still be queried with EGL_NO_SURFACE. */
		EGLint pb_attr[] = {
			EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_NONE
		};
		if (!eglChooseConfig(dpy, attr, &cfg, 1, &ncfg) || ncfg < 1) {
			printf("PROBE egl: no window config (pbuffer fallback)\n");
			if (!eglChooseConfig(dpy, pb_attr, &cfg, 1, &ncfg) ||
			    ncfg < 1) {
				printf("PROBE egl: no config at all\n");
				return 1;
			}
			opt_render = false;
		}
	}
	{
		EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, attr);
	}
	if (ctx == EGL_NO_CONTEXT) {
		printf("PROBE egl: no context 0x%x\n", eglGetError());
		return 1;
	}
	if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		printf("PROBE egl: surfaceless makeCurrent failed 0x%x\n",
		       eglGetError());
		return 1;
	}
	r = (const char *)glGetString(GL_RENDERER);
	printf("PROBE renderer: %s\n", r ? r : "(null)");
	printf("PROBE gl: vendor=%s version=%s\n",
	       (const char *)glGetString(GL_VENDOR),
	       (const char *)glGetString(GL_VERSION));

	if (!opt_render)
		return 0;

	/* The kmsro/renderonly allocation path: a scanout-capable gbm surface. */
	surf = EGL_NO_SURFACE;
	struct gbm_surface *gs =
		gbm_surface_create(gbm, opt_w, opt_h, GBM_FORMAT_XRGB8888,
				   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gs) {
		printf("PROBE gbm_surface: create failed (%s)\n", strerror(errno));
		return 1;
	}
	surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
	if (surf == EGL_NO_SURFACE) {
		printf("PROBE gbm_surface: eglCreateWindowSurface failed 0x%x\n",
		       eglGetError());
		return 1;
	}
	if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
		printf("PROBE gbm_surface: makeCurrent failed 0x%x\n", eglGetError());
		return 1;
	}
	glViewport(0, 0, opt_w, opt_h);
	glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	printf("PROBE gbm_surface: glGetError=0x%x\n", glGetError());
	if (!eglSwapBuffers(dpy, surf)) {
		printf("PROBE gbm_surface: eglSwapBuffers FAILED 0x%x\n",
		       eglGetError());
		return 1;
	}
	printf("PROBE gbm_surface: eglSwapBuffers ok\n");

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
	if (!bo) {
		printf("PROBE gbm_bo: lock_front_buffer failed (%s)\n",
		       strerror(errno));
		return 1;
	}
	int bo_fd = gbm_bo_get_fd(bo);
	uint64_t mod = gbm_bo_get_modifier(bo);
	printf("PROBE gbm_bo: fd=%d handle=%u stride=%u format=0x%x modifier=0x%llx\n",
	       bo_fd, gbm_bo_get_handle(bo).u32, gbm_bo_get_stride(bo),
	       gbm_bo_get_format(bo), (unsigned long long)mod);

	if (opt_addfb && kms_fd >= 0) {
		uint32_t handle = 0;
		int ret = drmPrimeFDToHandle(kms_fd, bo_fd, &handle);
		printf("PROBE prime: drmPrimeFDToHandle(kms fd, bo fd) -> %d handle=%u%s\n",
		       ret, handle, ret ? strerror(errno) : "");
		if (ret == 0) {
			uint32_t h[4] = { handle, 0, 0, 0 };
			uint32_t pitch[4] = { gbm_bo_get_stride(bo), 0, 0, 0 };
			uint32_t off[4] = { 0, 0, 0, 0 };
			uint64_t mods[4] = { mod, 0, 0, 0 };
			uint32_t fb = 0;
			ret = drmModeAddFB2WithModifiers(kms_fd, opt_w, opt_h,
							 GBM_FORMAT_XRGB8888, h, pitch,
							 off, mods, &fb,
							 DRM_MODE_FB_MODIFIERS);
			printf("PROBE addfb2_mod: -> %d fb=%u%s\n", ret, fb,
			       ret ? strerror(errno) : "");
			if (ret) {
				fb = 0;
				ret = drmModeAddFB2(kms_fd, opt_w, opt_h,
						    GBM_FORMAT_XRGB8888, h, pitch,
						    off, &fb, 0);
				printf("PROBE addfb2: -> %d fb=%u%s\n", ret, fb,
				       ret ? strerror(errno) : "");
			}
			if (fb)
				drmModeRmFB(kms_fd, fb);
		}
	}
	if (bo)
		gbm_surface_release_buffer(gs, bo);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
	eglDestroySurface(dpy, surf);
	gbm_surface_destroy(gs);
	return 0;
}

int main(int argc, char **argv)
{
	int kms_fd = -1;

	/* Line-buffered: on a KMS node whose modeset lock is wedged the probe
	 * blocks inside an ioctl, and the last printed line must say which. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--info"))
			opt_info = true;
		else if (!strcmp(argv[i], "--ver"))
			opt_ver = true;
		else if (!strcmp(argv[i], "--opens"))
			opt_opens = true;
		else if (!strcmp(argv[i], "--gbm"))
			opt_gbm = true;
		else if (!strcmp(argv[i], "--default"))
			opt_default = true;
		else if (!strcmp(argv[i], "--render"))
			opt_render = true;
		else if (!strcmp(argv[i], "--addfb"))
			opt_addfb = true;
		else if (!strcmp(argv[i], "--size") && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &opt_w, &opt_h);
		else if (!strncmp(argv[i], "/dev/", 5))
			node = argv[i];
		else if (argv[i][0] == '-')
			usage();
	}

	if (!opt_info && !opt_ver && !opt_opens && !opt_gbm && !opt_default)
		opt_info = true;
	if ((opt_info || opt_ver || opt_opens || opt_gbm) && !node)
		node = "/dev/dri/card0";

	if (opt_ver && !opt_info && node) {
		/* version + caps only: safe on a wedged modeset lock */
		kms_fd = open(node, O_RDWR | O_CLOEXEC);
		if (kms_fd < 0) {
			printf("PROBE open: %s: %s\n", node, strerror(errno));
			return 1;
		}
		printf("PROBE node: %s\n", node);
		print_ver("node", kms_fd);
		print_caps(kms_fd);
		close(kms_fd);
		if (!opt_opens)
			return 0;
	}
	if (opt_info && node) {
		kms_fd = open(node, O_RDWR | O_CLOEXEC);
		if (kms_fd < 0) {
			printf("PROBE open: %s: %s\n", node, strerror(errno));
			return 1;
		}
		printf("PROBE node: %s\n", node);
		print_ver("node", kms_fd);
		print_caps(kms_fd);
		print_modeset(kms_fd);
		print_devices();
		if (!opt_opens && !opt_gbm && !opt_ver) {
			close(kms_fd);
			return 0;
		}
	}
	if (opt_opens)
		do_opens();

	if (!opt_gbm && !opt_default)
		return 0;

	struct gbm_device *gbm = NULL;
	if (opt_gbm) {
		int fd = kms_fd >= 0 ? kms_fd : open(node, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			printf("PROBE open: %s: %s\n", node, strerror(errno));
			return 1;
		}
		if (kms_fd < 0)
			kms_fd = fd;
		gbm = gbm_create_device(fd);
		if (!gbm) {
			printf("PROBE gbm: gbm_create_device(%s) FAILED (%s)\n",
			       node, strerror(errno));
			return 1;
		}
		printf("PROBE gbm: backend=%s\n", gbm_device_get_backend_name(gbm));
	}
	int ret = run_egl(gbm, kms_fd);
	if (gbm)
		gbm_device_destroy(gbm);
	return ret;
}
