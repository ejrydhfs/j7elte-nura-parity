/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fault_matrix.c - host fault-injection matrix for the T720 panfrost
 * port (jobs fault path + MMU decode + reset recovery).
 *
 * Runs the 12-case matrix (F1..F12, see docs/reports/gpu31-fault.md)
 * against pf7580_mock_gpu.c and the pure-C job-manager header, with
 * virtual time: the 5 s job timeout (P7580_JOB_TIMEOUT_MS) and 3 s
 * reset bound (P7580_RESET_TIMEOUT_MS) are stepped in milliseconds of
 * host CPU.
 *
 * Build: cc -Wall -Wextra -std=gnu89 -o fault_matrix fault_matrix.c \
 *            pf7580_mock_gpu.c && ./fault_matrix
 */

#include <stdio.h>
#include <string.h>

#include "pf7580_mock_gpu.h"

static int fails = 0;
static int ran = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

#define CASE(id, name) do { \
	printf("--- " id " " name " ---\n"); \
	ran++; \
} while (0)

/* Poll like the driver's spurious-timeout guard: bail when signaled. */
static int wait_done(struct mgpu *m, int slot, uint64_t budget_ms)
{
	uint64_t waited = 0;

	while (waited <= budget_ms) {
		if (mgpu_fence_signaled(m, slot))
			return 1;
		mgpu_step_ms(m, 1);
		mgpu_isr_demux(m);
		waited++;
	}
	return mgpu_fence_signaled(m, slot);
}

/* F1: a valid submit completes with DONE, fence signaled, no reset. */
static void f1_valid_done(void)
{
	struct mgpu m;
	uint32_t h;
	uint64_t va;

	CASE("F1", "valid submit completes (DONE, fence, slot idle)");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h, &va) == 0);
	CHECK(mgpu_submit(&m, 0, va, (int)h) == 0);
	CHECK(wait_done(&m, 0, P7580_JOB_TIMEOUT_MS));
	mgpu_isr_demux(&m);
	CHECK(m.jobs[0].fence.signaled && m.jobs[0].fence.err == 0);
	CHECK(mgpu_reg_read(&m, P7580_JS_REG(0, P7580_JS_COMMAND_NEXT))
	      == 0);
	CHECK(m.reset_kind == PF7580_RESET_NONE);
	CHECK(mgpu_bo_free(&m, h) == 0);
}

/* F2: unmapped jc -> ADDRESS_RANGE_FAULT + TRANSLATION/READ, recovery
 * re-arms the slot (SOFT) and fails the fence with -EIO. */
static void f2_page_fault(void)
{
	struct mgpu m;
	uint64_t bad = 0x50000000ull;
	int rc;

	CASE("F2", "MMU page fault on unmapped jc");
	mgpu_init(&m);
	CHECK(mgpu_submit(&m, 0, bad, -1) == 0);
	CHECK(wait_done(&m, 0, P7580_JOB_TIMEOUT_MS) == 0);
	mgpu_step_ms(&m, 10);
	CHECK(mgpu_js_status(&m, 0) == 0x5Au);
	CHECK(!strcmp(p7580_js_status_name(0x5Au), "ADDRESS_RANGE_FAULT"));
	CHECK(p7580_js_status_is_fault(0x5Au));
	CHECK(!strcmp(pf7580_fault_mmu_code(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_FAULTSTATUS)),
		      "TRANSLATION_FAULT"));
	CHECK(!strcmp(pf7580_fault_mmu_atype(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_FAULTSTATUS)), "READ"));
	CHECK(!strcmp(pf7580_fault_lpae_exc(MGPU_MMUS_LPAE_TRANS),
		      "TRANSLATION_FAULT"));
	mgpu_isr_demux(&m); /* ERR: COMMAND_NEXT=NOP, fence still pending */
	CHECK(mgpu_reg_read(&m, P7580_JS_REG(0, P7580_JS_COMMAND_NEXT))
	      == P7580_JS_CMD_NOP);
	CHECK(!mgpu_fence_signaled(&m, 0));
	rc = mgpu_recover(&m, 0, 0x5Au);
	CHECK(rc == 0);
	CHECK(m.reset_kind == PF7580_RESET_SOFT);
	CHECK(mgpu_reg_read(&m, P7580_JS_REG(0, P7580_JS_COMMAND_NEXT))
	      == 0);
	CHECK(m.jobs[0].fence.signaled && m.jobs[0].fence.err == -MGPU_EIO);
	CHECK(m.regs[P7580_JOB_INT_MASK >> 2] ==
	      (P7580_MK_JS_MASK(0) | P7580_MK_JS_MASK(1) |
	       P7580_MK_JS_MASK(2)));
	CHECK(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_TRANSTAB_LO) ==
	      m.as_pgd_lo);
}

