/*
 * panfrost7580_fw.c - T720 needs no firmware; OPP + PRFCNT probe-init.
 *
 * Firmware: pf7580_fw_required() returns 0 for every GPU_ID.  The T720
 * (product 0x0720, any major/minor) is a Midgard part with no MCU and no
 * firmware load interface; neither kbase r15p0 nor mainline panfrost
 * references a blob for it.  If a future caller passes a non-T720 ID the
 * function still returns 0 but names the ID so the log shows the gate held.
 *
 * OPP table: the stock DT has no operating-points(-v2) node for the GPU
 * (gpu31-dt.md section 2: mali node carries exactly 6 properties), and the
 * stock frequencies live in C (gpu_exynos7580.c).  pf7580_register_opps()
 * mirrors that: each table entry becomes one OPP with dev_pm_opp_add()
 * (3.10-era include/linux/opp.h: opp_add(dev, Hz, uV)), so that the stock
 * devfreq governor + kbase-style target() can resolve (freq -> voltage).
 * Callers must still implement voltage-up-before-clk_set_rate on the way up
 * and voltage-down-after on the way down (kbase_devfreq_target,
 * backend/gpu/mali_kbase_devfreq.c:60-125); this file only registers data.
 *
 * Perfcnt init: pf7580_perfcnt_probe() reproduces the kbase enable preamble
 * without submitting work: PRFCNT_CONFIG <- MODE_OFF, then BASE_LO/HI and
 * the JM/SHADER/TILER/MMU_L2 enable masks for the given address space, then
 * PRFCNT_CONFIG <- MODE_MANUAL (kbase mali_kbase_instr_backend.c:145-181),
 * and returns -EBUSY while GPU_STATUS bit 2 (PRFCNT_ACTIVE) reads set
 * (mali_midg_regmap.h GPU_STATUS_PRFCNT_ACTIVE).  Actual sampling
 * (PRFCNT_CLEAR/SAMPLE via GPU_CMD, IRQ 16 handling) belongs to the perfcnt
 * ioctl slice, not to probe.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/opp.h>
#include <linux/err.h>

#include "panfrost7580_fw.h"

int pf7580_fw_required(u32 gpu_id, const char **why_not)
{
	static const char msg[] =
		"Midgard T720 has no MCU/firmware interface; "
		"kbase and panfrost drive the Job Manager directly";

	if (why_not)
		*why_not = msg;

	/* Any T720 revision: no blob.  Non-T720 IDs cannot occur on this
	 * SoC binding, but the gate logs them rather than loading. */
	if (((gpu_id >> 16) & 0xFFFFu) != 0x0720u)
		pr_warn("panfrost7580: unexpected GPU_ID 0x%08x, still no firmware\n",
			gpu_id);

	return 0;
}

const struct pf7580_opp pf7580_opp_table[] = {
	{ 160000000UL,  900000UL },
	{ 266000000UL,  900000UL },
	{ 350000000UL, 1000000UL },
	{ 440000000UL, 1000000UL },
	{ 534000000UL, 1000000UL },
	{ 668000000UL, 1100000UL },
	{ 734000000UL, 1100000UL },
	{ 800000000UL, 1100000UL },
};

const int pf7580_opp_count = ARRAY_SIZE(pf7580_opp_table);

int pf7580_register_opps(struct device *dev)
{
	int i, ret;

	for (i = 0; i < pf7580_opp_count; i++) {
		ret = opp_add(dev, pf7580_opp_table[i].freq_hz,
			      pf7580_opp_table[i].volt_uv);
		if (ret) {
			dev_err(dev, "OPP %lu Hz / %lu uV rejected: %d\n",
				pf7580_opp_table[i].freq_hz,
				pf7580_opp_table[i].volt_uv, ret);
			return ret;
		}
	}

	return 0;
}

int pf7580_perfcnt_probe(void __iomem *regs, u32 jm_en, u32 shader_en,
			 u32 tiler_en, u32 mmu_l2_en, u32 as_nr)
{
	u32 status;
	u32 config = (as_nr << 4) | PF7580_PRFCNT_MODE_OFF;

	status = readl(regs + 0x034); /* GPU_STATUS */
	if (status & (1u << 2)) /* PRFCNT_ACTIVE */
		return -EBUSY;

	writel(config, regs + PF7580_PRFCNT_CONFIG);
	writel(0, regs + PF7580_PRFCNT_BASE_LO);
	writel(0, regs + PF7580_PRFCNT_BASE_HI);
	writel(jm_en, regs + PF7580_PRFCNT_JM_EN);
	writel(shader_en, regs + PF7580_PRFCNT_SHADER_EN);
	/*
	 * Tiler enable goes through verbatim here.  If the silicon turns out
	 * to carry the BASE_HW_ISSUE_8186 erratum (kbase writes TILER_EN = 0
	 * before MODE_MANUAL and re-enables after), the perfcnt ioctl slice
	 * owns that quirk, next to the sampling path - not probe.
	 */
	writel(tiler_en, regs + PF7580_PRFCNT_TILER_EN);
	writel(mmu_l2_en, regs + PF7580_PRFCNT_MMU_L2_EN);
	writel((as_nr << 4) | PF7580_PRFCNT_MODE_MANUAL,
	       regs + PF7580_PRFCNT_CONFIG);

	return 0;
}
