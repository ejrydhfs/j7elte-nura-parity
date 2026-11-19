/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pf7580_mock_gpu.c - deterministic host model of the T720 job-slot +
 * MMU register block for the fault-injection matrix.
 *
 * Models the registers the driver fault path touches (JOB_INT_*, JS
 * NEXT/COMMAND/STATUS, GPU_CMD/COMPLETED, MMU AS block) with virtual
 * time (clock_ms), so the 5 s job timeout and 3 s reset bound run in
 * milliseconds of host CPU.
 *
 * Behavioural contract (mirrors code/gpu31/jobs/panfrost7580_job.c):
 *  - submit programs HEAD/CONFIG/COMMAND_NEXT=START; stuck slot -> EBUSY
 *  - unmapped jc (or force_fault[]) completes as ERR with JS_STATUS +
 *    AS_FAULTSTATUS latched; COMMAND_NEXT stays programmed (stuck)
 *  - ISR demux: CLEAR sticky bits, COMMAND_NEXT=NOP on ERR, signal DONE
 *  - recover: HARD_STOP stuck slots, drain (stop_honor), SOFT then HARD
 *    reset, CLEAR + re-arm MASK (present 0x7), signal fault fence -EIO
 *  - free of a busy BO -> EBUSY (lifetime rule); unknown handle -> ENOENT
 *
 * Forced-fault MMU status derivation: 0x42 (READ_FAULT) latches
 * PERMISSION/WRITE (shader read of a RO mapping); any other forced code
 * latches TRANSLATION/READ. The unmapped-jc path latches
 * TRANSLATION/READ.
 */

#include "pf7580_mock_gpu.h"

/* MMU IRQ block (values == mmu/pf7580_as.h:73-75). */
#define MGPU_MMU_INT_CLEAR	0x2004u
#define MGPU_MMU_INT_MASK	0x2008u
#define MGPU_MMU_INT_STAT	0x200Cu

/* Expected AS0 TRANSTAB after (re-)program: pgd 0x3F000000 | TABLE(3) |
 * READ_INNER(4); hi 0. MEMATTR is the kbase LPAE table (iommu header). */
#define MGPU_PGD_LO	0x3F000007u
#define MGPU_PGD_HI	0x00000000u
#define MGPU_MEMATTR_LO	0x884D4F48u
#define MGPU_MEMATTR_HI	0x00008D88u

/* Present slots: T720 MP2 js_present 0x7 (jobs report §1). */
#define MGPU_PRESENT_MASK \
	(P7580_MK_JS_MASK(0) | P7580_MK_JS_MASK(1) | P7580_MK_JS_MASK(2))

#define MGPU_BO_BASE	0x10000000ull
#define MGPU_BO_STRIDE	0x200000ull

static uint32_t mgpu_js_reg(int slot, uint32_t off)
{
	return P7580_JS_REG(slot, off);
}

static void mgpu_as_write(struct mgpu *m, uint32_t off, uint32_t val)
{
	m->regs[(MGPU_AS_BASE + off) >> 2] = val;
}

void mgpu_init(struct mgpu *m)
{
	int i;

	for (i = 0; i < (int)MGPU_REG_WORDS; i++)
		m->regs[i] = 0;
	for (i = 0; i < (int)MGPU_MAX_MAPS; i++)
		m->maps[i].used = 0;
	for (i = 0; i < (int)MGPU_MAX_BOS; i++) {
		m->bos[i].used = 0;
		m->bos[i].busy = 0;
		m->bos[i].map_idx = -1;
	}
	for (i = 0; i < (int)P7580_JM_MAX_SLOTS; i++) {
		m->jobs[i].active = 0;
		m->jobs[i].slot = i;
		m->jobs[i].complete_at = -2;
		m->jobs[i].fence.signaled = 0;
		m->jobs[i].fence.err = 0;
		m->hang_slot[i] = 0;
		m->force_fault[i] = -1;
		m->stop_honor[i] = 1;
	}
	m->clock_ms = 0;
	m->next_handle = 1;
	m->as_pgd_lo = MGPU_PGD_LO;
	m->as_pgd_hi = MGPU_PGD_HI;
	m->done_delay_ms = 2;
	m->soft_ok = 1;
	m->bypass_validate = 0;
	m->reset_kind = PF7580_RESET_NONE;
	m->gpu_cmd_log = 0;
	m->recover_ret = 0;
	m->uaf_attempted = 0;

	/* AS0 programmed, IRQs rearmed: the steady state the driver leaves
	 * after probe / after recovery. */
	mgpu_as_write(m, MGPU_AS_TRANSTAB_LO, MGPU_PGD_LO);
	mgpu_as_write(m, MGPU_AS_TRANSTAB_HI, MGPU_PGD_HI);
	mgpu_as_write(m, MGPU_AS_MEMATTR_LO, MGPU_MEMATTR_LO);
	mgpu_as_write(m, MGPU_AS_MEMATTR_HI, MGPU_MEMATTR_HI);
	mgpu_as_write(m, MGPU_AS_TRANSCFG_LO, 0);
	m->regs[P7580_JOB_INT_MASK >> 2] = MGPU_PRESENT_MASK;
}