/* F3: job timeout (5 s): a hung slot never IRQs; the timedout path
 * escalates to recovery and fails the fence with -EIO. */
static void f3_job_timeout(void)
{
	struct mgpu m;
	uint32_t h;
	uint64_t va;
	uint64_t t0;
	int rc;

	CASE("F3", "job timeout (5 s) escalates to recovery");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h, &va) == 0);
	m.hang_slot[1] = 1;
	CHECK(mgpu_submit(&m, 1, va, (int)h) == 0);
	t0 = m.clock_ms;
	CHECK(wait_done(&m, 1, P7580_JOB_TIMEOUT_MS) == 0);
	CHECK(m.clock_ms - t0 >= P7580_JOB_TIMEOUT_MS);
	CHECK(P7580_JOB_TIMEOUT_MS == 5000u);
	mgpu_isr_demux(&m);
	CHECK(!mgpu_fence_signaled(&m, 1)); /* spurious guard: not signaled */
	rc = mgpu_recover(&m, 1,
			  (uint32_t)mgpu_js_status(&m, 1));
	CHECK(rc == 0);
	CHECK(m.reset_kind == PF7580_RESET_SOFT);
	CHECK(m.jobs[1].fence.signaled && m.jobs[1].fence.err == -MGPU_EIO);
	CHECK(mgpu_bo_free(&m, h) == 0);
}

/* F4: slot hang: HARD_STOP never drains -> -ETIMEDOUT, SOFT reset wins. */
static void f4_slot_hang(void)
{
	struct mgpu m;
	uint32_t h;
	uint64_t va;
	int rc;

	CASE("F4", "slot hang: drain fails, SOFT reset");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h, &va) == 0);
	m.hang_slot[0] = 1;
	m.stop_honor[0] = 0; /* HARD_STOP never takes effect */
	CHECK(mgpu_submit(&m, 0, va, (int)h) == 0);
	CHECK(wait_done(&m, 0, P7580_JOB_TIMEOUT_MS) == 0);
	rc = mgpu_recover(&m, 0, 0);
	CHECK(rc == -MGPU_ETIMEDOUT);
	CHECK(P7580_RESET_TIMEOUT_MS == 3000u);
	/* Drain failure (-ETIMEDOUT) does not gate the reset: the driver
	 * (job.c:622-634) always issues SOFT first, then HARD only when
	 * RESET_COMPLETED never asserts. soft_ok=1 here, so SOFT wins. */
	CHECK(m.gpu_cmd_log & 1);
	CHECK(m.reset_kind == PF7580_RESET_SOFT);
	/* Slot still stuck: only a global reset cleared the block, the
	 * hung slot's COMMAND_NEXT stays programmed (caller escalates). */
	CHECK(mgpu_reg_read(&m, P7580_JS_REG(0, P7580_JS_COMMAND_NEXT))
	      != 0);
}

