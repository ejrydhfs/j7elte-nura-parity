# j7elte (Exynos 7580 / Mali-T720) GPU power + DVFS audit

Read-only audit of the vendor Nura/Samsung kernel tree. All paths below are
relative to the kernel tree root unless noted. Nothing here modifies the
kernel, DT, or config; this directory (`code/userspace/gpu/dvfs-power/`)
is the only thing this slice owns.

## 1. Scope / what was read

- `drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c` (748 lines)
- `drivers/gpu/arm/t7xx/r15p0/platform/exynos/mali_kbase_platform.h`
  (`gpu_dvfs_info` struct layout, lines 195-207)
- `drivers/gpu/arm/t7xx/r15p0/backend/gpu/mali_kbase_pm_driver.c`
  (`core_type_to_reg`, `kbase_pm_invoke`, `kbase_pm_transition_core_type`)
- `arch/arm64/mach-exynos/pm_domains.c` (`BUS_NOTIFY_BIND_DRIVER` path)
- Mali DT node + `pd-g3d` node in `arch/arm64/boot/dts/exynos7580.dtsi`
- G3D regulator (`vdd_g3d` / BUCK4) in
  `arch/arm64/boot/dts/exynos7580-j7xelte_common.dtsi`
- `drivers/thermal/exynos7580/gpu_cooling.c` (thermal clamp tables)
- `arch/arm64/mach-exynos/asv-exynos7580_cal.c` (G3D ASV voltage tables)
- `drivers/devfreq/Kconfig` + `.config` for `CONFIG_PM_DEVFREQ` status

## 2. Clock tree (6 clocks, DT-bound by name)

DT node `mali@0x11400000`, `arch/arm64/boot/dts/exynos7580.dtsi:1066-1076`:

```
clocks = <&clock 1>, <&clock 10>, <&clock 160>,
         <&clock 161>, <&clock 162>, <&clock 163>;
clock-names = "fin_pll", "fout_g3d_pll", "aclk_g3d",
              "mout_g3d", "dout_aclk_g3d", "aclk_g3d_400";
```

Acquired one by one with fatal `clk_get` in `gpu_get_clock()`
(`gpu_exynos7580.c:483-553`); any single failure aborts clock init.
Role of each member (from `gpu_set_clock`, `gpu_clock_on/off`,
`gpu_get_clock`):

| Clock | Role |
|---|---|
| `fin_pll` | PLL reference input. `clk_get` only; never reparented/re-rated by the GPU driver. |
| `fout_g3d_pll` | G3D PLL output = the actual frequency knob. `clk_set_rate(fout_g3d_pll, g3d_rate)` in `gpu_set_clock` (`gpu_exynos7580.c:436`). Final sanity check compares `aclk_g3d` rate against `fout_g3d_pll` rate (`gpu_exynos7580.c:463-468`). |
| `aclk_g3d` | Gated AXI/functional clock to the GPU. `clk_prepare_enable` in `gpu_clock_on` (`gpu_exynos7580.c:259-262`), `clk_disable_unprepare` in `gpu_clock_off` (`gpu_exynos7580.c:300-303`). `gpu_get_cur_clock` reads `clk_get_rate(aclk_g3d)/MHZ` (`gpu_exynos7580.c:227`). |
| `mout_g3d` | Glitch-free mux between PLL output and safe clock. Rate change sequence in `gpu_set_clock` (`gpu_exynos7580.c:421-458`): enable `aclk_g3d_400` -> `clk_set_parent(mout_g3d, aclk_g3d_400)` (park on safe clock) -> `clk_set_rate(fout_g3d_pll, ...)` -> `clk_set_parent(mout_g3d, fout_g3d_pll)` (switch back) -> disable `aclk_g3d_400`. |
| `dout_aclk_g3d` | Divider downstream of the mux. Acquired (`gpu_exynos7580.c:517-527`) but never re-rated/reparented by the vendor driver; left at bootloader/CMU default. |
| `aclk_g3d_400` | Fixed ~400 MHz safe/parking clock used only across the PLL re-rate window (enable before, disable after; `gpu_exynos7580.c:421-424`, `452-456`). Also written once: `__raw_writel(0x1, EXYNOS7580_MUX_SEL_G3D0)` at end of `gpu_get_clock` to select the mux path. |

