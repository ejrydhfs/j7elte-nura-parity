/* SPDX-License-Identifier: GPL-2.0 */
/*
 * abifuzz.c — host-only fuzzer/validator for the six v5.2 panfrost
 * ioctl argument encodings.
 *
 * Slices under test (host-runnable parts only):
 *   code/gpu31/drmcore/panfrost_ioctls_310.h  (ioctl numbers, UAPI macros,
 *     struct mirrors, ioctl table rows + flag contract)
 *   code/gpu31/gem/panfrost_drm.h             (G31_Gem UAPI mirror, the
 *     byte-identical reference; _310 typedefs are checked against it)
 *   handler validation contract from
 *     code/gpu31/gem/panfrost7580_ioctl.c +
 *     code/gpu31/jobs/panfrost7580_job.c (p7580_job_submit_args), as
 *     re-implemented pure-C reference predicates here (kernel bodies
 *     cannot run on the host; each predicate is line-annotated).
 *
 * Strategy: deterministic pseudo-random mutation of raw struct bytes
 * (LCG, fixed seed => reproducible), decoded to ioctl args, then
 * checked against the reference validators. Bug classes:
 *   A. kernel validation accepts something upstream v5.2 rejects
 *      (too loose — GPU reachable with bad jc/requirements/flags/pad).
 *   B. kernel validation rejects something v5.2 accepts (too strict —
 *      breaks stock Mesa/libdrm).
 *   C. layout drift between 310 mirrors and the UAPI mirror
 *      (size/offsets/sizeof mismatch caught statically too).
 * Plus: explicit directed probes for every flag/pad/size/timeout edge,
 * and ioctl-number/encoding-size drift checks.
 *
 * Build/run:  cc -Wall -Wextra -std=gnu89 -o abifuzz abifuzz.c && ./abifuzz
 * Exit status: 0 iff no findings (also prints PASS/FAIL per check so a
 * human can see coverage), 1 on any failure/finding.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include <drm/drm.h>			/* stub/drm/drm.h (host IOC model) */
#include "panfrost_drm.h"		/* G31_Gem UAPI mirror */
#include "panfrost_ioctls_310.h"	/* drmcore mirrors + table */

/* ---- error numbers (kernel ABI values, no kernel headers needed) ---- */
#define ABF_EINVAL	22
#define ABF_ENOENT	2
#define ABF_ENODEV	19
#define ABF_ETIMEDOUT	110
#define ABF_EBUSY	16

#define ABF_P7580_GPU_PROD_ID	0x720u
#define ABF_MAX_IN_SYNCS	16u
#define ABF_MAX_SUBMIT_BOS	64u
#define ABF_JD_REQ_FS		(1u << 0)

/* Upstream v5.2 slot map: FS -> JS0, else JS1 (ONLY_COMPUTE hidden
 * behind #if 0, so no other bit selects a slot at v5.2). */
#define ABF_NUM_JOB_SLOTS	3
#define ABF_JS_PRESENT_ALL	0x7u

/* ------------------------------------------------------------------ */
/* Reference validators: pure-C restatements of the kernel bodies.     */
/* ------------------------------------------------------------------ */

/* v5.2 panfrost_drv.c:panfrost_ioctl_get_param. */
static int ref_v52_get_param(const struct drm_panfrost_get_param *a,
			     uint64_t *value)
{
	if (a->pad != 0)
		return -ABF_EINVAL;
	if (a->param != DRM_PANFROST_PARAM_GPU_PROD_ID)
		return -ABF_EINVAL;
	*value = ABF_P7580_GPU_PROD_ID;
	return 0;
}

/* Current gem slice: panfrost7580_ioctl.c:panfrost_ioctl_get_param. */
static int ref_cur_get_param(const struct drm_panfrost_get_param *a,
			     uint64_t *value)
{
	if (a->pad)
		return -ABF_EINVAL;
	if (a->param != DRM_PANFROST_PARAM_GPU_PROD_ID)
		return -ABF_EINVAL;
	*value = ABF_P7580_GPU_PROD_ID;
	return 0;
}

/* v5.2 panfrost_drv.c:panfrost_ioctl_create_bo:
 *   if (!args->size || args->flags || args->pad) return -EINVAL; */
static int ref_v52_create_bo(const struct drm_panfrost_create_bo *a)
{
	if (!a->size || a->flags || a->pad)
		return -ABF_EINVAL;
	return 0;
}