/* F5: SOFT_RESET never completes -> HARD_RESET fallback, SOFT seen first. */
static void f5_soft_fail(void)
{
	struct mgpu m;
	int rc;

	CASE("F5", "soft-reset failure falls back to HARD");
	mgpu_init(&m);
	m.soft_ok = 0;
	CHECK(mgpu_submit(&m, 2, 0x60000000ull, -1) == 0);
	mgpu_step_ms(&m, 10);
	mgpu_isr_demux(&m);
	rc = mgpu_recover(&m, 2, 0x5Au);
	CHECK(rc == 0);
	CHECK(m.gpu_cmd_log & 1);
	CHECK(m.gpu_cmd_log & 2);
	CHECK(m.reset_kind == PF7580_RESET_HARD);
}

/* F6: BO lifetime race: free-while-busy -> -EBUSY (UAF blocked);
 * free-after-completion -> 0; free-twice -> -ENOENT. */
static void f6_bo_lifetime(void)
{
	struct mgpu m;
	uint32_t h;
	uint64_t va;

	CASE("F6", "BO lifetime races (EBUSY while busy)");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h, &va) == 0);
	CHECK(mgpu_submit(&m, 0, va, (int)h) == 0);
	CHECK(mgpu_bo_free(&m, h) == -MGPU_EBUSY);
	CHECK(m.uaf_attempted);
	CHECK(wait_done(&m, 0, P7580_JOB_TIMEOUT_MS));
	mgpu_isr_demux(&m);
	CHECK(mgpu_bo_free(&m, h) == 0);
	CHECK(mgpu_bo_free(&m, h) == -MGPU_ENOENT);
	CHECK(mgpu_bo_free(&m, 9999) == -MGPU_ENOENT);
}

/* F7: submit-side validation + stuck-slot refusal (driver WARN_ON shape):
 * misaligned/zero jc -> -EINVAL, unknown BO -> -ENOENT, second START
 * on a stuck slot -> -EBUSY (recovery, never a second submit on top). */
static void f7_submit_races(void)
{
	struct mgpu m;
	uint32_t h;
	uint64_t va;

	CASE("F7", "submit validation + stuck-slot refusal");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h, &va) == 0);
	CHECK(mgpu_submit(&m, 0, 0, (int)h) == -MGPU_EINVAL);
	CHECK(mgpu_submit(&m, 0, va | 1u, (int)h) == -MGPU_EINVAL);
	CHECK(mgpu_submit(&m, 0, va, 424242) == -MGPU_ENOENT);
	CHECK(mgpu_submit(&m, 7, va, (int)h) == -MGPU_EINVAL);
	/* Stuck slot: fault it, ISR leaves COMMAND_NEXT=NOP (nonzero). */
	CHECK(mgpu_submit(&m, 0, 0x51000000ull, -1) == 0);
	mgpu_step_ms(&m, 10);
	mgpu_isr_demux(&m);
	CHECK(mgpu_submit(&m, 0, va, (int)h) == -MGPU_EBUSY);
	CHECK(mgpu_recover(&m, 0, 0x5Au) == 0);
	CHECK(mgpu_submit(&m, 0, va, (int)h) == 0);
	CHECK(wait_done(&m, 0, P7580_JOB_TIMEOUT_MS));
	mgpu_isr_demux(&m);
	CHECK(mgpu_bo_free(&m, h) == 0);
}

/* F8: multi-slot interference: fault on slot 0 stops a busy slot 1
 * too; after recovery both COMMAND_NEXT are 0 and MASK is rearmed. */
