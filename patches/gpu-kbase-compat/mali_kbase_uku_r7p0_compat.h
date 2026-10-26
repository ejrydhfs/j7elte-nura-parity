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
 * @dst: destination 64-byte r15p0 atom
 * @src: source r7p0 atom (only @stride bytes are valid)
 * @stride: source stride, 48 or 56
 * @submit_gles_ctx_handle: handle from the r7p0 submit header, used when the
 *                          atom itself has no systrace gles_ctx_handle
 */
static inline void kbase_jd_atom_v2_from_r7p0(struct base_jd_atom_v2 *dst,
		const struct base_jd_atom_v2_r7p0 *src, u32 stride,
		u32 submit_gles_ctx_handle)
{
	memset(dst, 0, sizeof(*dst));
	dst->jc = src->jc;
	dst->udata = src->udata;
	dst->extres_list = src->extres_list;
	dst->nr_extres = src->nr_extres;
	/* UK < 10.3 uses compat_core_req; kbase_jd_submit() copies it over. */
	dst->compat_core_req = src->core_req;
	dst->pre_dep[0] = src->pre_dep[0];
	dst->pre_dep[1] = src->pre_dep[1];
	dst->atom_number = src->atom_number;
	dst->prio = src->prio;
	dst->device_nr = src->device_nr;
	dst->core_req = src->core_req;

	if (stride >= sizeof(*src))
		dst->gles_ctx_handle = src->gles_ctx_handle;
	else
		dst->gles_ctx_handle = submit_gles_ctx_handle;

	/* r7p0 has no frame_number/surfacep; both stay zero. */
}

/**
 * kbase_r7p0_job_submit_translate - translate the r7p0 submit header
 * @dst: destination r15p0 submit structure
 * @src: 32-byte r7p0 submit structure
 *
 * Return: true if the call can be translated safely.
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
	dst->addr = src->addr;
	dst->nr_atoms = src->nr_atoms;
	/* Keep the source stride: kbase_jd_submit() walks the user array. */
	dst->stride = src->stride;
	dst->gles_ctx_handle = src->gles_ctx_handle;
	dst->frame_number = 0;
	dst->surfacep = NULL;

	return true;
}

#endif /* _MALI_KBASE_UKU_R7P0_COMPAT_H_ */
