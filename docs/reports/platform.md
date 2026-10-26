# SoC platform parity — j7elte (thermal/TMU, devfreq, IOMMU v6, PMU/PM domains, cpufreq)

**Verdict: PARITY (P1–P4 pass). No kernel patch required. No device-tree deltas found.
No runtime firmware is used by any driver in this group (P4 N/A).**

Scope: `drivers/thermal/**` (incl. `drivers/thermal/exynos7580/`), `drivers/devfreq/**`,
`drivers/iommu/**`, `drivers/cpufreq/**`, `drivers/soc/**` (absent in this tree; PMU/PM-domain
code lives in `arch/arm64/mach-exynos/` + `drivers/base/power/`, verified read-only), and the
matching DT nodes. `drivers/bts/**` is display-owned (see overlap note). Static verification
only — no kernel build was run (cross-toolchain exists but building was not requested).

Reference points:
- ROM: `boot_out/symbols.txt` (55,634 symbols), `boot_out/dtbs/{000_800,001_19800,002_32800}.dts`.
- Nura tree `android_kernel_samsung_universal7580-…2a746c1` at baseline `b5e363a`; config
  `config-nura-j7elte`; DTS `arch/arm64/boot/dts/exynos7580.dtsi` +
  `exynos7580-j7elte_rev00.dts`.
- DT comparison base: Nura DTB compiled by the peer pipeline (`/tmp/nura_j7.dtb`), decompiled
  with `dtc -I dtb -O dts -s` (matches the ROM `dtc -s` decompiles byte-format-wise).

## P1 — Symbol coverage: PASS (691/691 accounted, 100 %)

Method: every ROM symbol matching `tmu|thermal|devfreq|ppmu|busfreq|pm_domain|exynos_pd|
cpufreq|sysmmu|iommu|exynos_pm|exynos_pmu` (691 after dropping the unrelated ethernet symbol
`smsc9500_rx_setmulticastlist`) was matched by identifier against the Nura sources
(`drivers/`, `arch/arm64/`, `arch/arm/mach-exynos/`, `include/`, `sound/`). 591 symbols match
source identifiers directly; the remaining 100 are compiler/linker- or macro-generated names,
each traced to its generating construct (table below). Zero unexplained symbols.

