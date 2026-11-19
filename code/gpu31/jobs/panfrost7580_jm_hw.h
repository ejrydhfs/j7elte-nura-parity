/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_jm_hw.h - Midgard job-manager hardware interface for the
 * T720 (Exynos 7580, j7elte) panfrost port. Pure-C (host-testable): only
 * fixed-width types, no kernel or shim dependencies.
 *
 * Provenance (all values cross-checked, kbase == mainline panfrost):
 *  [KT] Nura 3.10.108 drivers/gpu/arm/t7xx/r15p0/mali_midg_regmap.h
 *  [ML] mainline v5.2 drivers/gpu/drm/panfrost/panfrost_regs.h
 *  [KB] Nura 3.10.108 backend/gpu/mali_kbase_jm_hw.c (submit sequence)
 *  [V5] mainline v5.2 drivers/gpu/drm/panfrost/panfrost_job.c (submit seq)
 *
 * T720 specifics baked in here (from KT hwconfig tables + live device):
 *  - 3 job slots (gpu-verify: js_present 0x7; BASE_JM_MAX_NR_SLOTS 3)
 *  - BASE_HW_FEATURE list for t72x has NO FLUSH_REDUCTION, NO
 *    JOBCHAIN_DISAMBIGUATION, NO PROTECTED_MODE (features.h:72-84)
 *  - BASE_HW_ISSUE list for t72x HAS 10649 (issues.h:566-581), so
 *    JS_CONFIG_START_MMU is always set (same as [KB]:118 and [V5])
 */
#ifndef PANFROST7580_JM_HW_H
#define PANFROST7580_JM_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* 3 slots on Midgard; T720 MP2 reports js_present == 0x7 (gpu-verify). */
#define P7580_JM_MAX_SLOTS	3u

/* ------------------------------------------------------------------ */
/* Job control block. [KT] JOB_CONTROL_BASE 0x1000 (:199), JOB_SLOT0   */
/* 0x800 (:210), stride 0x80 (JOB_SLOT_REG :227); [ML] JS_BASE 0x1800,  */
/* JS_SLOT_STRIDE 0x80. Both give slot n base = 0x1800 + n*0x80.        */
/* ------------------------------------------------------------------ */
#define P7580_JOB_INT_RAWSTAT	0x1000u
#define P7580_JOB_INT_CLEAR	0x1004u
#define P7580_JOB_INT_MASK	0x1008u
#define P7580_JOB_INT_STAT	0x100Cu

#define P7580_JS_BASE		0x1800u
#define P7580_JS_STRIDE		0x0080u

/* Current (RO) view. */
#define P7580_JS_HEAD_LO	0x00u
#define P7580_JS_HEAD_HI	0x04u
#define P7580_JS_TAIL_LO	0x08u
#define P7580_JS_TAIL_HI	0x0Cu
#define P7580_JS_COMMAND	0x20u
#define P7580_JS_STATUS		0x24u

/* NEXT (RW) view: what the driver programs before START. */
#define P7580_JS_HEAD_NEXT_LO	0x40u
#define P7580_JS_HEAD_NEXT_HI	0x44u
#define P7580_JS_AFF_NEXT_LO	0x50u
#define P7580_JS_AFF_NEXT_HI	0x54u
#define P7580_JS_CONFIG_NEXT	0x58u
#define P7580_JS_COMMAND_NEXT	0x60u
#define P7580_JS_FLUSH_ID_NEXT	0x70u

#define P7580_JS_REG(n, r) \
	(P7580_JS_BASE + ((uint32_t)(n) * P7580_JS_STRIDE) + (r))

/* IRQ demux. [ML] MK_JS_MASK / JOB_INT_MASK_{DONE,ERR}. */
#define P7580_MK_JS_MASK(j)	(0x10001u << (j))
#define P7580_JOB_DONE_BIT(j)	(1u << (j))
#define P7580_JOB_ERR_BIT(j)	(1u << ((j) + 16))

/* JS_COMMAND values. [KT]:385-392, [ML] identical. */
#define P7580_JS_CMD_NOP	0x00u
#define P7580_JS_CMD_START	0x01u
#define P7580_JS_CMD_SOFT_STOP	0x02u
#define P7580_JS_CMD_HARD_STOP	0x03u