Guards: `gpu_set_clock` refuses to run when the power domain is off
(`gpu_exynos7580.c:400-406`) or the clock is gated (`gpu_exynos7580.c:408-413`).
Same power-off guard in `gpu_clock_on` (`gpu_exynos7580.c:246-250`) and
`gpu_clock_off` (`gpu_exynos7580.c:287-292`); all three take
`platform->exynos_pm_domain->access_lock` under `CONFIG_MALI_RT_PM`.

Raw register evidence for bring-up/debug exists in `gpu_register_dump()`
(`gpu_exynos7580.c:318-384`): PMU status `0x10464064`, PLL
`0x11460000/0x11460100/0x11460104/0x1146010C`, mux `0x11460200/0x11460300/
0x11460400`, div `0x11460600/0x11460700`, enable `0x11460800`.

## 3. Power domain: pd-g3d, attached for free by the notifier

- DT: `samsung,power-domain = <&pd_g3d>;` on the mali node
  (`exynos7580.dtsi:1075`); domain block `pd_g3d: pd-g3d@10464060`
  (`exynos7580.dtsi:1618-1622`, `compatible = "samsung,exynos-pd"`,
  `reg = <0x0 0x10464060 0x0 0x20>`, `pd-option = <0x1>`).
- Attach path needs **no driver code**: `exynos_pm_notifier_call`
  (`pm_domains.c:425-441`) handles `BUS_NOTIFY_BIND_DRIVER`, and
  `exynos_read_domain_from_dt` (`pm_domains.c:401-423`) parses the
  `samsung,power-domain` phandle and calls `exynos_add_device_to_pd`
  (`pm_domains.c:355-384`), i.e. `__pm_genpd_add_device(&pd->genpd, dev,
  &gpd_td)` + `pm_genpd_dev_need_restore(dev, true)`. Unbind reverses it
  via `BUS_NOTIFY_UNBOUND_DRIVER` -> `exynos_remove_device_from_pd`
  (`pm_domains.c:386-399`). The notifier itself is registered with
  `bus_register_notifier(&platform_bus_type, &platform_nb)`
  (`pm_domains.c:611`).
- So: **no `dev_pm_domain_attach_by_name()` and no `device_link_add()` are
  needed in any panfrost/DRM probe** on this tree — binding a
  `platform_driver` whose `of_node` carries `samsung,power-domain` is
  sufficient; generic PM-domain suspend/resume then gates `pd-g3d`.
- Domain sequencing, `exynos_pd_power` (`pm_domains.c:66+`): writes
  `pd->pd_option` to OPTION (`base+0x8`); for `PD_G3D` on 7580 writes magic
  `0xFFFFF0CF` to `base+0x10` (`pm_domains.c:87-89`); writes on/off to
  CONFIGURATION (`base+0x0`); polls STATUS (`base+0x4`) against
  `LOCAL_PWR_CFG` with a 1 ms timeout (`pm_domains.c:~93-100`).
- The kbase platform driver itself barely touches the domain:
  `gpu_is_power_on()` polls `EXYNOS_PMU_G3D_STATUS & LOCAL_PWR_CFG`
  (`gpu_exynos7580.c:187-191`); `gpu_power_set_reg()` writes
  `LOCAL_PWR_CFG` to `EXYNOS_PMU_G3D_CONFIGURATION` and spins until on
  (`gpu_exynos7580.c:193-201`); `gpu_power_init()` is a log-only stub
  returning 0 (`gpu_exynos7580.c:203-214`).

## 4. Core power-up order: L2 -> shaders -> tiler (inside kbase, not Exynos)