| Group (source) | ROM symbols | In Nura | Evidence |
|---|---|---|---|
| thermal core + of-thermal + hwmon (`drivers/thermal/{thermal_core,of-thermal,thermal_hwmon,step_wise}.c`, exynos7580 variant) | 106 | 106/106 | `thermal_init` `drivers/thermal/exynos7580/thermal_core.c:1888` (`fs_initcall`); TMU driver `exynos_tmu_driver` `drivers/thermal/exynos7580/exynos_tmu.c:912`, `module_platform_driver` :923; `exynos7580_tmu_registers` `exynos_tmu_data.c:462`, `exynos7580_default_tmu_data` :659; of_match `samsung,exynos7580-tmu` `exynos_tmu.c:650` |
| devfreq core + governors (`drivers/devfreq/devfreq.c`, `governor_simple{exynos,ondemand}.c`) | 193 | 193/193 | `devfreq_init` `devfreq.c:1012`; `devfreq_simple_exynos_init` `governor_simpleexynos.c:242`; 7580 bus drivers `exynos7580_bus_{int,isp,mif}.c` (`module_platform_driver` :410/:349/:509), `exynos7580_bus.c`, `exynos7580_ppmu.c:399` of_match `samsung,exynos7580-ppmu`, `exynos_ppmu_fw.c`/`FW_exynos_ppmu.c` (PPMU_*); thermal QoS `min_{int,isp,mif}_thermal_qos` `exynos7580_bus_{int,isp,mif}.c:40/37/57`, `mif_thermal_level_cs0/cs1` `exynos7580_bus.c` |
| IOMMU v6 + IOVMM v6 + IOMMU core (`drivers/iommu/{exynos-iommu-v6,exynos-iovmm-v6,exynos-iommu-log,iommu,of_iommu}.c`) | 130 | 130/130 | `exynos_iommu_init` `exynos-iommu-v6.c:2639` (`arch_initcall_sync` :2690), `exynos_iommu_prepare` :2938 (`subsys_initcall_sync`); `sysmmu_of_match` :1792 (`samsung,exynos7420-sysmmu`); iommu-bus scan `for_each_compatible_node(…, "samsung,exynos-iommu-bus")` :2582; `iommu_init` `iommu.c:915` |
| cpufreq core + table + stats (`drivers/cpufreq/{cpufreq,freq_table,cpufreq_stats}.c`) | 109 | 109/109 | `pure_initcall(init_cpufreq_transition_notifier_list)` `cpufreq.c:121`; `module_init(cpufreq_stats_init)` `cpufreq_stats.c:1278` |
| cpufreq interactive governor (`drivers/cpufreq/cpufreq_interactive.c` + `include/trace/events/cpufreq_interactive.h`) | 85 | 85/85 | `fs_initcall(cpufreq_interactive_init)` `cpufreq_interactive.c:1649`; trace events at `cpufreq_interactive.h:9–130` |
| SMP cpufreq driver (`drivers/cpufreq/exynos-smp-cpufreq.c`) | (in rows above) | — | `exynos_cpufreq_device_init` `exynos-smp-cpufreq.c:1343`, `module_platform_driver(exynos_smp_platdrv)` :1436 |
| cpu/gpu cooling (`drivers/thermal/exynos7580/{cpu_cooling,gpu_cooling}.c`) | 14 | 14/14 | built per `drivers/thermal/Makefile:21–25` |
| PMU / PM domains / suspend (`arch/arm64/mach-exynos/{pm_domains,pm,pmu_cal_sys,exynos-powermode*}.c`, `drivers/base/power/domain_governor.c`) | 43 | 43/43 | `subsys_initcall(exynos5_pm_domain_init)` `pm_domains.c:631`, `late_initcall_sync(exynos_pm_domain_idle)` :640; `exynos_pm_drvinit` `pm.c:313`/`exynos_pm_syscore_init` :327 (`arch_initcall` :320/:344); `pm_domain_always_on_gov` `domain_governor.c`; `samsung,exynos-pd` scans `pm_domains.c:323,452,570` |
| cross-driver consumers (other peers' scope) | 11 | 11/11 | `gpu_tmu_notifier` (GPU), `sc_sysmmu_fault_handler`/`exynos_mfc_sysmmu_fault_handler`/`vb2_ion_*` (media), `lpass_get_iommu_domain`/`dma_prop_iommu` (`sound/soc/samsung/lpass.c:287`/`dma.c:501`), `rndis_cpufreq_lock` (USB) — all present in the Nura tree |

### Generated symbols (100, all explained)

| Category | Count | Generator (Nura evidence) |
|---|---|---|
| `__initcall_<fn><level>[s]` wrappers | 22 | one per initcall site; base init functions all present (cited above), e.g. `__initcall_thermal_init5` ← `fs_initcall(thermal_init)` `exynos7580/thermal_core.c:1888` |
| `<driver>_init`/`_exit` module wrappers | 10 | `module_platform_driver()` expansions: `exynos_tmu.c:923`, `exynos7580_bus_int.c:410`, `exynos7580_bus_isp.c:349`, `exynos7580_bus_mif.c:509`, `exynos7580_ppmu.c:415` |
| `__devfreq_(init\|deinit)_of_table_<name>` | 4 | `DEVFREQ_INIT_OF_DECLARE`/`DEVFREQ_DEINIT_OF_DECLARE` (`drivers/devfreq/devfreq_exynos.h:368–378`) invoked at `exynos7580_bus.c:2476–2479` |
| ftrace/tracepoint artifacts (`__tracepoint*`, `__tpstrtab_*`, `__event_*`, `event_class_*`, `ftrace_*`, `perf_trace_*`, `print_fmt_*`) for the 10 `cpufreq_interactive` events | 64 | `DECLARE_EVENT_CLASS`/`DEFINE_EVENT`/`TRACE_EVENT` at `include/trace/events/cpufreq_interactive.h:9–130` |

A prior narrow sweep reported 47/47 (thermal) and 40/40 (iommu) on a driver-specific-symbol
basis; this sweep is a superset (adds generic subsystem core symbols) and is fully resolved.

## P2 — Device tree: PASS, no deltas

Compared node-by-node, property-by-property: every platform node in the ROM DTBs against the
compiled Nura DTB (decompiled with `dtc -s`, identical formatting). Property order inside a
node and phandle numbering are ignored per contract. Result: **all values identical**.

| Node (ROM 000_800.dts) | Nura (exynos7580.dtsi / j7elte_rev00.dts) | Result |
|---|---|---|
| `clock-controller@0x10010000` `samsung,exynos7580-clock` (:2718) | :376 | identical (`reg = <0x0 0x10010000 0x0 0x1000>`, `#clock-cells = <1>`) |
| `bus_int` `samsung,exynos7-devfreq-int` (:2726) | :382 | identical (reg, `bus_type_int=0`, `downdifferential=20`, `pm_qos_class=4`, `upthreshold=70`, `cal_qos_max=400000`, `default_qos=100000`, `initial_freq=400000`, `polling_period=0`) |
| `bus_mif` `samsung,exynos7-devfreq-mif` (:2739) | :397 | identical (incl. `downthreshold=60`, `urgentthreshold=80`, `idlethreshold=50`, `cal_qos_max=825000`, `default_qos=416000`) |
| `bus_isp` `samsung,exynos7-devfreq-isp` (:2754) | :415 | identical (`pm_qos_class=13`, `upthreshold=95`, `cal_qos_max=530000`, `default_qos=200000`) |
| `ppmu` `samsung,exynos7580-ppmu` + `ppmu-{int,mif,total}` (:2765) | :427 | identical (all reg sets) |
| `tmuctrl@10030000` / `@10060000` / `@10040000` `samsung,exynos7580-tmu` (:2861/:2867/:2897) | :554/:560/:588 | identical (incl. the 2-entry `reg` pairs and IRQs 0x1b3/0x72/0x1b4) |
| `pd-g3d/pd-mfcmscl/pd-aud/pd-disp/pd-isp@…` `samsung,exynos-pd` + spd children (:4259–4329) | :1618–1672 | identical (`pd-option`, `status = "disabled"` on pd-aud, `bts-status = "enabled"` on pd-disp/pd-isp/spd-mfc) |
| `iommu-domain_{disp,fimc-is,mfc,mscl}` `samsung,exynos-iommu-bus` + 7 × `sysmmu@…` `samsung,exynos7420-sysmmu` (:4096–…) | :1455–… | identical (domain-clients, reg, interrupts, qos, clocks, `sysmmu,block-when-stop`, `sysmmu,acg_disable`, all `pb-info`/`master_axi_id_list` entries) |
| `fixed-rate-clocks/oscclk` `samsung,exynos7580-oscclk` (:4834) | `exynos7580-j7elte_rev00.dts:31` | identical (`clock-frequency = <26000000>`) |
| `cpus` subtree (cpufreq `operating-points` read by `exynos-smp-cpufreq.c:515–532`) | `exynos7580.dtsi` | identical |

Cross-checked the other two ROM DTB variants: `001_19800.dts` and `002_32800.dts` differ from
the Nura build only in phandle numbers inside `domain-clients`/`master_axi_id_list` (e.g.
002 `0x6b→decon_fb@14830000` where 000/Nura use `0x65`; resolved every differing phandle to
the same target node). Ignored per contract.

Also compared against the peer full-DT sorted line diff (`/tmp/rom_000_800.sorted.dts` vs
`/tmp/nura_j7.sorted.dts`): the only content deltas in the whole DT are Nura-only
`arm-pmu`/`android-fstab` nodes and the BT `bt-en` pinctrl relocation (bluetooth peer's gap),
none of them platform-scope.

Note: the assignment-mentioned compatible `samsung,exynos7580-bus` does not exist in either
the ROM DTB or the Nura tree; bus-frequency coverage is the `bus_{int,mif,isp}` devfreq nodes
above (`samsung,exynos7-devfreq-*`), fully matched.

## P3 — Config: PASS

| Option | Nura config | Builds |
|---|---|---|
| `CONFIG_EXYNOS_THERMAL=y` (:1942), `CONFIG_EXYNOS_THERMAL_CORE=y` (:1949) | ✓ | `exynos7580/Makefile:4–7` (exynos_tmu.o + exynos_tmu_data.o + exynos_thermal_common.o) |
| `CONFIG_THERMAL=y` (:1930), `THERMAL_HWMON`/`THERMAL_OF`/`THERMAL_GOV_STEP_WISE` (:1931–1937), `CPU_THERMAL` (:1939), `GPU_THERMAL` (:1940) | ✓ | `drivers/thermal/Makefile:5–25` (exynos7580/thermal_core.o, of-thermal.o, step_wise.o, cpu/gpu_cooling.o) |
| `CONFIG_ARM_EXYNOS7580_BUS_DEVFREQ=y` (:3279) | ✓ | `drivers/devfreq/Makefile:15` (exynos7580_bus{,_mif,_int,_isp}.o, exynos7580_ppmu.o, exynos_ppmu_fw.o, FW_exynos_ppmu.o) |
| `CONFIG_EXYNOS_IOMMU_V6=y` (:3249), `CONFIG_EXYNOS_IOVMM_V6=y` (:3251), `EXYNOS7_IOMMU_CHECK_DF=y`, `EXYNOS_IOMMU_NO_MASTER_CLKGATE=y`, `EXYNOS_IOMMU_EVENT_LOG=y` | ✓ | `drivers/iommu/Makefile:16,20,21`; `CONFIG_IOMMU_API=y` (:3246) → iommu.o, `CONFIG_OF_IOMMU=y` (:3248) → of_iommu.o. Plain `CONFIG_EXYNOS_IOMMU` unset → the non-v6 `exynos-iommu.c` is not built, matching the ROM symbol set (v6-only families `exynos_sysmmu_*`, `sysmmu_pm_genpd_*`, `sysmmu_hook_driver_register`) |
| `CONFIG_ARM_EXYNOS_SMP_CPUFREQ=y` (:481) | ✓ | `drivers/cpufreq/Makefile:59` → exynos-smp-cpufreq.o |
| `CONFIG_CPU_FREQ=y` (:457), `CPU_FREQ_TABLE` (:458), `CPU_FREQ_STAT` (:459), `CPU_FREQ_GOV_INTERACTIVE=y` + default (:466,471) | ✓ | cpufreq.o, freq_table.o, cpufreq_stats.o, cpufreq_interactive.o |
| `CONFIG_SEC_PM=y` (:3223), `CONFIG_PM=y`/`PM_RUNTIME=y`/`SUSPEND=y` (:439/436/426), `PM_GENERIC_DOMAINS{,_SLEEP,_RUNTIME}=y` (:447–450), `CONFIG_ARCH_EXYNOS=y` (:252), `CONFIG_SOC_EXYNOS7580=y` (:289), `CONFIG_EXYNOS7580_PMU_CP=y` (:1116) | ✓ | `arch/arm64/mach-exynos/Makefile:17,21,23,25,27,34` (pm_domains-exynos7580.o, pm_domains.o, pmu.o, pmu_cal_sys{,_exynos7580}.o, exynos-powermode{,-smp}.o) |

## P4 — Firmware/blobs: N/A

No `request_firmware`/firmware blob of any kind in `drivers/thermal/**`, `drivers/devfreq/**`,
`drivers/iommu/**`, `drivers/cpufreq/**` (verified by grep across the whole subtrees).
No `parity/firmware/files-platform.list` is produced.

## P5 — Patch

None. P1–P4 pass; nothing to patch.

## Overlap with display (`drivers/bts`)

The `bts-status = "enabled"` properties on `pd-disp`, `pd-isp` and `spd-mfc` (and the
`bts_*` symbols, `CONFIG_EXYNOS7580_BTS=y` per `display.md:48`) belong to display's
bts-exynos7580 analysis; the property values are identical ROM↔Nura (verified above), so
display's BTS work and this report have no conflicting DT deltas. `gpu_dvfs_calculate_env_data_ppmu`
is GPU-scope; `rndis_cpufreq_lock` USB-scope; `sc/mfc` sysmmu fault handlers and
`lpass_get_iommu_domain` media/audio-scope — all merely registered into the platform
interfaces checked here and all present in the Nura tree.

## Residual risks / tests

- Static verification only (no kernel build or boot). Recommended: build via
  `parity/build.sh` and boot-test `/sys/class/thermal/thermal_zone*`, `/sys/class/devfreq/*`,
  `/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` (interactive),
  `/sys/kernel/debug/smmu` (or dmesg `sysmmu` probe lines), and suspend/resume.
- `CONFIG_SEC_PM_DEBUG=y` is enabled in Nura; the ROM build's debug options were not
  reconstructible from symbols alone. Debug-only code (`exynos_iommu_debugfs_*` is present in
  ROM symbols and matches `EXYNOS_IOMMU_EVENT_LOG=y`), so no functional impact.
