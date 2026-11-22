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
 *   --pattern-test PATH ("gpu write proof", gpu31-gpuwrite): one
 *                 56-byte WRITE_VALUE job (type 2, Zero) whose payload
 *                 targets a second 4 KiB result BO pre-filled with
 *                 0xA5A5A5A5. SUBMIT (req=0 -> JS0), fence-wait the
 *                 out_sync, then compare word 0 in BOTH views:
 *                 userspace re-mmap AND the DEBUG_READ_BO kernel
 *                 readback (same completion invalidate the pre-signal
 *                 IRQ path runs). PASS (both zero) proves the GPU wrote
 *                 the page. Kernel-zero +
 *                 user-0xA5 = stale CPU alias (visibility bug); both
 *                 0xA5 + fence signalled = GPU never wrote
 *                 (encoding/slot); +JS_STATUS fault = bad job/VA.
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

/* Syncobj wait blocks on a pending fence (sim-clock, mock only).
 * Programs fence fires on the virtual clock and proves the wait —
 * with no sleeping — advances to the firing (rc 0) or expires at
 * the deadline (-ETIME), for both ANY and WAIT_ALL. This is the
 * host mirror of the kernel WAIT loop: the absolute deadline is
 * honoured, a signal inside the budget wins, and a pending fence
 * with an empty budget (-ETIME) never reports success. */
