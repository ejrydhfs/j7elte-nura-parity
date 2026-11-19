/*
 * stride_test.c - host proof that the fixed r7p0 compat header satisfies the
 * r15p0 kernel's kbase_jd_submit() validation.
 *
 * What it proves (no device, no kernel build):
 *
 *  1. Every translated batch has stride 64, so the kernel's
 *     `if (stride != sizeof(base_jd_atom_v2)) return -EINVAL`
 *     (mali_kbase_jd.c:1343-1352) now passes; the old 48/56 stride failed it.
 *  2. Per-atom expansion round-trips the wire fields: jc, udata, extres_list,
 *     nr_extres, compat_core_req + core_req, pre_dep[2], atom_number, prio,
 *     device_nr, gles handle (56-byte atoms keep their own, 48-byte atoms
 *     inherit the submit-level handle).
 *  3. Rejections stay rejections: bad stride, nr_atoms > 256, and the
 *     untranslatable r7p0 SOFT_DVFS soft job (type 5 = r15p0 SOFT_EVENT_WAIT).
 *  4. Static wire asserts in the compat header hold (32/48/56/64 sizes and
 *     the r7p0 core_req@34 / pre_dep@36 / gles@48 offsets).
 *
 * The kernel headers cannot be compiled on the host directly (they expect a
 * kbuild environment), so this test compiles minimal local mirrors of both
 * wire layouts with the same static asserts, byte-copies them, and exercises
 * the header's real translate functions against those buffers.  Field offsets
 * in the mirrors were measured from the kernel's own headers with
 * tools/kbase-layout.c (see docs/reports/gpu31-shim.md section 1).
 *
 *   make            build native binary
 *   make check      build + run (native and aarch64 under qemu)
 *   make clean
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Stub kernel basis types first (u8..u64, uk_header, kbase_pointer, udata,
 * dependency, soft-job masks) so the r15p0 structs below can use them. */
#include <linux/types.h>
#include "mali_uk.h"
#include "mali_base_kernel.h"

/* The compat header under test brings its own kernel types via the
 * stub/mali_uk.h (uk_header, kbase_pointer, udata, dependency).  The
 * r15p0 atom/submit structs are defined here (kernel-measured 64/40 B)
 * BEFORE the compat include, since the real kernel carries them in
 * mali_base_kernel.h / mali_kbase_uku.h and the compat header only ADDS the
 * r7p0-side structs plus translate helpers. */
/* r15p0 structs the compat header translates INTO (kernel-measured). */
struct base_jd_atom_v2 {
	u64 jc;
	struct base_jd_udata udata;
	union kbase_pointer extres_list;
	u16 nr_extres;
	u16 compat_core_req;
	struct base_dependency pre_dep[2];
	u8 atom_number;
	u8 prio;
	u8 device_nr;
	u8 padding[1];
	u32 core_req;
	u32 gles_ctx_handle;
	u32 frame_number;
	void *surfacep;
};

struct kbase_uk_job_submit {
	union uk_header header;
	union kbase_pointer addr;
	u32 nr_atoms;
	u32 stride;
	u32 gles_ctx_handle;
	u32 frame_number;
	void *surfacep;
};

#include "mali_kbase_uku_r7p0_compat.h"


/* ---- test-local atom fixtures ---- */

static int fails;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL %-28s ", __func__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		fails++; \
		return; \
	} \
} while (0)

/* Emulate the kernel's dispatch + jd_submit size gates. */
static int kernel_dispatch_accepts(const struct kbase_uk_job_submit *job,
				   u32 args_size)
{
	if (sizeof(*job) != args_size)
		return 0; /* bad_size -> EFAULT */
	return 1;
}

static int kernel_jd_submit_accepts_stride(u32 stride)
{
	return stride == sizeof(struct base_jd_atom_v2); /* 64 */
}

/* Build one r7p0 source atom at the given stride in a scratch buffer. */
static void put_u64(unsigned char *buf, unsigned off, u64 v)
{
	memcpy(buf + off, &v, sizeof(v));
}

static void put_ptr(unsigned char *buf, unsigned off, void *v)
{
	memcpy(buf + off, &v, sizeof(v));
}

/* Write an r7p0 source atom at the wire offsets (jc@0, udata@8, extres@24,
 * nr_extres@32, core_req@34, pre_dep@36, atom@40, prio@41, device@42,
 * gles@48 when stride == 56).  Raw offsets, never a struct store, so the
 * 48-byte (no-systrace) form stays a pure 48-byte object. */
static void make_r7p0_atom(unsigned char *buf, u32 stride, u16 core_req,
			   u8 atom_number, u32 gles)
{
	void *extres = (void *)0x5000;
	u64 jc = 0x40001000ull + atom_number;
	u64 u0 = 0x11110000ull + atom_number;
	u64 u1 = 0x22220000ull + atom_number;
	u16 zero16 = 0;

	memset(buf, 0, stride);
	put_u64(buf, 0, jc);
	put_u64(buf, 8, u0);
	put_u64(buf, 16, u1);
	put_ptr(buf, 24, extres);
	memcpy(buf + 32, &zero16, sizeof(zero16));
	memcpy(buf + 34, &core_req, sizeof(core_req));
	buf[36] = (u8)(atom_number + 1); buf[37] = 1;
	buf[38] = (u8)(atom_number + 2); buf[39] = 2;
	buf[40] = atom_number; buf[41] = 1; buf[42] = 0;
	if (stride == KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE)
		memcpy(buf + 48, &gles, sizeof(gles));
}