`gpu_exynos7580.c` has **no** shader/tiler/L2 sequencing — that order lives
one layer down in the generic kbase PM core:

- `kbase_pm_transition_core_type()` + `kbase_pm_invoke(..., ACTION_PWRON)`
  (`backend/gpu/mali_kbase_pm_driver.c:490-530`), with the register
  translation `core_type_to_reg()` (`mali_kbase_pm_driver.c:60-115`):
  `ACTION_PWRON` maps to `STACK_PWRON_LO` / `SHADER_PWRON_LO` /
  `TILER_PWRON_LO` / L2-PWRON offsets, i.e. writes to the Midgard
  `SHADER_PWRON_{LO,HI}`, `TILER_PWRON_{LO,HI}`, `L2_PWRON` registers and
  completion is polled on the matching `*_READY_{LO,HI}` registers
  (`mali_kbase_pm_driver.c:924-937` lists the `SHADER_READY/TILER_READY/
  L2_READY` poll set).
- Policy layer is coarse on-demand
  (`backend/gpu/mali_kbase_pm_coarse_demand.c:32-39`): power is wanted when
  `active_count || shader_needed/inuse || tiler_needed/inuse`; L2 follows via
  `get_desired_cache_status()` (caches required by the wanted
  cores/shaders/tilers). In effect: **L2 first, then shader cores, then the
  tiler**, with READY-bit handshakes per stage. A panfrost port uses its own
  `panfrost_gpu_power_on()` equivalent but must preserve that order and the
  READY polls.
- Special case: `PWRON_DURING_PWROFF_TRANS` handling
  (`mali_kbase_pm_driver.c:501-519`) — a PWRON to a core already
  transitioning off is latched and re-applied when the transition
  completes. Preserve this race handling if the replacement driver does
  its own core gating.

## 5. Regulator: vdd_g3d = BUCK4, 600-1300 mV, global lookup

- `gpu_regulator_init()` does `regulator_get(NULL, "vdd_g3d")`
  (`gpu_exynos7580.c:711`) — a **global (NULL-device) lookup**, then
  `regulator_enable()` (`gpu_exynos7580.c:679-694`). Disable/put mirror it
  (`gpu_exynos7580.c:695-741`). `g3d_regulator`/`dvs_en_regulator` globals at
  `gpu_exynos7580.c:183-184` (the `dvs_en` handle is declared but unused).
- Voltage set path: `gpu_set_voltage()` (`gpu_exynos7580.c:590-628`)
  refuses to program while the domain is off (`gpu_exynos7580.c:593-599`),
  no-ops if already at target, optionally `regulator_sync_voltage()` under
  `CONFIG_EXYNOS_CL_DVFS_G3D`, then `regulator_set_voltage(g3d, vol, vol)`.
  ABB hooks (`set_match_abb(ID_G3D, gpu_dvfs_get_cur_asv_abb())`) wrap the
  transition in `gpu_set_voltage_{pre,post}` (`gpu_exynos7580.c:630-651`).
- DT rail, `exynos7580-j7xelte_common.dtsi:524-532`:
  `buck4_reg: BUCK4 { regulator-name = "vdd_g3d";
  regulator-min-microvolt = <600000>;
  regulator-max-microvolt = <1300000>;
  regulator-always-on; regulator-boot-on;
  regulator-ramp-delay = <12000>; regulator-initial-mode = <1>; }`.
  Note: `always-on + boot-on` means the rail is up from boot; the GPU driver
  only enables/tracks its own consumer reference.
- Operating window actually used is narrower than the rail: the DVFS table
  below programs **900000-1100000 uV** (see section 6). 600/1300 mV are the
  rail limits, not DVFS targets.

## 6. DVFS tables, governor, ASV, thermal

DVFS row layout, `mali_kbase_platform.h:195-207`:

```
{ clock(MHz), voltage(uV), asv_abb, min_threshold(%), max_threshold(%),
  down_staycount, time, mem_freq(kHz), int_freq(kHz), cpu_freq(kHz), cpu_max_freq(kHz) }
```

