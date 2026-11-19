/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_devfreq.h - GEM-owned DVFS hooks for the T720 panfrost port.
 *
 * v5.2 reference: panfrost_devfreq.c/.h against modern
 * dev_pm_opp_set_rate/clkname OPP. This tree has the OLD OPP API
 * (opp_add/opp_enable/opp_disable, opp.h) + old devfreq
 * (devfreq_add_device with governor name string, devfreq.h) and
 * CONFIG_PM_DEVFREQ=y + CONFIG_PM_OPP=y. The kbase DVFS source is
 * gpu_exynos7580.c:46-55 (8 stock rows, freq MHz + voltage uV +
 * thresholds) + asv-exynos7580_cal.c (per-chip ASV trim).
 *
 * This header + its companion .c are the ONLY devfreq implementation
 * for the panfrost driver (F-DV1). The power handle is struct
 * pf7580_power (code/gpu31/power/panfrost7580_power.h:
 * pf7580_set_rate/pf7580_set_voltage). The instance is per-device via
 * a file-static single-G3D handle (F-DV2: never dev drvdata -- the
 * owning DRM driver's drvdata belongs to the driver). OPP rows are
 * unwound on init failure (F-DV3). The jobs slice feeds load via
 * p7580_devfreq_set_busy() on each retired job.
 *
 * The pure-C ladder/ceil helpers and their host unit test live in the
 * standalone devfreq/ slice (G31_Devfreq owner) and are not duplicated
 * here; both slices share the same table values and voltage-first rule
 * by review agreement (arch report section 4 dedups at land).
 */

#ifndef PANFROST7580_DEVFREQ_H
#define PANFROST7580_DEVFREQ_H

#include <linux/types.h>
#include <linux/mutex.h>

struct device;
struct devfreq;
struct devfreq_dev_status;
struct pf7580_power;
struct p7580_dvfs;

struct p7580_dvfs_row {
	unsigned long freq_hz;
	unsigned long volt_uv;
};

int p7580_devfreq_init(struct device *dev, struct pf7580_power *power,
		       struct p7580_dvfs **out);
void p7580_devfreq_fini(struct p7580_dvfs *dvfs);
void p7580_devfreq_set_busy(struct p7580_dvfs *dvfs,
			    unsigned long busy_ns, unsigned long idle_ns);
int p7580_devfreq_target(struct device *dev, unsigned long *freq, u32 flags);
int p7580_devfreq_get_status(struct device *dev,
			     struct devfreq_dev_status *stat);
int p7580_devfreq_get_cur(struct device *dev, unsigned long *freq);
int p7580_devfreq_suspend(struct p7580_dvfs *dvfs);
int p7580_devfreq_resume(struct p7580_dvfs *dvfs);

#endif /* PANFROST7580_DEVFREQ_H */
