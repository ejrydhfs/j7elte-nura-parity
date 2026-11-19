/*
 *
 * Copyright (C) 2016 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/**
 * @file mali_kbase_uku_r7p0_compat.h
 *
 * Narrow wire-compatibility shim for the r7p0 / UK 10.0 userspace UAPI.
 *
 * The blob shipped by the ROM (Midgard r7p0-03rel0, UK 10.0) cannot talk to
 * the r15p0 / UK 10.6 kernel UAPI as-is: the job submit call and the atom
 * layout changed, and the Samsung vendor function ids moved by four because
 * UK 10.4/10.5 inserted SOFT_EVENT_UPDATE, MEM_JIT_INIT and TLSTREAM_ACQUIRE.
 *
 * Only the pieces that can be translated without losing information are
 * translated:
 *
 *  - the 32-byte r7p0 kbase_uk_job_submit into the 40-byte r15p0 one;
 *  - the 48-byte (no systrace) and 56-byte (CONFIG_MALI_SYSTRACE_SUPPORT)
 *    r7p0 base_jd_atom_v2 into the 64-byte r15p0 one;
 *  - the r7p0 vendor ids UK_FUNC_ID+37..+44 into the r15p0 ids +41..+48.
 *
 * Everything else is rejected by the dispatch path. In particular the r7p0
 * SOFT_DVFS soft job has no r15p0 equivalent (type 5 became SOFT_EVENT_WAIT)
 * and is therefore refused, and KBASE_FUNC_EXT_BUFFER_LOCK was removed in
 * UK 10.4 and is refused.
 */

#ifndef _MALI_KBASE_UKU_R7P0_COMPAT_H_
#define _MALI_KBASE_UKU_R7P0_COMPAT_H_

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include <mali_uk.h>
#include <mali_base_kernel.h>

/* sizeof(struct kbase_uk_job_submit) in t72x/r7p0 (UK 10.0). */
#define KBASE_UK_R7P0_JOB_SUBMIT_SIZE		32
/* sizeof(base_jd_atom_v2) in t72x/r7p0, without/with systrace support. */
#define KBASE_UK_R7P0_ATOM_V2_SIZE		48
#define KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE	56
/* First/last Samsung vendor function id and the r15p0 id shift. */
#define KBASE_UK_R7P0_VENDOR_FIRST		(UK_FUNC_ID + 37)
#define KBASE_UK_R7P0_VENDOR_LAST		(UK_FUNC_ID + 44)
#define KBASE_UK_R7P0_VENDOR_SHIFT		4
/* KBASE_FUNC_EXT_BUFFER_LOCK (UK_FUNC_ID + 17), removed in UK 10.4. */
#define KBASE_UK_R7P0_EXT_BUFFER_LOCK		(UK_FUNC_ID + 17)

/* t72x/r7p0/mali_kbase_uku.h: struct kbase_uk_job_submit (32 bytes). */
struct kbase_uk_job_submit_r7p0 {
	union uk_header header;
	union kbase_pointer addr;
	u32 nr_atoms;
	u32 stride;
	u32 gles_ctx_handle;
};

/*
 * t72x/r7p0/mali_base_kernel.h: base_jd_atom_v2 (56 bytes with
 * CONFIG_MALI_SYSTRACE_SUPPORT, 48 bytes without). core_req is a u16 at
 * offset 34; UK 10.3 moved it to a u32 at offset 44 in the 64-byte r15p0
 * structure.
 */
struct base_jd_atom_v2_r7p0 {
	u64 jc;
	struct base_jd_udata udata;
	union kbase_pointer extres_list;
	u16 nr_extres;
	u16 core_req;
	struct base_dependency pre_dep[2];
	base_atom_id atom_number;
	base_jd_prio prio;
	u8 device_nr;
	u8 padding[5];
	u32 gles_ctx_handle;
};

/* Wire-layout self checks; a mismatch would make the shim unsafe. */
typedef char kbase_r7p0_submit_size_check[
	(sizeof(struct kbase_uk_job_submit_r7p0) ==
		KBASE_UK_R7P0_JOB_SUBMIT_SIZE) ? 1 : -1];
typedef char kbase_r7p0_atom_size_check[
	(sizeof(struct base_jd_atom_v2_r7p0) ==
		KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE) ? 1 : -1];
typedef char kbase_r7p0_atom_core_req_offset_check[
	(offsetof(struct base_jd_atom_v2_r7p0, core_req) == 34) ? 1 : -1];
