/*
 * panfrost7580_power.c - PM / clock / reset sequencing for Mali-T720
 *                        (Exynos 7580, j7elte) panfrost-class driver.
 *
 * Exact sequences are cloned field-by-field from the stock kbase backend
 * that already drives this SoC (r15p0 platform/exynos), then trimmed to the
 * subset a render-only DRM driver needs. Register offsets come from
 * mali_midg_regmap.h; every non-obvious number cites its source below.
 *
 * PROBE order (fail-stop, unwind in reverse):
 *   1. clk_get x6 by DT clock-name (gpu_exynos7580.c: gpu_get_clock).
 *      No regulator_get here yet: stay out of the PMIC until the clock
 *      framework has validated the CMU handles.
 *   2. regulator_get(NULL, "vdd_g3d") (gpu_exynos7580.c: gpu_regulator_init).
 *      NULL device is mandatory: stock DT has no vdd_g3d-supply phandle.
 *      Then regulator_enable() (reference #1; rail itself is always-on).
 *   3. Voltage check: read aclk_g3d rate, map to the OPP table, and vote
 *      that voltage so the rail matches whatever the CMU left behind.
 *   4. mux select write (gpu_get_clock tail: 0x1 to MUX_SEL_G3D0).
 *   5. Runtime PM bring-up (kbase: pm_runtime_enable + autosuspend 50 ms,
 *      gpu_validate_attrib_data; kbase additionally does
 *      pm_suspend_ignore_children which a DRM driver must NOT copy because
 *      child GEM fences must still wake the parent):
 *        pm_runtime_set_autosuspend_delay(dev, PF7580_AUTOSUSPEND_MS);
 *        pm_runtime_use_autosuspend(dev);
 *        pm_runtime_enable(dev);
 *   6. pm_runtime_get_sync(): vendor genpd powers pd-g3d
 *      (OPTION 0x1, G3D magic 0xFFFFF0CF to base+0x10, CONFIGURATION write,
 *      STATUS poll, pm_domains.c: exynos_pd_power), then our runtime-resume
 *      callback runs: clock + SOFT_RESET + cores.
 *   7. pm_runtime_put_autosuspend(): let the block idle-suspend after
 *      PF7580_AUTOSUSPEND_MS like kbase's pm_schedule_suspend(delay_time).
 *
 * RUNTIME RESUME order (= power on):
 *   clock on -> SOFT_RESET (+HARD_RESET fallback) -> cores on
 *   (L2 -> shader -> tiler). This is kbase kbase_pm_power_up +
 *   kbase_pm_init_hw collapsed to the Midgard-mandatory subset, with the
 *   Exynos wrappings (pd-g3d, aclk_g3d, vdd_g3d) spelled out.
 *
 * RUNTIME SUSPEND order (= power off), mirroring kbase pm_callback_runtime_off:
 *   cores off (READY==0 poll) -> aclk_g3d disable.
 *   genpd then drops pd-g3d itself. NEVER regulator_disable(vdd_g3d) here:
 *   the s2mpu03 driver ignores BUCK4 disable while g3d_en=1 anyway
 *   (s2mpu03.c: s2m_disable_regmap "disregard BUCK4 disable"), so calling it
 *   only corrupts the consumer refcount with zero hardware effect; and
 *   kbase's gpu_regulator_disable is likewise only called at module term,
 *   never on the runtime path.
 *
 * PMIC sync (why the driver does nothing special): at s2mpu03 probe with
 * g3d_en=1 the PMIC driver writes B4CTRL1[7:6]=00 (GPIO control of BUCK4),
 * LDO_EFUSE[4]=1 and L10CTRL[7:6]=00 (BUCK4/LDO10 tracking, s2mpu03.c probe).
 * On 7580 j7elte the DT comments out buck4en_pin/buck4en_addr, so no GPIO
 * toggling happens and the sync bits just keep LDO10 tracking the (static)
 * BUCK4 output. The rail is regulator-always-on + boot-on, so the GPU driver
 * never enables/disables it around power transitions; it only votes voltage
 * (pf7580_set_voltage). See docs/reports/gpu31-power.md "BUCK4/LDO10".
 *
 * LDO10 BUG CLASS (documented, not worked around in code): LDO10 DT node is
 * regulator-always-on with NO regulator-boot-on and NO consumer in this
 * tree. Any future driver that regulator_get("vdd_ldo10") + enable/disable
 * around GPU jobs will fight the PMIC sync bits: while BUCK4/LDO10 tracking
 * is armed, LDO10's enable field is slaved to BUCK4, so an I2C disable is
 * either ignored (rail stays up, refcount corrupted) or races the tracker
 * (rail glitches while the GPU reads descriptors). Rule: never claim
 * vdd_ldo10 from GPU code; leave the rail to its current always-on state.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include "panfrost7580_power.h"

/* Poll (val & mask) == want, 10 us steps. kbase RESET_TIMEOUT is 100 ms;
 * keep the same ceiling so a wedged GPU fails probe instead of hanging boot
 * (kbase gpu_power_set_reg()'s infinite spin is deliberately NOT copied). */
