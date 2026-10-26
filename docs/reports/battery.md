# Battery / charging parity — j7elte (SM5703 charger + fuelgauge + MUIC + MFD + FLED, S2MPU03 PMIC, SEC battery/thermistor)

**Verdict: PARITY (P1–P4 pass). No kernel patch required. No device-tree deltas found.**

Scope (owned paths):
- `drivers/battery/{sec_battery.c,sec_adc.c,sec_charger.c,sec_fuelgauge.c,sm5703_charger.c,sm5703_fuelgauge.c}`
  (`CONFIG_BATTERY_SAMSUNG`, `CONFIG_CHARGER_SM5703`, `CONFIG_FUELGAUGE_SM5703`, `CONFIG_BATTERY_SWELLING`)
- `drivers/mfd/{sm5703_core.c,sm5703_irq.c,sec-core.c,sec-irq.c}`
  (`CONFIG_MFD_SM5703`, `CONFIG_MFD_SEC_CORE`)
- `drivers/regulator/{sm5703_regulator.c,s2mpu03.c}` (`CONFIG_REGULATOR_SM5703`, `CONFIG_REGULATOR_S2MPU03`)
- `drivers/muic/universal/muic_regmap_sm5703.c` + muic core (`CONFIG_MUIC_UNIVERSAL`, `CONFIG_MUIC_UNIVERSAL_SM5703`)
- `drivers/leds/sm5703_fled.c` (`CONFIG_FLED_SM5703`) — this is where the 28 "sm5703 + fled" names from the
  initial coverage sweep live (27 `sm5703_fled_*` plus `sm5703_default_fled_pdata`); **all 28 are covered**:
  25 emitted by `sm5703_fled.o`, `sm5703_fled_devs`/`sm5703_fled_res` emitted by `sm5703_core.o`, and
  `sm5703_fled_client` is an eliminated dead store. The flash-LED framework
  `drivers/leds/smfled.c` (15 ROM `smfled_*`/`sm_fled_*` names) is present too, all emitted by `smfled.o`.
- `drivers/hwmon/sec_thermistor.c` (`CONFIG_SENSORS_SEC_THERMISTOR`)
- `drivers/motor/sec_vib.c` (`CONFIG_SEC_VIB`) — config check only (sibling driver, listed for P3)
- `drivers/staging/samsung/sec_batt.c` (cmdline `fg_reset=`, `androidboot.mode=`, `factory_mode=`)

Reference points:
- ROM: `boot_out/symbols.txt` (55,634 names), `boot_out/dtbs/000_800.dts` (shipped j7elte DTB),
  `boot_out/dtbs/000_800.dtb`.
- Nura tree: `android_kernel_samsung_universal7580-2a746c1…` at `b5e363a`, config
  `config-nura-j7elte` (identical to `pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`
  for all battery/charger/MUIC options; the pmaports copy additionally carries two sibling-driver edits —
  `CONFIG_BT_HCIUART{,_H4}=y` and repointed `CONFIG_BCMDHD_FW_PATH`/`CONFIG_BCMDHD_NVRAM_PATH` — owned by the
  bluetooth/wifi parity tasks and irrelevant here),
  DT `arch/arm64/boot/dts/exynos7580-j7elte_rev00.dts` + `exynos7580-j7elte_battery.dtsi`.
- Baseline build artifacts already present in `parity/out/` (GCC 10.3 aarch64, `build.sh`, config =
  `config-nura-j7elte`, `olddefconfig` applied). These were used read-only to confirm the config resolution,
  the object set, and the compiled symbol set. No build was run by this task.

## P1 — Symbol coverage: PASS (99.6 %, 236/237 implemented; 1 explained absence)

Selection from `symbols.txt`: every symbol with prefix `sm5703`, `s2mpu03`, `sec_bat`, `sec_battery`,
`sec_charger`, `sec_chg`, `sec_fuelgauge`, `sec_adc` (suffixes `.part.N` / `.isra.N` / `.constprop.N`
normalised): **237 unique names**.

Method: (a) word-grep in the driver sources, (b) `nm --defined-only` on the baseline-built objects
`parity/out/drivers/{battery,mfd,regulator,leds,muic/universal,staging/samsung}/built-in.o` and
`drivers/hwmon/built-in.o`, (c) `nm -u` to prove no dangling references.