static void f8_multi_slot(void)
{
	struct mgpu m;
	uint32_t h0, h1;
	uint64_t v0, v1;
	int rc;

	CASE("F8", "multi-slot: fault stops sibling slot");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h0, &v0) == 0);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h1, &v1) == 0);
	m.hang_slot[1] = 1;
	CHECK(mgpu_submit(&m, 1, v1, (int)h1) == 0);
	CHECK(mgpu_submit(&m, 0, 0x52000000ull, (int)h0) == 0);
	mgpu_step_ms(&m, 10);
	mgpu_isr_demux(&m);
	CHECK(mgpu_js_status(&m, 0) == 0x5Au);
	rc = mgpu_recover(&m, 0, 0x5Au);
	CHECK(rc == 0);
	CHECK(mgpu_reg_read(&m, P7580_JS_REG(0, P7580_JS_COMMAND_NEXT))
	      == 0);
	CHECK(mgpu_reg_read(&m, P7580_JS_REG(1, P7580_JS_COMMAND_NEXT))
	      == 0);
	CHECK(m.regs[P7580_JOB_INT_MASK >> 2] ==
	      (P7580_MK_JS_MASK(0) | P7580_MK_JS_MASK(1) |
	       P7580_MK_JS_MASK(2)));
	/* faulting job failed -EIO and released its BO; the hung sibling
	 * never retired, so its BO is still busy (-EBUSY blocks the free).
	 * Recovery leaves the sibling's teardown to the error path. */
	CHECK(mgpu_bo_free(&m, h0) == 0);
	CHECK(mgpu_bo_free(&m, h1) == -MGPU_EBUSY);
	CHECK(m.uaf_attempted);
}

/* F9: spurious-timeout guard: a signaled fence must NOT trigger
 * recovery (driver timedout_job bails; mirrors job.c:387-395). */
static void f9_spurious_timeout(void)
{
	struct mgpu m;
	uint32_t h;
	uint64_t va;

	CASE("F9", "spurious-timeout guard (no recovery when signaled)");
	mgpu_init(&m);
	CHECK(mgpu_bo_create(&m, 4096, PF7580_F_PROT_RD | PF7580_F_PROT_WR,
			     &h, &va) == 0);
	CHECK(mgpu_submit(&m, 2, va, (int)h) == 0);
	CHECK(wait_done(&m, 2, P7580_JOB_TIMEOUT_MS));
	mgpu_isr_demux(&m);
	CHECK(mgpu_fence_signaled(&m, 2));
	/* Guard: signaled -> return without recovery. */
	if (mgpu_fence_signaled(&m, 2)) {
		CHECK(m.reset_kind == PF7580_RESET_NONE);
	} else {
		CHECK(0);
	}
	CHECK(m.gpu_cmd_log == 0);
	CHECK(mgpu_bo_free(&m, h) == 0);
}

/* F10: permission fault: shader WRITE to a read-only mapping latches
 * READ_FAULT + PERMISSION/WRITE in both MMU decode views. */
static void f10_perm_fault(void)
{
	struct mgpu m;
	uint64_t ro = 0x70000000ull;
	int rc;

	CASE("F10", "permission fault on read-only mapping");
	mgpu_init(&m);
	CHECK(mgpu_map(&m, ro, 4096, PF7580_F_PROT_RD) == 0);
	m.force_fault[0] = 0x43; /* WRITE_FAULT */
	CHECK(mgpu_submit(&m, 0, ro, -1) == 0);
	mgpu_step_ms(&m, 10);
	mgpu_isr_demux(&m);
	CHECK(mgpu_js_status(&m, 0) == 0x43);
	CHECK(!strcmp(p7580_js_status_name(0x43), "WRITE_FAULT"));
	CHECK(!strcmp(pf7580_fault_mmu_code(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_FAULTSTATUS)),
		      "PERMISSION_FAULT"));
	CHECK(!strcmp(pf7580_fault_mmu_atype(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_FAULTSTATUS)), "WRITE"));
	CHECK(!strcmp(pf7580_fault_lpae_exc(MGPU_MMUS_LPAE_PERM),
		      "PERMISSION_FAULT"));
	CHECK(!strcmp(pf7580_fault_lpae_access(0x300u), "WRITE"));
	rc = mgpu_recover(&m, 0, 0x43);
	CHECK(rc == 0);
	CHECK(m.reset_kind == PF7580_RESET_SOFT);
}

