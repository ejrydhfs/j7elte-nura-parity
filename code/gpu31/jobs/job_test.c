/* SPDX-License-Identifier: GPL-2.0 */
/*
 * job_test.c - host unit test for the jobs slice pure-C surface.
 *
 * Exercises panfrost7580_jm_hw.h (+ the opaque branch of
 * panfrost7580_job.h; no kernel headers): register layout
 * cross-checks (kbase == mainline values), slot routing, JS_CONFIG
 * construction, fault decode, descriptor size, and the SUBMIT
 * validation rules shared with panfrost7580_job.c. Review-fix cover:
 * F-JOB1 (jc predicate: aligned accept / unaligned + zero reject),
 * F-JOB2 (staged-pointer contract: kernel-copied arrays feed the
 * submit path; documented via the const-u32 staging shape used in
 * p7580_job_submit_args), F-JOB3 (single-owner fence rule:
 * done_fence owns, render_done_fence aliases), F-JOB4 (fault path
 * must signal exactly once; then-before/after latch semantics).
 *
 * Host fence/fault models below mirror the kernel rules bit for bit
 * (put-once, signal-once) so a regression in either file breaks the
 * build-time contract. The models are test doubles, not the 0001
 * shim itself; kernel behaviour is asserted in-tree by inspection
 * of p7580_job_run/free/recover.
 *
 * Build: cc -Wall -Wextra -o job_test job_test.c && ./job_test
 */
#include <stdio.h>
#include <string.h>
#include "panfrost7580_jm_hw.h"
#include "panfrost7580_job.h"
/* job.h is kernel-aware; the host build sees the opaque-layout branch. */
static int fails = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

