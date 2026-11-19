/*
 * render-node-probe v2 - classify a render node's GPU path before any Mesa work.
 *
 * Extends code/userspace/gpu/mesa-render/render-node-probe.c (v1, H1/H2/neither
 * verdict, exit 0/2/1) with:
 *   - deterministic exit codes for the harness (0/1/2/3/4, see below),
 *   - machine-readable JSON output (--json) with the full classification,
 *   - node-availability signals (exists / chardev / open_errno) so the
 *     harness can tell "no such node" from "neither path",
 *   - T720 product check (GPU_PROD_ID == 0x0720) for the device gate.
 *
 * Probes performed on the open fd (in order):
 *   1. DRM_IOCTL_VERSION driver name  (H1 signal: "panfrost").
 *   2. DRM_IOCTL_PANFROST_GET_PARAM for GPU_PROD_ID, REVISION,
 *      SHADER/TILER/L2/AS/JS_PRESENT, MMU_FEATURES, SELECTED_COHERENCY
 *      (H1 confirm; only when the driver is panfrost).
 *   3. kbase UKP_FUNC_ID_CHECK_VERSION 10.6 handshake (H2 signal; on a real
 *      DRM fd the kernel answers ENOTTY, harmless; version query only, no
 *      SET_FLAGS, no state change, safe to run on a live device as root).
 *
 * Deterministic exit codes (harness contract):
 *   0  H1  - panfrost UAPI answers (use stock panfrost_dri.so).
 *            Includes the H1+H2 hybrid: H1 wins, exit stays 0.
 *   3  H2  - kbase UK shim only (kbase_kmod backend + adapt patch).
 *   4  NEITHER - node opens but speaks neither path.
 *   2  NO_DEVICE - node cannot be opened (missing, permission, ...).
 *   1  ERROR - usage error (bad flag, too many args). ioctl-level failures
 *            are classification signals, never exit 1.
 *
 * Self-contained C99: only <drm/drm.h> + <drm/panfrost_drm.h> + libc, no
 * libdrm needed, so it cross-builds with plain aarch64-linux-gnu-gcc and
 * runs statically linked on the musl device userland.
 *
 * SPDX-License-Identifier: MIT
 */
/* Expose O_CLOEXEC under strict -std=c99; must precede any header. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/panfrost_drm.h>

/* kbase UK 10.6 version-check wire ABI (kbase_uapi.h shape; the ioctl cmd is
 * only used for _IOC_SIZE(cmd), type/number ignored by kbase_ioctl). */
#define UKP_FUNC_ID_CHECK_VERSION 0
#define KBASE_UK_IOCTL(size) _IOC(_IOC_WRITE, 0, 0, (size))

/* T720 product id the j7elte gate requires. */
#define T720_PROD_ID 0x0720u

enum {
	EXIT_H1 = 0,
	EXIT_ERROR = 1,
	EXIT_NO_DEVICE = 2,
	EXIT_H2 = 3,
	EXIT_NEITHER = 4
};

struct uk_version_check {
	union { uint32_t id; uint32_t ret; uint64_t sizer; } header;
	uint16_t major;
	uint16_t minor;
	uint8_t padding[4];
};

struct param_def {
	uint32_t id;
	const char *name;
};

/* Same GET_PARAM set v1 reports (H1 confirm). */
static const struct param_def param_defs[] = {
	{ DRM_PANFROST_PARAM_GPU_PROD_ID, "GPU_PROD_ID" },
	{ DRM_PANFROST_PARAM_GPU_REVISION, "GPU_REVISION" },
	{ DRM_PANFROST_PARAM_SHADER_PRESENT, "SHADER_PRESENT" },
	{ DRM_PANFROST_PARAM_TILER_PRESENT, "TILER_PRESENT" },
	{ DRM_PANFROST_PARAM_L2_PRESENT, "L2_PRESENT" },
	{ DRM_PANFROST_PARAM_AS_PRESENT, "AS_PRESENT" },
	{ DRM_PANFROST_PARAM_JS_PRESENT, "JS_PRESENT" },
	{ DRM_PANFROST_PARAM_MMU_FEATURES, "MMU_FEATURES" },
	{ DRM_PANFROST_PARAM_SELECTED_COHERENCY, "SELECTED_COHERENCY" },
};
#define N_PARAMS (sizeof(param_defs) / sizeof(param_defs[0]))