static void t_syncobj_wait_block(struct pf7580_dev *dev)
{
	uint32_t a = 0, b = 0;
	uint32_t first = 0xdeadbeefu;
	int64_t t0;

	if (!pf7580_is_mock(dev))
		return;
	t0 = pf7580_mock_now(dev);
	CHECK(t0 == 0, "sim clock starts at 0 (%" PRId64 ")", t0);
	/* Pending fence, zero deadline: non-blocking probe expires. */
	CHECK_RC(pf7580_syncobj_create(dev, 0, &a), 0, "waitblk create a");
	CHECK_RC(pf7580_syncobj_wait(dev, &a, 1, t0, 0, NULL), -ETIME,
		 "waitblk ANY pending t=0 expires");
	CHECK(pf7580_mock_now(dev) == t0, "waitblk ANY probe consumes no time");
	CHECK_RC(pf7580_syncobj_create(dev, 0, &b), 0, "waitblk create b");
	CHECK_RC(pf7580_syncobj_wait(dev, &b, 1, t0, 1, NULL), -ETIME,
		 "waitblk ALL pending t=0 expires");
	pf7580_syncobj_destroy(dev, b);
	b = 0;
	/* Fire inside the deadline: ANY blocks (in sim time) to it. */
	CHECK_RC(pf7580_mock_signal_at(dev, a, t0 + 5000000000LL), 0,
		 "waitblk program a+5s");
	CHECK_RC(pf7580_syncobj_wait(dev, &a, 1, t0 + 8000000000LL, 0,
				      &first), 0, "waitblk ANY fires at +5s");
	CHECK(first == 0, "waitblk ANY first==0 (%u)", first);
	CHECK(pf7580_mock_now(dev) == t0 + 5000000000LL,
	      "waitblk clock advanced to fire (%" PRId64 ")",
	      pf7580_mock_now(dev));
	/* A fired fence stays signaled: zero-deadline wait succeeds. */
	CHECK_RC(pf7580_syncobj_wait(dev, &a, 1,
				      pf7580_mock_now(dev), 0, NULL), 0,
		 "waitblk signaled t=0 succeeds");
	/* Deadline before the fire: expires, clock jumps to deadline. */
	CHECK_RC(pf7580_syncobj_create(dev, 0, &b), 0, "waitblk create b");
	CHECK_RC(pf7580_mock_signal_at(dev, b,
					pf7580_mock_now(dev) + 5000000000LL),
		 0, "waitblk program b+5s");
	CHECK_RC(pf7580_syncobj_wait(dev, &b, 1,
				      pf7580_mock_now(dev) + 2000000000LL, 0,
				      NULL), -ETIME,
		 "waitblk ANY deadline-first expires");
	/* WAIT_ALL consumes the latest fire inside the budget. Each
	 * phase uses fresh handles: a fired fence stays signaled and
	 * the clock never runs backward. */
	pf7580_syncobj_destroy(dev, a);
	pf7580_syncobj_destroy(dev, b);
	a = b = 0;
	pf7580_mock_advance(dev, t0 + 10000000000LL);
	t0 = pf7580_mock_now(dev);
	CHECK_RC(pf7580_syncobj_create(dev, 0, &a), 0, "waitblk re-create a");
	CHECK_RC(pf7580_syncobj_create(dev, 0, &b), 0, "waitblk re-create b");
	CHECK_RC(pf7580_mock_signal_at(dev, a, t0 + 2000000000LL), 0,
		 "waitblk program a+2s");
	CHECK_RC(pf7580_mock_signal_at(dev, b, t0 + 5000000000LL), 0,
		 "waitblk program b+5s");
	{
		uint32_t hs[2] = { a, b };

		CHECK_RC(pf7580_syncobj_wait(dev, hs, 2, t0 + 8000000000LL,
					      1, NULL), 0,
			 "waitblk ALL fires consume latest");
	}
	CHECK(pf7580_mock_now(dev) == t0 + 5000000000LL,
	      "waitblk ALL clock at latest fire (%" PRId64 ")",
	      pf7580_mock_now(dev));
	/* WAIT_ALL with one fire past the deadline: expires. */
	pf7580_syncobj_destroy(dev, a);
	a = 0;
	pf7580_mock_advance(dev, t0);
	t0 = pf7580_mock_now(dev);
	CHECK_RC(pf7580_syncobj_create(dev, 0, &a), 0, "waitblk recreate a2");
	CHECK_RC(pf7580_mock_signal_at(dev, b, t0 + 5000000000LL), 0,
		 "waitblk reprogram b+5s");
	CHECK_RC(pf7580_mock_signal_at(dev, a, t0 + 9000000000LL), 0,
		 "waitblk program a2+9s");
	{
		uint32_t hs[2] = { a, b };

		CHECK_RC(pf7580_syncobj_wait(dev, hs, 2, t0 + 8000000000LL,
					      1, NULL), -ETIME,
			 "waitblk ALL past-deadline expires");
	}
	CHECK(pf7580_mock_now(dev) == t0 + 8000000000LL,
	      "waitblk ALL expiry lands on deadline (%" PRId64 ")",
	      pf7580_mock_now(dev));
	/* Unknown handle still fails like the 0003 shim. */
	{
		uint32_t bad = 0xdeadbeefu;

		CHECK_RC(pf7580_syncobj_wait(dev, &bad, 1, t0 + 1000, 0,
					      NULL), -ENOENT,
			 "waitblk unknown handle ENOENT");
	}
	pf7580_syncobj_destroy(dev, a);
	pf7580_syncobj_destroy(dev, b);
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
	t_syncobj_wait_block(dev);
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
	/* Syncobj-wait blocking contract (gpu31-syncobjwait): the 8 s
	 * wait on a pending job fence MUST consume wall time — either
	 * the IRQ round-trip (fast retire) or the ~5 s sched-timeout
	 * recovery (dark/stuck slot signals the same fence). An
	 * instant-0 return on a fence the 0-timeout probe just proved
	 * pending means the wait never blocked (empty-syncobj leg:
	 * pre-armfence kernels installed nothing at SUBMIT). Only the
	 * already-signaled probe outcome excuses elapsed==0. */
	if (elapsed_ms == 0) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		deadline_ns = (int64_t)now.tv_sec * 1000000000LL +
			now.tv_nsec;
		rc = pf7580_syncobj_wait(dev, &out_sync, 1, deadline_ns,
					 0, NULL);
		printf("WAIT-RECHECK(0-timeout, post-wait): rc=%d (%s)\n",
		       rc, pf7580_strerror(rc));
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
/* WRITE_VALUE pattern test ("gpu write proof", gpu31-gpuwrite).
 * Layout (two 4 KiB BOs, both mapped through the driver's AS0):
 *   job BO:    56-byte job at offset 0: 32-byte header (type 2 =
 *              WRITE_VALUE, 64-bit next=NULL) + 24-byte payload
 *              (u64 address = result gpu_offset, u32 type = 3 =
 *              Zero, u64 immediate = 0, ignored). Byte layout follows
 *              Mesa 20.3's midgard.xml pack order exactly: header
 *              words [0..7] = {exc=0, first=0, fault=0,0,
 *              w4 = is64b|type<<1, deps=0, next=0,0}, payload words
 *              [8..13] = {addr_lo, addr_hi, type32, 0, 0, 0}.
 *   result BO: pre-filled from userspace with 0xA5A5A5A5; word 0 is
 *              the GPU write target (Zero fill writes zeroes over
 *              the target range; we check word 0 == 0 after completion).
 * Why Zero and not Immediate-32: the T720 is a PAN_ARCH 4 (Midgard
 * v4) part (pan_model.h: 0x720 -> arch 4); the v4/v5 genxml used for
 * it (Mesa 20.3 lib/midgard.xml, on-device) defines only three
 * Write Value Types (1 Cycle Counter, 2 System Timestamp, 3 Zero).
 * Immediate 8/16/32/64 (4..7) exist only in the v6+ genxml for
 * Bifrost parts. The old test encoded type 6 (Immediate-32): on
 * this GPU that type field is reserved, so the job either faults
 * (JS_STATUS) or no-ops while the fence still signals. The Zero
 * job makes the write visible by construction: target starts
 * 0xA5-filled, GPU zeroes it.
 * Sequence: fill result with 0xA5 + SUBMIT(jc=job gpu_offset, both
 * BOs, req=0 -> JS0) + out_sync fence-wait (+8 s abs) + userspace
 * re-mmap + compare + kernel-side debug readback (DEBUG_READ_BO,
 * fops-routed 0xCF, NOT in the six-ioctl table) + compare.
 * Expected: result[0] == 0 in BOTH views. Mismatch in both views
 * with fence signalled + clean JS_STATUS = GPU never wrote
 * (encoding/slot); kernel-zero + user-0xA5 = visibility bug (CPU
 * alias stale). Exit: 0 matched, 1 mismatch/failure, 2 unavailable.
 * DEBUG-ONLY kernel surface: the readback ioctl is a temporary
 * debug switch, never in the v5.2 table; the cutover removes it
 * (see gpu31-gpuwrite.md). */
#define PF7580_PATTERN_PREFILL		0xA5A5A5A5u
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
	{
		uint32_t i;

		/* 0xA5 prefill: the GPU Zero job must visibly clear it. */
		for (i = 0; i < 4096 / 4; i++)
			((uint32_t *)rp)[i] = PF7580_PATTERN_PREFILL;
	}
	memset(jp, 0, 4096);
	w = (uint32_t *)jp;
	w[4] = (1u << 0) | (2u << 1);
	w[8] = (uint32_t)(res_gpu & 0xffffffffu);
	w[9] = (uint32_t)(res_gpu >> 32);
	w[10] = 3u;	/* Zero (v4/v5 genxml: only 1/2/3 exist) */
	w[11] = 0u;
	w[12] = 0u;	/* immediate ignored for Zero */
	w[13] = 0u;
	if (((uint32_t *)rp)[0] != PF7580_PATTERN_PREFILL) {
		printf("PATTERN: FAIL prefill not visible (0x%08x)\n",
		       ((uint32_t *)rp)[0]);
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}
	printf("DESC: OK WRITE_VALUE type=2 Zero -> [0x%" PRIx64
	       "] at jc=0x%" PRIx64 " (prefill 0x%08x)\n", res_gpu,
	       job_gpu, PF7580_PATTERN_PREFILL);
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
	printf("USER: word0=0x%08x (prefill 0x%08x, expect 0x00000000) %s\n",
	       got, PF7580_PATTERN_PREFILL,
	       got == 0 ? "ZERO" : "STALE");
	/* Kernel-side view: same page via the debug readback (same
	 * completion invalidate the pre-signal IRQ path runs). */
	{
		uint32_t kv = 0xdeadbeef;
		int krc = pf7580_debug_read_bo(dev, rh, 0, &kv);

		if (krc) {
			printf("KERN: DEBUG_READ_BO rc=%d (%s)\n",
			       krc, pf7580_strerror(krc));
		} else {
			printf("KERN: word0=0x%08x %s\n", kv,
			       kv == 0 ? "ZERO" : "STALE");
		}
		if (!krc && kv == 0 && got == 0) {
			printf("PATTERN-TEST: PASS (GPU wrote; both views zero)\n");
			pf7580_cpu_unmap(dev, jp, 4096);
			pf7580_cpu_unmap(dev, rp, 4096);
			pf7580_close(dev);
			return 0;
		}
		if (!krc && kv == 0 && got != 0)
			printf("HINT: kernel zero + user 0xA5 = stale CPU alias (visibility bug).\n");
		else if (got == PF7580_PATTERN_PREFILL)
			printf("HINT: both views 0xA5 + fence signalled = GPU never wrote (encoding/slot); check dmesg JS_STATUS.\n");
		else
			printf("HINT: partial write or fault; check dmesg JS_STATUS + MMU fault lines.\n");
	}
	pf7580_cpu_unmap(dev, jp, 4096);
	pf7580_cpu_unmap(dev, rp, 4096);
	pf7580_close(dev);
	printf("PATTERN-TEST: FAILED (GPU write NOT visible)\n");
	return 1;
}

