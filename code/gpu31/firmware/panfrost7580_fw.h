/*
 * panfrost7580_fw.h - firmware verdict + perf probing surface for Mali-T720.
 *
 * VERDICT: this GPU needs NO firmware blob.  Midgard (T720) has no
 * microcontroller and no firmware interface: kbase r15p0 contains zero
 * request_firmware/release_firmware calls (verified by grep over the shared
 * tree), the stock J700MUBU2BRC2 image carries no mali/g3d/t72 blob in any
 * lib/vendor partition, and upstream panfrost loads no blob on any Midgard
 * part (mali_csffw is a Valhall-CSF concept that postdates this GPU).
 * Kbase and panfrost drive the same Job Manager register interface
 * directly.  There is therefore no mali_fw loader to implement for the
 * j7elte; the stub exists only so the probe path documents the decision.
 *
 * What panfrost DOES need at probe (implemented in panfrost7580_fw.c):
 *   - an OPP table (over the Exynos7580 C table from gpu_exynos7580.c)
 *     with voltage-up-before-freq / voltage-down-after ordering, and
 *   - the PRFCNT probe sequence (BASE/MODE per kbase
 *     mali_kbase_instr_backend.c) for the perfcnt ioctl surface.
 *
 * [kb] drivers/gpu/arm/t7xx/r15p0/{mali_kbase_defs.h:1319 (SMC only),
 *      mali_midg_regmap.h:60-100, backend/gpu/mali_kbase_{devfreq,instr_backend}.c}
 */
#ifndef PANFROST7580_FW_H
#define PANFROST7580_FW_H

#include <linux/types.h>

struct device;

/*
 * Firmware requirement query.  Returns 0 (no blob) on all T720 revisions;
 * the out-param documents the reason for the log line.  Kept as a function
 * (not a #define) so a future SoC gate has one place to change.
 */
int pf7580_fw_required(u32 gpu_id, const char **why_not);

/*
 * Static OPP ladder ported from the stock backend table
 * (gpu_exynos7580.c gpu_dvfs_table_default[]: clk MHz, volt uV):
 *   160/900000 266/900000 350/1000000 440/1000000
 *   534/1000000 668/1100000 734/1100000 800/1100000
 * Frequencies are Hz for the OPP/devfreq APIs.
 */
struct pf7580_opp {
	unsigned long freq_hz;
	unsigned long volt_uv;
};

extern const struct pf7580_opp pf7580_opp_table[];
extern const int pf7580_opp_count;

int pf7580_register_opps(struct device *dev);
int pf7580_perfcnt_probe(void __iomem *regs, u32 jm_en, u32 shader_en,
			 u32 tiler_en, u32 mmu_l2_en, u32 as_nr);

/* PRFCNT register block (offsets into the 0x5000 window). */
#define PF7580_PRFCNT_BASE_LO	0x060
#define PF7580_PRFCNT_BASE_HI	0x064
#define PF7580_PRFCNT_CONFIG	0x068
#define PF7580_PRFCNT_JM_EN	0x06C
#define PF7580_PRFCNT_SHADER_EN	0x070
#define PF7580_PRFCNT_TILER_EN	0x074
#define PF7580_PRFCNT_MMU_L2_EN	0x07C

#define PF7580_PRFCNT_MODE_OFF		0
#define PF7580_PRFCNT_MODE_MANUAL	1

#endif /* PANFROST7580_FW_H */