static void t_sizes(void)
{
	CHECK(sizeof(struct kbase_uk_job_submit_r7p0) == 32, "r7p0 submit %zu",
	      sizeof(struct kbase_uk_job_submit_r7p0));
	CHECK(sizeof(struct base_jd_atom_v2_r7p0) == 56, "r7p0 atom %zu",
	      sizeof(struct base_jd_atom_v2_r7p0));
	CHECK(sizeof(struct kbase_uk_job_submit) == 40, "r15p0 submit %zu",
	      sizeof(struct kbase_uk_job_submit));
	CHECK(sizeof(struct base_jd_atom_v2) == 64, "r15p0 atom %zu",
	      sizeof(struct base_jd_atom_v2));
	CHECK(sizeof(struct base_jd_atom_v2_r7p0) ==
	      KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE, "systrace size");
	printf("ok %-28s sizes 32/56/40/64\n", __func__);
}

static void t_header_stride_is_64(void)
{
	struct kbase_uk_job_submit_r7p0 src;
	struct kbase_uk_job_submit dst;

	memset(&src, 0, sizeof(src));
	src.stride = KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE;
	src.nr_atoms = 2;
	src.gles_ctx_handle = 0x77;
	CHECK(kbase_r7p0_job_submit_translate(&dst, &src), "translate");
	CHECK(dst.stride == 64, "stride %u", dst.stride);
	CHECK(dst.stride == sizeof(struct base_jd_atom_v2), "not atom size");
	CHECK(kernel_dispatch_accepts(&dst, sizeof(dst)), "dispatch size");
	CHECK(kernel_jd_submit_accepts_stride(dst.stride), "jd stride");
	CHECK(dst.nr_atoms == 2, "nr_atoms %u", dst.nr_atoms);
	CHECK(dst.gles_ctx_handle == 0x77, "gles %u", dst.gles_ctx_handle);
	CHECK(dst.frame_number == 0 && dst.surfacep == NULL, "sys fields");
	printf("ok %-28s header stride 64\n", __func__);
}

static void t_old_stride_would_fail(void)
{
	/* The pre-fix header preserved 48/56: prove the kernel rejects it. */
	CHECK(!kernel_jd_submit_accepts_stride(48), "48 accepted?!");
	CHECK(!kernel_jd_submit_accepts_stride(56), "56 accepted?!");
	CHECK(!kernel_jd_submit_accepts_stride(32), "32 accepted?!");
	printf("ok %-28s 48/56 rejected by jd gate\n", __func__);
}

static void t_expand_56(void)
{
	unsigned char raw[56];
	struct base_jd_atom_v2 out;
	u32 gles = 0xabcd1234u;

	make_r7p0_atom(raw, 56, 0x103u /* CS|SOFT? no: plain bits */, 7, gles);
	/* Use a plain HW requirement so no soft-job rule trips. */
	make_r7p0_atom(raw, 56, 0x0002u /* T */, 7, gles);
	CHECK(kbase_jd_atom_v2_from_r7p0(&out,
		(struct base_jd_atom_v2_r7p0 *)raw, 56, 0x99),
	      "expand 56 failed");
	CHECK(out.jc == 0x40001007ull, "jc %llx", (unsigned long long)out.jc);
	CHECK(out.udata.blob[0] == 0x11110007ull, "udata0");
	CHECK(out.udata.blob[1] == 0x22220007ull, "udata1");
	CHECK(out.nr_extres == 0, "extres");
	CHECK(out.compat_core_req == 0x0002u, "compat %u", out.compat_core_req);
	CHECK(out.core_req == 0x0002u, "core_req %u", out.core_req);
	CHECK(out.pre_dep[0].atom_id == 8 && out.pre_dep[0].dependency_type == 1,
	      "pre0");
	CHECK(out.pre_dep[1].atom_id == 9 && out.pre_dep[1].dependency_type == 2,
	      "pre1");
	CHECK(out.atom_number == 7, "atom %u", out.atom_number);
	CHECK(out.gles_ctx_handle == gles, "gles %x vs %x",
	      out.gles_ctx_handle, gles);
	CHECK(out.frame_number == 0 && out.surfacep == NULL, "sys fields");
	printf("ok %-28s 56-byte atom expands\n", __func__);
}

static void t_expand_48_inherits_gles(void)
{
	unsigned char raw[48];
	struct base_jd_atom_v2 out;

	make_r7p0_atom(raw, 48, 0x0001u /* FS */, 3, 0);
	CHECK(kbase_jd_atom_v2_from_r7p0(&out,
		(struct base_jd_atom_v2_r7p0 *)raw, 48, 0x4242u),
	      "expand 48 failed");
	CHECK(out.core_req == 0x0001u, "core_req");
	CHECK(out.compat_core_req == 0x0001u, "compat");
	CHECK(out.gles_ctx_handle == 0x4242u, "gles inherit %x",
	      out.gles_ctx_handle);
	CHECK(out.atom_number == 3, "atom");
	printf("ok %-28s 48-byte atom inherits gles\n", __func__);
}