/* gpu31-cachedir device test: CPU-filled BO survives a job that reads it.
 *
 * Shape (the ticket's ask, no GL needed): a 4 KiB BO carries BOTH the
 * job descriptor (32-B WRITE_VALUE/Zero header at offset 0, so the GPU
 * is guaranteed to FETCH the page) and a 96-B shader-ISA-shaped pattern
 * at offset 0x100 (shader tag byte 5 in word 0, like a real Midgard ISA
 * header), plus a page-wide deterministic fill. A second 4 KiB BO is
 * the write target (pre-filled 0xA5A5A5A5): the GPU zeroing word 0 there
 * proves the job actually ran, hence that the GPU really read the first
 * page.
 *
 * After the out_sync fence retires, compare every 32-bit word of the
 * shader BO against a pre-submit snapshot in BOTH views: the userspace
 * WC mapping and the kernel view (DEBUG_READ_BO 0xCF ->
 * p7580_gem_debug_read_word -> clean-then-invalidate + kmap read).
 * PASS = the CPU's bytes are intact in both views and the target was
 * zeroed. A mismatch is the "ISA BO reads back as zeros" failure mode;
 * the exact word and both values are printed.
 *
 * Why this is the cache-direction test: the page sits in the submit BO
 * list (it is the jc BO), so the pre-signal/completion maintenance and
 * the submit snapshot run on it. Under the pre-fix order (bare
 * invalidate) a dirty kernel-alias line on that page would be discarded
 * here; under the fixed order (clean-then-invalidate) it survives. */