/* JS_CONFIG bits. [KT]:408-418, [ML] identical. */
#define P7580_JS_CFG_START_FLUSH_CLEAN		(1u << 8)
#define P7580_JS_CFG_START_FLUSH_CLEAN_INV	(3u << 8)
#define P7580_JS_CFG_START_MMU			(1u << 10)
#define P7580_JS_CFG_JOB_CHAIN_FLAG		(1u << 11)
#define P7580_JS_CFG_END_FLUSH_CLEAN		(1u << 12)
#define P7580_JS_CFG_END_FLUSH_CLEAN_INV	(3u << 12)
#define P7580_JS_CFG_ENABLE_FLUSH_REDUCTION	(1u << 14)
#define P7580_JS_CFG_DISABLE_DESC_WR_BK		(1u << 15)
#define P7580_JS_CFG_THREAD_PRI(n)		((uint32_t)(n) << 16)

/* GPU-level reset used by fault recovery. [KT]:452-460, [ML] GPU_CMD. */
#define P7580_GPU_CMD			0x0030u
#define P7580_GPU_CMD_SOFT_RESET	0x01u
#define P7580_GPU_CMD_HARD_RESET	0x02u
#define P7580_GPU_INT_RAWSTAT		0x0020u
#define P7580_GPU_INT_CLEAR		0x0024u
#define P7580_GPU_IRQ_RESET_COMPLETED	(1u << 8)

/* ------------------------------------------------------------------ */
/* Timeouts. SHIM-5S is the drm_sched shim's hardcoded dispatch        */
/* timeout (0004 pump: msecs_to_jiffies(5000)); JOB_TIMEOUT_MS must     */
/* stay in sync with it until the one-line wiring to sched->timeout    */
/* lands (see jobs report). 3 s matches kbase's soft-job default       */
/* (mali_kbase_config_defaults.h:185) and the reset default (:210).    */
/* ------------------------------------------------------------------ */
#define P7580_JOB_TIMEOUT_MS	5000u
#define P7580_RESET_TIMEOUT_MS	3000u

/* Slot for a submit. [V5] panfrost_job_get_slot: FS -> JS0, else JS1. */
/* JS2 (compute) is wired but idle: v5.2 UAPI has no compute-only bit. */
#define P7580_SLOT_FRAGMENT	0
#define P7580_SLOT_VERTEX_TILER	1
#define P7580_SLOT_COMPUTE	2

/* Requirements bits (v5.2 UAPI, panfrost_drm.h). */
#define P7580_JD_REQ_FS		(1u << 0)
/* Kernel-internal routing only (never on the UAPI wire at v5.2). */
#define P7580_JD_REQ_ONLY_COMPUTE	(1u << 10)

static inline int p7580_job_slot(uint32_t requirements)
{
	if (requirements & P7580_JD_REQ_FS)
		return P7580_SLOT_FRAGMENT;
	if (requirements & P7580_JD_REQ_ONLY_COMPUTE)
		return P7580_SLOT_COMPUTE;
	return P7580_SLOT_VERTEX_TILER;
}

/*
 * JS_CONFIG_NEXT value for one submit on this SoC.
 *
 * Mirrors [KB] kbase_job_hw_submit (:102-131) minus the features T720
 * lacks: no FLUSH_REDUCTION, no PROTECTED_MODE write-back disable, no
 * JOBCHAIN_DISAMBIGUATION toggle (not in base_hw_features_t72x), no
 * SKIP_CACHE_* (v5.2 UAPI cannot express them). START_MMU is set
 * because t72x carries BASE_HW_ISSUE_10649 ([KB]:117-118, [V5] too).
 * as_nr occupies the low bits exactly as kbase lays it down (:102).
 */
static inline uint32_t p7580_js_config(uint32_t as_nr)
{
	return (as_nr & 0xFu) |
		P7580_JS_CFG_START_FLUSH_CLEAN_INV |
		P7580_JS_CFG_END_FLUSH_CLEAN_INV |
		P7580_JS_CFG_START_MMU |
		P7580_JS_CFG_THREAD_PRI(8);
}