int mgpu_mapped(struct mgpu *m, uint64_t addr)
{
	int i;

	if (!m)
		return 0;
	for (i = 0; i < (int)MGPU_MAX_MAPS; i++) {
		if (m->maps[i].used && addr >= m->maps[i].iova &&
		    addr < m->maps[i].iova + m->maps[i].size)
			return 1;
	}
	return 0;
}

int mgpu_map(struct mgpu *m, uint64_t iova, uint32_t size,
	     unsigned int prot)
{
	int i;

	if (!m || !size || (iova & 0xFFFu))
		return -MGPU_EINVAL;
	for (i = 0; i < (int)MGPU_MAX_MAPS; i++) {
		if (!m->maps[i].used)
			continue;
		if (iova < m->maps[i].iova + m->maps[i].size &&
		    m->maps[i].iova < iova + size)
			return -MGPU_EINVAL; /* overlap */
	}
	for (i = 0; i < (int)MGPU_MAX_MAPS; i++) {
		if (m->maps[i].used)
			continue;
		m->maps[i].used = 1;
		m->maps[i].iova = iova;
		m->maps[i].size = size;
		m->maps[i].prot = prot;
		return 0;
	}
	return -MGPU_EIO; /* table full */
}

int mgpu_unmap(struct mgpu *m, uint64_t iova)
{
	int i, j;

	if (!m)
		return -MGPU_EINVAL;
	for (i = 0; i < (int)MGPU_MAX_MAPS; i++) {
		if (!m->maps[i].used || m->maps[i].iova != iova)
			continue;
		m->maps[i].used = 0;
		for (j = 0; j < (int)MGPU_MAX_BOS; j++) {
			if (m->bos[j].used && m->bos[j].map_idx == i)
				m->bos[j].map_idx = -1;
		}
		return 0;
	}
	return -MGPU_ENOENT;
}

int mgpu_bo_create(struct mgpu *m, uint32_t size, unsigned int prot,
		   uint32_t *handle, uint64_t *iova)
{
	int i, mi;
	uint64_t va;
	uint32_t h;

	if (!m || !size || !handle || !iova)
		return -MGPU_EINVAL;
	mi = -1;
	for (i = 0; i < (int)MGPU_MAX_BOS; i++) {
		if (!m->bos[i].used) {
			mi = i;
			break;
		}
	}
	if (mi < 0)
		return -MGPU_EIO;
	h = m->next_handle++;
	va = MGPU_BO_BASE + (uint64_t)(h - 1) * MGPU_BO_STRIDE;
	if (mgpu_map(m, va, size, prot))
		return -MGPU_EIO;
	m->bos[mi].used = 1;
	m->bos[mi].handle = h;
	m->bos[mi].iova = va;
	m->bos[mi].size = size;
	m->bos[mi].prot = prot;
	m->bos[mi].busy = 0;
	m->bos[mi].map_idx = -1;
	for (i = 0; i < (int)MGPU_MAX_MAPS; i++) {
		if (m->maps[i].used && m->maps[i].iova == va) {
			m->bos[mi].map_idx = i;
			break;
		}
	}
	*handle = h;
	*iova = va;
	return 0;
}

int mgpu_bo_free(struct mgpu *m, uint32_t handle)
{
	int i;

	if (!m)
		return -MGPU_EINVAL;
	for (i = 0; i < (int)MGPU_MAX_BOS; i++) {
		if (!m->bos[i].used || m->bos[i].handle != handle)
			continue;
		if (m->bos[i].busy) {
			m->uaf_attempted = 1;
			return -MGPU_EBUSY;
		}
		if (m->bos[i].map_idx >= 0)
			m->maps[m->bos[i].map_idx].used = 0;
		m->bos[i].used = 0;
		m->bos[i].map_idx = -1;
		return 0;
	}
	return -MGPU_ENOENT;
}

