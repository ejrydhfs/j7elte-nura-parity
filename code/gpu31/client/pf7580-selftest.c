/* SPDX-License-Identifier: MIT */
/*
 * pf7580-selftest.c - bring-up test for the panfrost7580 userspace client.
 *
 * Modes:
 *   (default)     mock backend: full six-ioctl lifecycle + error paths.
 *   --print-abi   print wire sizes and ioctl encodings, no device needed.
 *   --device PATH run the safe real-node path (GET_PARAM/CREATE/MMAP/
 *                 GET_BO_OFFSET/WAIT + CPU map/write/read). SUBMIT is
 *                 mock-only: a test jc would fault real hardware.
 *                 PF7580_DEVICE also selects the node.
 *   --probe-only PATH run the SUBMIT-free node probe (GET_PARAM/CREATE_BO
 *                 4K/MMAP_BO/GET_BO_OFFSET only). Safe on live hardware:
 *                 never issues SUBMIT, WAIT_BO, or a CPU mapping.
 *
 * Exit codes: 0 success, 2 device absent, 1 failure. The mock suite runs
 * on any build host; the real-node path is skipped (exit 2) when the
 * node cannot be opened.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pf7580.h"

/* v5.2 UAPI truth: the driver's own mirror. Compile-time field check. */
#include "panfrost_drm.h"

_Static_assert(sizeof(struct pf7580_submit) ==
	       sizeof(struct drm_panfrost_submit), "submit mirror");
_Static_assert(sizeof(struct pf7580_wait_bo) ==
	       sizeof(struct drm_panfrost_wait_bo), "wait_bo mirror");
_Static_assert(sizeof(struct pf7580_create_bo) ==
	       sizeof(struct drm_panfrost_create_bo), "create_bo mirror");
_Static_assert(sizeof(struct pf7580_mmap_bo) ==
	       sizeof(struct drm_panfrost_mmap_bo), "mmap_bo mirror");
_Static_assert(sizeof(struct pf7580_get_param) ==
	       sizeof(struct drm_panfrost_get_param), "get_param mirror");
_Static_assert(sizeof(struct pf7580_get_bo_offset) ==
	       sizeof(struct drm_panfrost_get_bo_offset), "get_bo_offset mirror");
_Static_assert(offsetof(struct pf7580_submit, jc) ==
	       offsetof(struct drm_panfrost_submit, jc), "submit.jc");
_Static_assert(offsetof(struct pf7580_submit, requirements) ==
	       offsetof(struct drm_panfrost_submit, requirements),
	       "submit.requirements");

static int failures;

#define CHECK(cond, fmt, ...) do {					\
	if (!(cond)) {							\
		printf("FAIL %s:%d: " fmt "\n", __func__, __LINE__,	\
		       ##__VA_ARGS__);				\
		failures++;						\
	}								\
} while (0)

#define CHECK_RC(expr, want, what) do {					\
	int _rc = (expr);						\
	if (_rc != (want)) {						\
		printf("FAIL %s:%d: %s: got %d (%s), want %d (%s)\n",	\
		       __func__, __LINE__, (what), _rc,			\
		       pf7580_strerror(_rc), (want),			\
		       pf7580_strerror((want)));			\
		failures++;						\
	}								\
} while (0)

static void print_abi(void)
{
	printf("submit            %zu\n", sizeof(struct pf7580_submit));
	printf("wait_bo           %zu\n", sizeof(struct pf7580_wait_bo));
	printf("create_bo         %zu\n", sizeof(struct pf7580_create_bo));
	printf("mmap_bo           %zu\n", sizeof(struct pf7580_mmap_bo));
	printf("get_param         %zu\n", sizeof(struct pf7580_get_param));
	printf("get_bo_offset     %zu\n", sizeof(struct pf7580_get_bo_offset));
	printf("T720 prod id       0x%04x\n", PF7580_T720_PROD_ID);
}

/* SUBMIT-free node probe: GET_PARAM + CREATE_BO(4K) + MMAP_BO +
 * GET_BO_OFFSET only. No SUBMIT (a test jc would fault HW), no WAIT_BO,
 * no CPU mapping — the ioctl sequence mutates nothing beyond one
 * throwaway 4 KiB BO on the probed fd, which dies with close(). */