Default table, `gpu_exynos7580.c:46-55` (comment at `:44-45` documents the
tail columns as `pm_qos mem, pm_qos int, pm_qos cpu_kfc_min, pm_qos cpu_egl_max`):

| G3D MHz | uV | util up/down % | down_stay | MIF kHz | INT kHz | CPU min/max kHz |
|---|---|---|---|---|---|---|
| 800 | 1100000 | 99/100 | 1 | 825000 | 400000 | 1200000/1600000 |
| 734 | 1100000 | 98/99 | 1 | 825000 | 400000 | 1000000/1600000 |
| 668 | 1100000 | 78/85 | 1 | 825000 | 400000 | 800000/1600000 |
| 534 | 1000000 | 78/85 | 1 | 728000 | 334000 | 500000/1600000 |
| 440 | 1000000 | 78/85 | 1 | 667000 | 267000 | 500000/1600000 |
| 350 | 1000000 | 78/85 | 1 | 559000 | 267000 | 500000/1600000 |
| 266 | 900000 | 78/85 | 1 | 416000 | 200000 | 0/1600000 |
| 160 | 900000 | 10/20 | 1 | 416000 | 200000 | 0/1600000 |

Platform clamps/attributes, `gpu_exynos7580.c:63-88`: `GPU_MAX_CLOCK 800`,
`GPU_MAX_CLOCK_LIMIT 800`, `GPU_MIN_CLOCK 266`, `GPU_DVFS_START_CLOCK 266`,
`GPU_GOVERNOR_TYPE G3D_DVFS_GOVERNOR_INTERACTIVE`, all governor start clocks
266, highspeed clock 420 @ load 95, delay 0. All four governor slots
(default/interactive/static/booster) point at the **same** table
(`gpu_exynos7580.c:74-85`).

- Governor dispatch: `gpu_dvfs_governor_{default,interactive,static,booster}`
  in `platform/exynos/gpu_dvfs_governor.c:33-36+`; `gpu_dvfs_decide_max_clock()`
  is a stub returning 0 (`gpu_exynos7580.c:153-159`) — no extra max clamp.
- ASV: per-speed-group G3D voltage tables `volt_table_g3d_{1,2,3}` selected
  in `asv-exynos7580_cal.c:59-96` (`volt_table_g3d` pointer at `:22`,
  consumer `SYSC_DVFS_G3D` at `:116-117`). The DVFS-table voltages above are
  pre-ASV nominals; `set_match_abb(ID_G3D, ...)` applies the per-chip ABB
  trim around each transition.
- MIF floor coupling: `mif_min_table` (`gpu_exynos7580.c:57-61`),
  `GPU_PMQOS_MIF_MAX_CLOCK 825000` (`gpu_exynos7580.c:127`) — GPU steps drag
  a PM-QoS MIF/INT/CPU floor with them (the tail columns above).
- Thermal: `drivers/thermal/exynos7580/gpu_cooling.c:65-86` defines two
  clip tables, `gpu_freq_table_800` (800..160, 8 steps) and
  `gpu_freq_table_668` (668..160, 6 steps); `gpufreq_cooling_register()`
  picks the 668 table on `soc_is_exynos7580_v1()` silicon, else the 800
  table (`gpu_cooling.c:436-438`). Cooling states map 1:1 onto these steps
  via `get_property(... GET_LEVEL/GET_FREQ ...)` (`gpu_cooling.c:145-215`)
  and are enforced with `gpufreq_apply_cooling()` (`gpu_cooling.c:245+`).