static int mgpu_bo_lookup(struct mgpu *m, int handle)
{
	int i;

	for (i = 0; i < (int)MGPU_MAX_BOS; i++) {
		if (m->bos[i].used && m->bos[i].handle == (uint32_t)handle)
			return i;
	}
	return -1;
}

int mgpu_submit(struct mgpu *m, int slot, uint64_t jc, int bo_handle)
{
	struct mgpu_job *j;
	int bi = -1;

	if (!m || slot < 0 || slot >= (int)P7580_JM_MAX_SLOTS)
		return -MGPU_EINVAL;
	if (!m->bypass_validate) {
		if (!jc || (jc & 7u))
			return -MGPU_EINVAL;
	}
	if (mgpu_reg_read(m, mgpu_js_reg(slot, P7580_JS_COMMAND_NEXT)))
		return -MGPU_EBUSY; /* stuck slot: recovery, not a 2nd START */
	if (bo_handle >= 0) {
		bi = mgpu_bo_lookup(m, bo_handle);
		if (bi < 0)
			return -MGPU_ENOENT;
	}
	j = &m->jobs[slot];
	if (j->active)
		return -MGPU_EBUSY;

	mgpu_reg_write(m, mgpu_js_reg(slot, P7580_JS_HEAD_NEXT_LO),
		       (uint32_t)(jc & 0xFFFFFFFFu));
	mgpu_reg_write(m, mgpu_js_reg(slot, P7580_JS_HEAD_NEXT_HI),
		       (uint32_t)(jc >> 32));
	mgpu_reg_write(m, mgpu_js_reg(slot, P7580_JS_CONFIG_NEXT),
		       p7580_js_config(0));
	mgpu_reg_write(m, mgpu_js_reg(slot, P7580_JS_COMMAND_NEXT),
		       P7580_JS_CMD_START);

	j->active = 1;
	j->slot = slot;
	j->jc = jc;
	j->bo = bi;
	j->dispatched_at = m->clock_ms;
	j->fence.signaled = 0;
	j->fence.err = 0;
	j->will_fault = 0;
	if (m->force_fault[slot] >= 0) {
		j->will_fault = 1;
		j->js_code = (uint32_t)m->force_fault[slot];
		if (j->js_code == 0x43u)
			j->mmu_status = MGPU_MMUS_PERM_WRITE;
		else if (j->js_code == 0x42u)
			j->mmu_status = MGPU_MMUS_PERM_WRITE;
		else
			j->mmu_status = MGPU_MMUS_TRANS_READ;
		j->complete_at = (long)(m->clock_ms + m->done_delay_ms);
	} else if (!mgpu_mapped(m, jc)) {
		j->will_fault = 1;
		j->js_code = 0x5Au; /* ADDRESS_RANGE_FAULT */
		j->mmu_status = MGPU_MMUS_TRANS_READ;
		j->complete_at = (long)(m->clock_ms + m->done_delay_ms);
	} else if (m->hang_slot[slot]) {
		j->complete_at = -1; /* never IRQs: timeout path */
	} else {
		j->complete_at = (long)(m->clock_ms + m->done_delay_ms);
	}
	if (bi >= 0)
		m->bos[bi].busy = 1;
	return 0;
}

void mgpu_step_ms(struct mgpu *m, uint64_t ms)
{
	uint64_t target;
	int s;

	if (!m)
		return;
	target = m->clock_ms + ms;
	m->clock_ms = target;
	for (s = 0; s < (int)P7580_JM_MAX_SLOTS; s++) {
		struct mgpu_job *j = &m->jobs[s];
		uint32_t bit;

		if (!j->active || j->complete_at < 0)
			continue;
		if ((uint64_t)j->complete_at > target)
			continue;
		j->complete_at = -2; /* consumed */
		if (j->will_fault) {
			/* ERR: chain frozen, COMMAND_NEXT stays stuck. */
			m->regs[mgpu_js_reg(s, P7580_JS_STATUS) >> 2] =
				j->js_code;
			bit = P7580_JOB_ERR_BIT(s);
			m->regs[P7580_JOB_INT_RAWSTAT >> 2] |= bit;
			m->regs[P7580_JOB_INT_STAT >> 2] |= bit;
			m->regs[P7580_JOB_INT_RAWSTAT >> 2] |=
				P7580_JOB_DONE_BIT(s);
			m->regs[P7580_JOB_INT_STAT >> 2] |=
				P7580_JOB_DONE_BIT(s);
			mgpu_as_write(m, MGPU_AS_FAULTSTATUS, j->mmu_status);
			mgpu_as_write(m, MGPU_AS_FAULTADDR_LO,
				      (uint32_t)(j->jc & 0xFFFFFFFFu));
			mgpu_as_write(m, MGPU_AS_FAULTADDR_HI,
				      (uint32_t)(j->jc >> 32));
			m->regs[MGPU_MMU_INT_STAT >> 2] |= MGPU_MMU_PF(0);
		} else {
			/* DONE: slot idles, fence source asserted. */
			mgpu_reg_write(m, mgpu_js_reg(s, P7580_JS_COMMAND_NEXT),
				       0);
			bit = P7580_JOB_DONE_BIT(s);
			m->regs[P7580_JOB_INT_RAWSTAT >> 2] |= bit;
			m->regs[P7580_JOB_INT_STAT >> 2] |= bit;
		}
	}
}