/* Current: panfrost7580_ioctl.c (checks flags + size, never looks at pad). */
static int ref_cur_create_bo(const struct drm_panfrost_create_bo *a)
{
	if (a->flags)
		return -ABF_EINVAL;
	if (!a->size)
		return -ABF_EINVAL;
	return 0;
}

/* v5.2 panfrost_drv.c:panfrost_ioctl_mmap_bo:
 *   if (args->flags != 0) return -EINVAL; (+ handle lookup => ENOENT). */
static int ref_v52_mmap_bo(const struct drm_panfrost_mmap_bo *a)
{
	if (a->flags != 0)
		return -ABF_EINVAL;
	return 0;
}

/* Current: panfrost7580_ioctl.c: same flags-only check. */
static int ref_cur_mmap_bo(const struct drm_panfrost_mmap_bo *a)
{
	if (a->flags)
		return -ABF_EINVAL;
	return 0;
}

/* v5.2 panfrost_drv.c:panfrost_ioctl_get_bo_offset: NO pad check
 * (straight to handle lookup). */
static int ref_v52_get_bo_offset(const struct drm_panfrost_get_bo_offset *a)
{
	(void)a;
	return 0;
}

/* Current: panfrost7580_ioctl.c rejects nonzero pad. */
static int ref_cur_get_bo_offset(const struct drm_panfrost_get_bo_offset *a)
{
	if (a->pad)
		return -ABF_EINVAL;
	return 0;
}

/* v5.2 panfrost_drv.c:panfrost_ioctl_wait_bo:
 *   if (args->pad) return -EINVAL;
 *   timeout = drm_timeout_abs_to_jiffies(args->timeout_ns);
 *   ret = reservation_object_wait_timeout_rcu(..., true, true, timeout);
 *   if (!ret) ret = timeout ? -ETIMEDOUT : -EBUSY; */
static int ref_v52_wait_bo(const struct drm_panfrost_wait_bo *a)
{
	if (a->pad)
		return -ABF_EINVAL;
	return 0;
}

/* Current: panfrost7580_ioctl.c (fixed 2026-11-19): absolute-timeout
 * conversion via ktime_get_ts + nsecs_to_jiffies, then p7580_gem_wait;
 * timeout<0 becomes -errno, else 0. Return shape now matches v5.2:
 * a timed-out wait surfaces -ETIMEDOUT, a non-blocking poll -EBUSY. */
static int ref_cur_wait_bo(const struct drm_panfrost_wait_bo *a)
{
	if (a->pad)
		return -ABF_EINVAL;
	return 0;
}

/* v5.2 panfrost_job.c:panfrost_ioctl_submit head:
 *   if (!args->jc) return -EINVAL;
 *   if (args->requirements && args->requirements != PANFROST_JD_REQ_FS)
 *     return -EINVAL; */
static int ref_v52_submit(const struct drm_panfrost_submit *a)
{
	if (!a->jc)
		return -ABF_EINVAL;
	if (a->requirements && a->requirements != ABF_JD_REQ_FS)
		return -ABF_EINVAL;
	return 0;
}

/* GEM-slice shim body: panfrost7580_ioctl.c:panfrost_ioctl_submit:
 *   if (!args->jc || (args->jc & 7u)) return -EINVAL;
 *   if (args->requirements & ~PANFROST_JD_REQ_FS) return -EINVAL;
 *   if (in_sync_count > 16 || bo_handle_count > 64) return -EINVAL; */
static int ref_cur_submit_gem(const struct drm_panfrost_submit *a)
{
	if (!a->jc || (a->jc & 7u))
		return -ABF_EINVAL;
	if (a->requirements & ~ABF_JD_REQ_FS)
		return -ABF_EINVAL;
	if (a->in_sync_count > ABF_MAX_IN_SYNCS ||
	    a->bo_handle_count > ABF_MAX_SUBMIT_BOS)
		return -ABF_EINVAL;
	return 0;
}

/* Jobs-slice body: job.c:p7580_job_submit_args step 1 (line 445 verbatim:
 * "if ((args->jc & 7u) == 0u || args->jc == 0u) return -EINVAL;" — the
 * == 0u is inverted vs the gem shim's "& 7u" reject: rejects every
 * 8-aligned address, accepts 1..7. Almost certainly meant "!= 0u". */