static int run_probe_only(const char *path)
{
	struct pf7580_dev *dev;
	char err[256] = "";
	uint64_t prod = 0, map_off = 0, gpu_off = 0, gpu_off2 = 0;
	uint32_t h = 0;
	int rc, bad = 0;

	printf("-- pf7580 probe-only on %s --\n", path);
	rc = pf7580_open(path, &dev, err, sizeof(err));
	if (rc) {
		printf("probe-only %s unavailable: %s\n", path, err);
		return 2;
	}
	printf("open: OK (fd=%d mock=%d)\n", pf7580_fd(dev),
	       pf7580_is_mock(dev));
	rc = pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &prod);
	if (rc) {
		printf("GET_PARAM: FAIL %d (%s)\n", rc,
		       pf7580_strerror(rc));
		bad = 1;
	} else {
		printf("GET_PARAM: OK GPU_PROD_ID 0x%" PRIx64 "%s\n", prod,
		       prod == PF7580_T720_PROD_ID ? " (T720)" : " (mismatch)");
		if (prod != PF7580_T720_PROD_ID)
			bad = 1;
	}
	rc = pf7580_create_bo(dev, 4096, &h, &gpu_off);
	if (rc) {
		printf("CREATE_BO(4K): FAIL %d (%s)\n", rc,
		       pf7580_strerror(rc));
		bad = 1;
	} else {
		printf("CREATE_BO(4K): OK handle=%u gpu_offset=0x%" PRIx64
		       "\n", h, gpu_off);
		if (!h || !gpu_off) {
			printf("CREATE_BO(4K): FAIL null handle/offset\n");
			bad = 1;
		}
	}
	if (!bad) {
		rc = pf7580_mmap_bo(dev, h, &map_off);
		if (rc) {
			printf("MMAP_BO: FAIL %d (%s)\n", rc,
			       pf7580_strerror(rc));
			bad = 1;
		} else {
			printf("MMAP_BO: OK map_offset=0x%" PRIx64 "\n",
			       map_off);
		}
	}
	if (!bad) {
		rc = pf7580_get_bo_offset(dev, h, &gpu_off2);
		if (rc) {
			printf("GET_BO_OFFSET: FAIL %d (%s)\n", rc,
			       pf7580_strerror(rc));
			bad = 1;
		} else {
			printf("GET_BO_OFFSET: OK offset=0x%" PRIx64 "%s\n",
			       gpu_off2,
			       gpu_off2 == gpu_off ? " (matches CREATE_BO)"
						   : " (MISMATCH vs CREATE_BO)");
			if (gpu_off2 != gpu_off)
				bad = 1;
		}
	}
	printf("SUBMIT: skipped (probe-only)\n");
	pf7580_close(dev);
	printf(bad ? "PROBE-ONLY: FAILED\n" : "PROBE-ONLY: PASS\n");
	return bad ? 1 : 0;
}

/* T2: GET_PARAM. */
static void t_get_param(struct pf7580_dev *dev)
{
	uint64_t v = 0;

	CHECK_RC(pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &v), 0,
		 "get_param prod_id");
	CHECK(v == PF7580_T720_PROD_ID, "prod id 0x%" PRIx64, v);
	CHECK_RC(pf7580_get_param(dev, 99, &v), -EINVAL, "get_param bad id");
	CHECK_RC(pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, NULL),
		 -EINVAL, "get_param null out");
}

/* T3: CREATE_BO. */
static void t_create_bo(struct pf7580_dev *dev, uint32_t *keep,
			uint64_t *keep_off)
{
	uint32_t h = 0;
	uint64_t off = 0;

	CHECK_RC(pf7580_create_bo(dev, 4096, &h, &off), 0, "create 4k");
	CHECK(h != 0, "handle nonzero");
	CHECK(off != 0, "gpu offset nonzero");
	CHECK_RC(pf7580_create_bo(dev, 0, &h, &off), -EINVAL,
		 "create size 0");
	CHECK_RC(pf7580_create_bo(dev, 4096, NULL, &off), -EINVAL,
		 "create null handle");
	*keep = h;
	*keep_off = off;
}

/* T4: MMAP_BO / GET_BO_OFFSET round trip + unknown-handle paths. */
static void t_offsets(struct pf7580_dev *dev, uint32_t h, uint64_t gpu_off,
		      uint64_t *map_off)
{
	uint64_t m = 0, g = 0;

	CHECK_RC(pf7580_mmap_bo(dev, h, &m), 0, "mmap_bo");
	CHECK_RC(pf7580_get_bo_offset(dev, h, &g), 0, "get_bo_offset");
	CHECK(g == gpu_off, "bo offset matches create: 0x%" PRIx64
	      " vs 0x%" PRIx64, g, gpu_off);
	CHECK_RC(pf7580_mmap_bo(dev, 0xdeadbeefu, &m), -ENOENT,
		 "mmap_bo unknown handle");
	CHECK_RC(pf7580_get_bo_offset(dev, 0xdeadbeefu, &g), -ENOENT,
		 "get_bo_offset unknown handle");
	*map_off = m;
}

