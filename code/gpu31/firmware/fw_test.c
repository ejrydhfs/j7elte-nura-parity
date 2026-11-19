/* Host-side harness for the firmware verdict + OPP table.
 * Compiles the kernel-independent pieces (fw gate, OPP table) with
 * stubs so the verdict is unit-tested without a tree:
 *   gcc -Wall -Wextra -o fw_test fw_test.c && ./fw_test
 * The readl/writel sequence in pf7580_perfcnt_probe() needs the kernel
 * tree and is -fsyntax-only checked by the integrator, not here.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

typedef uint32_t u32;
#define pr_warn(fmt, ...) printf("WARN: " fmt "\n", ##__VA_ARGS__)

struct device { int dummy; };
struct pf7580_opp { unsigned long freq_hz; unsigned long volt_uv; };

#include "panfrost7580_fw_inc.c"

int main(void)
{
	const char *why = NULL;
	int i, fails = 0;

	/* 1. Live silicon ID from the j7elte (gpu-verify.md): no firmware. */
	if (pf7580_fw_required(0x07201000u, &why) != 0 || why == NULL) {
		printf("FAIL: T720 r1p0 verdict\n");
		fails++;
	} else {
		printf("PASS: GPU_ID 0x07201000 -> no blob (%s)\n", why);
	}

	/* 2. Non-T720 ID still returns 0 (gate logs, never loads). */
	if (pf7580_fw_required(0x08600000u, NULL) != 0) {
		printf("FAIL: non-T720 gate\n");
		fails++;
	} else {
		printf("PASS: non-T720 ID still no-load\n");
	}

	/* 3. OPP ladder: 8 entries, sorted, all inside the BUCK4 window. */
	if (pf7580_opp_count != 8) {
		printf("FAIL: opp count %d\n", pf7580_opp_count);
		fails++;
	}
	for (i = 0; i < pf7580_opp_count; i++) {
		if (i > 0 && pf7580_opp_table[i].freq_hz <=
			      pf7580_opp_table[i-1].freq_hz) {
			printf("FAIL: opp %d not sorted\n", i);
			fails++;
		}
		if (pf7580_opp_table[i].volt_uv < 600000UL ||
		    pf7580_opp_table[i].volt_uv > 1300000UL) {
			printf("FAIL: opp %d voltage %lu out of BUCK4 window\n",
			       i, pf7580_opp_table[i].volt_uv);
			fails++;
		}
	}
	if (!fails)
		printf("PASS: 8 OPPs sorted, 160-800 MHz, 900mV-1100mV\n");

	/* 4. Endpoints match the stock backend table exactly. */
	if (pf7580_opp_table[0].freq_hz != 160000000UL ||
	    pf7580_opp_table[7].freq_hz != 800000000UL ||
	    pf7580_opp_table[0].volt_uv != 900000UL ||
	    pf7580_opp_table[7].volt_uv != 1100000UL) {
		printf("FAIL: endpoints\n");
		fails++;
	} else {
		printf("PASS: endpoints 160MHz/900mV - 800MHz/1100mV\n");
	}

	printf(fails ? "RESULT: FAIL (%d)\n" : "RESULT: ALL PASS\n", fails);
	return fails ? 1 : 0;
}