static int ref_cur_submit_job(const struct drm_panfrost_submit *a)
{
	if ((a->jc & 7u) == 0u || a->jc == 0u)
		return -ABF_EINVAL;
	if (a->requirements & ~(ABF_JD_REQ_FS | (1u << 10)))
		return -ABF_EINVAL;
	if (a->in_sync_count > ABF_MAX_IN_SYNCS)
		return -ABF_EINVAL;
	if (a->bo_handle_count > ABF_MAX_SUBMIT_BOS)
		return -ABF_EINVAL;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Harness.                                                            */
/* ------------------------------------------------------------------ */
static int fails = 0;
static int findings = 0;
static unsigned int lcg_state = 0x12345678u;

static unsigned int lcg_next(void)
{
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return lcg_state >> 8;
}

static void fill_rand(unsigned char *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		buf[i] = (unsigned char)(lcg_next() & 0xFFu);
}

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

#define FINDING(tag, fmt, ...) do { \
	printf("FINDING %s: " fmt "\n", tag, ##__VA_ARGS__); \
	findings++; \
} while (0)

/* ---- static layout / encoding checks ---- */
static void check_layout(void)
{
	printf("[layout] struct sizes / offsets / ioctl encodings\n");

	/* 310 mirrors must be byte-identical to the UAPI mirror. */
	CHECK(sizeof(struct drm_panfrost_submit_310) ==
	      sizeof(struct drm_panfrost_submit));
	CHECK(sizeof(struct drm_panfrost_wait_bo_310) ==
	      sizeof(struct drm_panfrost_wait_bo));
	CHECK(sizeof(struct drm_panfrost_create_bo_310) ==
	      sizeof(struct drm_panfrost_create_bo));
	CHECK(sizeof(struct drm_panfrost_mmap_bo_310) ==
	      sizeof(struct drm_panfrost_mmap_bo));
	CHECK(sizeof(struct drm_panfrost_get_param_310) ==
	      sizeof(struct drm_panfrost_get_param));
	CHECK(sizeof(struct drm_panfrost_get_bo_offset_310) ==
	      sizeof(struct drm_panfrost_get_bo_offset));

	/* Field offsets must agree as well (catches reorderings). */
	CHECK(offsetof(struct drm_panfrost_submit_310, jc) ==
	      offsetof(struct drm_panfrost_submit, jc));
	CHECK(offsetof(struct drm_panfrost_submit_310, in_syncs) ==
	      offsetof(struct drm_panfrost_submit, in_syncs));
	CHECK(offsetof(struct drm_panfrost_submit_310, in_sync_count) ==
	      offsetof(struct drm_panfrost_submit, in_sync_count));
	CHECK(offsetof(struct drm_panfrost_submit_310, out_sync) ==
	      offsetof(struct drm_panfrost_submit, out_sync));
	CHECK(offsetof(struct drm_panfrost_submit_310, bo_handles) ==
	      offsetof(struct drm_panfrost_submit, bo_handles));
	CHECK(offsetof(struct drm_panfrost_submit_310, bo_handle_count) ==
	      offsetof(struct drm_panfrost_submit, bo_handle_count));
	CHECK(offsetof(struct drm_panfrost_submit_310, requirements) ==
	      offsetof(struct drm_panfrost_submit, requirements));
	CHECK(offsetof(struct drm_panfrost_wait_bo_310, timeout_ns) ==
	      offsetof(struct drm_panfrost_wait_bo, timeout_ns));
	CHECK(offsetof(struct drm_panfrost_create_bo_310, offset) ==
	      offsetof(struct drm_panfrost_create_bo, offset));
	CHECK(offsetof(struct drm_panfrost_mmap_bo_310, offset) ==
	      offsetof(struct drm_panfrost_mmap_bo, offset));
	CHECK(offsetof(struct drm_panfrost_get_param_310, value) ==
	      offsetof(struct drm_panfrost_get_param, value));
	CHECK(offsetof(struct drm_panfrost_get_bo_offset_310, offset) ==
	      offsetof(struct drm_panfrost_get_bo_offset, offset));

	/* Canonical v5.2 sizes (LP64): submit 40, wait 16, create 24,
	 * mmap 16, get_param 16, get_bo_offset 16. */
	CHECK(sizeof(struct drm_panfrost_submit) == 40u);
	CHECK(sizeof(struct drm_panfrost_wait_bo) == 16u);
	CHECK(sizeof(struct drm_panfrost_create_bo) == 24u);
	CHECK(sizeof(struct drm_panfrost_mmap_bo) == 16u);
	CHECK(sizeof(struct drm_panfrost_get_param) == 16u);
	CHECK(sizeof(struct drm_panfrost_get_bo_offset) == 16u);

	/* Ioctl numbers 0x00-0x05 on DRM_COMMAND_BASE. */
	CHECK(DRM_PANFROST_SUBMIT == 0x00);
	CHECK(DRM_PANFROST_WAIT_BO == 0x01);
	CHECK(DRM_PANFROST_CREATE_BO == 0x02);
	CHECK(DRM_PANFROST_MMAP_BO == 0x03);
	CHECK(DRM_PANFROST_GET_PARAM == 0x04);
	CHECK(DRM_PANFROST_GET_BO_OFFSET == 0x05);

	/* _310 ioctl macros must encode the SAME bytes as the UAPI ones. */
	CHECK(DRM_IOCTL_PANFROST_SUBMIT_310 == DRM_IOCTL_PANFROST_SUBMIT);
	CHECK(DRM_IOCTL_PANFROST_WAIT_BO_310 == DRM_IOCTL_PANFROST_WAIT_BO);
	CHECK(DRM_IOCTL_PANFROST_CREATE_BO_310 == DRM_IOCTL_PANFROST_CREATE_BO);
	CHECK(DRM_IOCTL_PANFROST_MMAP_BO_310 == DRM_IOCTL_PANFROST_MMAP_BO);
	CHECK(DRM_IOCTL_PANFROST_GET_PARAM_310 == DRM_IOCTL_PANFROST_GET_PARAM);
	CHECK(DRM_IOCTL_PANFROST_GET_BO_OFFSET_310 ==
	      DRM_IOCTL_PANFROST_GET_BO_OFFSET);

	/* Direction bits: SUBMIT/WAIT_BO write-only, the other four RW. */
	CHECK(ABF_IOC_DIR(DRM_IOCTL_PANFROST_SUBMIT) == ABF_IOC_WRITE);
	CHECK(ABF_IOC_DIR(DRM_IOCTL_PANFROST_WAIT_BO) == ABF_IOC_WRITE);
	CHECK(ABF_IOC_DIR(DRM_IOCTL_PANFROST_CREATE_BO) ==
	      (ABF_IOC_READ | ABF_IOC_WRITE));
	CHECK(ABF_IOC_DIR(DRM_IOCTL_PANFROST_MMAP_BO) ==
	      (ABF_IOC_READ | ABF_IOC_WRITE));
	CHECK(ABF_IOC_DIR(DRM_IOCTL_PANFROST_GET_PARAM) ==
	      (ABF_IOC_READ | ABF_IOC_WRITE));
	CHECK(ABF_IOC_DIR(DRM_IOCTL_PANFROST_GET_BO_OFFSET) ==
	      (ABF_IOC_READ | ABF_IOC_WRITE));

	/* Embedded size field must equal the struct size. */
	CHECK(ABF_IOC_SIZE(DRM_IOCTL_PANFROST_SUBMIT) ==
	      sizeof(struct drm_panfrost_submit));
	CHECK(ABF_IOC_SIZE(DRM_IOCTL_PANFROST_WAIT_BO) ==
	      sizeof(struct drm_panfrost_wait_bo));
	CHECK(ABF_IOC_SIZE(DRM_IOCTL_PANFROST_CREATE_BO) ==
	      sizeof(struct drm_panfrost_create_bo));
	CHECK(ABF_IOC_SIZE(DRM_IOCTL_PANFROST_MMAP_BO) ==
	      sizeof(struct drm_panfrost_mmap_bo));
	CHECK(ABF_IOC_SIZE(DRM_IOCTL_PANFROST_GET_PARAM) ==
	      sizeof(struct drm_panfrost_get_param));
	CHECK(ABF_IOC_SIZE(DRM_IOCTL_PANFROST_GET_BO_OFFSET) ==
	      sizeof(struct drm_panfrost_get_bo_offset));
}

/* ---- table flag contract ---- */
static int panfrost_ioctl_submit(struct drm_device *d, void *a,
				 struct drm_file *f)
{
	(void)d; (void)a; (void)f;
	return 0;
}
static int panfrost_ioctl_wait_bo(struct drm_device *d, void *a,
				  struct drm_file *f)
{
	(void)d; (void)a; (void)f;
	return 0;
}
static int panfrost_ioctl_create_bo(struct drm_device *d, void *a,
				    struct drm_file *f)
{
	(void)d; (void)a; (void)f;
	return 0;
}
static int panfrost_ioctl_mmap_bo(struct drm_device *d, void *a,
				  struct drm_file *f)
{
	(void)d; (void)a; (void)f;
	return 0;
}
static int panfrost_ioctl_get_param(struct drm_device *d, void *a,
				    struct drm_file *f)
{
	(void)d; (void)a; (void)f;
	return 0;
}
static int panfrost_ioctl_get_bo_offset(struct drm_device *d, void *a,
					struct drm_file *f)
{
	(void)d; (void)a; (void)f;
	return 0;
}

static struct drm_ioctl_desc panfrost_test_ioctls[] = {
	PANFROST_IOCTLS_310
};

static void check_table(void)
{
	unsigned int i;

	printf("[table] 6 rows, order, flags\n");
	CHECK(ARRAY_SIZE(panfrost_test_ioctls) == 6u);
	CHECK(panfrost_test_ioctls[0].cmd == DRM_IOCTL_PANFROST_SUBMIT_310);
	CHECK(panfrost_test_ioctls[1].cmd == DRM_IOCTL_PANFROST_WAIT_BO_310);
	CHECK(panfrost_test_ioctls[2].cmd == DRM_IOCTL_PANFROST_CREATE_BO_310);
	CHECK(panfrost_test_ioctls[3].cmd == DRM_IOCTL_PANFROST_MMAP_BO_310);
	CHECK(panfrost_test_ioctls[4].cmd == DRM_IOCTL_PANFROST_GET_PARAM_310);
	CHECK(panfrost_test_ioctls[5].cmd ==
	      DRM_IOCTL_PANFROST_GET_BO_OFFSET_310);

	/* v5.2 flag contract (panfrost_drv.c table): everything
	 * DRM_RENDER_ALLOW, SUBMIT additionally DRM_AUTH. */
	for (i = 0; i < ARRAY_SIZE(panfrost_test_ioctls); i++)
		CHECK((panfrost_test_ioctls[i].flags & DRM_RENDER_ALLOW) != 0);
	CHECK((panfrost_test_ioctls[0].flags & DRM_AUTH) != 0);
	for (i = 1; i < ARRAY_SIZE(panfrost_test_ioctls); i++)
		CHECK((panfrost_test_ioctls[i].flags & DRM_AUTH) == 0);
}

/* ---- directed probes ---- */
static void check_directed(void)
{
	struct drm_panfrost_get_param gp;
	struct drm_panfrost_create_bo cb;
	struct drm_panfrost_mmap_bo mb;
	struct drm_panfrost_get_bo_offset go;
	struct drm_panfrost_wait_bo wb;
	struct drm_panfrost_submit sb;
	uint64_t v;
	int r_v, r_c, r_j;

	printf("[directed] flag/pad/size/offset edges\n");

	/* GET_PARAM: pad must be rejected, only GPU_PROD_ID accepted. */
	memset(&gp, 0, sizeof(gp));
	gp.param = DRM_PANFROST_PARAM_GPU_PROD_ID;
	CHECK(ref_cur_get_param(&gp, &v) == 0 && v == ABF_P7580_GPU_PROD_ID);
	gp.pad = 1;
	CHECK(ref_cur_get_param(&gp, &v) == -ABF_EINVAL);
	CHECK(ref_v52_get_param(&gp, &v) == -ABF_EINVAL);
	gp.pad = 0;
	gp.param = 0xdead;
	CHECK(ref_cur_get_param(&gp, &v) == -ABF_EINVAL);
	gp.param = 1;	/* no param 1 at v5.2 (single-entry enum) */
	CHECK(ref_cur_get_param(&gp, &v) == -ABF_EINVAL);

	/* CREATE_BO: size 0 / flags / pad. */
	memset(&cb, 0, sizeof(cb));
	cb.size = 4096;
	CHECK(ref_cur_create_bo(&cb) == 0);
	cb.size = 0;
	CHECK(ref_cur_create_bo(&cb) == -ABF_EINVAL);
	cb.size = 4096;
	cb.flags = 1;
	CHECK(ref_cur_create_bo(&cb) == -ABF_EINVAL);
	cb.flags = 0xFFFFFFFFu;
	CHECK(ref_cur_create_bo(&cb) == -ABF_EINVAL);
	cb.flags = 0;
	cb.pad = 0x12345678u;
	r_v = ref_v52_create_bo(&cb);
	r_c = ref_cur_create_bo(&cb);
	CHECK(r_v == -ABF_EINVAL);
	if (r_c == 0)
		FINDING("F1", "CREATE_BO pad %#x: v5.2 rejects, port accepts",
			cb.pad);

	/* MMAP_BO: any nonzero flags rejected. */
	memset(&mb, 0, sizeof(mb));
	mb.handle = 1;
	CHECK(ref_cur_mmap_bo(&mb) == 0);
	mb.flags = 1;
	CHECK(ref_cur_mmap_bo(&mb) == -ABF_EINVAL);
	mb.flags = 0x80000000u;
	CHECK(ref_cur_mmap_bo(&mb) == -ABF_EINVAL);

	/* GET_BO_OFFSET: nonzero pad. */
	memset(&go, 0, sizeof(go));
	go.handle = 1;
	CHECK(ref_cur_get_bo_offset(&go) == 0);
	go.pad = 7;
	r_v = ref_v52_get_bo_offset(&go);
	r_c = ref_cur_get_bo_offset(&go);
	CHECK(r_v == 0);
	if (r_c == -ABF_EINVAL)
		FINDING("F2", "GET_BO_OFFSET pad %#x: v5.2 ignores, port rejects",
			go.pad);

	/* WAIT_BO: nonzero pad rejected both sides; timeout edges. */
	memset(&wb, 0, sizeof(wb));
	wb.handle = 1;
	wb.timeout_ns = 1000000000LL;
	CHECK(ref_cur_wait_bo(&wb) == 0);
	wb.pad = 1;
	CHECK(ref_cur_wait_bo(&wb) == -ABF_EINVAL);
	CHECK(ref_v52_wait_bo(&wb) == -ABF_EINVAL);
	wb.pad = 0;
	wb.timeout_ns = 0;	/* non-blocking poll shape */
	CHECK(ref_cur_wait_bo(&wb) == 0);
	wb.timeout_ns = -1;	/* negative: also non-blocking downstream */
	CHECK(ref_cur_wait_bo(&wb) == 0);

	/* SUBMIT: three validators — v5.2 (any nonzero jc), gem shim
	 * (8-aligned), jobs slice (INVERTED (jc&7)==0 reject: aligned
	 * fails, 1..7 passes; job.c:445). */
	memset(&sb, 0, sizeof(sb));
	sb.jc = 8;
	r_v = ref_v52_submit(&sb);
	r_c = ref_cur_submit_gem(&sb);
	r_j = ref_cur_submit_job(&sb);
	CHECK(r_v == 0);
	CHECK(r_c == 0);
	if (r_j == -ABF_EINVAL)
		FINDING("F3", "SUBMIT jc=8 (aligned): v5.2+gem accept, jobs slice rejects (inverted (jc&7)==0 test, job.c:445)");
	sb.jc = 0;
	CHECK(ref_v52_submit(&sb) == -ABF_EINVAL);
	CHECK(ref_cur_submit_gem(&sb) == -ABF_EINVAL);
	CHECK(ref_cur_submit_job(&sb) == -ABF_EINVAL);
	sb.jc = 4;
	r_v = ref_v52_submit(&sb);
	r_c = ref_cur_submit_gem(&sb);
	r_j = ref_cur_submit_job(&sb);
	CHECK(r_v == 0);
	CHECK(r_c == -ABF_EINVAL);
	if (r_j == 0)
		FINDING("F3b", "SUBMIT jc=4 (misaligned): jobs slice accepts, v5.2 accepts, gem shim rejects — slices disagree");
	sb.jc = 1;
	r_v = ref_v52_submit(&sb);
	r_c = ref_cur_submit_gem(&sb);
	r_j = ref_cur_submit_job(&sb);
	CHECK(r_v == 0);
	CHECK(r_c == -ABF_EINVAL);
	if (r_j == 0)
		FINDING("F3c", "SUBMIT jc=1: jobs slice accepts unaligned uaddr, dereferenced as u32 array (job.c:483-504)");
	sb.jc = 8;
	sb.requirements = ABF_JD_REQ_FS;
	CHECK(ref_v52_submit(&sb) == 0);
	CHECK(ref_cur_submit_gem(&sb) == 0);
	if (ref_cur_submit_job(&sb) == -ABF_EINVAL)
		FINDING("F3d", "SUBMIT jc=8 req=FS: jobs slice still rejects (jc inversion masks the happy path)");
	sb.requirements = 2;
	r_v = ref_v52_submit(&sb);
	r_c = ref_cur_submit_gem(&sb);
	r_j = ref_cur_submit_job(&sb);
	CHECK(r_v == -ABF_EINVAL && r_c == -ABF_EINVAL);
	CHECK(r_j == -ABF_EINVAL);


	/* requirements bit 10: accepted by jobs slice, rejected by
	 * v5.2 AND by the gem shim — internal divergence between the two
	 * port-side validators for the same ioctl. */
	sb.requirements = (1u << 10);
	r_v = ref_v52_submit(&sb);
	r_c = ref_cur_submit_gem(&sb);
	r_j = ref_cur_submit_job(&sb);
	CHECK(r_v == -ABF_EINVAL);
	if (r_j == 0 && r_c == -ABF_EINVAL)
		FINDING("F4", "SUBMIT req=0x400: gem shim rejects, jobs slice accepts, v5.2 rejects");

	sb.requirements = 0xFFFFFFFFu;
	CHECK(ref_v52_submit(&sb) == -ABF_EINVAL);
	CHECK(ref_cur_submit_gem(&sb) == -ABF_EINVAL);
	CHECK(ref_cur_submit_job(&sb) == -ABF_EINVAL);
	sb.requirements = 0;

	/* Caps enforced by both port validators, absent upstream
	 * (upstream over-reads userspace past the arrays instead). */
	sb.in_sync_count = ABF_MAX_IN_SYNCS;
	sb.bo_handle_count = ABF_MAX_SUBMIT_BOS;
	CHECK(ref_cur_submit_gem(&sb) == 0);
	sb.in_sync_count = ABF_MAX_IN_SYNCS + 1;
	CHECK(ref_cur_submit_gem(&sb) == -ABF_EINVAL);
	CHECK(ref_cur_submit_job(&sb) == -ABF_EINVAL);
	sb.in_sync_count = 0xFFFFFFFFu;
	CHECK(ref_cur_submit_gem(&sb) == -ABF_EINVAL);
	sb.in_sync_count = 0;
	sb.bo_handle_count = ABF_MAX_SUBMIT_BOS + 1;
	CHECK(ref_cur_submit_gem(&sb) == -ABF_EINVAL);
	CHECK(ref_cur_submit_job(&sb) == -ABF_EINVAL);
	sb.bo_handle_count = 0;
}

/* ---- randomised differential fuzz ---- */
static void check_fuzz(void)
{
	int i;
	int n_jc_diverge = 0, n_req_diverge = 0;
	int n_f1 = 0, n_f2 = 0;
	unsigned long long ex_jc = 0;
	unsigned ex_req = 0, ex_in = 0, ex_bo = 0;
	unsigned ex_pad1 = 0, ex_pad2 = 0;
	printf("[fuzz] 20000 randomised arg buffers per ioctl\n");
	for (i = 0; i < 20000; i++) {
		struct drm_panfrost_submit sb;
		struct drm_panfrost_create_bo cb;
		struct drm_panfrost_get_bo_offset go;
		int r_c, r_j;
		fill_rand((unsigned char *)&sb, sizeof(sb));
		r_c = ref_cur_submit_gem(&sb);
		r_j = ref_cur_submit_job(&sb);
		if ((r_c == 0) != (r_j == 0)) {
			if (sb.requirements & ~(ABF_JD_REQ_FS | (1u << 10)))
				{ }
			else if (sb.requirements & (1u << 10))
				n_req_diverge++;
			else if (sb.in_sync_count <= ABF_MAX_IN_SYNCS &&
				 sb.bo_handle_count <= ABF_MAX_SUBMIT_BOS)
				n_jc_diverge++;
			else {
				ex_jc = (unsigned long long)sb.jc;
				ex_req = sb.requirements;
				ex_in = sb.in_sync_count;
				ex_bo = sb.bo_handle_count;
			}
		}
		fill_rand((unsigned char *)&cb, sizeof(cb));
		if (ref_v52_create_bo(&cb) != 0 &&
		    ref_cur_create_bo(&cb) == 0) {
			if (!n_f1)
				ex_pad1 = cb.pad;
			n_f1++;
		}
		fill_rand((unsigned char *)&go, sizeof(go));
		if (ref_cur_get_bo_offset(&go) != 0) {
			if (!n_f2)
				ex_pad2 = go.pad;
			n_f2++;
		}
	}
	if (n_jc_diverge)
		FINDING("F3-fuzz", "%d/20000 SUBMIT iters: gem/job diverge on jc alignment (inverted job.c:445 test)", n_jc_diverge);
	if (n_req_diverge)
		FINDING("F4-fuzz", "%d/20000 SUBMIT iters: gem/job diverge on requirements bit 10 (ONLY_COMPUTE)", n_req_diverge);
	if (n_f1)
		FINDING("F1-fuzz", "%d/20000 CREATE_BO iters: port accepts nonzero pad (v5.2 rejects; e.g. %#x)", n_f1, ex_pad1);
	if (n_f2)
		FINDING("F2-fuzz", "%d/20000 GET_BO_OFFSET iters: port rejects nonzero pad (v5.2 ignores; e.g. %#x)", n_f2, ex_pad2);
	(void)ex_jc; (void)ex_req; (void)ex_in; (void)ex_bo;
}

/* ---- WAIT_BO timeout-shape check (fixed; now a PASS check) ---- */
static void check_wait_shape(void)
{
	/* 2026-11-19: panfrost7580_ioctl.c converts the absolute
	 * timeout_ns via ktime_get_ts + nsecs_to_jiffies and returns
	 * the p7580_gem_wait error verbatim (0 only on fence signal;
	 * -ETIMEDOUT/-EBUSY propagate). Return shape matches v5.2, so
	 * this section asserts the pad contract only. */
	printf("[wait] timeout-shape contract (static)\n");
	{
		struct drm_panfrost_wait_bo wb;
		memset(&wb, 0, sizeof(wb));
		CHECK(ref_cur_wait_bo(&wb) == 0);
		CHECK(ref_v52_wait_bo(&wb) == 0);
		wb.pad = 1;
		CHECK(ref_cur_wait_bo(&wb) == -ABF_EINVAL);
		CHECK(ref_v52_wait_bo(&wb) == -ABF_EINVAL);
	}
}

/* ---- SUBMIT out_sync / compat-ioctl contract (static) ---- */
static void check_out_sync_compat(void)
{
	printf("[contract] out_sync + compat-ioctl (static)\n");
	/* Neither the gem shim nor the jobs slice validates out_sync:
	 * any u32 passes validation (ENOENT surfaces later at
	 * sync_replace time). v5.2 looks the handle up first and
	 * returns -ENODEV for a bogus handle. No host execution
	 * possible — record as structural finding. */
	FINDING("F5", "SUBMIT out_sync: port validates nothing (bogus handle fails late at sync_replace); v5.2 returns -ENODEV up front (drm_syncobj_find)");
	/* Neither DEFINE_DRM_GEM_FOPS nor _COMPAT wires .compat_ioctl,
	 * so 32-bit compat tasks calling the 6 ioctls get -ENOTTY while
	 * exynos wires drm_compat_ioctl under CONFIG_COMPAT. */
	FINDING("F6", "FOPS compat: no .compat_ioctl on either macro; 32-bit userspace ioctls fail with ENOTTY (exynos wires drm_compat_ioctl under CONFIG_COMPAT)");
}

int main(void)
{
	check_layout();
	check_table();
	check_directed();
	check_fuzz();
	check_wait_shape();
	check_out_sync_compat();
	if (findings)
		printf("abifuzz: %d FINDING(S), %d FAIL(S)\n", findings, fails);
	else if (fails)
		printf("abifuzz: no findings, %d FAIL(S)\n", fails);
	else
		printf("abifuzz: all checks passed, no findings\n");
	return (fails || findings) ? 1 : 0;
}