void mgpu_isr_demux(struct mgpu *m)
{
	uint32_t status;
	int j;

	if (!m)
		return;
	status = m->regs[P7580_JOB_INT_STAT >> 2];
	if (!status)
		return;
	for (j = 0; j < (int)P7580_JM_MAX_SLOTS; j++) {
		uint32_t mk = P7580_MK_JS_MASK(j);
		struct mgpu_job *jb;

		if (!(status & mk))
			continue;
		mgpu_reg_write(m, P7580_JOB_INT_CLEAR, mk);
		if (status & P7580_JOB_ERR_BIT(j)) {
			/* Freeze the chain, keep STATUS/HEAD/TAIL. */
			mgpu_reg_write(m,
				       mgpu_js_reg(j, P7580_JS_COMMAND_NEXT),
				       P7580_JS_CMD_NOP);
		}
		if (status & P7580_JOB_DONE_BIT(j)) {
			jb = &m->jobs[j];
			if (jb->active && jb->slot == j && !jb->will_fault) {
				jb->fence.signaled = 1;
				jb->fence.err = 0;
				if (jb->bo >= 0)
					m->bos[jb->bo].busy = 0;
				jb->active = 0;
			}
		}
		status &= ~mk;
	}
}

int mgpu_fence_signaled(struct mgpu *m, int slot)
{
	if (!m || slot < 0 || slot >= (int)P7580_JM_MAX_SLOTS)
		return 0;
	return m->jobs[slot].fence.signaled;
}

