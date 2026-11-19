/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pf7580_mock_gpu.h - deterministic host model of the T720 job-slot +
 * MMU register block for the fault-injection matrix.
 *
 * Models exactly the registers the driver fault path touches:
 *  - JOB_INT_{RAWSTAT,CLEAR,MASK,STAT} (jm_hw.h:37-40)
 *  - JS slot NEXT/COMMAND/STATUS views (jm_hw.h:45-63,71-74)
 *  - GPU_CMD / GPU_INT_RESET_COMPLETED (jm_hw.h:87-94)
 *  - MMU AS block (AS_TRANSTAB/MEMATTR/LOCKADDR/COMMAND/FAULDADDR/
 *    FAULTSTATUS/STATUS/TRANSCFG) with the iommu header's offsets
 *    (panfrost7580_mmu.h:45-59) and command codes (:61-65)
 *
 * Time is virtual (clock_ms): dispatch latency, the 5 s job timeout
 * (P7580_JOB_TIMEOUT_MS) and the 3 s reset bound
 * (P7580_RESET_TIMEOUT_MS) are stepped explicitly by the matrix, so the
 * full timeout matrix runs in milliseconds of host CPU.
 *
 * C89-clean (matches the jobs slice -std=gnu89 style).
 */

#ifndef PF7580_MOCK_GPU_H
#define PF7580_MOCK_GPU_H

#include "pf7580_fault.h"

/* Register file covers GPU_CTRL (0x0020) through the AS block. */
#define MGPU_REG_BYTES	0x3000u
#define MGPU_REG_WORDS	(MGPU_REG_BYTES / 4u)

#define MGPU_MAX_MAPS	16
#define MGPU_MAX_BOS	64

/* MMU AS block (values == P7580_MMU_AS_* in iommu/panfrost7580_mmu.h). */
#define MGPU_AS_BASE		0x2400u
#define MGPU_AS_STRIDE		0x40u
#define MGPU_AS_TRANSTAB_LO	0x00u
#define MGPU_AS_TRANSTAB_HI	0x04u
#define MGPU_AS_MEMATTR_LO	0x08u
#define MGPU_AS_MEMATTR_HI	0x0Cu
#define MGPU_AS_LOCKADDR_LO	0x10u
#define MGPU_AS_LOCKADDR_HI	0x14u
#define MGPU_AS_COMMAND		0x18u
#define MGPU_AS_FAULTSTATUS	0x1Cu
#define MGPU_AS_FAULTADDR_LO	0x20u
#define MGPU_AS_FAULTADDR_HI	0x24u
#define MGPU_AS_STATUS		0x28u
#define MGPU_AS_TRANSCFG_LO	0x30u
#define MGPU_AS_CMD_UPDATE	0x01u
#define MGPU_AS_CMD_LOCK	0x02u
#define MGPU_AS_CMD_UNLOCK	0x03u
#define MGPU_AS_CMD_FLUSH_PT	0x04u
#define MGPU_AS_CMD_FLUSH_MEM	0x05u

/* MMU IRQ pairing (mmu/pf7580_as.h:236-239). */
#define MGPU_MMU_PF(n)	(1u << (n))
#define MGPU_MMU_BE(n)	(1u << ((n) + 16))

/* LPAE MMU fault-status words (single decoder, F-MMU4; pf7580_as.h
 * view). The retired short-code view (bits 5:3) is gone: the TRANS/
 * PERM/BUS aliases below now name the same LPAE exception bytes with
 * the access-type bits ORed in (READ/WRITE), matching what the IRQ
 * handler latches from faulting slots. */
#define MGPU_MMUS_LPAE_TRANS	0xC3u
#define MGPU_MMUS_LPAE_PERM	0xC8u
#define MGPU_MMUS_LPAE_BUS	0xD3u
#define MGPU_MMUS_LPAE_ACCESS	0xD8u
#define MGPU_MMUS_TRANS_READ	(MGPU_MMUS_LPAE_TRANS | (0x2u << 8))
#define MGPU_MMUS_PERM_WRITE	(MGPU_MMUS_LPAE_PERM | (0x3u << 8))
#define MGPU_MMUS_BUS_READ	(MGPU_MMUS_LPAE_BUS | (0x2u << 8))

