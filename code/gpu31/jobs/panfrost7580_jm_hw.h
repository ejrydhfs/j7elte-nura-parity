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

/* Slot for a submit. Matches mainline v5.2 panfrost_job_get_slot
 * (drivers/gpu/drm/panfrost/panfrost_job.c:102-122): FS -> JS0
 * (fragment); everything else -> JS1 (vertex/tiler). ONLY_COMPUTE ->
 * JS2 is kernel-internal (never on the v5.2 UAPI wire).
 *
 * Mesa 20.3 emits two submits per batch: the vertex/tiler chain with
 * requirements == 0 and the fragment job with PANFROST_JD_REQ_FS.
 * The chain (WRITE_VALUE type 2, VERTEX type 5, TILER type 7) must
 * run on JS1: on this 3-slot part a vertex/tiler chain dispatched on
 * JS0 faults (slot 0 ERR storm, fragment slot cannot run tiler work),
 * and every later submit faults behind it. Requirement-less therefore
 * means JS1 -- never JS0 (gpu31-mesa-render: 120/120 Mesa submits ERR
 * on slot 0 with req==0 routed to JS0; WRITE_VALUE alone on JS0 is
 * DONE, proving the slot, not the job, rejects the chain). NULL jobs
 * run on any slot, so the selftest pattern/submit tests are unaffected
 * by the routing (they only assert fence completion). */
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

/* ------------------------------------------------------------------ */
/* gpu31-chainsplit: chain walk + per-node shadow descriptors.        */
/*                                                                    */
/* The v5.2 UAPI submits ONE jc: the head of a linked chain. On this  */
/* part a chained submit is executed by the JM as one slot dispatch   */
/* (next pointers are transparent transfer); the chain's END_FLUSH_   */
/* CLEAN_INV therefore runs only once, after the LAST job. The        */
/* gpu31-chainexec measurement proved the tiler then does not observe */
/* the vertex shader's stores. The fix is a driver-side split: every  */
/* chain node becomes its own sched job (its own START, hence its own */
/* END_FLUSH_CLEAN_INV), serialised by drm_sched dependencies.        */
/*                                                                    */
/* A node cannot simply be dispatched with its original header: its   */
/* next pointer would make the JM keep walking the rest of the chain, */
/* and its job_index dependencies refer to nodes that are not in this */
/* dispatch. Each split node is therefore rewritten into a driver-    */
/* owned shadow descriptor: next = NULL, deps = 0, index = position+1.*/
/* The payload (inline after the 32-byte header) is copied verbatim;  */
/* all payload fields are self-contained GPU addresses, so copying is */
/* safe. Header layout [KT] mali_kbase_defs.h:1689-1712 / midgard.xml: */
/*   word 4: bit0 Is64, bits1..7 Type, bits8..15 barrier/reserved,     */
/*           bits16..31 Index                                          */
/*   word 5: dep1 low16 / dep2 high16                                  */
/*   words 6..7: next_job (32-bit form uses word 6 only)               */
/* ------------------------------------------------------------------ */
#define P7580_CHAIN_MAX_JOBS		16u
/* Shadow slot stride: 192 B is the largest Midgard job aggregate      */
/* (Midgard Tiler Job, midgard.xml), padded to one cache-line pair.   */
#define P7580_CHAIN_SLOT_STRIDE		256u
#define P7580_CHAIN_HDR_WORDS		8u
#define P7580_JOB_DESC_HDR_SIZE		32u

/* Grounded job type numerics ([KT] mali_kbase_replay.c:29-34 for       */
/* 1/5/7/8/9; 2 = WRITE_VALUE from the live chain decode).             */
#define P7580_JOB_TYPE_NULL		1u
#define P7580_JOB_TYPE_WRITE_VALUE	2u
#define P7580_JOB_TYPE_VERTEX		5u
#define P7580_JOB_TYPE_TILER		7u
#define P7580_JOB_TYPE_FUSED		8u
#define P7580_JOB_TYPE_FRAGMENT		9u

/* Total bytes (header + inline payload) a shadow slot must carry.      */
/* Null 32; Write Value 24 B payload (midgard.xml); Compute/Vertex     */
/* 96 B (midgard.xml Compute Job: Draw ends at +96); Tiler/Fused 192 B */
/* (Midgard Tiler Job size=192); Fragment 64 B (32 B header + 32 B     */
/* payload fields through Tile Enable Map Row Stride).                 */
static inline unsigned int p7580_chain_job_bytes(uint32_t type)
{
	switch (type) {
	case P7580_JOB_TYPE_NULL:
		return P7580_JOB_DESC_HDR_SIZE;
	case P7580_JOB_TYPE_WRITE_VALUE:
		return 64u;
	case P7580_JOB_TYPE_VERTEX:
		return 96u;
	case P7580_JOB_TYPE_TILER:
	case P7580_JOB_TYPE_FUSED:
		return 192u;
	case P7580_JOB_TYPE_FRAGMENT:
		return 64u;
	default:
		return 0u;
	}
}

static inline uint32_t p7580_chain_hdr_is64(const uint32_t w[8])
{
	return w[4] & 1u;
}

static inline uint32_t p7580_chain_hdr_type(const uint32_t w[8])
{
	return (w[4] >> 1) & 0x7fu;
}

static inline uint32_t p7580_chain_hdr_index(const uint32_t w[8])
{
	return w[4] >> 16;
}

/* next_job: Is64 selects the 64-bit union member (words 6+7); both     */
/* halves are cleared when isolating, so either form terminates.        */
static inline uint64_t p7580_chain_hdr_next(const uint32_t w[8])
{
	if (p7580_chain_hdr_is64(w))
		return ((uint64_t)w[7] << 32) | w[6];
	return (uint64_t)w[6];
}

static inline uint32_t p7580_chain_hdr_dep1(const uint32_t w[8])
{
	return w[5] & 0xffffu;
}

static inline uint32_t p7580_chain_hdr_dep2(const uint32_t w[8])
{
	return w[5] >> 16;
}

/* Turn one chain node into a standalone (single-job) descriptor:      */
/* clear both dependency indices and the next pointer, and renumber the */
/* index by its position in the split (1-based) so the JM never sees a */
/* reference to a job that is not in this dispatch. Type/Is64/barrier  */
/* survive.                                                            */
static inline void p7580_chain_hdr_isolate(uint32_t w[8], unsigned int pos)
{
	w[4] = (w[4] & 0x0000ffffu) | ((uint32_t)(pos + 1u) << 16);
	w[5] = 0;
	w[6] = 0;
	w[7] = 0;
}

#endif /* PANFROST7580_JM_HW_H */