#define PF7580_RESET_TIMEOUT_US	100000
#define PF7580_POLL_STEP_US	10
#define PF7580_CORE_TIMEOUT_US	100000

static int pf7580_poll(void __iomem *regs, u32 reg, u32 mask, u32 want,
		       unsigned long timeout_us)
{
	unsigned long waited = 0;
	u32 val;

	while (waited < timeout_us) {
		val = readl(regs + reg);
		if ((val & mask) == want)
			return 0;
		udelay(PF7580_POLL_STEP_US);
		waited += PF7580_POLL_STEP_US;
	}
	return -ETIMEDOUT;
}

int pf7580_opp_voltage(int mhz)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pf7580_opps); i++) {
		if (pf7580_opps[i].mhz == mhz)
			return pf7580_opps[i].uv;
	}
	return -EINVAL;
}

/* Claim all six stock clocks in kbase gpu_get_clock order; fatal on the
 * first failure with earlier claims released (kbase unwind order). */
static int pf7580_claim_clks(struct pf7580_power *p)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->clks); i++) {
		p->clks[i] = clk_get(p->dev, pf7580_clk_names[i]);
		if (IS_ERR_OR_NULL(p->clks[i])) {
			int ret = p->clks[i] ? (int)PTR_ERR(p->clks[i]) :
				-ENODEV;
			dev_err(p->dev, "pf7580: clk_get(%s) failed: %d\n",
				pf7580_clk_names[i], ret);
			p->clks[i] = NULL;
			while (i-- > 0) {
				clk_put(p->clks[i]);
				p->clks[i] = NULL;
			}
			return ret;
		}
	}
	p->fout_g3d_pll = p->clks[1];
	p->aclk_g3d = p->clks[2];
	p->mout_g3d = p->clks[3];
	p->aclk_g3d_400 = p->clks[5];
	return 0;
}

static void pf7580_put_clks(struct pf7580_power *p)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(p->clks); i++) {
		if (p->clks[i]) {
			clk_put(p->clks[i]);
			p->clks[i] = NULL;
		}
	}
	p->fout_g3d_pll = p->aclk_g3d = p->mout_g3d = p->aclk_g3d_400 = NULL;
}

/* aclk_g3d on, guarded like kbase gpu_clock_on: refuse when the caller has
 * not established that the domain is up (runtime-PM resumed), skip when
 * already enabled (clk_g3d_status). The caller holds p->lock. */
static int pf7580_clk_on_locked(struct pf7580_power *p, bool domain_on)
{
	int ret;

	if (!domain_on)
		return -EPERM;
	if (p->clock_enabled)
		return 0;
	ret = clk_prepare_enable(p->aclk_g3d);
	if (ret) {
		dev_err(p->dev, "pf7580: aclk_g3d enable failed: %d\n", ret);
		return ret;
	}
	p->clock_enabled = true;
	return 0;
}

static void pf7580_clk_off_locked(struct pf7580_power *p)
{
	if (!p->clock_enabled)
		return;
	clk_disable_unprepare(p->aclk_g3d);
	p->clock_enabled = false;
}