typedef char kbase_r7p0_atom_pre_dep_offset_check[
	(offsetof(struct base_jd_atom_v2_r7p0, pre_dep) == 36) ? 1 : -1];
typedef char kbase_r7p0_atom_gles_offset_check[
	(offsetof(struct base_jd_atom_v2_r7p0, gles_ctx_handle) == 48) ? 1 : -1];

/**
 * kbase_r7p0_soft_job_translatable - check an r7p0 core_req soft job type
 * @core_req: r7p0 base_jd_core_req (u16)
 *
 * r7p0 soft job types 1..4 have the same meaning in r15p0. Type 5 was
 * SOFT_DVFS in r7p0 but is SOFT_EVENT_WAIT in r15p0, so it must not be
 * translated (the kernel would read the wrong payload and misbehave).
 */
static inline bool kbase_r7p0_soft_job_translatable(u16 core_req)
{
	if (!(core_req & BASE_JD_REQ_SOFT_JOB))
		return true;

	switch (core_req & BASE_JD_REQ_SOFT_JOB_TYPE) {
	case BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME:
	case BASE_JD_REQ_SOFT_FENCE_TRIGGER:
	case BASE_JD_REQ_SOFT_FENCE_WAIT:
	case BASE_JD_REQ_SOFT_REPLAY:
		return true;
	default:
		return false;
	}
}

/**
 * kbase_jd_atom_v2_from_r7p0 - translate one r7p0 atom into the r15p0 layout
 * @dst: destination 64-byte r15p0 atom (fully written on success)
 * @src: source r7p0 atom; only the first @stride bytes are valid
 * @stride: source stride, 48 (no systrace) or 56 (systrace gles handle)
 * @submit_gles_ctx_handle: handle from the r7p0 submit header, used when the
 *                          atom itself has no systrace gles_ctx_handle
 *
 * Return: true if the atom can be translated safely, false if the core_req
 * carries a soft job type with no r15p0 equivalent (r7p0 SOFT_DVFS, type 5,
 * which collides with r15p0 SOFT_EVENT_WAIT).  The caller must translate the
 * whole batch first and refuse the call when any atom fails; all atoms of a
 * translated call run in one native call with stride 64.
 */
static inline bool kbase_jd_atom_v2_from_r7p0(struct base_jd_atom_v2 *dst,
		const struct base_jd_atom_v2_r7p0 *src, u32 stride,
		u32 submit_gles_ctx_handle)
{
	const unsigned char *raw = (const unsigned char *)src;
	u16 core_req;

	if (stride != KBASE_UK_R7P0_ATOM_V2_SIZE &&
	    stride != KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE)
		return false;

	memcpy(&core_req, raw + offsetof(struct base_jd_atom_v2_r7p0,
		core_req), sizeof(core_req));
	if (!kbase_r7p0_soft_job_translatable(core_req))
		return false;

	memset(dst, 0, sizeof(*dst));
	/* Raw-offset copies bounded by the validated stride: the 48-byte
	 * (no-systrace) form has no bytes at 48..55, so nothing above offset
	 * 48 may be read when stride == 48.  All fields below offset 48 exist
	 * in both variants. */
	memcpy(&dst->jc, raw + offsetof(struct base_jd_atom_v2_r7p0, jc),
		sizeof(dst->jc));
	memcpy(&dst->udata, raw + offsetof(struct base_jd_atom_v2_r7p0, udata),
		sizeof(dst->udata));
	memcpy(&dst->extres_list,
		raw + offsetof(struct base_jd_atom_v2_r7p0, extres_list),
		sizeof(dst->extres_list));
	memcpy(&dst->nr_extres,
		raw + offsetof(struct base_jd_atom_v2_r7p0, nr_extres),
		sizeof(dst->nr_extres));
	/* UK < 10.3 uses compat_core_req; kbase_jd_submit() copies it over. */
	dst->compat_core_req = core_req;
	memcpy(&dst->pre_dep[0],
		raw + offsetof(struct base_jd_atom_v2_r7p0, pre_dep),
		sizeof(dst->pre_dep));
	memcpy(&dst->atom_number,
		raw + offsetof(struct base_jd_atom_v2_r7p0, atom_number),
		sizeof(dst->atom_number));
	memcpy(&dst->prio, raw + offsetof(struct base_jd_atom_v2_r7p0, prio),
		sizeof(dst->prio));
	memcpy(&dst->device_nr,
		raw + offsetof(struct base_jd_atom_v2_r7p0, device_nr),
		sizeof(dst->device_nr));
	dst->core_req = core_req;

	if (stride >= sizeof(*src))
		memcpy(&dst->gles_ctx_handle,
			raw + offsetof(struct base_jd_atom_v2_r7p0,
				gles_ctx_handle),
			sizeof(dst->gles_ctx_handle));
	else
		dst->gles_ctx_handle = submit_gles_ctx_handle;

	/* r7p0 has no frame_number/surfacep; both stay zero. */
	return true;
}

