/*
 * pf7580_devfreq.c - panfrost devfreq/DVFS for Mali-T720 on 3.10.108.
 *
 * Follows the kbase backend shape (t7xx/r15p0/backend/gpu/
 * mali_kbase_devfreq.c) ported onto the power slice
 * (code/gpu31/power/: pf7580_set_rate / pf7580_set_voltage, parking
 * dance, -EAGAIN while gated):
 *
 *  - init: OPP ladder (opp_add + opp_enable, ascending) -> freq_table
 *    (ascending Hz, kmalloc_array like kbase_devfreq_init_freq_table)
 *    -> seed boot OPP 350 MHz (PF7580_DVFS_BOOT_HZ) ->
 *    devfreq_add_device ("simple_ondemand", NULL governor data: stock
 *    DFSO 90/5 defaults, no PM-QoS leg) with polling_ms 100 (kbase
 *    parity). The profile is per-instance (embedded in struct
 *    pf7580_dvfs, like kbase's kbdev->devfreq_profile): a static shared
 *    profile would corrupt freq_table/max_state on a second init.
 *  - target: devfreq_recommended_opp() ceil/floor by the caller's flags
 *    (LUB/GLB, under rcu_read_lock like kbase_devfreq_target and the MIF
 *    bus driver), skip on no-change, then GPU_SET_CLK_VOL order (up:
 *    V->f, down: f->V). On -EAGAIN from the power slice (clock gated /
 *    runtime suspended) leave state untouched and propagate:
 *    devfreq_monitor() logs it and retries next poll.
 *  - suspend/resume: devfreq suspend/resume_device wrappers (NULL-safe)
 *    for dev_pm_ops; power sequencing stays in the power slice.
 *  - max clamp: devfreq max_freq (the panfrost equivalent of the kbase
 *    TMU max-lock from thermal patch 0001), applied through
 *    update_devfreq() exactly like the max_freq sysfs store.
 *
 * Lookup: this slice never touches dev drvdata (the owning DRM driver
 * keeps it for its own struct; overwriting it would break the driver's
 * remove path). The single G3D block is resolved through a file-static
 * handle, the same pattern as kbase's global pkbdev
 * (platform/exynos/gpu_notifier.c); callbacks verify dev == dvfs->dev.
 *
 * Concurrency: devfreq core already serialises target()/get_status()
 * under devfreq->lock on 3.10 (update_devfreq + monitor take it); the
 * slice mutex additionally makes the OPP lookup + two-step actuation
 * atomic against concurrent sysfs max_freq writes that re-enter
 * target() through a second update_devfreq().
 */

#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/opp.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include "pf7580_devfreq.h"

/* Power-slice actuation (code/gpu31/power/panfrost7580_power.h). The
 * power handle is intentionally opaque here: this slice must build
 * without pulling the whole power header into the profile path. */
int pf7580_set_rate(void *power, unsigned long rate_hz);
int pf7580_set_voltage(void *power, int uv);

/* drivers/devfreq/devfreq.c:209 (EXPORT_SYMBOL), contract in
 * drivers/devfreq/governor.h:29: caller must hold devfreq->lock.
 * No public prototype on 3.10 (internal governor header only), so it
 * is declared here for pf7580_dvfs_set_max(), which mirrors the
 * max_freq sysfs store (devfreq.c store_max_freq). */
extern int update_devfreq(struct devfreq *devfreq);

struct pf7580_dvfs {
	struct device *dev;
	void *power;
	struct devfreq *df;
	struct devfreq_dev_profile profile; /* per-instance, not static */
	unsigned int *ftab;	/* profile.freq_table backing */
	struct mutex lock;	/* OPP lookup + V/f steps + counters */
	unsigned long long busy_ns;
	unsigned long long idle_ns;
	unsigned long cur_hz;	/* last committed OPP */
};

/* Single G3D block on 7580: exactly one live instance. init returns
 * -EBUSY while the handle is held. */
static struct pf7580_dvfs *pf7580_dvfs_g;