- `CONFIG_PM_DEVFREQ`: **`y`** in the vendor `.config` (`:3269`), but the
  GPU's own DVFS does **not** use the devfreq framework — the only 7580
  devfreq driver is `ARM_EXYNOS7580_BUS_DEVFREQ` for the MIF/INT/ISP busses
  (`drivers/devfreq/Kconfig:131-140`, `Makefile:15`). The GPU cooling
  device reuses the `cpufreq_frequency_table` type for its own table, not
  devfreq governors. A panfrost port should therefore expose devfreq
  (simple-ondemand) itself rather than expect the vendor handler to
  attach — there is no GPU devfreq node to inherit.

## 7. Panfrost-must-replicate vs vendor-notifier-gives-for-free

### MUST replicate in a panfrost/DRM port (vendor does not do it for you)

1. **Clock acquisition + parking sequence**: `clk_get` all six names
   (`fin_pll`, `fout_g3d_pll`, `aclk_g3d`, `mout_g3d`, `dout_aclk_g3d`,
   `aclk_g3d_400`); enable `aclk_g3d`; for every re-rate do the
   park-on-`aclk_g3d_400` -> `clk_set_rate(fout_g3d_pll)` ->
   switch-`mout_g3d`-back dance (`gpu_exynos7580.c:421-458`); refuse
   re-rate when domain off or clock gated.
2. **Regulator consumer**: `regulator_get("vdd_g3d")` +
   enable/disable, `regulator_set_voltage(vol, vol)` with the
   power-off guard; operating window 900000-1100000 uV across the 8 steps.
   A DT `vdd_g3d-supply` binding (or the same global lookup) is required —
   the rail will not follow the GPU otherwise.
3. **Core power order L2 -> shaders -> tiler with READY polls** plus the
   PWRON-during-PWROFF latch (`mali_kbase_pm_driver.c:501-519`).
   Panfrost's `panfrost_gpu_power_on()` is the natural home.
4. **DVFS policy**: 8-step table (160..800 MHz), utilization thresholds
   per row, voltage + PM-QoS MIF/INT/CPU floors per step, interactive
   governor default starting at 266 MHz. Upstream panfrost devfreq +
   OPP table is the clean vehicle; copy the (freq, uV) pairs into OPPs.
5. **Thermal clamp**: 8-step (or 6-step on v1 silicon) descending clip
   table; without it sustained load has no frequency backstop.
6. **ABB trim** (`set_match_abb(ID_G3D, asv)`) around voltage transitions
   for per-chip stability — safe to defer initially, but high-frequency
   steps (734/800 @ 1.10 V) are the first place instability will show.

### Gives for free (do NOT re-implement)

1. **Power-domain attach/detach**: `BUS_NOTIFY_BIND_DRIVER` notifier +
   `samsung,power-domain = <&pd_g3d>` handles `__pm_genpd_add_device` /
   remove automatically (`pm_domains.c:401-441`). No
   `dev_pm_domain_attach_by_name()` / `device_link_add()` calls needed.
2. **Domain on/off sequencing**: OPTION `0x1`, G3D magic `0xFFFFF0CF`,
   CONFIGURATION write + STATUS poll with timeout
   (`pm_domains.c:66-~110`) — genpd + runtime-PM invoke it.
3. **Rail presence**: BUCK4 `always-on + boot-on` (`j7xelte_common:524-532`)
   guarantees `vdd_g3d` exists before probe; the driver only manages its
   own enable refcount and voltage votes.
4. **Bus (MIF/INT/ISP) DVFS**: `ARM_EXYNOS7580_BUS_DEVFREQ` already scales
   the memory/interconnect side (`drivers/devfreq/Makefile:15`); the GPU
   driver only needs to keep issuing its PM-QoS floor requests.

## 8. Under-load risks

1. **Thermal clipping without a cooling map = overheat or hard throttle.**
   The vendor stack clips through `gpu_freq_table_800/668`
   (`gpu_cooling.c:65-86`, selected at `:436-438`). A bring-up driver that
   pins 800 MHz with no cooling registration will ride the junction limit
   until the SoC-level trip fires. Bring up at 266-350 MHz, add the cooling
   device before unlocking 668/800.