/* T5: CPU map write/read. */
static void t_cpu_map(struct pf7580_dev *dev, uint32_t h, uint64_t map_off)
{
	unsigned char *p;
	unsigned i;

	p = pf7580_cpu_map(dev, h, map_off, 4096);
	CHECK(p != NULL, "cpu map");
	if (!p)
		return;
	for (i = 0; i < 256; i++)
		p[i] = (unsigned char)(i ^ 0xa5);
	CHECK_RC(pf7580_cpu_unmap(dev, p, 4096), 0, "cpu unmap");
	p = pf7580_cpu_map(dev, h, map_off, 4096);
	CHECK(p != NULL, "cpu remap");
	if (!p)
		return;
	for (i = 0; i < 256; i++)
		CHECK(p[i] == (unsigned char)(i ^ 0xa5), "map byte %u", i);
	CHECK_RC(pf7580_cpu_unmap(dev, p, 4096), 0, "cpu unmap 2");
	CHECK(pf7580_cpu_map(dev, h, map_off + 1, 4096) == NULL,
	      "cpu map bad offset rejected");
}

/* T6: WAIT_BO. */
static void t_wait(struct pf7580_dev *dev, uint32_t h)
{
	CHECK_RC(pf7580_wait_bo(dev, h, 0), 0, "wait non-blocking");
	CHECK_RC(pf7580_wait_bo(dev, h, 1000000000ll), 0, "wait 1s");
	CHECK_RC(pf7580_wait_bo(dev, 0xdeadbeefu, 0), -ENOENT,
		 "wait unknown handle");
}

/* T7: SUBMIT validation. */
static void t_submit(struct pf7580_dev *dev, uint32_t h, uint64_t jc)
{
	struct pf7580_submit_args ok;
	struct pf7580_submit_args bad;
	uint32_t many[PF7580_SUBMIT_MAX_BOS + 1];
	uint32_t syncs[PF7580_SUBMIT_MAX_SYNCS + 1];
	unsigned i;

	for (i = 0; i <= PF7580_SUBMIT_MAX_BOS; i++)
		many[i] = h;
	for (i = 0; i <= PF7580_SUBMIT_MAX_SYNCS; i++)
		syncs[i] = 0;

	memset(&ok, 0, sizeof(ok));
	ok.jc = jc;
	ok.bo_handles = &h;
	ok.bo_handle_count = 1;
	CHECK_RC(pf7580_submit(dev, &ok), 0, "submit valid");

	bad = ok;
	bad.jc = 0;
	CHECK_RC(pf7580_submit(dev, &bad), -EINVAL, "submit jc 0");
	bad = ok;
	bad.jc = jc | 1u;
	CHECK_RC(pf7580_submit(dev, &bad), -EINVAL, "submit jc unaligned");
	bad = ok;
	bad.requirements = 0x8u;
	CHECK_RC(pf7580_submit(dev, &bad), -EINVAL, "submit bad req");
	bad = ok;
	bad.requirements = PF7580_JD_REQ_FS;
	CHECK_RC(pf7580_submit(dev, &bad), 0, "submit FS ok");
	bad = ok;
	bad.bo_handles = many;
	bad.bo_handle_count = PF7580_SUBMIT_MAX_BOS + 1;
	CHECK_RC(pf7580_submit(dev, &bad), -EINVAL, "submit too many bos");
	bad = ok;
	bad.in_syncs = syncs;
	bad.in_sync_count = PF7580_SUBMIT_MAX_SYNCS + 1;
	CHECK_RC(pf7580_submit(dev, &bad), -EINVAL, "submit too many syncs");
	bad = ok;
	bad.bo_handle_count = 1;
	bad.bo_handles = NULL;
	CHECK_RC(pf7580_submit(dev, &bad), -EINVAL, "submit null bos");
	CHECK_RC(pf7580_submit(dev, NULL), -EINVAL, "submit null args");
}