int mgpu_recover(struct mgpu *m, int fault_slot, uint32_t status)
{
	int i, ret = 0;
	int kind = PF7580_RESET_NONE;

	(void)status;
	if (!m || fault_slot < 0 ||
	    fault_slot >= (int)P7580_JM_MAX_SLOTS)
		return -MGPU_EINVAL;
	m->reset_kind = PF7580_RESET_NONE;

	/* 1. HARD_STOP the faulting slot + any other busy slot. */
	mgpu_reg_write(m, mgpu_js_reg(fault_slot, P7580_JS_COMMAND_NEXT),
		       P7580_JS_CMD_HARD_STOP);
	for (i = 0; i < (int)P7580_JM_MAX_SLOTS; i++) {
		if (i != fault_slot &&
		    mgpu_reg_read(m, mgpu_js_reg(i, P7580_JS_COMMAND_NEXT)))
			mgpu_reg_write(m, mgpu_js_reg(i, P7580_JS_COMMAND_NEXT),
				       P7580_JS_CMD_HARD_STOP);
	}

	/* 2. Drain: honored slots idle at once; the rest never do. */
	for (i = 0; i < (int)P7580_JM_MAX_SLOTS; i++) {
		if (mgpu_reg_read(m, mgpu_js_reg(i, P7580_JS_COMMAND_NEXT)) &&
		    m->stop_honor[i]) {
			mgpu_reg_write(m,
				       mgpu_js_reg(i, P7580_JS_COMMAND_NEXT),
				       0);
			m->clock_ms += 1;
		}
	}
	for (i = 0; i < (int)P7580_JM_MAX_SLOTS; i++) {
		if (mgpu_reg_read(m, mgpu_js_reg(i, P7580_JS_COMMAND_NEXT)))
			ret = -MGPU_ETIMEDOUT;
	}

	/* 3. GPU reset: SOFT first, HARD when COMPLETED never asserts. */
	mgpu_reg_write(m, P7580_GPU_CMD, P7580_GPU_CMD_SOFT_RESET);
	m->clock_ms += 5;
	if (m->regs[P7580_GPU_INT_RAWSTAT >> 2] &
	    P7580_GPU_IRQ_RESET_COMPLETED) {
		kind = PF7580_RESET_SOFT;
	} else {
		mgpu_reg_write(m, P7580_GPU_CMD, P7580_GPU_CMD_HARD_RESET);
		m->clock_ms += 5;
		kind = PF7580_RESET_HARD;
	}
	mgpu_reg_write(m, P7580_GPU_INT_CLEAR,
		       P7580_GPU_IRQ_RESET_COMPLETED);

	/* Reset wipes the AS program; the driver re-issues it. */
	mgpu_as_write(m, MGPU_AS_TRANSTAB_LO, 0);
	mgpu_as_write(m, MGPU_AS_TRANSTAB_HI, 0);

	/* 4. CLEAR + re-arm slot IRQs; re-program AS_TRANSTAB. */
	mgpu_reg_write(m, P7580_JOB_INT_CLEAR, MGPU_PRESENT_MASK);
	m->regs[P7580_JOB_INT_MASK >> 2] = MGPU_PRESENT_MASK;
	mgpu_as_write(m, MGPU_AS_TRANSTAB_LO, m->as_pgd_lo);
	mgpu_as_write(m, MGPU_AS_TRANSTAB_HI, m->as_pgd_hi);

	/* Latched fault state clears with the reset. */
	for (i = 0; i < (int)P7580_JM_MAX_SLOTS; i++)
		m->regs[mgpu_js_reg(i, P7580_JS_STATUS) >> 2] = 0;
	mgpu_as_write(m, MGPU_AS_FAULTSTATUS, 0);
	m->regs[MGPU_MMU_INT_STAT >> 2] = 0;

	/* 5. The faulting job's waiters must not hang: -EIO. */
	if (m->jobs[fault_slot].active) {
		m->jobs[fault_slot].fence.signaled = 1;
		m->jobs[fault_slot].fence.err = -MGPU_EIO;
		if (m->jobs[fault_slot].bo >= 0)
			m->bos[m->jobs[fault_slot].bo].busy = 0;
		m->jobs[fault_slot].active = 0;
		m->jobs[fault_slot].will_fault = 0;
	}

	m->reset_kind = kind;
	m->recover_ret = ret;
	return ret;
}

int mgpu_js_status(struct mgpu *m, int slot)
{
	if (!m || slot < 0 || slot >= (int)P7580_JM_MAX_SLOTS)
		return 0;
	return (int)m->regs[mgpu_js_reg(slot, P7580_JS_STATUS) >> 2];
}

uint32_t mgpu_reg_read(struct mgpu *m, uint32_t off)
{
	if (!m || (off >> 2) >= MGPU_REG_WORDS)
		return 0;
	return m->regs[off >> 2];
}

void mgpu_reg_write(struct mgpu *m, uint32_t off, uint32_t val)
{
	uint32_t idx;

	if (!m || (off >> 2) >= MGPU_REG_WORDS)
		return;
	idx = off >> 2;
	if (off == P7580_JOB_INT_CLEAR) {
		m->regs[P7580_JOB_INT_STAT >> 2] &= ~val;
		m->regs[P7580_JOB_INT_RAWSTAT >> 2] &= ~val;
		return;
	}
	if (off == P7580_GPU_INT_CLEAR) {
		m->regs[P7580_GPU_INT_RAWSTAT >> 2] &= ~val;
		return;
	}
	if (off == P7580_GPU_CMD) {
		if (val == P7580_GPU_CMD_SOFT_RESET) {
			m->gpu_cmd_log |= 1;
			if (m->soft_ok) {
				m->regs[P7580_GPU_INT_RAWSTAT >> 2] |=
					P7580_GPU_IRQ_RESET_COMPLETED;
			}
		} else if (val == P7580_GPU_CMD_HARD_RESET) {
			m->gpu_cmd_log |= 2;
			m->regs[P7580_GPU_INT_RAWSTAT >> 2] |=
				P7580_GPU_IRQ_RESET_COMPLETED;
		}
		return;
	}
	if (off == MGPU_MMU_INT_CLEAR) {
		m->regs[MGPU_MMU_INT_STAT >> 2] &= ~val;
		return;
	}
	m->regs[idx] = val;
}