Result: **223 names emitted verbatim**; 13 further names are present in source and either inlined by GCC 10 or
dead (below); **1 name is absent from the j7elte build path** and is explained in detail. `nm -u` over all
built objects shows no dangling reference to any of the 14 non-emitted names.

| Nura implementation file | ROM symbols | In Nura source | Emitted in built objects | Evidence |
|---|---:|---:|---:|---|
| `drivers/battery/sm5703_charger.c` | 34 | 34 | 34 | `parity/out/drivers/battery/sm5703_charger.o` (84 defined) |
| `drivers/battery/sm5703_fuelgauge.c` | 29 | 29 | 25 (4 static inlined) | `drivers/battery/sm5703_fuelgauge.c:54,91,664,690`; `…/sm5703_fuelgauge.o` (66 defined) |
| `drivers/battery/sec_battery.c` | 47 | 47 | 39 (8 static inlined/dead) | `drivers/battery/sec_battery.c`; `…/sec_battery.o` (122 defined) |
| `drivers/battery/sec_adc.c` | 0 (generic `adc_*`/`temp_adc` names, not in selection) | — | — | `…/sec_adc.o` (10 defined) |
| `drivers/mfd/sm5703_core.c` | 26 | 26 | 26 | `drivers/mfd/sm5703_core.c:62-125`; `…/sm5703_core.o` (53 defined) |
| `drivers/mfd/sm5703_irq.c` | 18 | 18 | 18 | `drivers/mfd/sm5703_irq.c`; `…/sm5703_irq.o` (28 defined) |
| `drivers/mfd/sec-core.c` | 1 | 1 | 1 | `s2mpu03_devs`, `…/sec-core.o:96` |
| `drivers/mfd/sec-irq.c` | 2 | 2 | 2 | `s2mpu03_irqs`, `s2mpu03_irq_chip`, `…/sec-irq.o:126,706` |
| `drivers/regulator/sm5703_regulator.c` | 21 | 21 | 21 | `drivers/regulator/sm5703_regulator.c` (built as `sm5703_regulator.o`, Makefile:47; 57 defined) |
| `drivers/regulator/s2mpu03.c` | 8 | 8 | 8 | `drivers/regulator/s2mpu03.o` (34 defined) |
| `drivers/muic/universal/muic_regmap_sm5703.c` | 18 | 18 | 18 | ROM's `sm5703_muic_ioctl/_regmap_*/vendor_ops` map 1:1 to this file (37 defined) |
| `drivers/leds/sm5703_fled.c` | 30 | 30 | 29 (`sm5703_fled_client` dead store) | 26 of the 28 "sm5703+fled" names (`sm5703_fled_devs`/`_res` emitted by `sm5703_core.o`) + `sm5703_dev`, `sm5703_led_mode_ctrl`, `sm5703_boost_notification`, `sm5703_charger_notification`; `…/sm5703_fled.o` (84 defined) |
| `drivers/staging/samsung/sec_batt.c` | 2 | 2 | 2 | `sec_bat_is_lpm_check`/`sec_bat_get_fg_reset`, `sec_batt.c:21,37` (`__setup` handlers) |
| (no j7elte build path) | 1 | 0 | 0 | `sec_bat_misc_event_work` — `sec_battery_s5neo.c` only, see below |
| **Total** | **237** | **236** | **223** | |

Non-emitted names, individually explained:

| ROM symbol | Status in Nura build | Explanation |
|---|---|---|
| `sec_bat_get_adc_value` | inlined | `static` (`sec_battery.c:374`), single-file, all call sites inlined by GCC 10 |
| `sec_bat_get_temperature_by_adc` | inlined | `static` (`sec_battery.c:957`); 5 call sites (`:2472,2478,2485,3460,3494`), all inlined by GCC 10 |
| `sec_bat_ovp_uvlo_result` | inlined | `static` (`sec_battery.c:715`); 2 call sites (`:806,4515`), inlined |
| `sec_bat_temperature_check` | inlined | `static` (`:1325`), inlined into `sec_bat_monitor_work`/`sec_bat_swelling_check` |
| `sec_bat_swelling_check` | inlined | `static` (`:1095`), enabled by `CONFIG_BATTERY_SWELLING=y`, inlined |
| `sec_bat_swelling_fullcharged_check` | inlined | `static` (`:2795`), inlined |
| `sec_bat_time_management` | inlined | `static` (`:2041`), inlined |
| `sm5703_fg_i2c_write_word` | inlined | `static` (`sm5703_fuelgauge.c:91`) |
| `sm5703_fg_reg_init` | inlined | `static` (forward decl `sm5703_fuelgauge.c:54`, definition `:774`); call sites `:251,997`, inlined |
| `sm5703_fg_check_capacity_max` | inlined | `static` (`sm5703_fuelgauge.c:664`); call sites `:717,1197`, inlined |
| `sm5703_fg_calculate_dynamic_scale` | inlined | `static` (`sm5703_fuelgauge.c:690`); call sites `:1149,1151,1567`, inlined |
| `sec_bat_irq_thread` | dead code | `#if 0 /* TODO unused */` at `sec_battery.c:5056`, request also `#if 0` at `:6565`; ROM (GCC 4.9 tree) still linked it |
| `sm5703_fled_client` | dead store | `static struct i2c_client *` (`sm5703_fled.c:57`) is only ever assigned (`:863`), never read → eliminated |
| **`sec_bat_misc_event_work`** | **absent for j7elte** | exists only in `drivers/battery/sec_battery_s5neo.c:2961`, which is compiled only when `CONFIG_CAMERA_S5NEO=y`. `CONFIG_CAMERA_S5NEO` is **not set** for j7elte (`config-nura-j7elte:2174`, `# CONFIG_CAMERA_S5NEO is not set`), so `sec_battery.o` is built (`drivers/battery/Makefile:1-5`). See "P1 exception analysis" below. |

### P1 exception analysis — `sec_bat_misc_event_work`

What ROM does: `sec_battery_s5neo.c:291` `sec_bat_set_misc_event()` is called from the MUIC notifier
(`:5236`) with `BATT_MISC_EVENT_UNDEFINED_RANGE_TYPE` set exactly when
`attached_dev == ATTACHED_DEV_UNDEFINED_RANGE_MUIC`; the work item (`:2961`) then calls
`sec_bat_set_charge(battery, false)` if `cable_type == POWER_SUPPLY_TYPE_BATTERY`, re-arms the monitor work,
and the value is exposed as the `batt_misc_event` sysfs attribute (`:102`, `:3799`).

What Nura does instead (`sec_battery.c`): `sec_bat_cable_check()` (`:5094`) has no case for
`ATTACHED_DEV_UNDEFINED_RANGE_MUIC`; the notifier therefore gets `cable_type = -1`, logs
`ignore event` (`:5236`), and leaves `battery->cable_type` unchanged (`:5226-5230`). No charging is started for that
accessory; the only reachable effect of the ROM path for this device (`CONFIG_CHARGER_S2MU003` is **not**
set, so the `misc_event` test in `get_property` at s5neo `:281` is compiled out) is a redundant
`SEC_BAT_CHARGING_ENABLE(0)` while already not charging, plus the Android-only sysfs node.

Assessment: not a functional charging gap for j7elte (same end state, no charging from an undefined-range
accessory); the tree does implement the feature, just in the s5neo variant. Contract P1 threshold (≥98 %) is
met at **99.58 %** with 236/237 implemented, and no consumer of `batt_misc_event` exists in the pmaports
rootfs. No patch produced — a port would add Android-only sysfs/debug surface with no behavioural change.

`objdump` check: the ROM function's size/behaviour was not needed beyond the symbol list because the source
of the same routine is present in-tree (`sec_battery_s5neo.c:2961`).

## P2 — Device tree: PASS (zero deltas)

