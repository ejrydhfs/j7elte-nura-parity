/*
 * panfrost7580_power.h - PM / clock / reset sequencing for Mali-T720
 *                        (Exynos 7580, j7elte) panfrost-class driver.
 *
 * Provenance (all read from the shared 3.10.108 tree, never modified):
 *  - clocks / regulator / DVFS order: drivers/gpu/arm/t7xx/r15p0/
 *    platform/exynos/{gpu_exynos7580.c,gpu_control.c,gpu_dvfs_api.c,
 *    gpu_notifier.c,mali_kbase_platform.h}
 *  - power-domain attach + on/off sequencing:
 *    arch/arm64/mach-exynos/pm_domains.c
 *  - PMIC BUCK4/LDO10 sync: drivers/regulator/s2mpu03.c,
 *    arch/arm64/boot/dts/exynos7580-j7elte_rev06.dts
 *  - reset / PWRON register map:
 *    drivers/gpu/arm/t7xx/r15p0/mali_midg_regmap.h
 *
 * Stock-DT invariants this code relies on (stock dt.img is never rebuilt,
 * so these are load-bearing, not advisory):
 *  - mali@0x11400000 carries clock-names fin_pll, fout_g3d_pll, aclk_g3d,
 *    mout_g3d, dout_aclk_g3d, aclk_g3d_400 and
 *    samsung,power-domain = <&pd_g3d>. There is NO vdd_g3d-supply property,
 *    so the regulator MUST be claimed with regulator_get(NULL, "vdd_g3d")
 *    (global name lookup, exactly as kbase does). A dev-based
 *    regulator_get(dev, "vdd_g3d") fails on the stock DT.
 *  - s2mpu03 DT has g3d_en = <1>; BUCK4/vdd_g3d is regulator-always-on +
 *    regulator-boot-on. The GPU driver only manages its own consumer
 *    refcount / voltage vote, never the rail itself.
 *  - There are NO clocks named sclk_g3d / gate_pclk on 7580 G3D
 *    (drivers/clk/samsung/clk-exynos7580.c GATE() list: aclk and pclk gates only,
 *    all CLK_IGNORE_UNUSED). Do NOT clk_get() those names; probe would fail.
 *
 * Wiring for the owning driver (dev_pm_ops stays with the driver so that
 * drvdata ownership never collides with this helper):
 *
 *	static int drv_runtime_suspend(struct device *dev)
 *	{ return pf7580_runtime_suspend(drv_to_power(dev)); }
 *	static int drv_runtime_resume(struct device *dev)
 *	{ return pf7580_runtime_resume(drv_to_power(dev)); }
 *	static int drv_suspend(struct device *dev)
 *	{ return pf7580_pm_suspend(dev, drv_to_power(dev)); }
 *	static int drv_resume(struct device *dev)
 *	{ return pf7580_pm_resume(dev, drv_to_power(dev)); }
 *	static const struct dev_pm_ops drv_pm_ops = {
 *		SET_SYSTEM_SLEEP_PM_OPS(drv_suspend, drv_resume)
 *		SET_RUNTIME_PM_OPS(drv_runtime_suspend, drv_runtime_resume,
 *				   NULL)
 *	};
 */

#ifndef PANFROST7580_POWER_H
#define PANFROST7580_POWER_H

#include <linux/types.h>

struct device;

/* Autosuspend idle delay (ms). Matches kbase default: platform
 * runtime_pm_delay_time is 50 when the attribute is 0
 * (mali_kbase_platform.c: gpu_validate_attrib_data). */
#define PF7580_AUTOSUSPEND_MS	50

/* Reset / IRQ register map (mali_midg_regmap.h). */
#define PF7580_GPU_COMMAND		0x030
#define PF7580_GPU_COMMAND_SOFT_RESET	0x01
#define PF7580_GPU_COMMAND_HARD_RESET	0x02
#define PF7580_GPU_IRQ_RAWSTAT		0x020
#define PF7580_GPU_IRQ_CLEAR		0x024
#define PF7580_GPU_IRQ_MASK		0x028
#define PF7580_RESET_COMPLETED		(1u << 8)