struct param_answer {
	uint64_t value;
	int ok;
};

static int panfrost_param(int fd, uint32_t param, uint64_t *val)
{
	struct drm_panfrost_get_param p;

	memset(&p, 0, sizeof(p));
	p.param = param;
	if (ioctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &p))
		return -errno;
	*val = p.value;
	return 0;
}

/* Short stable errno token for JSON (strerror stays human-only). */
static const char *errname(int e)
{
	switch (e) {
	case 0: return "OK";
	case EACCES: return "EACCES";
	case EAGAIN: return "EAGAIN";
	case EBADF: return "EBADF";
	case EBUSY: return "EBUSY";
	case EFAULT: return "EFAULT";
	case EINVAL: return "EINVAL";
	case EIO: return "EIO";
	case ELOOP: return "ELOOP";
	case ENAMETOOLONG: return "ENAMETOOLONG";
	case ENODEV: return "ENODEV";
	case ENOENT: return "ENOENT";
	case ENOMEM: return "ENOMEM";
	case ENOSYS: return "ENOSYS";
	case ENOTTY: return "ENOTTY";
	case ENXIO: return "ENXIO";
	case EPERM: return "EPERM";
	case EROFS: return "EROFS";
	default: return "E_UNKNOWN";
	}
}

static void json_escape(FILE *f, const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		switch (c) {
		case '"': fputs("\\\"", f); break;
		case '\\': fputs("\\\\", f); break;
		case '\n': fputs("\\n", f); break;
		case '\r': fputs("\\r", f); break;
		case '\t': fputs("\\t", f); break;
		default:
			if (c < 0x20)
				fprintf(f, "\\u%04x", c);
			else
				fputc(c, f);
		}
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--json] [node]\n"
		"  classify a render node: H1 (panfrost UAPI) vs H2 (kbase UK shim).\n"
		"  default node: /dev/dri/renderD128\n"
		"  exits: 0=H1 (incl. H1+H2), 3=H2, 4=NEITHER, 2=NO_DEVICE, 1=ERROR\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *path = "/dev/dri/renderD128";
	int want_json = 0;
	int i, npos = 0;
	struct stat st;
	int exists = 0, is_char = 0;
	int fd = -1, open_errno = 0;
	struct drm_version ver;
	char name[64] = { 0 }, date[64] = { 0 }, desc[256] = { 0 };
	int drm_ok = 0, h1 = 0, h2 = 0;
	int t720 = -1; /* -1 unknown, 0 no, 1 yes */
	struct param_answer ans[N_PARAMS];
	struct uk_version_check uk;
	int k_answered = 0, k_too_old = 0, k_errno = 0;
	uint32_t k_ret = 0;
	unsigned k_major = 0, k_minor = 0;
	const char *verdict;
	int code;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) {
			want_json = 1;
		} else if (!strcmp(argv[i], "--help") ||
			   !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "%s: unknown option %s\n",
				argv[0], argv[i]);
			usage(argv[0]);
			return EXIT_ERROR;
		} else if (npos++ == 0) {
			path = argv[i];
		} else {
			fprintf(stderr, "%s: only one node argument allowed\n",
				argv[0]);
			usage(argv[0]);
			return EXIT_ERROR;
		}
	}

	if (stat(path, &st) == 0) {
		exists = 1;
		is_char = S_ISCHR(st.st_mode) ? 1 : 0;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		open_errno = errno;

	/* DRM identity: harmless on any node. Any failure means "not a DRM
	 * driver" (ENOTTY on kbase/misc/regular files); still classification. */
	if (fd >= 0) {
		memset(&ver, 0, sizeof(ver));
		ver.name = name;
		ver.name_len = sizeof(name) - 1;
		ver.date = date;
		ver.date_len = sizeof(date) - 1;
		ver.desc = desc;
		ver.desc_len = sizeof(desc) - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &ver) == 0) {
			drm_ok = 1;
			h1 = !strcmp(name, "panfrost");
		}
	}

	if (h1) {
		for (i = 0; i < (int)N_PARAMS; i++) {
			uint64_t v = 0;

			if (panfrost_param(fd, param_defs[i].id, &v) == 0) {
				ans[i].value = v;
				ans[i].ok = 1;
			} else {
				ans[i].value = 0;
				ans[i].ok = 0;
			}
		}
		/* T720 gate: first param is GPU_PROD_ID. */
		if (ans[0].ok)
			t720 = ((uint32_t)ans[0].value == T720_PROD_ID) ? 1 : 0;
	} else {
		for (i = 0; i < (int)N_PARAMS; i++) {
			ans[i].value = 0;
			ans[i].ok = 0;
		}
	}

	/* kbase UK handshake: version query only, no state change. A real DRM
	 * driver rejects the unknown ioctl with ENOTTY; a kbase shim (or
	 * /dev/mali0 itself) answers UK 10.x. Do this after the DRM probes so
	 * a shim that also serves DRM ioctls is still fully characterised. */
	if (fd >= 0) {
		memset(&uk, 0, sizeof(uk));
		uk.header.id = UKP_FUNC_ID_CHECK_VERSION;
		uk.major = 10;
		uk.minor = 6;
		if (ioctl(fd, KBASE_UK_IOCTL(sizeof(uk)), &uk) == 0 &&
		    uk.header.ret == 0) {
			k_answered = 1;
			k_major = uk.major;
			k_minor = uk.minor;
			if ((uk.major == 10 && uk.minor >= 3) || uk.major > 10)
				h2 = 1;
			else
				k_too_old = 1;
		} else {
			if (uk.header.ret != 0)
				k_ret = uk.header.ret;
			k_errno = (errno != 0) ? errno : 0;
		}
	}

	if (fd < 0) {
		verdict = "NO_DEVICE";
		code = EXIT_NO_DEVICE;
	} else if (h1 && h2) {
		verdict = "H1+H2";
		code = EXIT_H1;
	} else if (h1) {
		verdict = "H1";
		code = EXIT_H1;
	} else if (h2) {
		verdict = "H2";
		code = EXIT_H2;
	} else {
		verdict = "NEITHER";
		code = EXIT_NEITHER;
	}

	if (want_json) {
		printf("{");
		printf("\"node\":\"");
		json_escape(stdout, path);
		printf("\",\"exists\":%s,\"chardev\":%s,",
		       exists ? "true" : "false", exists ?
		       (is_char ? "true" : "false") : "null");
		if (fd < 0)
			printf("\"open\":false,\"open_errno\":\"%s\",",
			       errname(open_errno));
		else
			printf("\"open\":true,\"open_errno\":null,");
		printf("\"drm\":%s,", drm_ok ? "true" : "false");
		if (drm_ok) {
			printf("\"driver\":\"");
			json_escape(stdout, name);
			printf("\",\"driver_desc\":\"");
			json_escape(stdout, desc);
			printf("\",");
		} else {
			printf("\"driver\":null,\"driver_desc\":null,");
		}
		printf("\"h1\":%s,\"h2\":%s,",
		       h1 ? "true" : "false", h2 ? "true" : "false");
		if (t720 < 0)
			printf("\"t720\":null,");
		else
			printf("\"t720\":%s,", t720 ? "true" : "false");
		printf("\"params\":{");
		if (h1) {
			for (i = 0; i < (int)N_PARAMS; i++) {
				if (i)
					printf(",");
				if (ans[i].ok)
					printf("\"%s\":%llu", param_defs[i].name,
					       (unsigned long long)ans[i].value);
				else
					printf("\"%s\":null", param_defs[i].name);
			}
		}
		printf("},");
		printf("\"kbase\":{\"answered\":%s,",
		       k_answered ? "true" : "false");
		if (fd < 0)
			printf("\"major\":null,\"minor\":null,"
			       "\"too_old\":false,\"errno\":null,\"ret\":0");
		else if (k_answered)
			printf("\"major\":%u,\"minor\":%u,\"too_old\":%s,"
			       "\"errno\":null,\"ret\":0",
			       k_major, k_minor,
			       k_too_old ? "true" : "false");
		else
			printf("\"major\":null,\"minor\":null,"
			       "\"too_old\":false,\"errno\":\"%s\",\"ret\":%u",
			       errname(k_errno), k_ret);
		printf("},");
		printf("\"verdict\":\"%s\",\"exit\":%d}\n", verdict, code);
		if (fd >= 0)
			close(fd);
		return code;
	}

	printf("render-node-probe v2: %s\n", path);
	printf("node: %s (%s%s)\n", path, exists ? "exists" : "missing",
	       exists ? (is_char ? ", chardev" : ", not chardev") : "");
	if (fd < 0) {
		printf("open: failed (%s)\n", strerror(open_errno));
	} else if (drm_ok) {
		printf("drm driver: %s (%s)\n", name, desc);
	} else {
		printf("not a DRM driver (DRM_IOCTL_VERSION: %s)\n",
		       strerror(errno));
	}

	if (h1) {
		int j;

		printf("GET_PARAM:\n");
		for (j = 0; j < (int)N_PARAMS; j++) {
			if (ans[j].ok)
				printf("  %-22s: 0x%llx (%llu)\n",
				       param_defs[j].name,
				       (unsigned long long)ans[j].value,
				       (unsigned long long)ans[j].value);
			else
				printf("  %-22s: ioctl failed\n",
				       param_defs[j].name);
		}
		if (t720 == 1)
			printf("T720 check: GPU_PROD_ID 0x720, device gate OK\n");
		else if (t720 == 0)
			printf("T720 check: GPU_PROD_ID 0x%x, NOT the j7elte T720\n",
			       (unsigned)(ans[0].value & 0xFFFFu));
		else
			printf("T720 check: GPU_PROD_ID unreadable\n");
		printf("H1 check: panfrost UAPI answers\n");
	} else if (fd >= 0) {
		printf("not a panfrost DRM driver; GET_PARAM skipped\n");
	}

	if (fd >= 0) {
		if (k_answered && !k_too_old)
			printf("kbase handshake: ok, kernel UK %u.%u\n",
			       k_major, k_minor);
		else if (k_answered)
			printf("kbase handshake: UK %u.%u too old (need >= 10.3)\n",
			       k_major, k_minor);
		else if (k_errno == ENOTTY || k_errno == EINVAL)
			printf("kbase handshake: not answered (%s)\n",
			       strerror(k_errno));
		else if (k_errno)
			printf("kbase handshake: failed (%s), ret=%u\n",
			       strerror(k_errno), k_ret);
		else
			printf("kbase handshake: rejected (ret=%u)\n", k_ret);
	}

	if (!strcmp(verdict, "H1+H2"))
		printf("verdict: H1+H2 (hybrid node; H1 path wins)\n");
	else if (!strcmp(verdict, "H1"))
		printf("verdict: H1 (panfrost UAPI: use stock panfrost_dri.so)\n");
	else if (!strcmp(verdict, "H2"))
		printf("verdict: H2 (kbase UK shim: kbase_kmod + adapt patch)\n");
	else if (!strcmp(verdict, "NO_DEVICE"))
		printf("verdict: NO_DEVICE (node unavailable)\n");
	else
		printf("verdict: NEITHER (no panfrost UAPI, no UK handshake)\n");
	printf("exit: %d\n", code);

	if (fd >= 0)
		close(fd);
	return code;
}