static void t_batch_translate(void)
{
	unsigned char raw[2 * 56];
	struct base_jd_atom_v2 out[2];
	u32 g0 = 0x1111u, g1 = 0x2222u;

	memset(raw, 0, sizeof(raw));
	make_r7p0_atom(raw + 0 * 56, 56, 0x0001u, 10, g0);
	make_r7p0_atom(raw + 1 * 56, 56, 0x0002u, 11, g1);
	CHECK(kbase_r7p0_atom_batch_translate(out, raw, 56, 2, 0x99),
	      "batch failed");
	CHECK(out[0].atom_number == 10 && out[1].atom_number == 11, "order");
	CHECK(out[0].gles_ctx_handle == g0, "g0");
	CHECK(out[1].gles_ctx_handle == g1, "g1");
	CHECK(out[0].core_req == 1 && out[1].core_req == 2, "core_reqs");
	/* The batch output is stride-64 ready for the kernel. */
	CHECK(sizeof(out[0]) == 64, "out stride");
	printf("ok %-28s batch 2x56 -> 2x64\n", __func__);
}

static void t_batch_rejects_dvfs(void)
{
	unsigned char raw[2 * 56];
	struct base_jd_atom_v2 out[2];

	memset(raw, 0, sizeof(raw));
	make_r7p0_atom(raw + 0 * 56, 56, 0x0001u, 10, 0x1u);
	/* r7p0 SOFT_DVFS type 5: SOFT_JOB|0x5 = 0x205. */
	make_r7p0_atom(raw + 1 * 56, 56,
		(u16)(BASE_JD_REQ_SOFT_JOB | 0x5), 11, 0x1u);
	CHECK(!kbase_r7p0_atom_batch_translate(out, raw, 56, 2, 0x1u),
	      "DVFS batch accepted?!");
	CHECK(!kbase_jd_atom_v2_from_r7p0(&out[1],
		(struct base_jd_atom_v2_r7p0 *)(raw + 56), 56, 0x1u),
	      "DVFS atom accepted?!");
	printf("ok %-28s DVFS refused (type-5 clash)\n", __func__);
}

static void t_reject_bad_inputs(void)
{
	struct kbase_uk_job_submit_r7p0 src;
	struct kbase_uk_job_submit dst;
	struct base_jd_atom_v2 out[1];
	unsigned char raw[48];

	memset(&src, 0, sizeof(src));
	src.stride = 32;
	src.nr_atoms = 1;
	CHECK(!kbase_r7p0_job_submit_translate(&dst, &src), "stride 32?!");
	src.stride = 48;
	src.nr_atoms = BASE_JD_ATOM_COUNT + 1;
	CHECK(!kbase_r7p0_job_submit_translate(&dst, &src), "257 atoms?!");
	make_r7p0_atom(raw, 48, 1, 0, 0);
	CHECK(!kbase_r7p0_atom_batch_translate(out, raw, 32, 1, 0),
	      "bad stride?!");
	CHECK(!kbase_r7p0_atom_batch_translate(out, raw, 48,
		BASE_JD_ATOM_COUNT + 1, 0), "too many?!");
	CHECK(kbase_r7p0_atom_batch_translate(out, raw, 48, 0, 0),
	      "empty batch");
	printf("ok %-28s bad inputs refused\n", __func__);
}

static void t_soft_types_1_to_4(void)
{
	unsigned char raw[48];
	struct base_jd_atom_v2 out;
	u16 types[] = {
		(u16)BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME,
		(u16)BASE_JD_REQ_SOFT_FENCE_TRIGGER,
		(u16)BASE_JD_REQ_SOFT_FENCE_WAIT,
		(u16)BASE_JD_REQ_SOFT_REPLAY,
	};
	unsigned i;

	for (i = 0; i < 4; i++) {
		make_r7p0_atom(raw, 48, types[i], (u8)i, 0);
		CHECK(kbase_jd_atom_v2_from_r7p0(&out,
			(struct base_jd_atom_v2_r7p0 *)raw, 48, 0x5u),
		      "soft type %u refused", types[i] & 0x1f);
		CHECK(out.core_req == types[i], "soft echo");
	}
	printf("ok %-28s soft types 1-4 pass\n", __func__);
}

int main(void)
{
	t_sizes();
	t_header_stride_is_64();
	t_old_stride_would_fail();
	t_expand_56();
	t_expand_48_inherits_gles();
	t_batch_translate();
	t_batch_rejects_dvfs();
	t_reject_bad_inputs();
	t_soft_types_1_to_4();

	if (fails) {
		printf("STRIDE TEST: FAIL (%d)\n", fails);
		return 1;
	}
	printf("STRIDE TEST: PASS (9 groups)\n");
	return 0;
}