/* Core power registers (mali_midg_regmap.h). */
#define PF7580_SHADER_PRESENT_LO	0x100
#define PF7580_SHADER_PRESENT_HI	0x104
#define PF7580_TILER_PRESENT_LO		0x110
#define PF7580_TILER_PRESENT_HI		0x114
#define PF7580_L2_PRESENT_LO		0x120
#define PF7580_L2_PRESENT_HI		0x124
#define PF7580_SHADER_READY_LO		0x140
#define PF7580_SHADER_READY_HI		0x144
#define PF7580_TILER_READY_LO		0x150
#define PF7580_TILER_READY_HI		0x154
#define PF7580_L2_READY_LO		0x160
#define PF7580_L2_READY_HI		0x164
#define PF7580_SHADER_PWRON_LO		0x180
#define PF7580_SHADER_PWRON_HI		0x184
#define PF7580_TILER_PWRON_LO		0x190
#define PF7580_TILER_PWRON_HI		0x194
#define PF7580_L2_PWRON_LO		0x1A0
#define PF7580_L2_PWRON_HI		0x1A4

/* Stock 7580 DVFS operating points actually programmed by kbase
 * (gpu_exynos7580.c: gpu_dvfs_table_default[]): { clock MHz, voltage uV }.
 * Rail limits are 600000..1300000 uV (BUCK4 DT node); the driver must stay
 * inside the rows below, never the rail extremes. */
struct pf7580_opp {
	int mhz;
	int uv;
};

static const struct pf7580_opp pf7580_opps[] = {
	{ 800, 1100000 },
	{ 734, 1100000 },
	{ 668, 1100000 },
	{ 534, 1000000 },
	{ 440, 1000000 },
	{ 350, 1000000 },
	{ 266,  900000 },
	{ 160,  900000 },
};

struct pf7580_power {
	struct device	*dev;
	void __iomem	*regs;
	struct clk	*clks[6];	/* parallel to pf7580_clk_names */
	struct clk	*aclk_g3d;	/* == clks[2], functional clock */
	struct clk	*mout_g3d;	/* == clks[3], re-rate parking mux */
	struct clk	*fout_g3d_pll;	/* == clks[1], rate knob */
	struct clk	*aclk_g3d_400;	/* == clks[5], safe parking clock */
	struct regulator *vdd_g3d;	/* BUCK4, global lookup "vdd_g3d" */
	struct mutex	lock;		/* serialises power/clk/voltage */
	bool		clock_enabled;	/* aclk_g3d prepare_enable state */
};

/* Stock DT clock-names in clk_get order (kbase gpu_get_clock). Index 2/3/1/5
 * are aliased above; 0 (fin_pll) and 4 (dout_aclk_g3d) are get-only. */
static const char * const pf7580_clk_names[6] = {
	"fin_pll", "fout_g3d_pll", "aclk_g3d",
	"mout_g3d", "dout_aclk_g3d", "aclk_g3d_400",
};

/*
 * Probe: claim clocks + regulator, park the G3D mux, enable runtime PM,
 * power the block through the exact stock sequence, and idle-suspend.
 * regs must already be ioremapped by the caller (IORESOURCE_MEM,0 =
 * 0x11400000/0x5000). The power domain needs NO driver code: the vendor
 * bus notifier (pm_domains.c: exynos_pm_notifier_call, BUS_NOTIFY_BIND_DRIVER)
 * already added this device to pd-g3d from samsung,power-domain, so
 * pm_runtime_get_sync() below powers pd-g3d via genpd automatically.
 * Returns 0 with *out filled, or a negative errno (clocks/regulator are
 * released again on failure, mirroring kbase gpu_get_clock unwind order).
 */
int pf7580_power_probe(struct device *dev, void __iomem *regs,
		       struct pf7580_power **out);