/* -EBUSY/-ENOENT/-EINVAL without <linux/errno.h> on the host. */
#define MGPU_EBUSY	16
#define MGPU_ENOENT	2
#define MGPU_EINVAL	22
#define MGPU_EIO	5
#define MGPU_ETIMEDOUT	110

struct mgpu_map {
	int used;
	uint64_t iova;
	uint32_t size;
	unsigned int prot;
};

struct mgpu_bo {
	int used;
	uint32_t handle;
	uint64_t iova;
	uint32_t size;
	unsigned int prot;
	int busy;	/* in-flight job references */
	int map_idx;	/* index into maps[], -1 when unmapped */
};

struct mgpu_fence {
	int signaled;
	int err;	/* negative errno delivered at signal time */
};

struct mgpu_job {
	int active;
	int slot;
	uint64_t jc;
	int bo;		/* index into bos[], -1 for none */
	uint64_t dispatched_at;
	long complete_at;	/* -1 = hang, never completes */
	int will_fault;
	uint32_t js_code;	/* JS_STATUS latched on fault completion */
	uint32_t mmu_status;	/* AS_FAULTSTATUS latched on fault completion */
	struct mgpu_fence fence;
};

struct mgpu {
	uint32_t regs[MGPU_REG_WORDS];
	uint64_t clock_ms;
	struct mgpu_map maps[MGPU_MAX_MAPS];
	struct mgpu_bo bos[MGPU_MAX_BOS];
	struct mgpu_job jobs[P7580_JM_MAX_SLOTS];
	uint32_t next_handle;
	uint32_t as_pgd_lo;	/* expected TRANSTAB_LO (pgd | flags) */
	uint32_t as_pgd_hi;
	/* Test knobs. */
	int hang_slot[P7580_JM_MAX_SLOTS];	/* submit on slot never IRQs */
	long done_delay_ms;		/* dispatch->DONE latency, default 2 */
	int force_fault[P7580_JM_MAX_SLOTS];	/* -1 none, else JS_STATUS code */
	int stop_honor[P7580_JM_MAX_SLOTS];	/* HARD_STOP drains slot? */
	int soft_ok;			/* SOFT_RESET raises COMPLETED? */
	int bypass_validate;		/* let misaligned jc reach the HW model */
	/* Observations for the matrix. */
	int reset_kind;			/* enum pf7580_reset_kind */
	int gpu_cmd_log;		/* bit0 SOFT seen, bit1 HARD seen */
	int recover_ret;
	int uaf_attempted;
};

void mgpu_init(struct mgpu *m);
int mgpu_map(struct mgpu *m, uint64_t iova, uint32_t size,
	     unsigned int prot);
int mgpu_unmap(struct mgpu *m, uint64_t iova);
int mgpu_bo_create(struct mgpu *m, uint32_t size, unsigned int prot,
		   uint32_t *handle, uint64_t *iova);
int mgpu_bo_free(struct mgpu *m, uint32_t handle);
/* Submit a job; bo handle -1 means no BO. Returns 0 or -MGPU_*. */
int mgpu_submit(struct mgpu *m, int slot, uint64_t jc, int bo_handle);
void mgpu_step_ms(struct mgpu *m, uint64_t ms);
/* Driver-shaped IRQ demux: CLEAR sticky bits, NOP on ERR, signal DONE. */
void mgpu_isr_demux(struct mgpu *m);
/* 1 when the job's fence is signaled (spurious-timeout guard shape). */
int mgpu_fence_signaled(struct mgpu *m, int slot);
/* Driver-shaped recovery; fills reset_kind/recover_ret observations. */
int mgpu_recover(struct mgpu *m, int fault_slot, uint32_t status);
int mgpu_js_status(struct mgpu *m, int slot);
uint32_t mgpu_reg_read(struct mgpu *m, uint32_t off);
void mgpu_reg_write(struct mgpu *m, uint32_t off, uint32_t val);
int mgpu_mapped(struct mgpu *m, uint64_t addr);

#endif /* PF7580_MOCK_GPU_H */
