/*
 * pf7580_devfreq.h - panfrost devfreq/DVFS for Mali-T720 (Exynos 7580,
 *                     j7elte) on the 3.10.108 vendor tree.
 *
 * Provenance (all read from the shared tree, never modified):
 *  - OPP rows (freq MHz, voltage uV): the stock kbase table
 *    gpu_dvfs_table_default[] in
 *    drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c:46-55
 *    (8 rows: 800/734/668/534/440/350/266/160 MHz at
 *    1100/1100/1100/1000/1000/1000/900/900 mV).
 *  - target() voltage/clock order (GPU_SET_CLK_VOL, up: V then f,
 *    down: f then V): drivers/gpu/arm/r15p0/backend/gpu/
 *    mali_kbase_devfreq.c kbase_devfreq_target(); same order in
 *    platform/exynos/gpu_dvfs_api.c:27-38.
 *  - OPP API (opp_add/opp_enable/opp_find_freq_ceil/opp_get_freq/
 *    opp_get_voltage, RCU read side): include/linux/opp.h:30-49 +
 *    drivers/base/power/opp.c:257-440 (freq in Hz, voltage in uV).
 *  - devfreq core (devfreq_add_device(dev, profile, "simple_ondemand",
 *    NULL), devfreq_recommended_opp, devfreq_suspend/resume_device,
 *    freq_table/max_state accounting): include/linux/devfreq.h:89-171,
 *    drivers/devfreq/devfreq.c:150-240/540-577 (3.10.108 vendor copy).
 *  - Governor: drivers/devfreq/governor_simpleondemand.c
 *    (DFSO_UPTHRESHOLD 90 / DFSO_DOWNDIFFERENCTIAL 5 / DFSO_WEIGHT 100;
 *    NULL governor data is legal: defaults apply, no PM-QoS leg).
 *    CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND=y in the vendor .config.
 *    kbase likewise passes NULL governor data
 *    (mali_kbase_devfreq.c kbase_devfreq_init: devfreq_add_device(...,
 *    "simple_ondemand", NULL)) with polling_ms 100.
 *  - Clock/voltage actuation: code/gpu31/power/panfrost7580_power.h
 *    (pf7580_set_rate / pf7580_set_voltage; parking dance, -EAGAIN
 *    while gated, boot 50 ms autosuspend).
 *  - Thermal max-lock path this plugs under:
 *    code/gpu31/thermal/patches/0001-gpu-cdev-state-to-tmu-maxlock.patch
 *    (cdev state -> TMU lock -> gpu_dvfs_clock_lock(TMU_LOCK)) and
 *    gpu31-thermal.md (live 5-level ASV table 668..266, max_state 5,
 *    "thermal-gpufreq-0"). No CONFIG_DEVFREQ_THERMAL/of_devfreq_cooling
 *    on this tree (thermal report section 1), so the max clamp is the
 *    devfreq max_freq knob / TMU max-lock, not a cooling device.
 *
 * Relationship to the sibling GEM-slice draft
 * (code/gpu31/gem/panfrost7580_devfreq.[ch], G31_Gem2): that file is a
 * GEM-internal hook sketch owned by the GEM slice. This header + its
 * companion .c are the standalone DVFS slice for the integrator: full
 * OPP ladder registration, governor profile, suspend/idle behaviour,
 * and thermal max-lock interaction, with a host logic test. The two
 * overlap by design (same table, same order rule); dedup at land time
 * keeps one canonical copy (Main/Integrator call). This slice never
 * edits the GEM file.
 * Wiring for the owning DRM driver (integrator):
 *
 *	struct my_drv { struct pf7580_power *pwr; struct pf7580_dvfs *dvfs; };
 *
 *	probe:  pf7580_power_probe(...) -> pf7580_dvfs_init(dev, pwr, &dvfs)
 *	        (pins boot OPP 350 MHz first, then enables polling);
 *	remove: pf7580_dvfs_fini(dvfs) then pf7580_power_remove(pwr);
 *	pm:     suspend: pf7580_dvfs_suspend(dvfs);
 *	        resume:  pf7580_dvfs_resume(dvfs);
 *	job:    on submit fence / retired fence, call
 *	        pf7580_dvfs_mark_busy(dvfs, busy_ns_delta, idle_ns_delta)
 *	        (the get_dev_status hook drains these accumulators).
 * Locking: a single slice-local handle serialises OPP lookup +
 * voltage/clock steps (mirrors the kbase target() no-concurrency
 * assumption: devfreq core already holds devfreq->lock across
 * get_target_freq/target on this tree; update_devfreq documents that
 * its caller must hold it).
 *
 * Lookup: the slice never touches dev drvdata (the owning DRM driver
 * keeps it for its own struct). The single G3D block is resolved
 * through a file-static handle, the same pattern as kbase's global
 * pkbdev (gpu_notifier.c); callbacks verify dev == dvfs->dev.
 */