/* T9: one-shot fault injection on all six ioctls. */
static void t_inject(struct pf7580_dev *dev, uint32_t h)
{
	uint64_t v = 0;
	uint32_t nh = 0;
	uint64_t off = 0;
	struct pf7580_submit_args ok;

	memset(&ok, 0, sizeof(ok));
	ok.jc = 0x10000000ull;
	ok.bo_handles = &h;
	ok.bo_handle_count = 1;

	pf7580_mock_fail_next(dev, EIO);
	CHECK_RC(pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &v), -EIO,
		 "inject get_param");
	pf7580_mock_fail_next(dev, EIO);
	CHECK_RC(pf7580_create_bo(dev, 4096, &nh, &off), -EIO,
		 "inject create_bo");
	pf7580_mock_fail_next(dev, EIO);
	CHECK_RC(pf7580_mmap_bo(dev, h, &off), -EIO, "inject mmap_bo");
	pf7580_mock_fail_next(dev, EIO);
	CHECK_RC(pf7580_get_bo_offset(dev, h, &off), -EIO,
		 "inject get_bo_offset");
	pf7580_mock_fail_next(dev, EIO);
	CHECK_RC(pf7580_wait_bo(dev, h, 0), -EIO, "inject wait_bo");
	pf7580_mock_fail_next(dev, EIO);
	CHECK_RC(pf7580_submit(dev, &ok), -EIO, "inject submit");
	/* Injection is one-shot: the path works again. */
	CHECK_RC(pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &v), 0,
		 "inject consumed");
	CHECK(v == PF7580_T720_PROD_ID, "prod id after inject");
}

static int run_mock_suite(void)
{
	struct pf7580_dev *dev;
	uint32_t h = 0;
	uint64_t gpu_off = 0, map_off = 0;
	struct pf7580_submit_args life;
	int rc;

	printf("-- pf7580 mock suite (v5.2 six-ioctl lifecycle) --\n");
	rc = pf7580_open_mock(&dev);
	CHECK_RC(rc, 0, "open_mock");
	if (rc)
		return 1;
	CHECK(pf7580_is_mock(dev), "is_mock");
	CHECK(pf7580_fd(dev) == -1, "mock fd is -1");

	t_get_param(dev);
	t_create_bo(dev, &h, &gpu_off);
	t_offsets(dev, h, gpu_off, &map_off);
	t_cpu_map(dev, h, map_off);
	t_wait(dev, h);
	t_submit(dev, h, gpu_off);
	t_inject(dev, h);

	/* T8: full lifecycle in call order. */
	memset(&life, 0, sizeof(life));
	life.jc = gpu_off;
	life.bo_handles = &h;
	life.bo_handle_count = 1;
	CHECK_RC(pf7580_submit(dev, &life), 0, "lifecycle submit");
	CHECK_RC(pf7580_wait_bo(dev, h, 1000000000ll), 0, "lifecycle wait");

	pf7580_close(dev);
	printf(failures ? "MOCK SUITE: FAILED (%d)\n" : "MOCK SUITE: PASS\n",
	       failures);
	return failures ? 1 : 0;
}

