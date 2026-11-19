/* SPDX-License-Identifier: GPL-2.0 */
/*
 * devfreq_test.c - host unit test for the devfreq slice pure-C surface.
 *
 * Exercises pf7580_devfreq.h only (no kernel headers): OPP ladder
 * order/content/boot seed, ceil resolution (recommended-OPP semantics),
 * GPU_SET_CLK_VOL step direction (V-first up across a voltage corner,
 * f-first down, no-op on same OPP / same-voltage steps), and the
 * governor/poll contract constants.
 *
 * Build: cc -Wall -Wextra -o devfreq_test devfreq_test.c && ./devfreq_test
 */
#include <stdio.h>
#include <string.h>
#include "pf7580_devfreq.h"

static int fails = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

int main(void)
{
	int i;

	/* Ladder: 8 rows, ascending Hz, verbatim stock values. */
	CHECK(PF7580_DVFS_OPP_COUNT == 8u);
	CHECK(pf7580_dvfs_opps[0].freq_hz == 160000000UL);
	CHECK(pf7580_dvfs_opps[0].volt_uv == 900000UL);
	CHECK(pf7580_dvfs_opps[1].freq_hz == 266000000UL);
	CHECK(pf7580_dvfs_opps[2].freq_hz == 350000000UL);
	CHECK(pf7580_dvfs_opps[2].volt_uv == 1000000UL);
	CHECK(pf7580_dvfs_opps[5].freq_hz == 668000000UL);
	CHECK(pf7580_dvfs_opps[5].volt_uv == 1100000UL);
	CHECK(pf7580_dvfs_opps[7].freq_hz == 800000000UL);
	CHECK(pf7580_dvfs_opps[7].volt_uv == 1100000UL);
	for (i = 1; i < (int)PF7580_DVFS_OPP_COUNT; i++)
		CHECK(pf7580_dvfs_opps[i].freq_hz > pf7580_dvfs_opps[i - 1].freq_hz);

	/* Boot seed is an OPP member (350 MHz @ 1.00 V). */
	CHECK(PF7580_DVFS_BOOT_HZ == 350000000UL);
	CHECK(pf7580_dvfs_opp_ceil_idx(PF7580_DVFS_BOOT_HZ) == 2);

	/* Ceil semantics: exact hits, in-between rounds up, top clamps,
	 * zero yields the floor (loop-start scan, like opp_find_freq_ceil
	 * from 0 in kbase_devfreq_init_freq_table). */
	CHECK(pf7580_dvfs_opp_ceil_idx(160000000UL) == 0);
	CHECK(pf7580_dvfs_opp_ceil_idx(160000001UL) == 1);
	CHECK(pf7580_dvfs_opp_ceil_idx(266000000UL) == 1);
	CHECK(pf7580_dvfs_opp_ceil_idx(267000000UL) == 2);
	CHECK(pf7580_dvfs_opp_ceil_idx(800000000UL) == 7);
	CHECK(pf7580_dvfs_opp_ceil_idx(800000001UL) == 7);
	CHECK(pf7580_dvfs_opp_ceil_idx(0UL) == 0);
	CHECK(pf7580_dvfs_opp_ceil_idx(1UL << 31) == 7);

	/* Voltage corners: 160/266 @ 0.90 V; 350/440/534 @ 1.00 V;
	 * 668/734/800 @ 1.10 V. Up across a corner is V-first (+1),
	 * down across a corner is f-first (-1). */
	CHECK(pf7580_dvfs_volt_first(534000000UL, 668000000UL) == 1);
	CHECK(pf7580_dvfs_volt_first(668000000UL, 534000000UL) == -1);
	CHECK(pf7580_dvfs_volt_first(266000000UL, 350000000UL) == 1);
	CHECK(pf7580_dvfs_volt_first(350000000UL, 266000000UL) == -1);

	/* Same-voltage steps: no voltage order constraint (0). */
	CHECK(pf7580_dvfs_volt_first(160000000UL, 266000000UL) == 0);
	CHECK(pf7580_dvfs_volt_first(668000000UL, 800000000UL) == 0);
	CHECK(pf7580_dvfs_volt_first(800000000UL, 668000000UL) == 0);

	/* No-op on same OPP (0), including non-OPP requests that ceil
	 * to the same entry. */
	CHECK(pf7580_dvfs_volt_first(350000000UL, 350000000UL) == 0);
	CHECK(pf7580_dvfs_volt_first(300000000UL, 349000000UL) == 0);

	/* Full-ladder sweep direction: bottom->top crosses corners up,
	 * top->bottom crosses corners down. */
	CHECK(pf7580_dvfs_volt_first(160000000UL, 800000000UL) == 1);
	CHECK(pf7580_dvfs_volt_first(800000000UL, 160000000UL) == -1);

	/* Governor/poll contract: simple_ondemand (kbase parity, NULL
	 * governor data -> stock 90/5 defaults), 100 ms poll. */
	CHECK(!strcmp(PF7580_DEVFREQ_GOVERNOR, "simple_ondemand"));
	CHECK(PF7580_DEVFREQ_POLL_MS == 100);

	if (fails == 0)
		printf("devfreq_test: all checks passed\n");
	else
		printf("devfreq_test: %d FAILURE(S)\n", fails);
	return fails ? 1 : 0;
}
