/* drmshim.c - LD_PRELOAD shim that rewrites DRM driver names.
 *
 * Mesa pairs a KMS node with a render node purely by the *kernel* driver
 * name (drmGetVersion()->name):
 *
 *   Mesa 20  kmsro_drm_winsys.c: drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER)
 *   Mesa 26  pipe_loader_*:      strcmp(render_dev_name, "panfrost") on every
 *                                platform render node
 *
 * Our render node answers to "panfrost7580" and our KMS node to "exynos"
 * (the second one already matches).  This shim rewrites the name seen by
 * userspace so the pairing can be proven on a stock Mesa without touching
 * the kernel:
 *
 *   DRM_NAME_SHIM="panfrost7580=panfrost[,exynos7580=exynos]" \
 *   LD_PRELOAD=./drmshim.so kmscube -D /dev/dri/card0
 *
 * DRM_VER_SHIM="1.1" additionally rewrites drmVersion.version_major/minor,
 * which is the second gate Mesa 26's panfrost_kmod applies (>= 1.1).
 *
 * Only the name/name_len of the returned drmVersion are changed; the struct
 * stays malloc'd by libdrm so drmFreeVersion() keeps working.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <xf86drm.h>

static const char *shim_name(const char *name)
{
	static char spec[512];
	static int loaded;
	static struct { char from[64], to[64]; } map[8];
	static int nmap;

	if (!loaded) {
		const char *env = getenv("DRM_NAME_SHIM");
		loaded = 1;
		if (!env)
			return NULL;
		snprintf(spec, sizeof(spec), "%s", env);
		char *save = NULL;
		for (char *tok = strtok_r(spec, ",", &save); tok && nmap < 8;
		     tok = strtok_r(NULL, ",", &save)) {
			char *eq = strchr(tok, '=');
			if (!eq)
				continue;
			*eq = '\0';
			snprintf(map[nmap].from, sizeof(map[nmap].from), "%s", tok);
			snprintf(map[nmap].to, sizeof(map[nmap].to), "%s", eq + 1);
			nmap++;
		}
	}
	if (!name)
		return NULL;
	for (int i = 0; i < nmap; i++)
		if (!strcmp(name, map[i].from))
			return map[i].to;
	return NULL;
}

drmVersionPtr drmGetVersion(int fd)
{
	static drmVersionPtr (*real)(int);
	static int ver_major, ver_minor, ver_loaded;

	if (!real)
		real = (drmVersionPtr (*)(int))dlsym(RTLD_NEXT, "drmGetVersion");
	if (!ver_loaded) {
		const char *env = getenv("DRM_VER_SHIM");
		ver_loaded = 1;
		if (env)
			sscanf(env, "%d.%d", &ver_major, &ver_minor);
	}

	drmVersionPtr v = real(fd);
	if (!v)
		return v;

	if (getenv("DRM_VER_SHIM")) {
		fprintf(stderr, "drmshim: fd %d version %d.%d -> %d.%d\n", fd,
			v->version_major, v->version_minor, ver_major, ver_minor);
		v->version_major = ver_major;
		v->version_minor = ver_minor;
	}

	const char *to = shim_name(v->name);
	if (to) {
		char from[64];
		snprintf(from, sizeof(from), "%s", v->name);
		char *copy = strdup(to);
		if (copy) {
			free(v->name);
			v->name = copy;
			v->name_len = strlen(copy);
			fprintf(stderr, "drmshim: fd %d name '%s' -> '%s'\n",
				fd, from, to);
		}
	}
	return v;
}