#ifndef PF7580_DEVFREQ_H
#define PF7580_DEVFREQ_H

#ifdef __KERNEL__
#include <linux/device.h>
#include <linux/devfreq.h>
#else
/* Host test build: forward declarations only (no kernel types). */
struct device;
struct devfreq_dev_status;
typedef unsigned int u32;
#endif


/* OPP ladder: 8 stock rows, ascending for the freq_table walk.
 * { clock Hz, voltage uV }. Source: gpu_exynos7580.c
 * gpu_dvfs_table_default[] (MHz/mV), converted verbatim to Hz/uV.
 * opp_add() takes Hz (opp.c:389-396 "@freq: Frequency in Hz"); the
 * 3.10 MIF bus driver likewise stores opp_get_freq results
 * (Hz-scale) straight into its freq_table.
 *
 * Naming: the ladder symbols carry a dvfs_ infix on purpose. The
 * power header (code/gpu31/power/panfrost7580_power.h) already owns
 * a TU-local `struct pf7580_opp { int mhz; int uv; }` ladder and the
 * firmware header owns `struct pf7580_opp { freq_hz; volt_uv }` plus
 * an extern table; reusing that tag/table name here would break any
 * driver TU that includes both headers (probe needs both). Shapes
 * stay identical on purpose; only the names are namespaced. */
struct pf7580_dvfs_opp {
	unsigned long freq_hz;
	unsigned long volt_uv;
};

#define PF7580_DVFS_BOOT_HZ	350000000UL

static const struct pf7580_dvfs_opp pf7580_dvfs_opps[] = {
	{ 160000000UL,  900000UL },
	{ 266000000UL,  900000UL },
	{ 350000000UL, 1000000UL },
	{ 440000000UL, 1000000UL },
	{ 534000000UL, 1000000UL },
	{ 668000000UL, 1100000UL },
	{ 734000000UL, 1100000UL },
	{ 800000000UL, 1100000UL },
};

#define PF7580_DVFS_OPP_COUNT \
	(sizeof(pf7580_dvfs_opps) / sizeof(pf7580_dvfs_opps[0]))

/* Governor tuning: kbase parity.
 * kbase backend uses "simple_ondemand" + NULL governor data with
 * polling_ms 100 (mali_kbase_devfreq.c). Tunables below are only what
 * we pass: the drain-side table/poll seed. DFSO_UPTHRESHOLD=90 /
 * DFSO_DOWNDIFFERENCTIAL=5 apply inside the governor (defaults, data
 * NULL), matching kbase behaviour of shipping stock governor knobs. */
#define PF7580_DEVFREQ_GOVERNOR	"simple_ondemand"
#define PF7580_DEVFREQ_POLL_MS	100

/* --- Pure host-testable helpers (no kernel types) --- */
/* Ceil an arbitrary request to the OPP ladder (recommended-OPP
 * semantics: least OPP >= request; requests above the top clamp to
 * the top OPP; request 0 yields the floor). Returns index 0..7. */
static inline int pf7580_dvfs_opp_ceil_idx(unsigned long req_hz)
{
	int i;
	for (i = 0; i < (int)PF7580_DVFS_OPP_COUNT; i++) {
		if (pf7580_dvfs_opps[i].freq_hz >= req_hz)
			return i;
	}
	return (int)PF7580_DVFS_OPP_COUNT - 1;
}

/* Voltage step direction for a transition cur -> want.
 * Returns +1 (V before f), -1 (f before V), 0 (no voltage change or
 * no-op). Encodes the GPU_SET_CLK_VOL rule for the host test. */
static inline int pf7580_dvfs_volt_first(unsigned long cur_hz,
				    unsigned long want_hz)
{
	unsigned long cur_v, want_v;
	int ci = pf7580_dvfs_opp_ceil_idx(cur_hz);
	int wi = pf7580_dvfs_opp_ceil_idx(want_hz);
	if (pf7580_dvfs_opps[ci].freq_hz == pf7580_dvfs_opps[wi].freq_hz)
		return 0;
	cur_v = pf7580_dvfs_opps[ci].volt_uv;
	want_v = pf7580_dvfs_opps[wi].volt_uv;
	if (want_v == cur_v)
		return 0;
	return (want_v > cur_v) ? 1 : -1;
}


struct pf7580_dvfs;

/*
 * Register the 8 OPPs (opp_add + opp_enable, ascending), build the
 * per-instance profile freq_table (ascending Hz), seed the boot OPP,
 * and attach devfreq with PF7580_DEVFREQ_GOVERNOR. power is the
 * code/gpu31/power/ pf7580_power handle (opaque here to avoid a
 * cross-include). The owning driver's dev drvdata is left alone (the
 * instance is found through a file-static handle). Returns 0 with *out
 * filled, -EBUSY if an instance already lives, negative errno.
 */