static struct pf7580_dvfs *pf7580_dvfs_get(struct device *dev)
{
	if (pf7580_dvfs_g && pf7580_dvfs_g->dev == dev)
		return pf7580_dvfs_g;
	return NULL;
}

int pf7580_dvfs_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct pf7580_dvfs *dvfs = pf7580_dvfs_get(dev);
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
	 * request from above (update_devfreq, devfreq.c:188-195) and
	 * clears it when min_freq lifts it from below. Forwarding them
	 * (instead of forcing 0) is what makes a non-OPP max_freq ceiling
	 * resolve down to the right OPP instead of up past it. */
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

	/* GPU_SET_CLK_VOL order (dvfs_api.c:20-31; kbase_devfreq_target):
	 * scale-UP programs the voltage first then the rate (the
	 * 534->668 step crosses the 1.00 V -> 1.10 V corner;
	 * clock-before-voltage browns out: power report F4); scale-DOWN
	 * programs the rate first then the voltage. */
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

int pf7580_dvfs_get_status(struct device *dev,
			   struct devfreq_dev_status *stat)
{
	struct pf7580_dvfs *dvfs = pf7580_dvfs_get(dev);

	if (!dvfs || !stat)
		return -EINVAL;
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

int pf7580_dvfs_get_cur(struct device *dev, unsigned long *freq)
{
	struct pf7580_dvfs *dvfs = pf7580_dvfs_get(dev);

	if (!dvfs || !freq)
		return -EINVAL;
	*freq = dvfs->cur_hz;
	return 0;
}

void pf7580_dvfs_mark_busy(struct pf7580_dvfs *dvfs,
			   unsigned long busy_ns, unsigned long idle_ns)
{
	if (!dvfs)
		return;
	mutex_lock(&dvfs->lock);
	dvfs->busy_ns += busy_ns;
	dvfs->idle_ns += idle_ns;
	mutex_unlock(&dvfs->lock);
}

/* devfreq profile exit: runs inside devfreq_remove_device (kbase parity:
 * kbase_devfreq_exit -> kbase_devfreq_term_freq_table). Frees only the
 * table this instance allocated. */
static void pf7580_dvfs_exit(struct device *dev)
{
	struct pf7580_dvfs *dvfs = pf7580_dvfs_get(dev);

	if (!dvfs)
		return;
	kfree(dvfs->ftab);
	dvfs->ftab = NULL;
	dvfs->profile.freq_table = NULL;
	dvfs->profile.max_state = 0;
}

int pf7580_dvfs_init(struct device *dev, void *power,
		     struct pf7580_dvfs **out)
{
	struct pf7580_dvfs *dvfs;
	unsigned int i;
	int ret;

	if (!dev || !power || !out)
		return -EINVAL;
	if (pf7580_dvfs_g)
		return -EBUSY;

	dvfs = kzalloc(sizeof(*dvfs), GFP_KERNEL);
	if (!dvfs)
		return -ENOMEM;
	dvfs->dev = dev;
	dvfs->power = power;
	dvfs->cur_hz = PF7580_DVFS_BOOT_HZ;
	mutex_init(&dvfs->lock);

	dvfs->profile.initial_freq = PF7580_DVFS_BOOT_HZ;
	dvfs->profile.polling_ms = PF7580_DEVFREQ_POLL_MS;
	dvfs->profile.target = pf7580_dvfs_target;
	dvfs->profile.get_dev_status = pf7580_dvfs_get_status;
	dvfs->profile.get_cur_freq = pf7580_dvfs_get_cur;
	dvfs->profile.exit = pf7580_dvfs_exit;

	/* OPP ladder, ascending (opp_add keeps the list sorted; walk the
	 * table in order so freq_table is ascending like
	 * kbase_devfreq_init_freq_table). opp_add makes entries available
	 * by default; the explicit opp_enable() pins that (no-op on 3.10
	 * for fresh entries, but documents intent and survives a
	 * disable-all elsewhere). */
	for (i = 0; i < PF7580_DVFS_OPP_COUNT; i++) {
		ret = opp_add(dev, pf7580_dvfs_opps[i].freq_hz,
			      pf7580_dvfs_opps[i].volt_uv);
		if (ret)
			goto err_free;
		ret = opp_enable(dev, pf7580_dvfs_opps[i].freq_hz);
		if (ret)
			goto err_free;
	}

