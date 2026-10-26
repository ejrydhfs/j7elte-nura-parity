# Sensors parity — j7elte (K2HH, CM36672P, STK3013, hall)

**Verdict: PARITY (P1–P4 pass). No kernel patch required. No device-tree deltas found.**

Scope: accelerometer K2HH (`stm,k2hh-i2c`), prox/ALS CM36672P (`cm36672p`), prox/ALS
STK3013 (`stk,stk3013`), hall/flip-cover (`hall`). Static verification only — no aarch64
cross-toolchain is installed, so no kernel build or on-device test was performed.

Reference points:
- ROM: `boot_out/symbols.txt`, `boot_out/dtbs/000_800.dts` (also checked `001_19800.dts`),
  strings from `boot_out/vmlinux.elf`, `system.raw` (ext4, no `/system` prefix).
- Nura tree: `android_kernel_samsung_universal7580-.../` at `b5e363a`, config
  `config-nura-j7elte` (sha512 `23e49e49…d0156`, identical to the config-samsung-j7elte.aarch64
  sha512 in `parity/pmaports/device/archived/linux-samsung-j7elte/APKBUILD:70`), DT
  `arch/arm64/boot/dts/exynos7580-j7elte_rev00.dts` (+ `exynos7580.dtsi`).

## P1 — Symbol coverage: PASS (91/91, 100 %)

| Driver | ROM symbols (`symbols.txt` prefix) | Implemented in Nura source | Evidence |
|---|---|---|---|
| k2hh | 41 | 41/41 | `drivers/sensors/k2hh.c` |
| cm36672p | 15 | 15/15 | `drivers/sensors/cm36672p.c` |
| stk3013 | 21 | 21/21 | `drivers/sensors/stk3013.c` |
| hall | 14 | 14/14 | `drivers/input/flip_cover.c` |

All names were matched against the driver sources. Only the compiler-generated local
clones need explanation (all originate from existing static functions, so they are not
missing features):

| ROM symbol | Source function |
|---|---|
| `k2hh_i2c_read.isra.3` | `k2hh_i2c_read()` (`drivers/sensors/k2hh.c`) |
| `k2hh_i2c_write.isra.4` | `k2hh_i2c_write()` |
| `k2hh_set_range.constprop.7` | `k2hh_set_range()` |
| `stk3013_enable_ps.isra.14` | `stk3013_enable_ps()` (`drivers/sensors/stk3013.c`) |
| `stk3013_get_ps_reading.isra.5` | `stk3013_get_ps_reading()` |
| `stk3013_set_ps_offset.isra.12` | `stk3013_set_ps_offset()` |
| `stk3013_set_ps_thd_h.isra.10` / `…_l.isra.11` | `stk3013_set_ps_thd_h()` / `…_l()` |

The 14 `hall_*` symbols are **not** from `drivers/sensors/`; they are the flip-cover
driver: `hall_probe/remove/suspend/resume` at `drivers/input/flip_cover.c:320,412,440,471`,
`hall_dt_ids` at `:432` with `{ .compatible = "hall" }` at `:433`, `hall_open/close` at
`:243/254`, `hall_detect_show` at `:128`, `hall_attrs`/`hall_attr_group` at `:141/150`,
`hall_device_driver` at `:491`, `hall_init/exit` at `:504/510`. This exactly matches the
ROM `hall` DT node and the ROM `hall_*` symbol set.

No other sensor driver was built into the ROM kernel: zero symbols with prefixes
`ppsx60 bh1733 cm3323 cm36652 cm36655 cm36686 tcs3490 tmd3782 tmd37xx sx9306 sx9310
sx9500 max86902 bma2x2 bmm050 yas537`, and no SSP/sensorhub symbols (the only `ssp`
hit, `mgmt_ssp_enable_complete`, is Bluetooth SSP). Therefore this is the complete
sensor set of the device; there is no magnetometer, barometer or HRM driver in the ROM
kernel, and the corresponding wiki rows are template values.

## P2 — Device tree: PASS, no deltas

Compared node by node, property by property (`000_800.dts` vs `exynos7580-j7elte_rev00.dts`).
Phandle/label numbers are ignored per contract; every phandle in the ROM `000_800` DTB was
resolved to its target node (gpa1 = `0x2b`, gpa3 = `0x29`, LDO3 = `0x2c`, sm5703_ldo2 =
`0x2e`, LDO34 = `0x2f`) and matches the Nura labels.