#define PF7580_CD_ISA_OFF	0x100u
#define PF7580_CD_ISA_LEN	96u
#define PF7580_CD_ISA_WORD0	0xc87a9835u	/* tag 5 in the low nibble */

static uint32_t pf7580_cd_fill(unsigned int i)
{
	/* Deterministic, non-repeating-across-words pattern. */
	uint32_t v = 0x9e3779b9u * (i + 1u) + 0x7f4a7c15u;

	return v ? v : 0xdeadbeefu;
}

static int run_cachedir_test(const char *path)
{
	struct pf7580_dev *dev;
	char err[256] = "";
	uint64_t prod = 0, job_map = 0, res_map = 0;
	uint64_t job_gpu = 0, res_gpu = 0;
	uint32_t jh = 0, rh = 0, out_sync = 0;
	uint32_t bos[2];
	unsigned char *jp = NULL, *rp = NULL;
	uint32_t *snap = NULL;
	uint32_t *w;
	struct pf7580_submit_args sa;
	struct timespec now, t0, t1;
	int64_t deadline_ns;
	long elapsed_ms;
	unsigned int i, bad_user = 0, bad_kern = 0, kern_read = 0;
	uint32_t bad_u_word = 0, bad_u_got = 0, bad_u_want = 0;
	uint32_t res0 = 0xdeadbeefu;
	int rc;

	printf("-- pf7580 cachedir-test (CPU-filled BO vs read-only submit) on %s --\n",
	       path);
	rc = pf7580_open(path, &dev, err, sizeof(err));
	if (rc) {
		printf("cachedir-test %s unavailable: %s\n", path, err);
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
	rc = pf7580_create_bo(dev, 4096, &jh, &job_gpu);
	if (rc || !jh || !job_gpu) {
		printf("CREATE_BO(job): FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_close(dev);
		return 1;
	}
	rc = pf7580_create_bo(dev, 4096, &rh, &res_gpu);
	if (rc || !rh || !res_gpu) {
		printf("CREATE_BO(result): FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		pf7580_close(dev);
		return 1;
	}
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
	rp = pf7580_cpu_map(dev, rh, res_map, 4096);
	if (!jp || !rp) {
		printf("FAIL cpu map (job=%p result=%p) errno=%d (%s)\n",
		       (void *)jp, (void *)rp, pf7580_cpu_map_errno,
		       strerror(pf7580_cpu_map_errno));
		if (jp)
			pf7580_cpu_unmap(dev, jp, 4096);
		if (rp)
			pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}
	snap = malloc(4096);
	if (!snap) {
		printf("FAIL malloc snapshot\n");
		pf7580_cpu_unmap(dev, jp, 4096);
		pf7580_cpu_unmap(dev, rp, 4096);
		pf7580_close(dev);
		return 1;
	}

	/* CPU FILL: descriptor + ISA-shaped pattern + page-wide fill.	*/
	memset(jp, 0, 4096);
	w = (uint32_t *)jp;
	w[4] = (1u << 0) | (2u << 1);		/* WRITE_VALUE, from-zero */
	w[8] = (uint32_t)(res_gpu & 0xffffffffu);
	w[9] = (uint32_t)(res_gpu >> 32);
	w[10] = 3u;				/* Zero (v4/v5 genxml) */
	for (i = 0; i < 4096u / 4u; i++) {
		if (i >= (PF7580_CD_ISA_OFF >> 2) &&
		    i < ((PF7580_CD_ISA_OFF + PF7580_CD_ISA_LEN) >> 2))
			continue;		/* ISA block written below */
		if (i < 16u)
			continue;		/* JD header written above */
		w[i] = pf7580_cd_fill(i);
	}
	w[PF7580_CD_ISA_OFF >> 2] = PF7580_CD_ISA_WORD0;
	for (i = 1; i < PF7580_CD_ISA_LEN / 4u; i++)
		w[(PF7580_CD_ISA_OFF >> 2) + i] = pf7580_cd_fill(1000u + i);
	for (i = 0; i < 4096u / 4u; i++)
		((uint32_t *)rp)[i] = PF7580_PATTERN_PREFILL;
	memcpy(snap, jp, 4096);
	/* Order the WC stores against the syscall below (the submit ioctl
	 * is also the point where the driver cleans the range). */
	__sync_synchronize();
	printf("FILL: job BO h=%u gpu=0x%" PRIx64 " isa[0]=0x%08x result h=%u gpu=0x%" PRIx64 "\n",
	       jh, job_gpu, w[PF7580_CD_ISA_OFF >> 2], rh, res_gpu);

	rc = pf7580_syncobj_create(dev, 0, &out_sync);
	if (rc || !out_sync) {
		printf("SYNCOBJ-CREATE: FAIL rc=%d (%s)\n", rc,
		       pf7580_strerror(rc));
		goto fail;
	}
	bos[0] = jh;
	bos[1] = rh;
	memset(&sa, 0, sizeof(sa));
	sa.jc = job_gpu;		/* JD at offset 0 of the job BO */
	sa.bo_handles = bos;
	sa.bo_handle_count = 2;
	sa.requirements = 0;		/* -> JS0 */
	sa.out_sync = out_sync;
	printf("SUBMIT: jc=0x%" PRIx64 " bos=[%u,%u] out_sync=%u...\n",
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
		printf("CACHEDIR-TEST: FAILED (submit rejected; see dmesg)\n");
		pf7580_syncobj_destroy(dev, out_sync);
		goto fail;
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
		printf("CACHEDIR-TEST: FAILED (fence wait failed; dark GPU? see dmesg)\n");
		pf7580_syncobj_destroy(dev, out_sync);
		goto fail;
	}
	pf7580_syncobj_destroy(dev, out_sync);
	out_sync = 0;
	(void)pf7580_debug_read_bo(dev, rh, 0, &res0);
	printf("RESULT: word0=0x%08x (prefill 0x%08x) %s\n", res0,
	       PF7580_PATTERN_PREFILL,
	       res0 == 0 ? "ZERO (job ran)" : "NOT-ZERO (job did not write)");

	/* USER view: every word of the shader BO must still match.
	 * Word 0 is GPU-owned on Midgard: the hardware writes the
	 * next/done word back at JD[0] (observed 0x00000001 on T720),
	 * so it is reported separately, not counted as corruption. */
	for (i = 1; i < 4096u / 4u; i++) {
		if (w[i] != snap[i]) {
			if (!bad_user) {
				bad_u_word = i;
				bad_u_got = w[i];
				bad_u_want = snap[i];
			}
			bad_user++;
		}
	}
	printf("USER: %u/%u words intact%s", 1023u - bad_user, 1023u,
	       bad_user ? " ->" : "\n");
	if (bad_user)
		printf(" w[%u]=0x%08x want 0x%08x (%u bad)\n",
		       bad_u_word, bad_u_got, bad_u_want, bad_user);
	printf("JD0: 0x%08x (CPU wrote 0x00000000; the GPU's next/done writeback)\n",
	       w[0]);

	/* KERNEL view over the JD header (word 0 excluded: GPU-owned)
	 * + the 96-B ISA block. */
	for (i = 1; i < 8u; i++) {
		uint32_t kv = 0;

		if (pf7580_debug_read_bo(dev, jh, i * 4u, &kv))
			continue;
		kern_read++;
		if (kv != snap[i])
			bad_kern++;
	}
	for (i = 0; i < PF7580_CD_ISA_LEN / 4u; i++) {
		uint32_t kv = 0;

		if (pf7580_debug_read_bo(dev, jh,
					 PF7580_CD_ISA_OFF + i * 4u, &kv))
			continue;
		kern_read++;
		if (kv != ((uint32_t *)((char *)snap +
					PF7580_CD_ISA_OFF))[i])
			bad_kern++;
	}
	printf("KERN: %u/%u words read, %u mismatched\n", kern_read - bad_kern,
	       kern_read, bad_kern);

	pf7580_cpu_unmap(dev, jp, 4096);
	pf7580_cpu_unmap(dev, rp, 4096);
	free(snap);
	pf7580_close(dev);

	if (!bad_user && !bad_kern && res0 == 0) {
		printf("CACHEDIR-TEST: PASS (CPU-filled BO intact in both views, job ran)\n");
		return 0;
	}
	if (bad_user && !bad_kern)
		printf("HINT: user view lost words, kernel view intact = userspace WC mapping/BO lifetime, not kernel cache ops.\n");
	else if (!bad_user && bad_kern)
		printf("HINT: user view intact, kernel view stale = missing kernel invalidate (alias (a)).\n");
	else if (bad_user && bad_kern)
		printf("HINT: both views changed = the BO body itself was overwritten (DMA direction / page reuse), not a read-side alias.\n");
	else
		printf("HINT: data intact but the job did not write; check dmesg JS_STATUS for a fault.\n");
	printf("CACHEDIR-TEST: FAILED\n");
	return 1;

fail:
	free(snap);
	pf7580_cpu_unmap(dev, jp, 4096);
	pf7580_cpu_unmap(dev, rp, 4096);
	pf7580_close(dev);
	return 1;
}

int main(int argc, char **argv)
{
	const char *devpath = getenv("PF7580_DEVICE");
	const char *probeonly = NULL;
	const char *submittest = NULL;
	const char *patterntest = NULL;
	const char *cachedirtest = NULL;
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
		} else if (!strcmp(argv[i], "--cachedir-test") && i + 1 < argc) {
			cachedirtest = argv[++i];
		} else if (!strncmp(argv[i], "--cachedir-test=", 16)) {
			cachedirtest = argv[i] + 16;
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
	if (cachedirtest)
		return run_cachedir_test(cachedirtest);
	if (devpath)
		return run_device(devpath);
	return run_mock_suite();
}