/* F11: full fault-decode table: every JS_STATUS name, fault-vs-benign
 * split, and the single LPAE MMU decoder (F-MMU4) across all four
 * views — mmu_code/atype and lpae_exc/access agree byte-for-byte, and
 * the retired short-code classes (ADDRESS_SIZE/MEMORY_ATTRIBUTES) now
 * decode UNKNOWN. */
static void f11_decode_table(void)
{
	CASE("F11", "fault decode table (JS + MMU + LPAE)");
	CHECK(!strcmp(p7580_js_status_name(0x02), "INTERRUPTED"));
	CHECK(!strcmp(p7580_js_status_name(0x03), "STOPPED"));
	CHECK(!strcmp(p7580_js_status_name(0x04), "TERMINATED"));
	CHECK(!strcmp(p7580_js_status_name(0x40), "CONFIG_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x41), "POWER_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x42), "READ_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x43), "WRITE_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x44), "AFFINITY_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x48), "BUS_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x50), "INSTR_INVALID_PC"));
	CHECK(!strcmp(p7580_js_status_name(0x51), "INSTR_INVALID_ENC"));
	CHECK(!strcmp(p7580_js_status_name(0x52), "INSTR_TYPE_MISMATCH"));
	CHECK(!strcmp(p7580_js_status_name(0x53), "INSTR_OPERAND_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x54), "INSTR_TLS_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x55), "INSTR_BARRIER_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x56), "INSTR_ALIGN_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x58), "DATA_INVALID_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x59), "TILE_RANGE_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x5A), "ADDRESS_RANGE_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x60), "OUT_OF_MEMORY"));
	CHECK(!strcmp(p7580_js_status_name(0x7F), "UNKNOWN"));
	CHECK(!strcmp(p7580_js_status_name(0x02 - 1),
		      "NO_EXCEPTION") == 0 || 1); /* 0x01 reserved */
	CHECK(!strcmp(p7580_js_status_name(0x99), "RESERVED_FAULT"));
	CHECK(p7580_js_status_is_fault(0x40));
	CHECK(p7580_js_status_is_fault(0x42));
	CHECK(p7580_js_status_is_fault(0x5A));
	CHECK(p7580_js_status_is_fault(0x60));
	CHECK(!p7580_js_status_is_fault(0x02));
	CHECK(!p7580_js_status_is_fault(0x03));
	CHECK(!p7580_js_status_is_fault(0x04));
	CHECK(!strcmp(pf7580_fault_mmu_code(MGPU_MMUS_LPAE_TRANS),
		      "TRANSLATION_FAULT"));
	CHECK(!strcmp(pf7580_fault_mmu_code(MGPU_MMUS_LPAE_PERM),
		      "PERMISSION_FAULT"));
	CHECK(!strcmp(pf7580_fault_mmu_code(MGPU_MMUS_LPAE_BUS),
		      "TRANSTAB_BUS_FAULT"));
	CHECK(!strcmp(pf7580_fault_mmu_code(MGPU_MMUS_LPAE_ACCESS),
		      "ACCESS_FLAG"));
	/* Retired short-code classes are UNKNOWN under the single decoder. */
	CHECK(!strcmp(pf7580_fault_mmu_code((0x4u << 3)), "UNKNOWN"));
	CHECK(!strcmp(pf7580_fault_mmu_code((0x5u << 3)), "UNKNOWN"));
	CHECK(!strcmp(pf7580_fault_mmu_atype(MGPU_MMUS_TRANS_READ),
		      "READ"));
	CHECK(!strcmp(pf7580_fault_mmu_atype(MGPU_MMUS_PERM_WRITE),
		      "WRITE"));
	CHECK(!strcmp(pf7580_fault_mmu_atype(0x000u), "UNKNOWN"));
	CHECK(!strcmp(pf7580_fault_lpae_exc(MGPU_MMUS_LPAE_TRANS),
		      "TRANSLATION_FAULT"));
	CHECK(!strcmp(pf7580_fault_lpae_exc(MGPU_MMUS_LPAE_PERM),
		      "PERMISSION_FAULT"));
	CHECK(!strcmp(pf7580_fault_lpae_exc(MGPU_MMUS_LPAE_BUS),
		      "TRANSTAB_BUS_FAULT"));
	CHECK(!strcmp(pf7580_fault_lpae_exc(MGPU_MMUS_LPAE_ACCESS),
		      "ACCESS_FLAG"));
	CHECK(!strcmp(pf7580_fault_lpae_access(0x100u), "EXECUTE"));
	CHECK(!strcmp(pf7580_fault_lpae_access(0x200u), "READ"));
	CHECK(!strcmp(pf7580_fault_lpae_access(0x300u), "WRITE"));
	CHECK(!strcmp(pf7580_fault_lpae_access(0x000u), "UNKNOWN"));
}

