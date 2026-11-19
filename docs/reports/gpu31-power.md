# GPU31 power — PM runtime for panfrost on Exynos 7580 (G3D domain, clocks, BUCK4/LDO10)

Scope: host-side patch under `code/gpu31/power/` implementing the
power/clock/reset sequence a panfrost-class driver needs on j7elte
(kernel 3.10.108, Mali-T720), cloned from the stock kbase backend that
already drives this SoC. Device state note (2026-11-19): the phone is UP
(kernel #35, SSH/RNDIS working); stale "link down" notes in older reports
do not apply to this slice. No device I/O was needed here — this slice is
host-side code + report only. Nobody except `G31_Integrator` builds the
kernel and nobody except `G31_M1Verify` flashes (after explicit go); this
slice does neither. The shared kernel tree was never modified and never
built in; only read for provenance. Stock `dt.img` is never rebuilt, so
everything below works against the stock DT as-is.

## Deliverables

- `code/gpu31/power/panfrost7580_power.h` — `struct pf7580_power`, OPP
  table, register offsets, full call-order contract.
- `code/gpu31/power/panfrost7580_power.c` — probe, runtime get/put
  (suspend/resume), system-sleep suspend/resume, re-rate parking dance,
  voltage voter, remove.
- `code/gpu31/power/README` — wiring + integrator notes.
- This report: `docs/reports/gpu31-power.md`.

## 1. Ground truth reused (read-only)

| Item | Source (shared tree) | What was taken |
|---|---|---|
| 6 clocks + acquire order + parking re-rate + guards | `drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c` (`gpu_get_clock` :483-553, `gpu_set_clock` :392-477, `gpu_clock_on/off` :235-313) | `clk_get` order, `aclk_g3d` enable gating, park-on-`aclk_g3d_400` dance, refuse re-rate when domain off / clock gated |
| Voltage order (up: V then f; down: f then V) | `platform/exynos/gpu_dvfs_api.c:27-38` (`GPU_SET_CLK_VOL`) | documented caller rule on `pf7580_set_rate` / `pf7580_set_voltage` |
| Regulator get/enable/voltage + power-off guard | `gpu_exynos7580.c:679-741`, `:590-628` | `regulator_get(NULL,"vdd_g3d")`, enable at probe, `set_voltage(vol,vol)`, no programming while dark |
| OPP rows (freq MHz, voltage uV) | `gpu_exynos7580.c:46-55` (`gpu_dvfs_table_default`) | 8-row `pf7580_opps[]` verbatim |
| Runtime callbacks (clock on/off around genpd, DVFS hooks) | `platform/exynos/gpu_notifier.c:161-350` (`gpu_power_on/off/suspend`, `pm_callback_runtime_on/off`, autosuspend) | suspend = cores off + clock off; resume = clock on + reset + cores on; 50 ms autosuspend; **no** PM notifier in the new driver (genpd + dev_pm_ops own it; a second notifier would double-drive) |
| Domain attach (needs no driver code) | `arch/arm64/mach-exynos/pm_domains.c` (`exynos_read_domain_from_dt`, `exynos_pm_notifier_call`, G3D magic `0xFFFFF0CF`, 1 ms STATUS poll) | rely on `BUS_NOTIFY_BIND_DRIVER` + `samsung,power-domain`; never touch PMU CONFIGURATION |
| Reset + PWRON register map | `drivers/gpu/arm/t7xx/r15p0/mali_midg_regmap.h` | `GPU_COMMAND` 0x030, SOFT 0x01 / HARD 0x02, `RESET_COMPLETED` bit 8, IRQ regs, `*_PRESENT/*_PWRON/*_READY` offsets |
| PMIC BUCK4/LDO10 sync | `drivers/regulator/s2mpu03.c` (enable/disable disregard + is_enabled-via-PMU while `g3d_en`, probe sync bits), `arch/arm64/boot/dts/exynos7580-j7elte_rev06.dts` (`g3d_en=1`, BUCK4 `vdd_g3d` always-on+boot-on 600-1300 mV, LDO10 `vdd_ldo10` always-on, no boot-on, no consumer) | driver only refcounts/votes `vdd_g3d`; never touches `vdd_ldo10` |
| Prior slice context | `code/userspace/gpu/dvfs-power/POWER.md`, `code/userspace/gpu/panfrost/{design.md,kmod/panfrost7580.c}` | clock roles, domain-free-attach, DVFS policy, M0 probe precedent (this patch supersedes M0's ad-hoc power legs with the full sequence) |

Not copied: kbase's infinite `gpu_power_set_reg` spin (replaced by the
genpd 1 ms-timeout path), `regulator_disable(vdd_g3d)` on the runtime path
(s2mpu03 ignores it under `g3d_en=1` anyway), `pm_suspend_ignore_children`
(DRM children must wake the parent), ABB/`set_match_abb` (7580 `GPU_DYNAMIC_ABB=0`,
safe to defer), devfreq (bus-only on this tree), thermal cooling registration
(deferred to the DVFS slice; bring-up pins 266 MHz).

## 2. Exact sequence

### Probe (`pf7580_power_probe`)

1. `devm_kzalloc` context, `mutex_init`.
2. `clk_get` × 6 in kbase order: `fin_pll`, `fout_g3d_pll`, `aclk_g3d`,
   `mout_g3d`, `dout_aclk_g3d`, `aclk_g3d_400`. Any failure aborts and
   unwinds earlier claims (kbase order). There are no `sclk_g3d` /
   `gate_pclk` clocks on 7580 G3D — do not add them.
3. `regulator_get(NULL, "vdd_g3d")` — NULL device is mandatory (stock DT has
   no `vdd_g3d-supply` phandle; a dev-based lookup fails). Then
   `regulator_enable()`.
4. Read `clk_get_rate(aclk_g3d)`, map MHz → OPP voltage, `set_voltage` so the
   rail matches the CMU's left-behind rate. Unknown rate → warn, continue.
5. `pm_runtime_set_autosuspend_delay(50 ms)` + `pm_runtime_use_autosuspend()`
   + `pm_runtime_enable()` (kbase default `runtime_pm_delay_time = 50`).
6. `pm_runtime_get_sync()` → genpd powers pd-g3d (OPTION, G3D magic,
   CONFIGURATION, STATUS poll) → `pf7580_runtime_resume()` (below).
7. `pm_runtime_put_autosuspend()` → idle-suspend after 50 ms like kbase's
   `pm_schedule_suspend(delay_time)`.

### Runtime resume = power on (`pf7580_runtime_resume`)

1. `clk_prepare_enable(aclk_g3d)` (skip if already enabled).
2. `SOFT_RESET` → mask `RESET_COMPLETED` → poll `IRQ_RAWSTAT` ≤ 100 ms →
   `HARD_RESET` fallback once → clear + unmask. (kbase `RESET_TIMEOUT`.)
3. Latch `SHADER/TILER/L2_PRESENT_LO`, power on **L2 → shader → tiler**,
   polling each `*_READY_LO` ≤ 100 ms.
4. On any failure: gate the clock again, return errno (probe fails loudly;
   resume reports through the PM core).

### Runtime suspend = power off (`pf7580_runtime_suspend`)

1. Cores off in reverse (**tiler → shader → L2**), `PWRON=0`, poll
   `READY==0`; a stage that stays ready only warns — the clock must still
   be gated so the domain can drop.
2. `clk_disable_unprepare(aclk_g3d)`.
3. genpd drops pd-g3d by itself. Never `regulator_disable(vdd_g3d)` here
   (ignored by s2mpu03 under `g3d_en=1`; would only corrupt the refcount).

### System sleep (`pf7580_pm_suspend` / `pf7580_pm_resume`)

- suspend: no-op if already runtime-suspended, else the runtime-suspend
  path (gate the clock before the SoC sleeps).
- resume: unconditional full power-on (pd-g3d was off ⇒ GPU state lost).
- No `pm_notifier` (kbase's `gpu_pm_notifier` pattern is not copied): genpd
  owns domain sequencing and the DRM driver already has `dev_pm_ops`.

### Re-rate (`pf7580_set_rate`) + voltage (`pf7580_set_voltage`)

- Rate: refuse `-EAGAIN` while gated; park `mout_g3d → aclk_g3d_400` →
  `clk_set_rate(fout_g3d_pll)` → parent back → unpark (kbase `gpu_set_clock`).
- Voltage: no-op at target; refuse `-EAGAIN` while gated (kbase power-off guard).
- Caller orders them per `GPU_SET_CLK_VOL`: **up: voltage then clock;
  down: clock then voltage**. Reversal is failure mode F4 below.
- ABB/`set_match_abb` intentionally absent (`GPU_DYNAMIC_ABB=0` on 7580).

### Remove (`pf7580_power_remove`)

Force-resume, run the suspend path synchronously, `pm_runtime_disable`,
`regulator_disable` + `put` (the only place disable is legal — mirrors
kbase `gpu_regulator_term`), `clk_put` × 6. Domain detach is the vendor bus
notifier's job on unbind.

## 3. BUCK4/LDO10 sync interaction (already seen in s2mpu03)

- `g3d_en = <1>` in stock DT arms the special path: `s2m_enable` /
  `s2m_disable_regmap` disregard BUCK4, and `s2m_is_enabled_regmap` reads the
  G3D PMU STATUS instead of the PMIC enable bit. Net effect: `vdd_g3d` I2C
  enable/disable from GPU code is a refcount-only no-op while the domain is
  managed by genpd — which is exactly why the runtime path never disables it.
- At PMIC probe the driver writes `B4CTRL1[7:6]=00` (BUCK4 to GPIO control),
  `LDO_EFUSE[4]=1` + `L10CTRL[7:6]=00` (BUCK4↔LDO10 tracking). On j7elte the
  `buck4en_pin/addr` properties are commented out so no GPIO toggling is
  programmed; the sync bits simply keep LDO10 tracking the static BUCK4
  output. Rail is `always-on + boot-on`, so the GPU driver only votes voltage.
- Nobody in this tree consumes `vdd_ldo10` (regulator-always-on, no boot-on,
  no `regulator_get` anywhere). Leave it alone.

## 4. Failure modes (the LDO10 bug class + ordering traps)

| # | Fault | Mechanism | Rule |
|---|---|---|---|
| F1 | **LDO10 enable/disable from GPU code** | While BUCK4/LDO10 tracking is armed, LDO10's enable field is slaved to BUCK4: an I2C disable is ignored (refcount corrupted, rail stays up) or races the tracker (rail glitches mid-job → descriptor reads fault, GPU hangs with READY bits stuck). | **Never `regulator_get("vdd_ldo10")` from GPU code.** No LDO10 handle exists in this patch by design. |
| F2 | Clock before domain | Touching CON0/MUX while pd-g3d is off hangs the bus (`gpu_set_clock` returns -1 for this reason). | All register access under a runtime-PM reference; `set_rate`/`set_voltage` refuse `-EAGAIN` while gated. |
| F3 | `regulator_disable(vdd_g3d)` on suspend | Ignored by HW (s2mpu03 disregard) but corrupts the consumer refcount; next resume's enable may then actually cut the rail. | Disable only in `remove` (module term path). |
| F4 | Clock-before-voltage on the way up | 534→668 and 668→800 cross the 1.00 V→1.10 V corner (`ramp-delay 12000`); PLL at 800 MHz on 1.00 V browns out under load. | Enforce `GPU_SET_CLK_VOL` order (up: V→f, down: f→V). |
| F5 | Copying `gpu_power_set_reg`'s infinite spin | PMU handshake stall (missing rail/clock) becomes an unkillable hang. | All polls bounded (100 ms reset, 100 ms per core stage); genpd's own 1 ms timeout covers the domain leg. |
| F6 | `sclk_g3d` / `gate_pclk` clk_get | Names do not exist on 7580 G3D (CMU GATE list is `aclk_*`/`pclk_*` only); probe would fail on stock DT. | Claim exactly the six stock `clock-names`. |
| F7 | Dev-based `regulator_get(dev, "vdd_g3d")` | Stock DT has no `vdd_g3d-supply`; lookup fails, probe aborts. | `regulator_get(NULL, "vdd_g3d")` — global name, as kbase does. |
| F8 | Second PM notifier / `pm_suspend_ignore_children` | Double-drives the domain under dev_pm_ops; children (GEM fences) must wake the parent on a DRM driver. | Neither is in this patch; dev_pm_ops + genpd only. |
| F9 | DVFS before cooling on v1 silicon | 734/800 OPPs overclock derated parts; sustained load with no thermal clamp rides the junction limit. | Bring up at 266 MHz; gate top OPPs on `soc_is_exynos7580_v1()` + cooling device (DVFS slice owns this). |
| F10 | `devm` + `regulator_put` double-free on probe error | `devm_kzalloc`'d context freed by the `err` path via `devm_kfree` only once — no `devm_regulator_get` is used, so manual `put` is balanced. | Unwind order in code: PM → voltage → regulator → clocks → `devm_kfree`. |

## 5. Verification (host-side; integrator owns the rest)

- `gcc -fsyntax-only` against the 3.10.108 tree headers for both files:
  procedure in `code/gpu31/power/README` (read-only include pass, **not** a
  kernel build — only `G31_Integrator` builds).
- No device I/O in this slice (phone shared with GPS cold-boot + FM audio;
  no reboot from anyone but the flash gate). Runtime proof (GPU_ID /
  MMU_FEATURES / READY bitmaps, suspend/resume cycle, OPP step under load)
  belongs to `G31_M1Verify` after the integrator wires this helper into the
  panfrost probe — tracked as open, not claimed.
- `git status` scoped to `code/gpu31/power/` + `docs/reports/gpu31-power.md`
  only; no shared-tree modification.

## 6. Open questions for later slices (not blockers)

1. DVFS policy + devfreq/OPP registration + cooling device (M4 in
   `gpu-panfrost.md`): this patch provides `set_rate`/`set_voltage`/OPPs;
   the governor owns calling them.
2. `early_clk_gating_status` is 0 on 7580 (`GPU_EARLY_CLK_GATING 0`), so the
   simple gate-on-suspend path here matches; no early-gating leg was added.
3. `dvs_en_regulator` is declared but unused in kbase 7580 (`GPU_DVS 0`) —
   no DVS hook was added.