	/* Governor statistics table: ascending Hz copy (unsigned int,
	 * matching struct devfreq_dev_profile.freq_table on 3.10). */
	dvfs->ftab = kmalloc_array(PF7580_DVFS_OPP_COUNT, sizeof(dvfs->ftab[0]),
				  GFP_KERNEL);
	if (!dvfs->ftab) {
		ret = -ENOMEM;
		goto err_free;
	}
	for (i = 0; i < PF7580_DVFS_OPP_COUNT; i++)
		dvfs->ftab[i] = (unsigned int)pf7580_dvfs_opps[i].freq_hz;
	dvfs->profile.freq_table = dvfs->ftab;
	dvfs->profile.max_state = PF7580_DVFS_OPP_COUNT;

	/* Publish before the poller can see us: the governor START event
	 * inside devfreq_add_device queues the first poll. */
	pf7580_dvfs_g = dvfs;
	dvfs->df = devfreq_add_device(dev, &dvfs->profile,
				      PF7580_DEVFREQ_GOVERNOR, NULL);
	if (IS_ERR(dvfs->df)) {
		ret = PTR_ERR(dvfs->df);
		dvfs->df = NULL;
		pf7580_dvfs_g = NULL;
		goto err_tab;
	}

	*out = dvfs;
	return 0;

err_tab:
	kfree(dvfs->ftab);
	dvfs->profile.freq_table = NULL;
	dvfs->profile.max_state = 0;
err_free:
	kfree(dvfs);
	return ret;
}

void pf7580_dvfs_fini(struct pf7580_dvfs *dvfs)
{
	if (!dvfs)
		return;
	if (dvfs->df)
		devfreq_remove_device(dvfs->df);
	/* profile->exit ran inside remove and freed the table copy. */
	if (pf7580_dvfs_g == dvfs)
		pf7580_dvfs_g = NULL;
	kfree(dvfs);
}

int pf7580_dvfs_suspend(struct pf7580_dvfs *dvfs)
{
	/* NULL-safe like kbase (CONFIG_MALI_DEVFREQ unset path):
	 * devfreq_suspend_device(NULL) is -EINVAL; sleep must not fail
	 * for a missing DVFS handle. */
	if (!dvfs)
		return 0;
	return devfreq_suspend_device(dvfs->df);
}

int pf7580_dvfs_resume(struct pf7580_dvfs *dvfs)
{
	if (!dvfs)
		return 0;
	return devfreq_resume_device(dvfs->df);
}

int pf7580_dvfs_set_max(struct pf7580_dvfs *dvfs, unsigned long max_hz)
{
	struct devfreq *df;
	unsigned long eff = max_hz;
	struct opp *opp;
	int ret;

	if (!dvfs || !dvfs->dev)
		return -ENODEV;
	df = dvfs->df;
	if (!df)
		return -ENODEV;

	/* Resolve non-OPP ceilings by ceil (recommended-OPP semantics);
	 * 0 releases the clamp. */
	if (eff) {
		rcu_read_lock();
		opp = devfreq_recommended_opp(dvfs->dev, &eff, 0);
		if (IS_ERR(opp)) {
			rcu_read_unlock();
			return PTR_ERR(opp);
		}
		eff = opp_get_freq(opp);
		rcu_read_unlock();
	}

	/* Same sequence as the max_freq sysfs store (devfreq.c
	 * store_max_freq): hold devfreq->lock across the clamp write and
	 * the re-evaluation so a concurrent poll cannot slip a target()
	 * in between. update_devfreq() routes through the governor (the
	 * core sets LUB when the clamp bites) into pf7580_dvfs_target(),
	 * which applies the f-before-V down-step. -EAGAIN (gated /
	 * suspended) just leaves the clamp armed for the next poll. */
	mutex_lock(&df->lock);
	df->max_freq = eff;
	ret = update_devfreq(df);
	mutex_unlock(&df->lock);
	return ret;
}
