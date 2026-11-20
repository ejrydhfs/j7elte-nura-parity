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
 *   --submit-test PATH issue exactly one NULL job (32 B, type 1) at the
 *                 BO's own gpu_offset + fence-wait it. Proves dispatch +
 *                 retire; does NOT prove GPU writes are visible (a NULL
 *                 job writes nothing).
 *   --pattern-test PATH ("coherency proof", gpu31-coherency): write one
 *                 56-byte WRITE_VALUE job (Mesa midgard.xml: type 2,
 *                 Immediate-32) whose payload targets a second 4 KiB
 *                 result BO at a known GPU VA with a known u32 pattern
 *                 (0xC0C3E8E1). SUBMIT it (req=0 -> JS1), fence-wait the
 *                 out_sync, re-mmap the result BO, and compare the word.
 *                 PASS (pattern word == expected) proves the full
 *                 COHERENCY_NONE chain: CPU write -> sync_for_device ->
 *                 GPU DMA write -> IRQ retire -> sync_for_cpu -> CPU read.
 *                 FAIL prints expected-vs-actual + JS_STATUS hint.
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
#include <unistd.h>

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
	printf("syncobj_handle    %zu\n", sizeof(struct pf7580_syncobj_handle));
	printf("syncobj_wait      %zu\n", sizeof(struct pf7580_syncobj_wait));
	printf("T720 prod id       0x%04x\n", PF7580_T720_PROD_ID);
}

/* Mesa 20.3's exact GET_PARAM sequence (pan_props.c panfrost_query_raw):
 * the struct is zeroed, param set, IOWR(0x44/16 B) issued; non-PROD_ID
 * probes must fail -EINVAL (never success-with-garbage) so the caller
 * takes its compiled-in default. Exercises the first EINVAL row. */
static void t_get_param_abi(struct pf7580_dev *dev)
{
	uint64_t v = 0xdeadbeefull;

	CHECK_RC(pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &v), 0,
		 "get_param prod_id u64");
	CHECK(v == PF7580_T720_PROD_ID, "prod id 0x%" PRIx64, v);
	/* Mesa probes SHADER_PRESENT with required=false; the driver must
	 * fail EINVAL (Mesa then uses 0xffff -> MP2 core count). */
	CHECK_RC(pf7580_get_param(dev, 2, &v), -EINVAL,
		 "get_param SHADER_PRESENT EINVAL");
	CHECK_RC(pf7580_get_param(dev, 1, &v), -EINVAL,
		 "get_param REVISION EINVAL");
}

/* Mesa 20.3's exact fence-snapshot sequence (pan_screen.c
 * panfrost_fence_create): CREATE signaled ctx syncobj, export it with
 * flags=1 (SYNC_FILE, as libdrm 2.4.134 encodes), import the fd into
 * a fresh syncobj, wait on it. Exercises the second EINVAL row fix. */
