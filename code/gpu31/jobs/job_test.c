/* SPDX-License-Identifier: GPL-2.0 */
/*
 * job_test.c - host unit test for the jobs slice pure-C surface.
 *
 * Exercises panfrost7580_jm_hw.h only (no kernel headers): register
 * layout cross-checks (kbase == mainline values), slot routing,
 * JS_CONFIG construction, fault decode, descriptor size, and the
 * SUBMIT validation rules shared with panfrost7580_job.c.
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

	/* Slot routing (v5.2: FS->JS0, else JS1; compute internal). */
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

	/* Timeouts: SHIM-5S parity, kbase defaults. */
	CHECK(P7580_JOB_TIMEOUT_MS == 5000u);
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

	if (fails == 0)
		printf("job_test: all checks passed\n");
	else
		printf("job_test: %d FAILURE(S)\n", fails);
	return fails ? 1 : 0;
}