/**
 * kbase_r7p0_atom_batch_translate - expand a whole r7p0 atom array to 64-byte
 * @dst: caller-provided output array of at least @nr_atoms 64-byte atoms
 * @src: source user bytes; @stride bytes per atom are valid
 * @stride: 48 (no systrace) or 56 (systrace gles handle)
 * @nr_atoms: atom count (must be <= BASE_JD_ATOM_COUNT)
 * @submit_gles_ctx_handle: header-level gles handle for 48-byte atoms
 *
 * Translates every atom with kbase_jd_atom_v2_from_r7p0(), walking the source
 * @stride bytes at a time and writing 64-byte atoms.  Returns true only when
 * every atom translated; on the first failure the output is left partial and
 * the caller must refuse the whole submit (never issue a half-translated
 * batch).  nr_atoms == 0 is a no-op success.
 */
static inline bool kbase_r7p0_atom_batch_translate(
		struct base_jd_atom_v2 *dst, const void *src, u32 stride,
		u32 nr_atoms, u32 submit_gles_ctx_handle)
{
	u32 i;

	if (stride != KBASE_UK_R7P0_ATOM_V2_SIZE &&
	    stride != KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE)
		return false;
	if (nr_atoms > BASE_JD_ATOM_COUNT)
		return false;

	for (i = 0; i < nr_atoms; i++) {
		const struct base_jd_atom_v2_r7p0 *s =
			(const struct base_jd_atom_v2_r7p0 *)
			((const unsigned char *)src + (size_t)i * stride);
		if (!kbase_jd_atom_v2_from_r7p0(&dst[i], s, stride,
				submit_gles_ctx_handle))
			return false;
	}
	return true;
}

/**
 * kbase_r7p0_job_submit_translate - translate the r7p0 submit header
 * @dst: destination r15p0 submit structure
 * @src: 32-byte r7p0 submit structure
 *
 * Return: true if the call can be translated safely.
 *
 * On success dst->stride is always sizeof(struct base_jd_atom_v2) = 64, never
 * the 48/56 source stride: mali_kbase_jd.c:kbase_jd_submit() rejects any other
 * stride (-EINVAL).  The caller MUST expand every source atom to a 64-byte
 * base_jd_atom_v2 (kbase_r7p0_atom_batch_translate()) and point dst->addr at
 * that array before issuing the native call.  Reusing src->addr with the old
 * stride would misalign the kernel's stride-wise walk.  frame_number/surfacep
 * are zero (r7p0 has no such fields).
 */
static inline bool kbase_r7p0_job_submit_translate(
		struct kbase_uk_job_submit *dst,
		const struct kbase_uk_job_submit_r7p0 *src)
{
	if (src->stride != KBASE_UK_R7P0_ATOM_V2_SIZE &&
	    src->stride != KBASE_UK_R7P0_ATOM_V2_SIZE_SYSTRACE)
		return false;

	/* atom_number is 8 bits and indexes BASE_JD_ATOM_COUNT slots. */
	if (src->nr_atoms > BASE_JD_ATOM_COUNT)
		return false;

	memset(dst, 0, sizeof(*dst));
	dst->header = src->header;
	dst->nr_atoms = src->nr_atoms;
	dst->stride = sizeof(struct base_jd_atom_v2);
	dst->gles_ctx_handle = src->gles_ctx_handle;
	dst->frame_number = 0;
	dst->surfacep = NULL;

	/* NOTE: dst->addr is deliberately left as the caller found it (zero).
	 * The translation shim owns no kernel address space; it cannot repoint
	 * the user array.  The dispatch-site glue (see docs/reports/gpu31-shim.md
	 * section 2) allocates a 64-byte array, runs
	 * kbase_r7p0_atom_batch_translate() into it, and sets dst->addr to the
	 * translated array before calling kbase_jd_submit().  Copying src->addr
	 * here would invite submitting the r7p0 array at stride 64, which the
	 * kernel walks atom-by-atom and would read out of bounds. */
	return true;
}

#endif /* _MALI_KBASE_UKU_R7P0_COMPAT_H_ */