/*
 * SOFT_RESET then wait RESET_COMPLETED; on timeout issue HARD_RESET once and
 * wait again. Mirrors kbase_pm_reset_do_normal (SOFT first, HARD fallback,
 * RESET_COMPLETED poll) minus the waitqueue: polling is fine at resume
 * context and keeps this helper callable without kbase's pm state machine.
 * Caller must hold p->lock with the clock on and the domain up.
 */
static int pf7580_soft_reset_locked(struct pf7580_power *p)
{
	int ret;

	writel(PF7580_GPU_COMMAND_SOFT_RESET, p->regs + PF7580_GPU_COMMAND);
	writel(PF7580_RESET_COMPLETED, p->regs + PF7580_GPU_IRQ_MASK);
	ret = pf7580_poll(p->regs, PF7580_GPU_IRQ_RAWSTAT,
			  PF7580_RESET_COMPLETED, PF7580_RESET_COMPLETED,
			  PF7580_RESET_TIMEOUT_US);
	if (!ret)
		goto done;

	dev_warn(p->dev, "pf7580: SOFT_RESET timed out, trying HARD_RESET\n");
	writel(PF7580_GPU_COMMAND_HARD_RESET, p->regs + PF7580_GPU_COMMAND);
	ret = pf7580_poll(p->regs, PF7580_GPU_IRQ_RAWSTAT,
			  PF7580_RESET_COMPLETED, PF7580_RESET_COMPLETED,
			  PF7580_RESET_TIMEOUT_US);
	if (ret)
		dev_err(p->dev, "pf7580: HARD_RESET timed out\n");
done:
	writel(PF7580_RESET_COMPLETED, p->regs + PF7580_GPU_IRQ_CLEAR);
	writel(0, p->regs + PF7580_GPU_IRQ_MASK);
	return ret;
}

/*
 * Cores on, L2 -> shader -> tiler (kbase core_type_to_reg order and
 * panfrost_gpu_power_on order agree: L2 first, then L1s). Each stage polls
 * its READY bits; a stage that does not come ready fails the power-up.
 * present_* are the *_PRESENT bitmaps read before PWRON. Caller holds lock,
 * clock on, reset complete.
 */
static int pf7580_cores_on_locked(struct pf7580_power *p,
				  u32 shader_present, u32 tiler_present,
				  u32 l2_present)
{
	int ret;

	writel(l2_present, p->regs + PF7580_L2_PWRON_LO);
	writel(0, p->regs + PF7580_L2_PWRON_HI);
	ret = pf7580_poll(p->regs, PF7580_L2_READY_LO, l2_present,
			  l2_present, PF7580_CORE_TIMEOUT_US);
	if (ret) {
		dev_err(p->dev, "pf7580: L2 PWRON timed out (want %08x)\n",
			l2_present);
		return ret;
	}

	writel(shader_present, p->regs + PF7580_SHADER_PWRON_LO);
	writel(0, p->regs + PF7580_SHADER_PWRON_HI);
	ret = pf7580_poll(p->regs, PF7580_SHADER_READY_LO, shader_present,
			  shader_present, PF7580_CORE_TIMEOUT_US);
	if (ret) {
		dev_err(p->dev, "pf7580: shader PWRON timed out (want %08x)\n",
			shader_present);
		return ret;
	}

	writel(tiler_present, p->regs + PF7580_TILER_PWRON_LO);
	writel(0, p->regs + PF7580_TILER_PWRON_HI);
	ret = pf7580_poll(p->regs, PF7580_TILER_READY_LO, tiler_present,
			  tiler_present, PF7580_CORE_TIMEOUT_US);
	if (ret) {
		dev_err(p->dev, "pf7580: tiler PWRON timed out (want %08x)\n",
			tiler_present);
		return ret;
	}
	return 0;
}

/* Cores off in reverse (tiler -> shader -> L2), polling READY==0. Used on
 * the runtime-suspend path; kbase likewise powers cores down before
 * clock-off while the domain is still up. */