/* Real-node path: everything except SUBMIT (a test jc would fault HW). */
static int run_device(const char *path)
{
	struct pf7580_dev *dev;
	char err[256] = "";
	uint64_t prod = 0, map_off = 0, gpu_off = 0, gpu_off2 = 0;
	uint32_t h = 0;
	unsigned char *p;
	int rc;

	printf("-- pf7580 device path on %s --\n", path);
	rc = pf7580_open(path, &dev, err, sizeof(err));
	if (rc) {
		printf("device %s unavailable: %s\n", path, err);
		printf("ABI checks passed, device path not executed\n");
		return 2;
	}
	rc = pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &prod);
	if (rc) {
		printf("FAIL get_param: %s\n", pf7580_strerror(rc));
		failures++;
	} else {
		printf("GPU_PROD_ID 0x%" PRIx64 "%s\n", prod,
		       prod == PF7580_T720_PROD_ID ? " (T720)" : " (mismatch)");
		if (prod != PF7580_T720_PROD_ID)
			failures++;
	}
	if (!failures) {
		rc = pf7580_create_bo(dev, 16384, &h, &gpu_off);
		if (rc) {
			printf("FAIL create_bo: %s\n",
			       pf7580_strerror(rc));
			failures++;
		} else {
			printf("BO handle=%u gpu_offset=0x%" PRIx64 "\n",
			       h, gpu_off);
		}
	}
	if (!failures) {
		rc = pf7580_get_bo_offset(dev, h, &gpu_off2);
		if (rc || gpu_off2 != gpu_off) {
			printf("FAIL get_bo_offset: rc=%d off=0x%" PRIx64
			       "\n", rc, gpu_off2);
			failures++;
		}
		rc = pf7580_mmap_bo(dev, h, &map_off);
		if (rc) {
			printf("FAIL mmap_bo: %s\n", pf7580_strerror(rc));
			failures++;
		}
	}
	if (!failures) {
		p = pf7580_cpu_map(dev, h, map_off, 16384);
		if (!p) {
			printf("FAIL cpu map: mmap(fd=%d, off=0x%" PRIx64
			       ", len=%zu) errno=%d (%s)\n", pf7580_fd(dev),
			       pf7580_cpu_map_off, pf7580_cpu_map_len,
			       pf7580_cpu_map_errno,
			       strerror(pf7580_cpu_map_errno));
			failures++;
		} else {
			memset(p, 0x5a, 256);
			if (p[255] != 0x5a) {
				printf("FAIL cpu mapping not writable\n");
				failures++;
			} else {
				printf("CPU map R/W ok\n");
			}
			rc = pf7580_cpu_unmap(dev, p, 16384);
			if (rc) {
				printf("FAIL cpu unmap: %s\n",
				       pf7580_strerror(rc));
				failures++;
			}
		}
	}
	if (!failures) {
		rc = pf7580_wait_bo(dev, h, 0);
		if (rc) {
			printf("FAIL wait_bo: %s\n", pf7580_strerror(rc));
			failures++;
		} else {
			printf("wait_bo(0) ok\n");
		}
	}
	pf7580_close(dev);
	printf(failures ? "DEVICE PATH: FAILED (%d)\n" : "DEVICE PATH: PASS\n",
	       failures);
	return failures ? 1 : 0;
}

/* Real-submit test: exactly one NULL job after a BO is mapped.
 * Sequence: GET_PARAM + CREATE_BO(4K) + MMAP_BO + GET_BO_OFFSET (via
 * CREATE_BO offset) + CPU map + write a 32-byte NULL job descriptor
 * (type 1, 32-bit next=NULL, jobabi section 1) at BO offset 0 +
 * SUBMIT(jc=gpu_offset, 1 BO, req=0 -> JS1) + WAIT_BO(handle,
 * absolute CLOCK_MONOTONIC now+8s). One submit only, never looped:
 * on any fault the driver state is left for dmesg, not retried.
 * Exit: 0 submitted+waited, 1 failed, 2 node unavailable. */