int pf7580_dvfs_init(struct device *dev, void *power,
		     struct pf7580_dvfs **out);

/* Detach devfreq (devfreq_remove_device runs profile->exit, which frees
 * the freq_table copy) and free the instance. Leaves OPP entries
 * registered (opp table is device-lifetime on 3.10: no opp_remove
 * export used by any in-tree devfreq driver). */
void pf7580_dvfs_fini(struct pf7580_dvfs *dvfs);

/*
 * devfreq profile callbacks (wired into devfreq_dev_profile by init;
 * exported for the host test + integrator assertions):
 *  - target: resolve *freq through devfreq_recommended_opp()
 *    (ceil to OPP, LUB/GLB by flags), then step voltage+clock in
 *    GPU_SET_CLK_VOL order (up: V->f; down: f->V) via
 *    pf7580_set_voltage/pf7580_set_rate.
 *  - get_status: report busy/total/current and drain counters.
 *  - get_cur: report the last committed OPP.
 */
int pf7580_dvfs_target(struct device *dev, unsigned long *freq, u32 flags);
int pf7580_dvfs_get_status(struct device *dev,
			   struct devfreq_dev_status *stat);
int pf7580_dvfs_get_cur(struct device *dev, unsigned long *freq);

/* Feed one completed window (ns) into the load accumulators the
 * get_status hook drains. The jobs slice calls this on job retire
 * (busy = GPU-active ns) and on idle gaps (idle = wall ns); both may
 * be 0 (poll-only quanta still reset the governor baseline). */
void pf7580_dvfs_mark_busy(struct pf7580_dvfs *dvfs,
			   unsigned long busy_ns, unsigned long idle_ns);

/*
 * Suspend/resume pair for dev_pm_ops (mirrors kbase NULL-safety:
 * devfreq_suspend_device(NULL) returns -EINVAL; our wrapper treats a
 * NULL dvfs as a no-op 0 so system sleep never fails for lack of a
 * DVFS handle):
 *  - suspend: devfreq_suspend_device() (monitor stop) so polling does
 *    not re-rate while the runtime PM path gates aclk_g3d; call BEFORE
 *    the runtime-suspend leg (clock must still be on for the final
 *    readback-free stop).
 *  - resume: devfreq_resume_device() (monitor restart); call AFTER the
 *    runtime-resume leg (clock + reset + cores back first, so the
 *    resumed poller's first target() sees -EAGAIN-free hardware).
 * Idle: no hook. Runtime autosuspend (50 ms, power slice) already
 * parks the rail at the boot OPP clock-off; devfreq polling at 100 ms
 * only fires while the device is runtime-resumed (monitor work holds
 * no PM ref; when suspended, target() short-circuits -EAGAIN and the
 * core logs once per poll at most).
 */
int pf7580_dvfs_suspend(struct pf7580_dvfs *dvfs);
int pf7580_dvfs_resume(struct pf7580_dvfs *dvfs);

/*
 * Thermal max-lock interaction (no devfreq cooling device on this
 * tree: CONFIG_DEVFREQ_THERMAL is unset, so of_devfreq_cooling is
 * absent and the kbase path is gpu_dvfs_clock_lock(TMU_LOCK), not a
 * devfreq notifier):
 *  - On the kbase tree the 0001 thermal patch turns cdev state into
 *    TMU max locks inside gpu_notifier; on the panfrost tree the same
 *    effect is devfreq max_freq: clamp dvfs->df->max_freq to the
 *    allowed ceiling and let update_devfreq() apply it on the next
 *    poll (devfreq.c:192-195: freq = min(freq, max_freq) with LUB).
 *  - pf7580_dvfs_set_max() applies that clamp immediately (and drops
 *    to it now if we sit above it, via update_devfreq-equivalent
 *    target()). max_hz==0 releases the clamp. Frequencies not on the
 *    OPP ladder resolve by ceil (devfreq_recommended_opp semantics).
 *  - The live ASV table on this unit is the 5-level variant
 *    {668,534,440,350,266} (gpu31-thermal.md section 1); clamp values
 *    above the live ceiling fail closed in kbase
 *    (gpu_dvfs_get_level "max lock error") and resolve to the top OPP
 *    here. Integrator note: align the TMU clamp table with the live
 *    ASV table (thermal patch 0002 follow-up) before relying on
 *    sub-668 ceilings.
 */
int pf7580_dvfs_set_max(struct pf7580_dvfs *dvfs, unsigned long max_hz);

#endif /* PF7580_DEVFREQ_H */