| Node | ROM (`boot_out/dtbs/000_800.dts`) | Nura rev00 | Match |
|---|---|---|---|
| `k2hh-i2c@1D` | `:3003-3018` | `:1153-1168` | identical |
| `cm36672p@60` | `:3020-3037` | `:1169-1186` | identical |
| `stk3013@48` | `:3039-3060` | `:1187-1208` | identical |
| `hall` | `:5345-5351` | `:1421-1426` | identical |
| `sensor_i2c` pinctrl | `:2229-2235` (phandle `0x2a`) | `:1545-1550` | identical |
| `proximity-int` pinctrl | `:413-418` (phandle `0x2d`) | `:1141-1145` | identical |
| `hall-irq` pinctrl | `:351-357` (phandle `0x9a`) | `:227-232` | identical |
| `hsi2c@13870000` parent | `:2989` (children end `:3061`) | `:1147` (children end `:1209`; base props `exynos7580.dtsi:704-717`) | identical after merge |

Values verified equal:

- **k2hh**: `reg=<0x1d>`, irq `gpa1 0 0`, `k2hh,vio-supply=ldo3_reg`,
  axis_map x/y/z = `1/0/2`, negate x/y/z = `0/1/1`, `poll_interval=100`, `min_interval=2`.
  Both nodes have no `status`/`pinctrl`.
- **cm36672p**: `reg=<0x60>`, `pinctrl-0=proximity_int` (gpa1-4, func 0, pud 3),
  irq `gpa1 4 0`, `vdd=sm5703_ldo2_reg`, `vled=ldo34_reg`, `default_hi_thd=17`,
  `default_low_thd=13`, `cancel_hi_thd=10`, `cancel_low_thd=7`, `ps_it=4`,
  `led_current=5`, `default_trim=0`.
- **stk3013**: `reg=<0x48>`, irq `gpa1 4 0`, `vdd=sm5703_ldo2_reg`, `vio=ldo34_reg`,
  `state-reg=0x2d`, `psctrl-reg=0x70`, `alsctrl-reg=0x2a`, `ledctrl-reg=0xff`,
  `wait-reg=3`, `ps-thd-h=18`, `ps-thd-l=13`, `ps-cancel-thd-h=10`,
  `ps-cancel-thd-l=7`, `ps-cal-skip-adc=8`, `ps-cal-fail-adc=17`,
  `ps-default-offset=0`, `transmittance=500`.
- **hall**: `compatible="hall"`, `pinctrl-0=hall_irq` (gpa3-6, func 0xf, pud 0, drv 0),
  `hall,gpio_flip_cover=<gpa3 6 0>`, `status="okay"`.

Note on the I2C pins: the ROM DT contains an *unreferenced* `hs-i2c0-bus` group
(`000_800.dts:1215`, pud 3); the group actually referenced by `hsi2c@13870000` is
`sensor_i2c` (pud 0). Nura's rev00 overrides the dtsi default `hs_i2c0_bus` with
`&sensor_i2c` (`exynos7580-j7elte_rev00.dts:1149`), so the effective pin configuration is
identical to the ROM's. The ROM `001_19800` DTB has the same sensor nodes and pinctrl
values, so the comparison is not DTB-specific.

No sensorhub, regulator or GPIO delta exists. ROM and Nura both leave `cm36672p@60` and
`stk3013@48` enabled on the same `gpa1-4` interrupt (the shipped HAL selects one).

## P3 — Config and registration: PASS

Config `config-nura-j7elte` (hash-verified as the config built by the pmaports APKBUILD):

- `CONFIG_SENSORS=y` (line 3481) — builds `drivers/sensors/sensors_core.o`
  (`drivers/sensors/Makefile:7`), reachable via `drivers/Makefile:181`.
