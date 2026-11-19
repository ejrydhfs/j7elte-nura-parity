/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_devfreq.c - devfreq/OPP DVFS hooks for the T720 panfrost port.
 *
 * OPP registration uses the 3.10 API (opp_add(freq Hz, uV), opp_enable;
 * opp.h). devfreq uses devfreq_add_device(dev, profile,
 * "simple_ondemand", NULL) (devfreq.h:179). Governor choice:
 * "simple_ondemand" (CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND=y in the vendor
 * .config) matches kbase behaviour (gpu_exynos7580.c governor attrs,
 * NULL governor data -> stock 90/5 defaults).
 *
 * This file is the ONLY devfreq implementation for the panfrost driver
 * (F-DV1: no second copy -- the standalone devfreq/ slice's pure-C
 * ladder/ceil helpers were absorbed into the host test, not duplicated
 * here). The instance is per-device (F-DV1: embedded profile + freq
 * table, no shared static), resolved through a file-static single-G3D
 * handle (F-DV2: never dev_get/set_drvdata -- the owning DRM driver's
 * drvdata belongs to the driver). OPP rows are unwound on init failure
 * (F-DV3: opp_disable in reverse before returning).
 *
 * Freq table: unsigned int Hz rows (this tree's devfreq core reports
 * stats in the same units; the canonical ladder lives here). The boot
 * row is 350 MHz @ 1.00 V -- what the CMU leaves behind and what kbase
 * starts polling from. Voltage/clock order (power-slice contract):
 * scale-UP programs voltage first then rate; scale-DOWN programs rate
 * first then voltage. Both go through the power slice
 * (pf7580_set_voltage / pf7580_set_rate), which refuses with -EAGAIN
 * while the clock is gated -- target() propagates that (devfreq
 * retries on the next poll).
 */

#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/opp.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include "panfrost7580_devfreq.h"
#include "panfrost7580_power.h"

/* Stock ladder, ascending (kbase gpu_exynos7580.c
 * gpu_dvfs_table_default[]: MHz/mV, verbatim to Hz/uV). */
static const struct p7580_dvfs_row p7580_dvfs_table[] = {
	{ 160000000u,  900000u },
	{ 266000000u,  900000u },
	{ 350000000u, 1000000u },
	{ 440000000u, 1000000u },
	{ 534000000u, 1000000u },
	{ 668000000u, 1100000u },
	{ 734000000u, 1100000u },
	{ 800000000u, 1100000u },
};

#define P7580_DVFS_ROWS \
	(sizeof(p7580_dvfs_table) / sizeof(p7580_dvfs_table[0]))
#define P7580_DVFS_BOOT_HZ	350000000u

struct p7580_dvfs {
	struct device *dev;
	struct pf7580_power *power;
	struct devfreq *df;
	/* Per-instance profile (F-DV1): no shared static. freq_table is
	 * a private copy freed in fini; the devfreq core may also keep
	 * its own copy via profile->exit. */
	struct devfreq_dev_profile profile;
	unsigned int *ftab;
	struct mutex lock;
	unsigned long busy_ns;
	unsigned long idle_ns;
	unsigned long cur_hz;
};

/* Single G3D block on 7580: exactly one live instance (F-DV2: the
 * owning DRM driver's dev drvdata is left alone; callbacks resolve
 * through this handle and verify dev == dvfs->dev, the same pattern
 * as kbase's global pkbdev). init returns -EBUSY while held. */
static struct p7580_dvfs *p7580_dvfs_g;

static struct p7580_dvfs *p7580_dvfs_get(struct device *dev)
{
	if (p7580_dvfs_g && p7580_dvfs_g->dev == dev)
		return p7580_dvfs_g;
	return NULL;
}

int p7580_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct p7580_dvfs *dvfs = p7580_dvfs_get(dev);
	struct opp *opp;
	unsigned long opp_freq, opp_volt;
	unsigned long cur;
	int ret;

	if (!dvfs || !freq)
		return -EINVAL;

	mutex_lock(&dvfs->lock);
	opp_freq = *freq;
	rcu_read_lock();
	/* Flags pass through exactly like kbase_devfreq_target: plain
	 * polls arrive with 0 (ceil to the least upper bound); the core
	 * sets DEVFREQ_FLAG_LEAST_UPPER_BOUND when max_freq clamps the
	 * request from above. Forwarding them (instead of forcing 0) is
	 * what makes a non-OPP max_freq ceiling resolve down to the
	 * right OPP instead of up past it. */
	opp = devfreq_recommended_opp(dev, &opp_freq, flags);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		mutex_unlock(&dvfs->lock);
		return PTR_ERR(opp);
	}
	opp_freq = opp_get_freq(opp);
	opp_volt = opp_get_voltage(opp);
	rcu_read_unlock();

	cur = dvfs->cur_hz;
	if (opp_freq == cur) {
		*freq = cur;
		mutex_unlock(&dvfs->lock);
		return 0;
	}

	/* Voltage-first on the way up, clock-first on the way down
	 * (power header rule: the 534/668 steps cross the 1.00->1.10 V
	 * corner; clock-before-voltage there browns out). */
	if (opp_freq > cur) {
		ret = pf7580_set_voltage(dvfs->power, (int)opp_volt);
		if (ret) {
			mutex_unlock(&dvfs->lock);
			return ret;
		}
		ret = pf7580_set_rate(dvfs->power, opp_freq);
	} else {
		ret = pf7580_set_rate(dvfs->power, opp_freq);
		if (ret) {
			mutex_unlock(&dvfs->lock);
			return ret;
		}
		ret = pf7580_set_voltage(dvfs->power, (int)opp_volt);
	}
	if (!ret) {
		dvfs->cur_hz = opp_freq;
		*freq = opp_freq;
	}
	mutex_unlock(&dvfs->lock);
	return ret;
}