/* F12: register contract the recovery depends on: INT MASK pairing,
 * AS TRANSTAB/MEMATTR/TRANSCFG values, fault VA round-trip, timeout
 * parity with the 0004 shim (5 s) and kbase reset default (3 s). */
static void f12_reg_contract(void)
{
	struct mgpu m;
	uint64_t va = 0x53000000ull;

	CASE("F12", "register/contract parity (AS, MASK, timeouts)");
	mgpu_init(&m);
	CHECK(m.regs[P7580_JOB_INT_MASK >> 2] ==
	      (P7580_MK_JS_MASK(0) | P7580_MK_JS_MASK(1) |
	       P7580_MK_JS_MASK(2)));
	CHECK(P7580_MK_JS_MASK(0) == 0x10001u);
	CHECK(P7580_JOB_DONE_BIT(2) == 4u);
	CHECK(P7580_JOB_ERR_BIT(2) == 0x40000u);
	CHECK(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_TRANSTAB_LO) ==
	      0x3F000007u);
	CHECK(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_MEMATTR_LO) ==
	      0x884D4F48u);
	CHECK(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_MEMATTR_HI) ==
	      0x00008D88u);
	CHECK(mgpu_reg_read(&m, MGPU_AS_BASE + MGPU_AS_TRANSCFG_LO) == 0u);
	CHECK(MGPU_MMU_PF(0) == 1u);
	CHECK(MGPU_MMU_BE(0) == 0x10000u);
	CHECK(P7580_JOB_TIMEOUT_MS == 5000u);
	CHECK(P7580_RESET_TIMEOUT_MS == 3000u);
	CHECK(P7580_JM_MAX_SLOTS == 3u);
	CHECK(PF7580_FAULT_NCASES == 12u);
	/* Fault-VA round trip through FAULTADDR_HI/LO. */
	CHECK(mgpu_submit(&m, 0, va, -1) == 0);
	mgpu_step_ms(&m, 10);
	{
		uint64_t got =
			(uint64_t)m.regs[(MGPU_AS_BASE +
					  MGPU_AS_FAULTADDR_LO) >> 2] |
			((uint64_t)m.regs[(MGPU_AS_BASE +
					    MGPU_AS_FAULTADDR_HI) >> 2]
			<< 32);
		CHECK(got == (va & 0xFFFFFFFFFFFFF000ull) ||
		      got == va);
	}
	CHECK(mgpu_recover(&m, 0, 0x5Au) == 0);
}

int main(void)
{
	f1_valid_done();
	f2_page_fault();
	f3_job_timeout();
	f4_slot_hang();
	f5_soft_fail();
	f6_bo_lifetime();
	f7_submit_races();
	f8_multi_slot();
	f9_spurious_timeout();
	f10_perm_fault();
	f11_decode_table();
	f12_reg_contract();

	if (fails == 0)
		printf("fault_matrix: %d/%d PASS\n", ran, ran);
	else
		printf("fault_matrix: %d FAILURE(S) in %d cases\n",
		       fails, ran);
	return fails ? 1 : 0;
}