static int run_submit_test(const char *path)
{
	struct pf7580_dev *dev;
	char err[256] = "";
	uint64_t prod = 0, map_off = 0, gpu_off = 0;
	uint32_t h = 0;
	unsigned char *p;
	struct pf7580_submit_args sa;
	struct timespec now, t0, t1;
	int64_t deadline_ns;
	long elapsed_ms;
	int rc;

	printf("-- pf7580 submit-test on %s --\n", path);
	rc = pf7580_open(path, &dev, err, sizeof(err));
	if (rc) {
		printf("submit-test %s unavailable: %s\n", path, err);
		return 2;
	}
	printf("open: OK (fd=%d mock=%d)\n", pf7580_fd(dev),
	       pf7580_is_mock(dev));
	rc = pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &prod);
	if (rc || prod != PF7580_T720_PROD_ID) {
		printf("GET_PARAM: FAIL rc=%d (%s) prod=0x%" PRIx64 "\n",
		       rc, pf7580_strerror(rc), prod);
		pf7580_close(dev);
		return 1;
	}
	printf("GET_PARAM: OK GPU_PROD_ID 0x%" PRIx64 " (T720)\n", prod);
	rc = pf7580_create_bo(dev, 4096, &h, &gpu_off);
	if (rc || !h || !gpu_off || (gpu_off & 7u)) {
		printf("CREATE_BO(4K): FAIL rc=%d (%s) h=%u off=0x%" PRIx64
		       "\n", rc, pf7580_strerror(rc), h, gpu_off);
		pf7580_close(dev);
		return 1;
	}
	printf("CREATE_BO(4K): OK handle=%u gpu_offset=0x%" PRIx64 "\n",
	       h, gpu_off);
	rc = pf7580_mmap_bo(dev, h, &map_off);
	if (rc) {
		printf("MMAP_BO: FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_close(dev);
		return 1;
	}
	printf("MMAP_BO: OK map_offset=0x%" PRIx64 "\n", map_off);
	p = pf7580_cpu_map(dev, h, map_off, 4096);
	if (!p) {
		printf("FAIL cpu map: mmap(fd=%d, off=0x%" PRIx64
		       ", len=%zu) errno=%d (%s)\n", pf7580_fd(dev),
		       pf7580_cpu_map_off, pf7580_cpu_map_len,
		       pf7580_cpu_map_errno,
		       strerror(pf7580_cpu_map_errno));
		pf7580_close(dev);
		return 1;
	}
	/* 32-byte NULL job (jobabi section 1): type 1, 32-bit next=NULL. */
	printf("DESC: writing NULL job at gpu_offset=0x%" PRIx64 "...\n",
	       gpu_off);
	fflush(stdout);
	memset(p, 0, 4096);
	p[16] = (unsigned char)((1u << 1) | 0u);
	__builtin___clear_cache((char *)p, (char *)p + 4096);
	if (p[16] != 0x02 || p[0] != 0) {
		printf("DESC: FAIL read-back mismatch\n");
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	printf("DESC: OK NULL job (type=1, 32 B) at gpu_offset=0x%" PRIx64
	       "\n", gpu_off);
	memset(&sa, 0, sizeof(sa));
	sa.jc = gpu_off;
	sa.bo_handles = &h;
	sa.bo_handle_count = 1;
	sa.requirements = 0;
	printf("SUBMIT: issuing one NULL job (jc=0x%" PRIx64
	       " req=0 slot=JS1)...\n", gpu_off);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_submit(dev, &sa);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("SUBMIT: rc=%d (%s) elapsed=%ld ms\n", rc,
	       pf7580_strerror(rc), elapsed_ms);
	if (rc) {
		printf("SUBMIT-TEST: FAILED (submit rejected; see dmesg)\n");
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline_ns = (int64_t)now.tv_sec * 1000000000LL +
		now.tv_nsec + 8000000000LL;
	printf("WAIT_BO: waiting (+8s abs deadline %lld ns)...\n",
	       (long long)deadline_ns);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_wait_bo(dev, h, deadline_ns);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("WAIT_BO(+8s abs): rc=%d (%s) elapsed=%ld ms\n", rc,
	       pf7580_strerror(rc), elapsed_ms);
	if (rc) {
		printf("SUBMIT-TEST: FAILED (wait failed; see dmesg)\n");
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	rc = pf7580_cpu_unmap(dev, p, 4096);
	if (rc)
		printf("WARN cpu unmap: %s\n", pf7580_strerror(rc));
	rc = pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &prod);
	printf("POST GET_PARAM: rc=%d (%s) prod=0x%" PRIx64 "%s\n", rc,
	       pf7580_strerror(rc), prod,
	       (!rc && prod == PF7580_T720_PROD_ID) ? " (node alive)" :
						      " (MISMATCH)");
	pf7580_close(dev);
	if (rc || prod != PF7580_T720_PROD_ID) {
		printf("SUBMIT-TEST: FAILED (node unhealthy after submit)\n");
		return 1;
	}
	printf("SUBMIT-TEST: PASS\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *devpath = getenv("PF7580_DEVICE");
	const char *probeonly = NULL;
	const char *submittest = NULL;
	int i;

	/* Unbuffered: on HW a hang must leave partial output behind. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	print_abi();
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--print-abi"))
			return 0;
		if (!strcmp(argv[i], "--probe-only") && i + 1 < argc) {
			probeonly = argv[++i];
		} else if (!strncmp(argv[i], "--probe-only=", 13)) {
			probeonly = argv[i] + 13;
		} else if (!strcmp(argv[i], "--submit-test") && i + 1 < argc) {
			submittest = argv[++i];
		} else if (!strncmp(argv[i], "--submit-test=", 14)) {
			submittest = argv[i] + 14;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			devpath = argv[++i];
		} else if (!strncmp(argv[i], "--device=", 9)) {
			devpath = argv[i] + 9;
		} else {
			fprintf(stderr, "usage: %s [--print-abi] [--device PATH] [--probe-only PATH] [--submit-test PATH]\n",
				argv[0]);
			return 1;
		}
	}
	if (probeonly)
		return run_probe_only(probeonly);
	if (submittest)
		return run_submit_test(submittest);
	if (devpath)
		return run_device(devpath);
	return run_mock_suite();
}