2. **BUCK4 droop on the 534->668 and 668->800 jumps.** Both cross a voltage
   corner (1.00 V -> 1.10 V at 668; MIF floor 728000 -> 825000 kHz at the
   same step). The vendor orders voltage-first on the way up (ABB pre/post
   + `regulator_set_voltage` before `clk_set_rate` via the DVFS handler).
   Reversing that order (clock before voltage) risks brownout crashes under
   load. `regulator-ramp-delay = <12000>` (`j7xelte_common:530`) bounds how
   fast the rail can follow — do not skip the post-settle.
3. **v1 silicon tops out at 668 MHz.** `soc_is_exynos7580_v1()` selects the
   668 table (`gpu_cooling.c:436-438`); programming 734/800 OPPs on v1
   silicon overclocks a part Samsung already derated. Gate the top two OPPs
   on non-v1 SoC revision.
4. **Clock change while gated/off hangs the re-rate.** `gpu_set_clock`
   returns -1 without touching the PLL when power is off or `aclk_g3d`
   ungated-off (`gpu_exynos7580.c:400-413`); blindly writing CON0/MUX while
   `pd-g3d` is off hangs the bus. Always gate frequency work on
   `gpu_is_power_on()` + `__clk_is_enabled(aclk_g3d)`.
5. **PMU spin without timeout.** `gpu_power_set_reg()` spins forever until
   `LOCAL_PWR_CFG` reads back on (`gpu_exynos7580.c:197-200`). If the PMU
   handshake stalls (e.g. rail or clock missing), that is a hard hang, not
   an error. A new driver should use the genpd timeout path
   (`pm_domains.c` ~1 ms poll) instead of copying this loop.
6. **No GPU devfreq entity to inherit** (`drivers/devfreq/Kconfig:131-140`
   is bus-only; `CONFIG_PM_DEVFREQ=y` serves MIF/INT/ISP). Power/topology
   tools that expect a `devfreq` GPU node will find nothing until the new
   driver registers one.

## 9. Exact evidence index (file:line)

- DVFS rows: `drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c:46-55`
- Row layout: `.../platform/exynos/mali_kbase_platform.h:195-207`
- Governor attrs (start 266, highspeed 420@95): `gpu_exynos7580.c:63-88`
- Clock globals: `gpu_exynos7580.c:176-181`; acquire: `:483-553`;
  parking re-rate: `:421-458`; guards: `:400-413`; on/off: `:235-313`
- MUX select write: `gpu_exynos7580.c:556`
  (`__raw_writel(0x1, EXYNOS7580_MUX_SEL_G3D0)`)
- Register dump map: `gpu_exynos7580.c:318-384`
- PMU poll/write: `gpu_exynos7580.c:187-201`
- Regulator get/enable/voltage: `gpu_exynos7580.c:679-741`, `:590-628`
- ABB hooks: `gpu_exynos7580.c:630-651`
- Notifier attach: `arch/arm64/mach-exynos/pm_domains.c:425-441`;
  DT parse: `:401-423`; add/remove: `:355-399`; G3D magic: `:87-89`
- Mali DT clocks + PD: `arch/arm64/boot/dts/exynos7580.dtsi:1066-1076`;
  pd-g3d node: `:1618-1622`
- BUCK4/vdd_g3d: `arch/arm64/boot/dts/exynos7580-j7xelte_common.dtsi:524-532`
- Cooling tables/select: `drivers/thermal/exynos7580/gpu_cooling.c:65-86`, `:436-438`
- ASV G3D tables: `arch/arm64/mach-exynos/asv-exynos7580_cal.c:22,59-96,116-117`
- Core PWRON/READY: `.../backend/gpu/mali_kbase_pm_driver.c:60-115,490-530,924-937`;
  coarse demand: `.../mali_kbase_pm_coarse_demand.c:32-39`
- Devfreq bus-only: `drivers/devfreq/Kconfig:131-140`; `CONFIG_PM_DEVFREQ=y` (vendor `.config:3269`)