/* Remove: cancel runtime PM, force the block off synchronously, release
 * clocks + regulator. The domain itself is detached by the vendor bus
 * notifier on unbind; never touch PMU CONFIGURATION here. */
void pf7580_power_remove(struct pf7580_power *p);

/*
 * Runtime PM callbacks (owning driver wires these into dev_pm_ops):
 *  - suspend: cores off (L2/shader/tiler PWRON=0 + READY==0 poll),
 *    aclk_g3d disable. genpd then drops pd-g3d.
 *  - resume: aclk_g3d enable, SOFT_RESET (+HARD_RESET fallback),
 *    cores on L2 -> shader -> tiler with READY polls,
 *    boot OPP voltage check.
 * Both are best-effort on the suspend path (dev_warn + still gate the
 * clock) and strict on probe/resume.
 */
int pf7580_runtime_suspend(struct pf7580_power *p);
int pf7580_runtime_resume(struct pf7580_power *p);

/* Fault-recovery core power cycle (gpu31-readback): cores off + clock
 * gate, then the full power-on sequence (clock, SOFT/HARD_RESET with
 * RESET_COMPLETED poll, L2/shader/tiler PWRON with READY polls). Needed
 * because a JM POWER_FAULT latch (JS_STATUS 0x41) survives the GPU
 * SOFT/HARD reset issued by p7580_job_recover(): without a cycle every
 * following job on that slot reports the same fault. Process context,
 * caller holds a runtime-PM ref; the MMU AS0 reprogram and JOB_INT
 * re-arm are done by the jobs slice after this returns. */
int pf7580_power_cycle(struct pf7580_power *p);

/*
 * System-sleep callbacks:
 *  - suspend: if the device is not already runtime-suspended, run the
 *    runtime-suspend path so the clock is gated before the SoC sleeps.
 *    No PM notifier is used: this tree's genpd owns domain sequencing and
 *    a second notifier (like kbase's gpu_pm_notifier) would double-drive it
 *    under a DRM driver that already has dev_pm_ops.
 *  - resume: full power-on (clock + reset + cores), unconditionally: after
 *    system suspend the GPU state is lost (pd-g3d was off).
 */
int pf7580_pm_suspend(struct device *dev, struct pf7580_power *p);
int pf7580_pm_resume(struct device *dev, struct pf7580_power *p);

/*
 * Re-rate fout_g3d_pll with the stock parking dance (kbase gpu_set_clock):
 * enable aclk_g3d_400 -> parent mout_g3d to it -> set_rate -> parent back ->
 * disable parking clock. Caller must hold a runtime-PM reference across the
 * call; refuses with -EAGAIN when the clock is gated, -EPERM when called
 * with the domain off is reported by the caller checking power state first.
 * Voltage ordering is the CALLER's job (GPU_SET_CLK_VOL rule, dvfs_api.c):
 * scaling up: pf7580_set_voltage() first, then pf7580_set_rate();
 * scaling down: pf7580_set_rate() first, then pf7580_set_voltage().
 * Reversing the up-order browns out the 534->668 / 668->800 steps, which
 * cross the 1.00 V -> 1.10 V corner (see docs/reports/gpu31-power.md F4).
 */
int pf7580_set_rate(struct pf7580_power *p, unsigned long rate_hz);

/*
 * Vote a voltage on vdd_g3d. No-op when already at target; refuses with
 * -EAGAIN while the clock is gated (mirrors kbase gpu_set_voltage's
 * power-off guard: programming VSEL with the domain dark applies on the
 * next power-up at an uncontrolled point of the sequence).
 */
int pf7580_set_voltage(struct pf7580_power *p, int uv);

/* Look up the stock voltage for a clock rate (MHz); used at probe/resume to
 * make sure the rail matches the rate the bootloader/CMU left behind. */
int pf7580_opp_voltage(int mhz);

#endif /* PANFROST7580_POWER_H */