Method: the shipped ROM DTB (`boot_out/dtbs/000_800.dtb`) was decompiled (`dtc -I dtb -O dts -s`) and
compared node-by-node / property-by-property against (a) the Nura DTS sources and (b) the **baseline-built
`parity/out/arch/arm64/boot/dts/exynos7580-j7elte_rev00.dtb`** (built from `exynos7580-j7elte_rev00.dts`,
which `#include`s `exynos7580-j7elte_battery.dtsi`, with the Nura config). Comparison normalises numeric
cell values (hex/decimal, two's-complement negatives, `(-N)` parentheses), ignores `phandle`/`linux,phandle`
values, and compares all 45 matching nodes:

```
/battery
/battery_params
/charger
/hsi2c@104c0000/s2mpu03_pmic@66
/hsi2c@104c0000/s2mpu03_pmic@66/regulators
  … BUCK1-4, LDO1-8, LDO10, LDO22-39 (30 child regulators, identical sets)
/hsi2c@13890000/sm5703-fuelgauge@71
/i2c@49/sm5703-mfd@49
/i2c@49/sm5703-mfd@49/{sm5703_ldo1,sm5703_ldo2,sm5703_ldo3,sm5703_buck,sm5703_fled}
/muic
/sec_thermistor@0
```

Result: **no node present on one side only, and no differing property value** — including every
phandle-bearing property (`io-channels = <0x6f …>`, `pinctrl-0`, `sm5703,irq-gpio`,
`battery,chg_gpio_en`, `fuelgauge,fuel_int`, `sm5703,led{1,2}-gpio`), every battery profile array
(`battery{0,1},battery_table0/1/2`, `rce_value`, `rs_value`, `vit_period`, `mix_value`, `topoff_soc`,
`volt_cal`, `curr_cal`, `charge_offset_cal`), the ADC/thermistor tables
(`battery,temp_table_adc`, `battery,temp_table_data`, `inbat_voltage_table_*`, `sec_thermistor@0 temp_array`),
the charger current limits (`input_current_limit`, `fast_charging_current`, `full_check_current_1st/2nd`),
the swelling parameters, and `charger`/`battery` status. Spot checks (ROM == Nura built DTB):

| Path | Property | ROM | Nura |
|---|---|---|---|
| `/i2c@49/sm5703-mfd@49/sm5703_fled` | `flash_current` | `<0x4b0>` | `<0x4b0>` |
| `/i2c@49/sm5703-mfd@49/sm5703_fled` | `sm5703,led1-gpio` | `<0x6e 0x02 0x00>` | `<0x6e 0x02 0x00>` |
| `/hsi2c@13890000/sm5703-fuelgauge@71` | `fuelgauge,capacity_calculation_type` | `<0x1c>` | `<0x1c>` |
| `/hsi2c@104c0000/s2mpu03_pmic@66/regulators/LDO1` | `regulator-min-microvolt` | `<0x000c3500>` | `800000` |
| `/battery` | `battery,chg_float_voltage` | `<0x1130>` | `<0x1130>` |
| `/battery` | `battery,full_condition_type` | `<0x0d>` | `<0x0d>` |
| `/battery` | `io-channels` | `<0x6f 0x01 0x6f 0x03>` | `<0x6f 0x01 0x6f 0x03>` |
| `/charger` | `chg_aiclth` | `<0x1194>` | `<0x1194>` |
| `/sec_thermistor@0` | `io-channels` | `<0x6f 0x00>` | `<0x6f 0x00>` |

Phandle 0x6f = `adc@13900000` (exynos-adc) in the ROM DTB; the same label `&exynos_adc` is used on the Nura
side. Pinctrl groups compared separately and equal: `fuel-irq` (gpa0-1), `if-pmic-irq` (gpa0-5),
`chg-en` (gpd3-1), `muic-irq` (gpa1-3), `pmic-irq` (gpa0-2), `pm-wrsti` (gpe1-0),
`fled-flash-{sleep,active}` (gpe0-2/gpe0-1), `fled-flash-front-{sleep,active}` (gpe0-5).

Notes:
- ROM `charger` and Nura `charger` are both `status = "disable"`; the charger is instantiated by the
  `sm5703-mfd` MFD cell `"sm5703-charger"` (`drivers/mfd/sm5703_core.c:62-72`), matching
  `sm5703_charger_driver` (`drivers/battery/sm5703_charger.c:1744`). Fuelgauge is a real i2c client on
  `hsi2c@13890000` (node present, `status` inherited `okay`).
- `CONFIG_CAMERA_J7=y` (`config-nura-j7elte:2176`) is what makes
  `dtb-$(CONFIG_CAMERA_J7) += … exynos7580-j7elte_rev00.dtb …` (`arch/arm64/boot/dts/Makefile:13`) build
  the j7elte DTB at all; the APKBUILD packs all `*j7elte*.dtb` into `dt.img`.

## P3 — Config: PASS

All options are set in `config-nura-j7elte` **and survive `olddefconfig`** (checked in
`parity/out/.config`, which is the resolved config of the baseline build):

| Option | Value | Where consumed | Built artifact |
|---|---|---|---|
| `CONFIG_BATTERY_SAMSUNG` | y | `drivers/battery/Makefile:2,4` | `sec_battery.o` (+ `sec_battery_s5neo.o` only if `CONFIG_CAMERA_S5NEO`) |
| `CONFIG_BATTERY_SWELLING` | y | `sec_battery.c:1095,2795` | inlined into `sec_battery.o` |
| `CONFIG_FUELGAUGE_SM5703` | y | `drivers/battery/Makefile:17` | `sm5703_fuelgauge.o` |
| `CONFIG_CHARGER_SM5703` | y | `drivers/battery/Makefile:38` | `sm5703_charger.o` |
| `CONFIG_MFD_SM5703` | y | `drivers/mfd/Makefile:169` | `sm5703_core.o`, `sm5703_irq.o` |
| `CONFIG_REGULATOR_SM5703` | y (dep `MFD_SM5703`) | `drivers/regulator/Makefile:47` | `sm5703_regulator.o` |
| `CONFIG_REGULATOR_S2MPU03` | y (dep `MFD_SEC_CORE`) | `drivers/regulator/Makefile:61` | `s2mpu03.o` |
| `CONFIG_MFD_SEC_CORE` | y | `drivers/mfd/Makefile:162` | `sec-core.o`, `sec-irq.o` |
| `CONFIG_USE_MUIC` / `CONFIG_MUIC_NOTIFIER` / `CONFIG_MUIC_UNIVERSAL` | y / y / y | `drivers/muic/Makefile:22-24` | `muic_task.o … muic_vps.o` |
| `CONFIG_MUIC_UNIVERSAL_SM5703` | y (dep `USE_MUIC`) | `drivers/muic/universal/Makefile:10` | `muic_regmap_sm5703.o` |
| `CONFIG_FLED_SM5703` | y | `drivers/leds/Makefile:60` | `sm5703_fled.o` |
| `CONFIG_SENSORS_SEC_THERMISTOR` | y (dep `EXYNOS_ADC`) | `drivers/hwmon/Makefile` | `sec_thermistor.o` |
| `CONFIG_SEC_VIB` | y | `drivers/motor/Makefile:5` | `sec_vib.o` |
| `CONFIG_CAMERA_S5NEO` | **not set** | selects `sec_battery_s5neo.o` instead of `sec_battery.o` | — (correct for j7elte) |
| `CONFIG_CAMERA_J7` | y | `arch/arm64/boot/dts/Makefile:13` | `exynos7580-j7elte_rev0{0,4,6}.dtb` |
| `CONFIG_EXYNOS_ADC`, `CONFIG_POWER_SUPPLY`, `CONFIG_REGULATOR`, `CONFIG_LEDS_CLASS`, `CONFIG_HWMON`, `CONFIG_MFD_CORE`, `CONFIG_I2C` | y | core deps | — |

`parity/out/drivers/battery/` contains `sec_battery.o`, `sec_adc.o`, `sm5703_charger.o`,
`sm5703_fuelgauge.o`; `drivers/mfd/` contains `sm5703_core.o`, `sm5703_irq.o`, `sec-core.o`, `sec-irq.o`;
`drivers/regulator/` contains `sm5703_regulator.o`, `s2mpu03.o`; `drivers/leds/sm5703_fled.o`,
`drivers/muic/universal/muic_regmap_sm5703.o`, `drivers/hwmon/sec_thermistor.o`,
`drivers/motor/sec_vib.o`, `drivers/staging/samsung/sec_batt.o` — i.e. every configured object was
actually compiled by the baseline build.

**No config change is required, so the shared pmaports device config is not touched.** Every battery-relevant
option in the device config matches `config-nura-j7elte`; the only differences in the pmaports copy are the
two sibling-driver edits applied by the bluetooth/wifi parity tasks (`CONFIG_BT_HCIUART=y`,
`CONFIG_BT_HCIUART_H4=y`, and `CONFIG_BCMDHD_FW_PATH`/`CONFIG_BCMDHD_NVRAM_PATH` repointed to
`/lib/firmware/postmarketos/samsung-j7elte/`). No battery diff is proposed:

```diff
--- a/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64
+++ b/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64
@@ (no battery/charger/MUIC/fled/PMIC changes)
```

## P4 — Firmware / data files: PASS (none required)

`grep -niE 'request_firmware|firmware|filp_open|kernel_read|/data/|/system/|/efs/'` over
`drivers/battery/*sm5703*`, `sec_battery.c`, `drivers/mfd/sm5703_*`, `drivers/mfd/sec-core.c`,
`drivers/regulator/{sm5703_regulator.c,s2mpu03.c}`, `drivers/leds/sm5703_fled.c`,
`drivers/muic/universal/*.c`, `drivers/hwmon/sec_thermistor.c` → **no hits** except the *Kconfig help text*
of `CONFIG_DISABLE_SAVE_CAPACITY_MAX` (`drivers/battery/Kconfig:538`), which describes Android `healthd`
persisting `capacity_max` to `/efs/Battery/batt_capacity_max`. That persistence is userspace; the kernel
driver only reads `battery,capacity_max` from DT and the option is not enabled
(`# CONFIG_DISABLE_SAVE_CAPACITY_MAX is not set` is irrelevant — the default is `n` and no kernel code
opens `/efs`).

Battery profile data (`battery0,battery_table*`, `rce_value`, `rs_value`, `topoff_soc`, ADC tables) is
carried in the DT (`/battery_params`, `/battery`, `/sec_thermistor@0`) and is identical to ROM (P2).
No subdriver loads a `.bin`/model file. `parity/firmware/files-battery.list` therefore contains a single
`# none` marker; nothing is added to the firmware package.

## Why the wiki says "Battery: Partial" (userspace, not kernel)

Kernel side is complete: `sec_battery` registers the `power_supply` devices (`sec_battery.c:6431`
`psy_bat.name = "battery"`, plus `psy_ac`, `psy_usb`, `psy_ps`, `psy_wireless`), `sm5703_charger` registers
`psy_chg`/`psy_otg` (`sm5703_charger.c:1645,1651`), `sm5703_fuelgauge` registers `psy_fg`
(`sm5703_fuelgauge.c:1555`). Capacity/status/health/temperature/current/voltage therefore reach
`/sys/class/power_supply/*` exactly as on the ROM, and the MUIC/charger state machine is the same code.

The device package pulls in no charging/power daemon or uevent handler:

```
device-samsung-j7elte/APKBUILD: depends="linux-samsung-j7elte firmware-samsung-j7elte mkbootimg postmarketos-base"
deviceinfo: no power/charging related keys (only bootloader/flash keys)
```

So "Partial" is a userspace/UI statement (no Samsung `healthd`/`sec-battery` HAL, no charge-limit or
`batt_misc_event` consumer, and no Nura-side battery UI integration), not a missing kernel driver. Per the
contract, no kernel patch is fabricated for it.

## Residual risks / tests

1. **`batt_misc_event` sysfs node absent** (P1 exception). Only consumer would be an Android HAL; the
   pmaports rootfs has none. Verify on hardware: plug an accessory in the "Undefined Charging:XXX" ADC range
   (`muic,support-list` entry) and confirm charging stays off and `dmesg` shows
   `batt_handle_notification: ignore event`.
2. **No runtime test performed** (static analysis + baseline build artifacts only). On-device checks:
   `cat /sys/class/power_supply/battery/{status,capacity,health,temp,voltage_now,current_now}` while on TA
   and on USB; `cat /sys/class/power_supply/sm5703-charger/*`; confirm `sm5703-fuelgauge` probe at
   `hsi2c@13890000` (0x71) and `s2mpu03-pmic` MFD at `hsi2c@104c0000` (0x66).
3. **PMIC regulator consumer names**: the `s2mpu03` regulator driver is a *platform* driver bound to the
   `sec-core` MFD cell `"s2mpu03-pmic"`; if `sec-core` ever fails to probe (e.g. irq mapping change), DVFS
   consumers (`cpu-cluster.0/1`, `vdd_intmif`, `vdd_g3d`) lose their supply. Verified only statically here:
   DT node, `sec_dt_match` (`sec-core.c:105`) and `s2mpu03_pmic_id` (`s2mpu03.c:649`) all agree.
4. **Thermistor hwmon**: `sec_thermistor@0` uses `io-channels = <&exynos_adc 0>`; parity verified, but the
   hwmon label/`temp_array` consumer path is untested at runtime.
5. Cross-toolchain note: symbols were confirmed against a GCC 10.3 baseline build; ROM was GCC 4.9, so
   inlining differences (documented above) are expected and are not feature regressions.