static void t_syncobj_abi(struct pf7580_dev *dev)
{
	uint32_t ctx_sync = 0, imp = 0;
	int fd = -1;

	CHECK_RC(pf7580_syncobj_create(dev, 1, &ctx_sync), 0,
		 "syncobj create signaled (ctx)");
	CHECK_RC(pf7580_syncobj_handle_to_fd(dev, ctx_sync, 1, &fd), 0,
		 "handle_to_fd SYNC_FILE (Mesa export)");
	CHECK(fd >= 0, "exported fd %d", fd);
	CHECK_RC(pf7580_syncobj_fd_to_handle(dev, fd, 1, &imp), 0,
		 "fd_to_handle SYNC_FILE (Mesa import)");
	CHECK(imp != 0 && imp != ctx_sync, "import handle %u", imp);
	CHECK_RC(pf7580_syncobj_wait(dev, &imp, 1, 0, 1, NULL), 0,
		 "wait imported signaled");
	CHECK_RC(pf7580_syncobj_destroy(dev, imp), 0, "destroy imported");
	if (!pf7580_is_mock(dev))
		close(fd);
	CHECK_RC(pf7580_syncobj_destroy(dev, ctx_sync), 0,
		 "destroy ctx syncobj");
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
	/* Mesa-exact ABI sequences (gpu31-abialign): must pass on mock. */
	t_get_param_abi(dev);
	t_syncobj_abi(dev);
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
		/* Mesa-exact sequences on silicon (gpu31-abialign): the
		 * GET_PARAM u64 probe order and the export/import fence
		 * snapshot. SUBMIT-free; SUBMIT is --submit-test only. */
		uint64_t v = 0;

		rc = pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &v);
		if (rc || v != PF7580_T720_PROD_ID) {
			printf("FAIL get_param u64: rc=%d (%s) v=0x%" PRIx64
			       "\n", rc, pf7580_strerror(rc), v);
			failures++;
		} else {
			printf("GET_PARAM u64: OK 0x%" PRIx64 "\n", v);
		}
		rc = pf7580_get_param(dev, 2, &v);
		if (rc != -EINVAL) {
			printf("FAIL get_param SHADER_PRESENT: rc=%d (%s), want -EINVAL\n",
			       rc, pf7580_strerror(rc));
			failures++;
		} else {
			printf("GET_PARAM SHADER_PRESENT: EINVAL ok (Mesa default path)\n");
		}
	}
	if (!failures) {
		uint32_t ctx_sync = 0, imp = 0;
		int efd = -1;

		rc = pf7580_syncobj_create(dev, 1, &ctx_sync);
		if (rc || !ctx_sync) {
			printf("FAIL syncobj_create signaled: %s\n",
			       pf7580_strerror(rc));
			failures++;
		} else {
			printf("SYNCOBJ_CREATE signaled: OK handle=%u\n",
			       ctx_sync);
			rc = pf7580_syncobj_handle_to_fd(dev, ctx_sync,
							  1, &efd);
			if (rc || efd < 0) {
				printf("FAIL handle_to_fd SYNC_FILE: %s\n",
				       pf7580_strerror(rc));
				failures++;
			} else {
				printf("HANDLE_TO_FD SYNC_FILE: OK fd=%d\n",
				       efd);
				rc = pf7580_syncobj_fd_to_handle(dev, efd,
								  1, &imp);
				if (rc || !imp) {
					printf("FAIL fd_to_handle SYNC_FILE: %s\n",
					       pf7580_strerror(rc));
					failures++;
				} else {
					printf("FD_TO_HANDLE SYNC_FILE: OK handle=%u (round-trip)\n",
					       imp);
				}
				close(efd);
				if (imp)
					pf7580_syncobj_destroy(dev, imp);
			}
			pf7580_syncobj_destroy(dev, ctx_sync);
		}
	}
	if (!failures) {
		rc = pf7580_wait_bo(dev, h, 0);
		if (rc) {
			printf("FAIL wait_bo: %s\n",
			       pf7580_strerror(rc));
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
	uint32_t out_sync = 0;
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
	/* Job-completion proof via the job's OWN fence (out_sync), not the
	 * vacuous WAIT_BO: the BO resv never carries the job fence (close-fix
	 * 476b75d), so WAIT_BO on a fresh BO returns instantly whether or not
	 * the GPU retired the job. The out_sync syncobj holds the sched_job
	 * fence installed at submit (NULL only on the not-yet-dispatched leg). */
	printf("SYNCOBJ-CREATE: creating out_sync...\n");
	fflush(stdout);
	rc = pf7580_syncobj_create(dev, 0, &out_sync);
	printf("SYNCOBJ-CREATE: rc=%d (%s) handle=%u\n", rc,
	       pf7580_strerror(rc), out_sync);
	if (rc || !out_sync) {
		printf("SUBMIT-TEST: FAILED (no out_sync; see dmesg)\n");
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.jc = gpu_off;
	sa.bo_handles = &h;
	sa.bo_handle_count = 1;
	sa.requirements = 0;
	sa.out_sync = out_sync;
	printf("SUBMIT: issuing one NULL job (jc=0x%" PRIx64
	       " req=0 slot=JS1 out_sync=%u)...\n", gpu_off, out_sync);
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
		pf7580_syncobj_destroy(dev, out_sync);
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	/* Pre-poll on the job's own fence: pending (-ETIME) means the fence
	 * is live and the job really dispatched. Already-0 means fast retire,
	 * the NULL-install leg, or a skip — dmesg (run_job/IRQ trail) decides. */
	printf("FENCE-PROBE: 0-timeout SYNCOBJ-WAIT on out_sync=%u (expect -ETIME=pending)...\n",
	       out_sync);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_syncobj_wait(dev, &out_sync, 1, 0, 0, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("FENCE-PROBE(0-timeout): rc=%d (%s) elapsed=%ld ms\n", rc,
	       pf7580_strerror(rc), elapsed_ms);
	if (!rc)
		printf("FENCE-PROBE: already signalled (fast retire, NULL-install leg, or skip; see dmesg)\n");
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline_ns = (int64_t)now.tv_sec * 1000000000LL +
		now.tv_nsec + 8000000000LL;
	printf("SYNCOBJ-WAIT: waiting out_sync=%u (+8s abs deadline %lld ns)...\n",
	       out_sync, (long long)deadline_ns);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_syncobj_wait(dev, &out_sync, 1, deadline_ns, 0, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("SYNCOBJ-WAIT(+8s abs): rc=%d (%s) elapsed=%ld ms\n", rc,
	       pf7580_strerror(rc), elapsed_ms);
	if (rc) {
		printf("SUBMIT-TEST: FAILED (job fence wait failed; bounded -ETIME means dark-GPU timeout, see dmesg)\n");
		pf7580_syncobj_destroy(dev, out_sync);
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	printf("SYNCOBJ-DESTROY: handle=%u...\n", out_sync);
	fflush(stdout);
	rc = pf7580_syncobj_destroy(dev, out_sync);
	printf("SYNCOBJ-DESTROY: rc=%d (%s)\n", rc, pf7580_strerror(rc));
	out_sync = 0;
	if (rc) {
		printf("SUBMIT-TEST: FAILED (destroy failed; see dmesg)\n");
		pf7580_cpu_unmap(dev, p, 4096);
		pf7580_close(dev);
		return 1;
	}
	/* Legacy WAIT_BO leg kept for the resv record (informational: vacuous
	 * on a fresh BO — empty resv returns instantly — so its rc proves
	 * nothing about the job; SYNCOBJ-WAIT above is the proof). */
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline_ns = (int64_t)now.tv_sec * 1000000000LL +
		now.tv_nsec + 8000000000LL;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_wait_bo(dev, h, deadline_ns);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("WAIT_BO(+8s, informational, vacuous on fresh BO): rc=%d (%s) elapsed=%ld ms\n",
	       rc, pf7580_strerror(rc), elapsed_ms);
	rc = pf7580_cpu_unmap(dev, p, 4096);
	if (rc)
		printf("WARN cpu unmap: %s\n", pf7580_strerror(rc));
	rc = pf7580_get_param(dev, PF7580_PARAM_GPU_PROD_ID, &prod);
	printf("POST GET_PARAM: rc=%d (%s) prod=0x%" PRIx64 "%s\n", rc,
	       pf7580_strerror(rc), prod,
	       (!rc && prod == PF7580_T720_PROD_ID) ? " (node alive)" :
						      " (MISMATCH)");
	if (rc || prod != PF7580_T720_PROD_ID) {
		printf("CLOSE: NOT reached (node unhealthy); closing fd...\n");
		fflush(stdout);
		printf("CLOSE: calling pf7580_close now\n");
		fflush(stdout);
		pf7580_close(dev);
		printf("CLOSE: returned (fd closed)\n");
		printf("SUBMIT-TEST: FAILED (node unhealthy after submit)\n");
		return 1;
	}
	printf("SUBMIT-TEST: PASS (submit+fence proven; closing fd...)\n");
	fflush(stdout);
	printf("CLOSE: calling pf7580_close now\n");
	fflush(stdout);
	pf7580_close(dev);
	printf("CLOSE: returned (fd closed); process exiting\n");
	fflush(stdout);
	return 0;
}
/* WRITE_VALUE pattern test ("coherency proof", gpu31-coherency).
 * Layout (two 4 KiB BOs, both mapped through the driver's AS0):
 *   job BO:    56-byte job at offset 0: 32-byte header (type 2 =
 *              WRITE_VALUE, 64-bit next=NULL) + 24-byte payload
 *              (u64 address = result gpu_offset, u32 type = 6 =
 *              Immediate-32, u64 immediate = PATTERN). Byte layout
 *              follows Mesa 20.3's midgard.xml pack order exactly:
 *              header words [0..7] = {exc=0, first=0, fault=0,0,
 *              w4 = is64b|type<<1, deps=0, next=0,0},
 *              payload words [8..13] = {addr_lo, addr_hi, type32,
 *              0, imm_lo, imm_hi}.
 *   result BO: zeroed; word 0 is the GPU write target.
 *
 * Sequence: SUBMIT(jc=job gpu_offset, both BOs, req=0 -> JS1) +
 * out_sync fence-wait (+8 s abs) + munmap + re-mmap result + compare.
 * Expected: result[0] == PATTERN. Before the coherency fix the CPU
 * read back 0 (stale cache lines: flush_dcache_page is a flag-clear
 * no-op on arm64, no HIGHMEM); after the fix (dma_map at bind,
 * sync_for_device at SUBMIT, sync_for_cpu at completion) it reads
 * the GPU-written pattern. A faulting job (bad address/type) shows
 * result[0] == 0 AND a nonzero JS_STATUS — distinguishable in dmesg
 * from a coherency miss (IRQ DONE + clean status, word still 0).
 * Exit: 0 pattern matched, 1 mismatch/failure, 2 node unavailable. */
#define PF7580_PATTERN_EXPECTED		0xC0C3E8E1u
static int run_pattern_test(const char *path)
{
	struct pf7580_dev *dev;
	char err[256] = "";
	uint64_t prod = 0, job_map = 0, res_map = 0;
	uint64_t job_gpu = 0, res_gpu = 0;
	uint32_t jh = 0, rh = 0;
	uint32_t out_sync = 0;
	uint32_t bos[2];
	unsigned char *jp = NULL, *rp = NULL;
	uint32_t *w;
	struct pf7580_submit_args sa;
	struct timespec now, t0, t1;
	int64_t deadline_ns;
	long elapsed_ms;
	uint32_t got = 0;
	int rc;

	printf("-- pf7580 pattern-test (WRITE_VALUE coherency) on %s --\n",
	       path);
	rc = pf7580_open(path, &dev, err, sizeof(err));
	if (rc) {
		printf("pattern-test %s unavailable: %s\n", path, err);
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
	rc = pf7580_create_bo(dev, 4096, &jh, &job_gpu);
	if (rc || !jh || !job_gpu || (job_gpu & 7u)) {
		printf("CREATE_BO(job): FAIL rc=%d (%s) h=%u off=0x%" PRIx64
		       "\n", rc, pf7580_strerror(rc), jh, job_gpu);
		pf7580_close(dev);
		return 1;
	}
	rc = pf7580_create_bo(dev, 4096, &rh, &res_gpu);
	if (rc || !rh || !res_gpu || (res_gpu & 7u)) {
		printf("CREATE_BO(result): FAIL rc=%d (%s) h=%u off=0x%" PRIx64
		       "\n", rc, pf7580_strerror(rc), rh, res_gpu);
		pf7580_close(dev);
		return 1;
	}
	printf("CREATE_BO: job h=%u gpu=0x%" PRIx64 " result h=%u gpu=0x%" PRIx64
	       "\n", jh, job_gpu, rh, res_gpu);
	rc = pf7580_mmap_bo(dev, jh, &job_map);
	if (rc) {
		printf("MMAP_BO(job): FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_close(dev);
		return 1;
	}
	rc = pf7580_mmap_bo(dev, rh, &res_map);
	if (rc) {
		printf("MMAP_BO(result): FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_close(dev);
		return 1;
	}
	jp = pf7580_cpu_map(dev, jh, job_map, 4096);
	if (!jp) {
		printf("FAIL cpu map job: mmap(fd=%d, off=0x%" PRIx64
		       ", len=%zu) errno=%d (%s)\n", pf7580_fd(dev),
		       pf7580_cpu_map_off, pf7580_cpu_map_len,
		       pf7580_cpu_map_errno,
		       strerror(pf7580_cpu_map_errno));
		pf7580_close(dev);
		return 1;
	}
	rp = pf7580_cpu_map(dev, rh, res_map, 4096);
	if (!rp) {
		printf("FAIL cpu map result: mmap(fd=%d, off=0x%" PRIx64
		       ", len=%zu) errno=%d (%s)\n", pf7580_fd(dev),
		       pf7580_cpu_map_off, pf7580_cpu_map_len,
		       pf7580_cpu_map_errno,
		       strerror(pf7580_cpu_map_errno));
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_close(dev);
		return 1;
	}
	memset(jp, 0, 4096);
	memset(rp, 0, 4096);
	w = (uint32_t *)jp;
	w[4] = (1u << 0) | (2u << 1);
	w[8] = (uint32_t)(res_gpu & 0xffffffffu);
	w[9] = (uint32_t)(res_gpu >> 32);
	w[10] = 6u;
	w[11] = 0u;
	w[12] = (uint32_t)PF7580_PATTERN_EXPECTED;
	w[13] = 0u;
	__builtin___clear_cache((char *)jp, (char *)jp + 4096);
	__builtin___clear_cache((char *)rp, (char *)rp + 4096);
	if (((uint32_t *)rp)[0] != 0) {
		printf("PATTERN: FAIL result not zero before submit (0x%08x)\n",
		       ((uint32_t *)rp)[0]);
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}
	printf("DESC: OK WRITE_VALUE type=2 imm32=0x%08x -> [0x%" PRIx64
	       "] at jc=0x%" PRIx64 "\n", PF7580_PATTERN_EXPECTED, res_gpu,
	       job_gpu);
	rc = pf7580_syncobj_create(dev, 0, &out_sync);
	if (rc || !out_sync) {
		printf("SYNCOBJ-CREATE: FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}
	bos[0] = jh;
	bos[1] = rh;
	memset(&sa, 0, sizeof(sa));
	sa.jc = job_gpu;
	sa.bo_handles = bos;
	sa.bo_handle_count = 2;
	sa.requirements = 0;
	sa.out_sync = out_sync;
	printf("SUBMIT: WRITE_VALUE jc=0x%" PRIx64 " bos=[%u,%u] out_sync=%u...\n",
	       job_gpu, jh, rh, out_sync);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_submit(dev, &sa);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("SUBMIT: rc=%d (%s) elapsed=%ld ms\n", rc,
	       pf7580_strerror(rc), elapsed_ms);
	if (rc) {
		printf("PATTERN-TEST: FAILED (submit rejected; see dmesg)\n");
		pf7580_syncobj_destroy(dev, out_sync);
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline_ns = (int64_t)now.tv_sec * 1000000000LL +
		now.tv_nsec + 8000000000LL;
	printf("SYNCOBJ-WAIT: waiting out_sync=%u (+8s abs)...\n", out_sync);
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = pf7580_syncobj_wait(dev, &out_sync, 1, deadline_ns, 0, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
		(t1.tv_nsec - t0.tv_nsec) / 1000000L;
	printf("SYNCOBJ-WAIT(+8s abs): rc=%d (%s) elapsed=%ld ms\n", rc,
	       pf7580_strerror(rc), elapsed_ms);
	if (rc) {
		printf("PATTERN-TEST: FAILED (fence wait failed; dark-GPU timeout, see dmesg)\n");
		pf7580_syncobj_destroy(dev, out_sync);
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}
	pf7580_syncobj_destroy(dev, out_sync);
	out_sync = 0;
	pf7580_cpu_unmap(dev, rp, 4096);
	rp = NULL;
	rc = pf7580_mmap_bo(dev, rh, &res_map);
	if (rc) {
		printf("RE-MMAP(result): FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_close(dev);
		return 1;
	}
	rp = pf7580_cpu_map(dev, rh, res_map, 4096);
	if (!rp) {
		printf("FAIL cpu remap result: errno=%d (%s)\n",
		       pf7580_cpu_map_errno,
		       strerror(pf7580_cpu_map_errno));
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_close(dev);
		return 1;
	}
	got = ((uint32_t *)rp)[0];
	printf("PATTERN: expected=0x%08x actual=0x%08x %s\n",
	       PF7580_PATTERN_EXPECTED, got,
	       got == PF7580_PATTERN_EXPECTED ? "MATCH" : "MISMATCH");
	if (got != PF7580_PATTERN_EXPECTED)
		printf("HINT: MISMATCH with fence signalled = stale CPU lines (coherency); "
		       "MISMATCH with JS_STATUS fault in dmesg = bad job/VA.\n");
	pf7580_cpu_unmap(dev, jp, 4096);
	pf7580_cpu_unmap(dev, rp, 4096);
	pf7580_close(dev);
	printf(got == PF7580_PATTERN_EXPECTED ?
	       "PATTERN-TEST: PASS (GPU write visible to CPU)\n" :
	       "PATTERN-TEST: FAILED (GPU write NOT visible)\n");
	return got == PF7580_PATTERN_EXPECTED ? 0 : 1;
}

int main(int argc, char **argv)
{
	const char *devpath = getenv("PF7580_DEVICE");
	const char *probeonly = NULL;
	const char *submittest = NULL;
	const char *patterntest = NULL;
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
		} else if (!strcmp(argv[i], "--pattern-test") && i + 1 < argc) {
			patterntest = argv[++i];
		} else if (!strncmp(argv[i], "--pattern-test=", 15)) {
			patterntest = argv[i] + 15;
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			devpath = argv[++i];
		} else if (!strncmp(argv[i], "--device=", 9)) {
			devpath = argv[i] + 9;
		} else {
			fprintf(stderr, "usage: %s [--print-abi] [--device PATH] [--probe-only PATH] [--submit-test PATH] [--pattern-test PATH]\n",
				argv[0]);
			return 1;
		}
	}
	if (probeonly)
		return run_probe_only(probeonly);
	if (submittest)
		return run_submit_test(submittest);
	if (patterntest)
		return run_pattern_test(patterntest);
	if (devpath)
		return run_device(devpath);
	return run_mock_suite();
}