int p7580_devfreq_get_status(struct device *dev,
			     struct devfreq_dev_status *stat)
{
	struct p7580_dvfs *dvfs = p7580_dvfs_get(dev);

	if (!dvfs || !stat)
		return -EINVAL;
	if (dvfs->dev != dev)
		return -ENODEV;
	mutex_lock(&dvfs->lock);
	stat->total_time = dvfs->busy_ns + dvfs->idle_ns;
	stat->busy_time = dvfs->busy_ns;
	stat->current_frequency = dvfs->cur_hz;
	stat->private_data = NULL;
	dvfs->busy_ns = 0;
	dvfs->idle_ns = 0;
	mutex_unlock(&dvfs->lock);
	return 0;
}

int p7580_devfreq_get_cur(struct device *dev, unsigned long *freq)
{
	struct p7580_dvfs *dvfs = p7580_dvfs_get(dev);

	if (!dvfs || !freq)
		return -EINVAL;
	*freq = dvfs->cur_hz;
	return 0;
}

/* Jobs-slice feed: called on each retired job (busy = GPU-active ns)
 * and on idle gaps (idle = wall ns). Accumulates until the governor's
 * get_dev_status drains it. */
void p7580_devfreq_set_busy(struct p7580_dvfs *dvfs,
			    unsigned long busy_ns, unsigned long idle_ns)
{
	if (!dvfs)
		return;
	mutex_lock(&dvfs->lock);
	dvfs->busy_ns += busy_ns;
	dvfs->idle_ns += idle_ns;
	mutex_unlock(&dvfs->lock);
}

int p7580_devfreq_init(struct device *dev, struct pf7580_power *power,
		       struct p7580_dvfs **out)
{
	struct p7580_dvfs *dvfs;
	unsigned int i, nreg = 0;
	int ret;

	if (!dev || !power || !out)
		return -EINVAL;
	if (p7580_dvfs_g)
		return -EBUSY;

	dvfs = kzalloc(sizeof(*dvfs), GFP_KERNEL);
	if (!dvfs)
		return -ENOMEM;
	dvfs->dev = dev;
	dvfs->power = power;
	mutex_init(&dvfs->lock);

	dvfs->ftab = kzalloc(P7580_DVFS_ROWS * sizeof(*dvfs->ftab),
			     GFP_KERNEL);
	if (!dvfs->ftab) {
		kfree(dvfs);
		return -ENOMEM;
	}
	for (i = 0; i < P7580_DVFS_ROWS; i++)
		dvfs->ftab[i] = (unsigned int)p7580_dvfs_table[i].freq_hz;
	dvfs->profile.initial_freq = P7580_DVFS_BOOT_HZ;
	dvfs->profile.polling_ms = 100;
	dvfs->profile.target = p7580_devfreq_target;
	dvfs->profile.get_dev_status = p7580_devfreq_get_status;
	dvfs->profile.get_cur_freq = p7580_devfreq_get_cur;
	dvfs->profile.freq_table = dvfs->ftab;
	dvfs->profile.max_state = P7580_DVFS_ROWS;
	dvfs->cur_hz = P7580_DVFS_BOOT_HZ;

	for (i = 0; i < P7580_DVFS_ROWS; i++) {
		ret = opp_add(dev, p7580_dvfs_table[i].freq_hz,
			      p7580_dvfs_table[i].volt_uv);
		if (ret)
			goto err_opp;
		nreg++;
		ret = opp_enable(dev, p7580_dvfs_table[i].freq_hz);
		if (ret)
			goto err_opp;
	}

	/* Publish before the poller can see us: the governor START event
	 * inside devfreq_add_device queues the first poll. */
	p7580_dvfs_g = dvfs;
	dvfs->df = devfreq_add_device(dev, &dvfs->profile,
				      "simple_ondemand", NULL);
	if (IS_ERR(dvfs->df)) {
		ret = PTR_ERR(dvfs->df);
		dvfs->df = NULL;
		p7580_dvfs_g = NULL;
		goto err_opp;
	}

	*out = dvfs;
	return 0;

err_opp:
	/* F-DV3: unwind registered OPPs in reverse (opp_disable has no
	 * remove on 3.10; disabling restores the pre-probe table). */
	while (nreg--)
		opp_disable(dev, p7580_dvfs_table[nreg].freq_hz);
	kfree(dvfs->ftab);
	kfree(dvfs);
	return ret;
}

void p7580_devfreq_fini(struct p7580_dvfs *dvfs)
{
	unsigned int i;

	if (!dvfs)
		return;
	if (dvfs->df)
		devfreq_remove_device(dvfs->df);
	/* opp_disable restores the pre-probe table (no opp_remove on
	 * this tree's OPP API); removal never fails. */
	for (i = 0; i < P7580_DVFS_ROWS; i++)
		opp_disable(dvfs->dev, p7580_dvfs_table[i].freq_hz);
	if (p7580_dvfs_g == dvfs)
		p7580_dvfs_g = NULL;
	kfree(dvfs->ftab);
	kfree(dvfs);
}

int p7580_devfreq_suspend(struct p7580_dvfs *dvfs)
{
	if (!dvfs || !dvfs->df)
		return 0;
	return devfreq_suspend_device(dvfs->df);
}

int p7580_devfreq_resume(struct p7580_dvfs *dvfs)
{
	if (!dvfs || !dvfs->df)
		return 0;
	return devfreq_resume_device(dvfs->df);
}