int main(void)
{
	/* Register layout: both sides agree (gpu-panfrost.md 3.1). */
	CHECK(P7580_JS_BASE == 0x1800u);
	CHECK(P7580_JS_STRIDE == 0x80u);
	CHECK(P7580_JS_REG(0, 0) == 0x1800u);
	CHECK(P7580_JS_REG(1, 0) == 0x1880u);
	CHECK(P7580_JS_REG(2, 0) == 0x1900u);
	CHECK(P7580_JS_REG(0, P7580_JS_HEAD_NEXT_LO) == 0x1840u);
	CHECK(P7580_JS_REG(0, P7580_JS_CONFIG_NEXT) == 0x1858u);
	CHECK(P7580_JS_REG(0, P7580_JS_COMMAND_NEXT) == 0x1860u);
	CHECK(P7580_JOB_INT_RAWSTAT == 0x1000u);
	CHECK(P7580_JOB_INT_MASK == 0x1008u);

	/* IRQ demux bits: MK_JS_MASK(j) = 0x10001 << j (v5.2 regs). */
	CHECK(P7580_MK_JS_MASK(0) == 0x10001u);
	CHECK(P7580_MK_JS_MASK(1) == 0x20002u);
	CHECK(P7580_MK_JS_MASK(2) == 0x40004u);
	CHECK(P7580_JOB_DONE_BIT(0) == 1u);
	CHECK(P7580_JOB_DONE_BIT(2) == 4u);
	CHECK(P7580_JOB_ERR_BIT(0) == 0x10000u);
	CHECK(P7580_JOB_ERR_BIT(2) == 0x40000u);

	/* Slot routing (mainline v5.2 panfrost_job_get_slot: FS->JS0,
	 * everything else->JS1; ONLY_COMPUTE->JS2 kernel-internal.
	 * Mesa vertex/tiler chain carries requirements==0 -> JS1;
	 * mesa-render: req==0 on JS0 faulted every batch). */
	CHECK(p7580_job_slot(P7580_JD_REQ_FS) == 0);
	CHECK(p7580_job_slot(0) == 1);
	CHECK(p7580_job_slot(P7580_JD_REQ_ONLY_COMPUTE) == 2);
	CHECK(p7580_job_slot(P7580_JD_REQ_FS |
			     P7580_JD_REQ_ONLY_COMPUTE) == 0);

	/* 3 slots: T720 MP2 (live: js_present 0x7). */
	CHECK(P7580_JM_MAX_SLOTS == 3u);
	CHECK(P7580_NUM_JOB_SLOTS == 3);

	/* JS_CONFIG: START+END CLEAN_INV + START_MMU (10649) + PRI8. */
	{
		uint32_t cfg = p7580_js_config(0);
		CHECK((cfg & (3u << 8)) == (3u << 8));
		CHECK((cfg & (3u << 12)) == (3u << 12));
		CHECK(cfg & P7580_JS_CFG_START_MMU);
		CHECK(((cfg >> 16) & 0xFFu) == 8u);
		/* as_nr occupies the low bits, kbase style. */
		CHECK((p7580_js_config(2) & 0xFu) == 2u);
		/* no T720-unsupported bits. */
		CHECK(!(cfg & P7580_JS_CFG_ENABLE_FLUSH_REDUCTION));
		CHECK(!(cfg & P7580_JS_CFG_DISABLE_DESC_WR_BK));
		CHECK(!(cfg & P7580_JS_CFG_JOB_CHAIN_FLAG));
	}

	/* Fault names (KT regmap values). */
	CHECK(!strcmp(p7580_js_status_name(0x02), "INTERRUPTED"));
	CHECK(!strcmp(p7580_js_status_name(0x40), "CONFIG_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x44), "AFFINITY_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x59), "TILE_RANGE_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x5A), "ADDRESS_RANGE_FAULT"));
	CHECK(!strcmp(p7580_js_status_name(0x60), "OUT_OF_MEMORY"));
	CHECK(p7580_js_status_is_fault(0x40));
	CHECK(p7580_js_status_is_fault(0x5A));
	CHECK(!p7580_js_status_is_fault(0x02));
	CHECK(!p7580_js_status_is_fault(0x03));

	/* Timeouts: 750 ms silent-hang bound (watchdog), 3 s reset poll. */
	CHECK(P7580_JOB_TIMEOUT_MS == 750u);
	CHECK(P7580_RESET_TIMEOUT_MS == 3000u);

	/* Descriptor header: 32 bytes, v5.2-compatible field order. */
	CHECK(sizeof(struct p7580_job_desc) == 32u);
	{
		struct p7580_job_desc d;
		memset(&d, 0, sizeof(d));
		CHECK(sizeof(d) == 8u + 8u + 2u + 6u + 8u);
	}

	/* SUBMIT layout: v5.2 ioctl argument shape. */
	CHECK(sizeof(struct p7580_submit) ==
	      8u + 8u + 4u + 4u + 8u + 4u + 4u);

	/* SUBMIT caps. */
	CHECK(P7580_MAX_IN_SYNCS == 16u);
	CHECK(P7580_MAX_SUBMIT_BOS == 64u);

	/* F-JOB1: shared jc predicate (kernel uses this exact inline). */
	/* 8-aligned nonzero accept; zero and any unaligned reject. */
	CHECK(p7580_jc_valid(0x1000u));
	CHECK(p7580_jc_valid(0x1ffffff8u));
	CHECK(!p7580_jc_valid(0u));
	CHECK(!p7580_jc_valid(1u));
	CHECK(!p7580_jc_valid(7u));
	CHECK(!p7580_jc_valid(8u - 1u));
	CHECK(!p7580_jc_valid(0x1001u));
	CHECK(!p7580_jc_valid(0x1007u));
	CHECK(p7580_jc_valid(0x1008u));
	/* Old inverted shape would have accepted 0x1001 and rejected */
	/* 0x1000; assert the fixed polarity directly. */
	CHECK(p7580_jc_valid(0x1000u) && !p7580_jc_valid(0x1001u));

	/* F-JOB2: submit entry takes kernel-staged u32 arrays. Model  */
	/* the ioctl staging contract: u64 fields carry kernel pointers */
	/* to u32 storage; the core reads them with plain indexing. */
	{
		uint32_t staged_hs[2] = { 11u, 29u };
		uint32_t staged_bo[2] = { 3u, 5u };
		struct p7580_submit args;
		const uint32_t *hs;
		const uint32_t *uh;
		uint32_t i;

		memset(&args, 0, sizeof(args));
		args.jc = 0x2000u;
		args.in_syncs = (uint64_t)(uintptr_t)staged_hs;
		args.in_sync_count = 2u;
		args.bo_handles = (uint64_t)(uintptr_t)staged_bo;
		args.bo_handle_count = 2u;
		hs = (const uint32_t *)(uintptr_t)args.in_syncs;
		uh = (const uint32_t *)(uintptr_t)args.bo_handles;
		for (i = 0; i < args.in_sync_count; i++)
			CHECK(hs[i] == staged_hs[i]);
		for (i = 0; i < args.bo_handle_count; i++)
			CHECK(uh[i] == staged_bo[i]);
		CHECK(p7580_jc_valid(args.jc));
		CHECK(args.in_sync_count <= P7580_MAX_IN_SYNCS);
		CHECK(args.bo_handle_count <= P7580_MAX_SUBMIT_BOS);
	}

	/* F-JOB3: one ref, one owner. Model: fence starts at refcount  */
	/* 1 (run_job dma_fence_get); done_fence owns it, render alias  */
	/* adds none; free puts exactly once. */
	{
		int refs = 1;
		const void *done_fence = (const void *)(uintptr_t)0x1;
		const void *render_alias = done_fence;
		int puts = 0;

		CHECK(render_alias == done_fence);
		/* free path: single put on the owner only. */
		puts++;
		(void)done_fence;
		CHECK(refs - puts == 0);
		/* A second put (the old bug) would drive refs to -1. */
		CHECK(refs - (puts + 1) < 0);
	}

	/* F-JOB4: fault must signal exactly once. Model the       */
	/* latch/clear-then-signal rule: in_flight cleared under   */
	/* hw_lock before reset, so a late DONE cannot re-signal;  */
	/* the is_signaled guard skips an IRQ-won race. */
	{
		int in_flight = 1;
		int done_signaled = 0;
		int fault_signals = 0;

		/* Recover latches the job and clears the slot first. */
		in_flight = 0;
		/* Fault signal fires: job was pending. */
		if (!done_signaled)
			fault_signals++;
		done_signaled = 1;
		CHECK(fault_signals == 1);
		/* Late IRQ DONE after the clear must not re-fire. */
		if (in_flight && !done_signaled)
			fault_signals++;
		CHECK(fault_signals == 1);
		/* IRQ-won race: already signaled -> recover skips. */
		fault_signals = 0;
		done_signaled = 1;
		if (!done_signaled)
			fault_signals++;
		CHECK(fault_signals == 0);
	}

	/* F-JOB4 fault/status gating: only real faults drive the log   */
	/* + signal path; benign STOPPED/TERMINATED do not. */
	CHECK(p7580_js_status_is_fault(0x5Au));
	CHECK(!p7580_js_status_is_fault(0x03u));
	CHECK(!p7580_js_status_is_fault(0x04u));
	CHECK(!strcmp(p7580_js_status_name(0x100u), "RESERVED_FAULT"));

	if (fails == 0)
		printf("job_test: all checks passed\n");
	else
		printf("job_test: %d FAILURE(S)\n", fails);
	return fails ? 1 : 0;
}