- `CONFIG_SENSORS_K2HH=y` (3482) → `drivers/sensors/Makefile:10`.
- `CONFIG_SENSORS_CM36672P=y` (3495) → `drivers/sensors/Makefile:18`.
- `CONFIG_SENSORS_STK3013=y` (3502) → `drivers/sensors/Makefile:22`.
- `CONFIG_INPUT_FLIP_COVER=y` (1431) → `drivers/input/Makefile:30` (hall driver).
- Dependencies met: `CONFIG_I2C=y` (1632), `CONFIG_INPUT=y` (1412), `CONFIG_INPUT_EVDEV=y`
  (1426), `CONFIG_REGULATOR=y` (2054), `CONFIG_OF=y` (1001), `CONFIG_SYSFS=y` (3611).
  The three sensor Kconfig entries depend only on `I2C`
  (`drivers/sensors/Kconfig:14-21,118-123,167-172`).
- `CONFIG_CERTIFY_HALL` (1433) and `CONFIG_SENSORS_HALL_IRQ_CTRL` (1432) are off and are
  irrelevant: they select `certify_hall.c`/key IRQ control, while the `hall` DT node is
  bound by `flip_cover.c` (`compatible = "hall"`, line 433). ROM `symbols.txt` has none of
  the `certify_hall_*` symbols, confirming the ROM used the same driver.
- `CONFIG_SENSORS_K2HH_VDD` is unset; ROM matches: `k2hh,vdd` is absent from
  `vmlinux.elf` strings while `k2hh,vio`, `cm36672p,vdd`, `cm36672p,vled`, `stk,vdd`,
  `stk,vio` are present.

Registration paths are identical to the ROM fork: input-subsystem devices plus sysfs,
no IIO. k2hh registers `input_register_device()` at `k2hh.c:1177` with name
`accelerometer_sensor` (`k2hh.c:39`) and creates the sensors symlink/attribute group at
`:1181/:1186`; cm36672p registers `proximity_sensor` at `cm36672p.c:1117/1123` and its
attribute group at `:1136`; stk3013 registers `proximity_sensor` at
`stk3013.c:1630/1634` and its group at `:1647`; hall registers an input device
(`flip_cover.c:389`) and `hall_attr_group` under `sec_key` (`:382`). The same device names
(`accelerometer_sensor`, `proximity_sensor`, `hall_detect`) are present as strings in the
ROM `vmlinux.elf`, so userspace ABI matches.

No config diff is needed; the pmaports tree was not modified.

## P4 — Firmware/blobs: N/A (no firmware), PASS

None of the four drivers calls `request_firmware()`; they only read calibration data at
runtime:

- `k2hh.c:41` `/efs/FactoryApp/accel_calibration_data`
- `cm36672p.c:61` `/efs/FactoryApp/prox_cal`
- `stk3013.c:143` `/efs/FactoryApp/prox_cal`

The same two path strings are present in the ROM `vmlinux.elf`, and the ROM
`/vendor/firmware` contains no sensor blob (only BT/camera/mfc). These files live on the
EFS partition, not in `system.raw`, and are not present in
`parity/firmware/MANIFEST.sha512`. Their absence is non-fatal: k2hh ignores the return
value on enable (`k2hh.c:692`, zeros calibration), cm36672p tolerates `-ENOENT`
(`cm36672p.c:445-447`), stk3013 only logs (`stk3013.c:1531-1538`). Hence
`parity/firmware/files-sensors.list` contains no mappings.

ROM userspace ships `sensors.universal7580.so` (`/lib/hw` in `system.raw`), but that is a
ROM HAL, not kernel firmware and not usable under Nura.

## Residual risks / untested behaviour

1. **No userspace consumer.** The Nura device package
   (`parity/pmaports/device/archived/device-samsung-j7elte/APKBUILD`) depends only on the
   kernel, `mkbootimg`, `postmarketos-base`; no sensor daemon/HAL is installed, and
   `iio-sensor-proxy` does not support these Samsung input/sysfs drivers (they are not IIO).
   The kernel feature is therefore untested, not broken.
2. **No calibration data** on a converted device (`/efs/FactoryApp/*`): readings work but
   with zero/offset compensation; userspace can write calibration via the k2hh/stk3013
   sysfs attributes if `/efs` is mounted.
3. **No build/boot test**: no aarch64 cross-toolchain; verification is source/symbol/DT/config
   comparison (`git apply --check` not applicable because no patch is produced).
4. cm36672p and stk3013 both claim gpa1-4; only one can be active in userspace. This is
   inherited from the ROM DT and is not a regression.