static void pf7580_cores_off_locked(struct pf7580_power *p)
{
	int ret;

	writel(0, p->regs + PF7580_TILER_PWRON_LO);
	writel(0, p->regs + PF7580_TILER_PWRON_HI);
	ret = pf7580_poll(p->regs, PF7580_TILER_READY_LO, 0xFFFFFFFF, 0,
			  PF7580_CORE_TIMEOUT_US);
	if (ret)
		dev_warn(p->dev, "pf7580: tiler did not idle on power-off\n");

	writel(0, p->regs + PF7580_SHADER_PWRON_LO);
	writel(0, p->regs + PF7580_SHADER_PWRON_HI);
	ret = pf7580_poll(p->regs, PF7580_SHADER_READY_LO, 0xFFFFFFFF, 0,
			  PF7580_CORE_TIMEOUT_US);
	if (ret)
		dev_warn(p->dev, "pf7580: shaders did not idle on power-off\n");

	writel(0, p->regs + PF7580_L2_PWRON_LO);
	writel(0, p->regs + PF7580_L2_PWRON_HI);
	ret = pf7580_poll(p->regs, PF7580_L2_READY_LO, 0xFFFFFFFF, 0,
			  PF7580_CORE_TIMEOUT_US);
	if (ret)
		dev_warn(p->dev, "pf7580: L2 did not idle on power-off\n");
}

int pf7580_runtime_resume(struct pf7580_power *p)
{
	u32 shader_present, tiler_present, l2_present;
	int ret;

	if (!p)
		return -ENODEV;

	mutex_lock(&p->lock);

	/* 1. functional clock (kbase gpu_control_enable_clock). */
	ret = pf7580_clk_on_locked(p, true);
	if (ret)
		goto out;

	/* 2. reset the block (kbase kbase_pm_init_hw reset leg). */
	ret = pf7580_soft_reset_locked(p);
	if (ret)
		goto err_clk;

	/* 3. cores on L2 -> shader -> tiler with READY polls. */
	shader_present = readl(p->regs + PF7580_SHADER_PRESENT_LO);
	tiler_present = readl(p->regs + PF7580_TILER_PRESENT_LO);
	l2_present = readl(p->regs + PF7580_L2_PRESENT_LO);
	ret = pf7580_cores_on_locked(p, shader_present, tiler_present,
				     l2_present);
	if (ret)
		goto err_clk;

	mutex_unlock(&p->lock);
	return 0;

err_clk:
	pf7580_clk_off_locked(p);
out:
	mutex_unlock(&p->lock);
	return ret;
}

int pf7580_runtime_suspend(struct pf7580_power *p)
{
	if (!p)
		return -ENODEV;

	mutex_lock(&p->lock);
	pf7580_cores_off_locked(p);
	pf7580_clk_off_locked(p);
	mutex_unlock(&p->lock);
	return 0;
}

int pf7580_pm_suspend(struct device *dev, struct pf7580_power *p)
{
	/* Already idle-suspended: nothing to gate, genpd handles the rest. */
	if (pm_runtime_suspended(dev))
		return 0;
	return pf7580_runtime_suspend(p);
}

int pf7580_pm_resume(struct device *dev, struct pf7580_power *p)
{
	(void)dev;
	/* After system suspend pd-g3d was off, so GPU state is lost:
	 * always run the full power-on path. */
	return pf7580_runtime_resume(p);
}

int pf7580_set_rate(struct pf7580_power *p, unsigned long rate_hz)
{
	unsigned long cur;
	int ret;

	if (!p)
		return -ENODEV;

	mutex_lock(&p->lock);
	if (!p->clock_enabled) {
		ret = -EAGAIN;
		goto out;
	}

	cur = clk_get_rate(p->aclk_g3d);
	if (cur == rate_hz) {
		ret = 0;
		goto out;
	}

	/* Park on the safe clock, re-rate the PLL, switch back
	 * (kbase gpu_set_clock). */
	ret = clk_prepare_enable(p->aclk_g3d_400);
	if (ret)
		goto out;
	ret = clk_set_parent(p->mout_g3d, p->aclk_g3d_400);
	if (ret)
		goto out_park;
	ret = clk_set_rate(p->fout_g3d_pll, rate_hz);
	if (ret)
		goto out_park;
	ret = clk_set_parent(p->mout_g3d, p->fout_g3d_pll);
out_park:
	clk_disable_unprepare(p->aclk_g3d_400);
out:
	mutex_unlock(&p->lock);
	return ret;
}