/* ------------------------------------------------------------------ */
/* JS_STATUS fault decode. [KT]:418-451.                               */
/* ------------------------------------------------------------------ */
struct p7580_js_fault {
	uint32_t code;
	const char *name;
};

static const struct p7580_js_fault p7580_js_faults[] = {
	{ 0x02u, "INTERRUPTED" },
	{ 0x03u, "STOPPED" },
	{ 0x04u, "TERMINATED" },
	{ 0x40u, "CONFIG_FAULT" },
	{ 0x41u, "POWER_FAULT" },
	{ 0x42u, "READ_FAULT" },
	{ 0x43u, "WRITE_FAULT" },
	{ 0x44u, "AFFINITY_FAULT" },
	{ 0x48u, "BUS_FAULT" },
	{ 0x50u, "INSTR_INVALID_PC" },
	{ 0x51u, "INSTR_INVALID_ENC" },
	{ 0x52u, "INSTR_TYPE_MISMATCH" },
	{ 0x53u, "INSTR_OPERAND_FAULT" },
	{ 0x54u, "INSTR_TLS_FAULT" },
	{ 0x55u, "INSTR_BARRIER_FAULT" },
	{ 0x56u, "INSTR_ALIGN_FAULT" },
	{ 0x58u, "DATA_INVALID_FAULT" },
	{ 0x59u, "TILE_RANGE_FAULT" },
	{ 0x5Au, "ADDRESS_RANGE_FAULT" },
	{ 0x60u, "OUT_OF_MEMORY" },
	{ 0x7Fu, "UNKNOWN" },
};

static inline const char *p7580_js_status_name(uint32_t status)
{
	unsigned int i;

	for (i = 0; i < sizeof(p7580_js_faults) /
			sizeof(p7580_js_faults[0]); i++) {
		if (p7580_js_faults[i].code == status)
			return p7580_js_faults[i].name;
	}
	return (status < 0x40u) ? "NO_EXCEPTION" : "RESERVED_FAULT";
}

/* True for fault codes that mean the slot chain is dead and the       */
/* resume/recovery path must run (vs benign STOPPED/TERMINATED).       */
static inline int p7580_js_status_is_fault(uint32_t status)
{
	return status >= 0x40u;
}

/* ------------------------------------------------------------------ */
/* Job descriptor header (what job->jc points at). [KT]                */
/* mali_kbase_defs.h:1689-1712, "identical for all GPU archs". 32      */
/* bytes: 4 + 4 + 8 + 2 (bitfield bytes) + 2 + 2 + 2 + 8.               */
/* ------------------------------------------------------------------ */
struct p7580_job_desc {
	uint32_t exception_status;
	uint32_t first_incomplete_task;
	uint64_t fault_pointer;
	uint8_t job_descriptor_size : 1;
	uint8_t job_type : 7;
	uint8_t job_barrier : 1;
	uint8_t _reserved_01 : 1;
	uint8_t _reserved_1 : 1;
	uint8_t _reserved_02 : 1;
	uint8_t _reserved_03 : 1;
	uint8_t _reserved_2 : 1;
	uint8_t _reserved_04 : 1;
	uint8_t _reserved_05 : 1;
	uint16_t job_index;
	uint16_t job_dependency_index_1;
	uint16_t job_dependency_index_2;
	union {
		uint64_t _64;
		uint32_t _32;
	} next_job;
};

/* v5.2 SUBMIT ioctl argument, layout kept byte-identical so stock     */
/* Mesa panfrost_dri.so is the unmodified userspace (gpu31-target).    */
struct p7580_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

/* SUBMIT jc rule (v5.2: nonzero, 8-byte aligned GPU address). Shared by */
/* the GEM validate stub and p7580_job_submit_args so the two can never  */
/* disagree (F-JOB1: the jobs copy had the align test inverted). Pure-C  */
/* so the host unit test exercises the exact kernel predicate.           */
static inline int p7580_jc_valid(uint64_t jc)
{
	return jc != 0 && (jc & 7u) == 0;
}

#endif /* PANFROST7580_JM_HW_H */