int pf7580_set_voltage(struct pf7580_power *p, int uv)
{
	int cur, ret;

	if (!p)
		return -ENODEV;

	mutex_lock(&p->lock);
	if (!p->clock_enabled) {
		ret = -EAGAIN;
		goto out;
	}
	cur = regulator_get_voltage(p->vdd_g3d);
	if (cur == uv) {
		ret = 0;
		goto out;
	}
	ret = regulator_set_voltage(p->vdd_g3d, uv, uv);
	if (ret)
		dev_err(p->dev, "pf7580: vdd_g3d %d uV failed: %d\n", uv, ret);
out:
	mutex_unlock(&p->lock);
	return ret;
}

int pf7580_power_probe(struct device *dev, void __iomem *regs,
		       struct pf7580_power **out)
{
	struct pf7580_power *p;
	unsigned long rate;
	int uv, ret;

	if (!dev || !regs || !out)
		return -EINVAL;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->regs = regs;
	mutex_init(&p->lock);

	/* 1. clocks, by stock DT clock-name (kbase gpu_get_clock order). */
	ret = pf7580_claim_clks(p);
	if (ret)
		goto err;

	/* 2. regulator by global name: stock DT has no vdd_g3d-supply. */
	p->vdd_g3d = regulator_get(NULL, "vdd_g3d");
	if (IS_ERR_OR_NULL(p->vdd_g3d)) {
		ret = p->vdd_g3d ? PTR_ERR(p->vdd_g3d) : -ENODEV;
		dev_err(dev, "pf7580: regulator_get(vdd_g3d) failed: %d\n",
			ret);
		p->vdd_g3d = NULL;
		goto err;
	}
	ret = regulator_enable(p->vdd_g3d);
	if (ret) {
		dev_err(dev, "pf7580: vdd_g3d enable failed: %d\n", ret);
		goto err_reg;
	}

	/* 3. voltage must match the rate the CMU left behind. */
	rate = clk_get_rate(p->aclk_g3d);
	uv = pf7580_opp_voltage((int)(rate / 1000000));
	if (uv < 0) {
		dev_warn(dev, "pf7580: boot rate %lu Hz has no OPP entry\n",
			 rate);
	} else {
		ret = regulator_set_voltage(p->vdd_g3d, uv, uv);
		if (ret) {
			dev_err(dev, "pf7580: boot voltage %d uV failed: %d\n",
				uv, ret);
			goto err_vol;
		}
	}

	/* 4. runtime PM: 50 ms autosuspend like kbase, then power the block.
	 * The vendor bus notifier already attached us to pd-g3d, so the
	 * get_sync below sequences the domain (genpd) + our resume callback
	 * (clock + reset + cores) in one call. */
	pm_runtime_set_autosuspend_delay(dev, PF7580_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pf7580: initial power-up failed: %d\n", ret);
		pm_runtime_put_noidle(dev);
		goto err_pm;
	}
	pm_runtime_put_autosuspend(dev);

	*out = p;
	return 0;

err_pm:
	pm_runtime_disable(dev);
err_vol:
	regulator_disable(p->vdd_g3d);
err_reg:
	regulator_put(p->vdd_g3d);
	p->vdd_g3d = NULL;
err:
	pf7580_put_clks(p);
	devm_kfree(dev, p);
	return ret;
}

void pf7580_power_remove(struct pf7580_power *p)
{
	struct device *dev;

	if (!p)
		return;
	dev = p->dev;

	pm_runtime_get_sync(dev);
	pf7580_runtime_suspend(p);
	pm_runtime_put_noidle(dev);
	pm_runtime_disable(dev);

	if (p->vdd_g3d) {
		regulator_disable(p->vdd_g3d);
		regulator_put(p->vdd_g3d);
		p->vdd_g3d = NULL;
	}
	pf7580_put_clks(p);
	devm_kfree(dev, p);
}
