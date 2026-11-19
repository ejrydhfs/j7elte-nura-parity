# GPS rails: DT inventory, live state, and the BCM4752 supply decision

Slice: kernel DT + running regulator list (j7elte / SM-J700M, Nura 3.10.108).

## 1. What was read, and how trustworthy each source is

| Source | Path | Use |
|---|---|---|
| Running DT (decompiled) | `arch/arm64/boot/dts/exynos7580-j7elte_rev06.dtb` (`dtc -I dtb -O dts`) | rail set, names, ranges, `always-on`/`boot-on`, GPS node |
| Live DT | `/proc/device-tree` | confirms the board is the **rev06** DT: `model_info-hw_rev_end=0xff`, `vtouch_2.2v` @2.2 V (rev00 has `vtouch_1.8v` @1.8 V and `hw_rev_end=3`; the 4 `sm5703` rails exist in every revision, so they do not discriminate) |
| Stock DT | `AP.tar.md5` -> `boot.img` -> DTB #3 at file offset `0x1221000` (`model_info-hw_rev=6`) | cross-reference |
| PMIC driver | `drivers/regulator/s2mpu03.c`, `include/linux/mfd/samsung/s2mpu03.h` | enable-bit semantics, probe-time raw writes |
| Live state | `/sys/class/regulator/regulator.N/*` + consumer subdirs | hardware enable bits (see note) |

**Note on `state`:** for these LDOs `state` is not bookkeeping — `s2m_is_enabled_regmap()` (`s2mpu03.c:111-134`) does a real `sec_reg_read()` of the rail's control register and tests `enable_mask` (bits `[7:6]`, `S2MPU03_ENABLE_MASK=0x03<<6`). The only exception is **BUCK4**, whose `is_enabled` reads `EXYNOS_PMU_G3D_STATUS` because the rail is GPIO-gated when `g3d_en=1`. So every `state=` value below is the PMIC's actual state, and a rail that reads `disabled` is genuinely off.

**Stock vs kernel DT:** the whole `regulators { }` subtree of the stock rev06 DTB and of the kernel's rev06 DTB is **byte-identical** (unified diff: 0 lines). No rail was added, removed, renamed, re-ranged or had its `always-on`/`boot-on` flags changed between stock and our kernel.

## 2. Full rail table (36 regulators: 31 s2mpu03 + 4 sm5703 + dummy)

`AO` = `regulator-always-on`, `BO` = `regulator-boot-on` (both from DT). Voltages in mV from DT (`min-max`) and live `microvolts` (programmed setpoint, **not** a measurement). Consumers are the actual subdirs under `/sys/class/regulator/regulator.N/`.

| # | name | DT range | flags | live uV | state | users | consumer |
|---|------|----------|-------|---------|-------|-------|----------|
| 0 | regulator-dummy | - | - | - | - | 0 | - |
| 1 | cpu-cluster.1 | 600-1400 | AO BO | 887500 | enabled | 0 | cpu4-cpu-cluster.1 |
| 2 | cpu-cluster.0 | 600-1400 | AO BO | 1312500 | enabled | 0 | cpu0-cpu-cluster.0 |
| 3 | vdd_intmif | 600-1200 | AO BO | 1006250 | enabled | 0 | 17000010.bus_int, 17000020.bus_mif, 17000030.bus_isp |
| 4 | vdd_g3d | 600-1300 | AO BO | 812500 | **disabled** | 0 | - (PMU G3D domain off; see 5.2) |
| 5 | vdd_ldo1 | 800-1200 | AO | 1000000 | enabled | 0 | - |
| 6 | vqmmc | 1800-2800 | BO | 2800000 | enabled | 1 | 13560000.dwmmc2-vqmmc |
| 7 | vdd_ldo3 | 1600-2000 | AO BO | 1800000 | enabled | 0 | 0-001d-k2hh,vio (accel) |
| 8 | vdd_ldo4 | 1200 | AO BO | 1200000 | enabled | 0 | - |
| 9 | vdd_ldo5 | 1075-1350 | AO BO | 1200000 | enabled | 0 | - |
| 10 | vdd_ldo6 | 900-1225 | AO BO | 1000000 | enabled | 0 | - |
| 11 | vdd_ldo7 | 1600-2000 | AO BO | 1800000 | enabled | 0 | - |
| 12 | vdd_ldo8 | 2250-3300 | AO BO | 3000000 | enabled | 0 | - |
| 13 | vdd_ldo10 | 800-1350 | AO | 1050000 | enabled | 0 | - (see 5.1) |
| 14 | codec_ldo22 | 1000-2375 | - | 1800000 | disabled | 0 | 3-a004-vdd1 |
| 15 | codec_ldo23 | 1000-2375 | - | 1800000 | disabled | 0 | 3-a004-vdd2 |
| 16 | vdd_ldo24 | 1800-3375 | AO BO | 2850000 | enabled | 0 | - |
| 17 | vdd_ldo25 | 1000-2375 | AO BO | 1800000 | enabled | 0 | - |
| 18 | vmmc | 2800 | BO | 2800000 | disabled | 0 | 13560000.dwmmc2-vmmc |
| 19 | VDDIO_1.8V_CAM | 1800 | - | 1800000 | disabled | 0 | - |
| 20 | vtouch_2.2v | 2200 | BO | 2200000 | enabled | 1 | - (touch) |
| 21 | VDD_VTCAM_SENSOR_A2P8 | 2800 | - | 2800000 | disabled | 0 | - |
| 22 | LCD_3P0 | 3000 | BO | 3000000 | enabled | 1 | - |
| 23 | LCD_1P8 | 1800 | BO | 1800000 | enabled | 1 | - |
| 24 | VDDIO_1.8V_VT | 1800 | - | 1800000 | disabled | 0 | - |
| 25 | tsp_avdd | 3000 | - | 3000000 | enabled | 1 | - (touch analog) |
| 26 | VDD_SENSOR_2P85 | 2850 | - | 2850000 | disabled | 0 | 0-0048-stk,vio (light/prox) |
| 27 | VDD_CAM_SENSOR_A2P8 | 2800 | - | 2800000 | disabled | 0 | - |
| 28 | VDD_CAM_1P2 | 1200 | - | 1200000 | disabled | 0 | - |
| 29 | VDD_MOTOR | 3300 | - | 3300000 | disabled | 0 | - |
| 30 | ldo38 | 2800 | AO | 2800000 | enabled | 0 | **none** |
| 31 | ldo39 | 1800 | AO | 1800000 | enabled | 0 | **none** |
| 32 | VDDAF_2.8V_CAM | 2750-2800 (sm5703 ldo1) | - | 1500000* | disabled | 0 | - |
| 33 | VDD_SENSOR_2P8 | 2800 (sm5703 ldo2) | - | 2800000 | disabled | 0 | 0-0048-stk,vdd |
| 34 | none | 3000 (sm5703 ldo3) | - | 3000000 | disabled | 0 | - |
| 35 | VDD_VTCAM_1P2 | 1200 (sm5703 buck) | - | 1200000 | disabled | 0 | - |

\* sm5703 get_voltage returns a bogus uV while disabled (1500000); its DT range is 2750-2800 mV. Cosmetic; the rail is off because the camera stack never enables it.

Consumers with `users=0` but `state=enabled` are the always-on rails (framework claim) plus rails no device binds to by phandle — this is normal Samsung DT style (rails referenced by *name* from board code, not by phandle).

## 3. Does the DT tell us which rails feed the BCM4752? No.

Live and stock DT GPS node are identical and contain **no supply phandles**:

```
gps {
    compatible = "samsung,exynos-bcm4752";
    gpios = <&gpd2 6 0x1>;      /* GPS_PWR_EN */
    status = "okay";
};
```

Only one GPIO, no `vdd-supply`/`vio-supply`/`*-supply` anywhere in the node, and no `regulator_get()`-by-phandle in the driver. `exynos7580-s5neo_rev08.dts` has the identical node (same `gpd2 6` pin), so this is the family-wide pattern, not a j7elte omission.

**Stock kernel proof that stock did not use a rail API for GPS:** stock `boot.img` header gives kernel_size `0x00f3f4a8` at page offset `0x800`; extracting that 15,990,952-byte arm64 Image and grepping its strings yields
`samsung,exynos-bcm4752`, `GPS_PWR_EN`, `failed to get device node of bcm4752`, `fail to request gpio(GPS_PWR_EN)` — and **not a single `vdd_ldo*` string and no `[GPS]` rail message**. (`regulator_get` occurs 4x in the Image from unrelated drivers, so absence of the *rail name* is the meaningful signal: nothing in the stock kernel resolves a supply by name for GPS.)
=> On stock, GPS power = assert `GPS_PWR_EN` and rely on supplies that are already up (LK default + `always-on` DT). The `regulator_get(NULL, "vdd_ldo10")` block in `drivers/gps/sec_gps_bcm4752.c:69-113` is **our** addition, not reconstructed stock behaviour.

## 4. Shortlist: 1.8 V / 2.8-3.3 V rails that could be VDDIO / VBAT

Ranked by (a) voltage class, (b) `always-on` (a module whose power is gated by a separate EN pin must sit on an always-on rail), (c) *no* consumer in the DT/sysfs (nothing else claims it).

| Candidate pair | V | flags | live state | verdict |
|---|---|---|---|---|
| **ldo39** | 1.8 | AO | **enabled**, 1800000 uV, 0 users | **best VDDIO candidate** — unnamed ("ldo39"), no consumer, always-on. On a3xelte this rail is the NFC 1.8 V rail (`sec-nfc,i2c_1p8 = "ldo39"`); j7elte has no NFC node, so it is free here. |
| **ldo38** | 2.8 | AO | **enabled**, 2800000 uV, 0 users | **best VBAT candidate** — unnamed, no consumer, always-on, classic partner of ldo39. Note 2.80 V is at/below the 2.85 V-class rails used elsewhere on this PMIC; if the module is still dead this is the rail to scope. [INFERENCE on the BCM4752 2.85 V floor - datasheet class, not measured here] |
| vdd_ldo24 | 2.85 | AO BO | enabled, 2850000 uV, 0 users | alternate VBAT: exact 2.85 V, always-on, unclaimed |
| vdd_ldo25 | 1.8 | AO BO | enabled, 1800000 uV, 0 users | alternate VDDIO, unclaimed |
| vdd_ldo7 | 1.8 | AO BO | enabled, 1800000 uV, 0 users | possible (UART/modem IO class), unclaimed |
| vdd_ldo8 | 3.0 | AO BO | enabled, 3000000 uV, 0 users | only plausible "3.3 V-class" rail that is up |

Ruled out because they are consumed by other subsystems and/or off:
`vdd_ldo3` (accel), `vqmmc`/`vmmc` (SD), `codec_ldo22/23` (audio), `vtouch_2.2v`/`tsp_avdd` (touch), `LCD_3P0`/`LCD_1P8`, `VDDIO_1.8V_CAM`/`VDD_VTCAM_*`/`VDD_CAM_*`/`VDD_SENSOR_2P8*`/`VDDAF_2.8V_CAM`/`none` (camera, mostly off), `VDD_MOTOR` (the only 3.3 V rail, off), `vdd_g3d` (GPU), `vdd_ldo10` (see below).

No enabled rail is a 3.3 V rail. If the BCM4752 truly required 3.3 V VBAT, no such rail is up — but stock has the identical configuration, so stock could not have supplied 3.3 V from the PMIC either.

## 5. The s2mpu03 mask/opmode bug class, checked

### 5.1 LDO10 (`vdd_ldo10`, 1.05 V) — the one real instance

`s2mpu03.c` registration loop (lines 537-549) registers each rail with `init_data` from DT, so the core enables the `always-on` LDO10 immediately. Then, still in the same probe:

```c
if (pdata->g3d_en) {                                   /* s2mpu03.c:559 */
    sec_reg_update(iodev, S2MPU03_REG_B4CTRL1, 0x00, 0xC0);   /* BUCK4 -> GPIO ctrl */
    sec_reg_update(iodev, S2MPU03_REG_LDO_EFUSE, 0x10, 0x10); /* sync buck4 <-> ldo10 */
    sec_reg_update(iodev, S2MPU03_REG_L10CTRL,  0x00, 0xC0);  /* CLEARS LDO10 [7:6] */
}
```

`g3d_en = <1>` in the running DT (`exynos7580-j7elte_rev06.dtb.dts.tmp:3059`), so this branch always runs. Result: the framework has just enabled LDO10, and the probe then clears its enable bits behind the framework's back — `state` would have read `disabled` while `num_users`/`always-on` said otherwise. That is exactly the "DT says always-on, hardware is off, mask/opmode write did it" class. **It is a real bug, but it is not the GPS bug**: LDO10 is the 1.05 V rail EFUSE-synced to BUCK4 (GPU), its voltage class cannot be a BCM4752 supply, and stock cleared it the same way on a phone where GPS worked.

Our current driver (`sec_gps_bcm4752.c:95-115`) clears `LDO_EFUSE[4]` (`0x5B` bit 0x10) and re-sets `0x34[7:6]=0xC0`, which does restore the rail: it now reads `enabled` and `microvolts=1050000`. Keep it (it makes the rail state consistent) or drop it — the GPS verdict does not depend on it.

### 5.2 BUCK4 (`vdd_g3d`) — handled, not a bug

`s2m_enable()/s2m_disable_regmap()/s2m_is_enabled_regmap()` special-case `reg_id == S2MPU03_BUCK4 && g3d_en` (lines 84, 99, 117): enable/disable are no-ops and `state` is read from `EXYNOS_PMU_G3D_STATUS`. `vdd_g3d` reading `disabled` at 812500 uV while the DT says `always-on` is therefore *intentional* — the G3D power domain is off because nothing uses the GPU. Not related to GPS.

### 5.3 Everything else

Scanning all 35 `sec_reg_update`/`sec_reg_write` calls after line 540 (i.e. everything the probe does after registering the rails): the only per-rail enable-bit writes are the two `0xC0` masks above (`0x23` B4CTRL1 and `0x34` L10CTRL, lines 562/576). `SELMIF` (line 585, `0x3D` mask `0x7F`) only reassigns Buck3/LDO4/6/7/8 gating to `PWREN_MIF`, `RTC_BUF` (line 591) sets the low-jitter bit, and the 30 sequence-table writes (`0x5E`-`0x7B`) program power-up *order*, not enable state. No other always-on rail is cleared. Confirmed empirically: **every** DT `always-on` rail except BUCK4 reads `enabled` in hardware (`ldo1,3,4,5,6,7,8,10,24,25,38,39`).

## 6. Decision

**All rails that can plausibly supply the BCM4752 are up; there is no rail left to enable for GPS.**

* The supplies a module like this needs (1.8 V IO and a 2.8-3.0 V battery/RF rail) exist and are hardware-enabled: `ldo39` 1.8 V, `ldo38` 2.8 V, `vdd_ldo25` 1.8 V, `vdd_ldo24` 2.85 V, `vdd_ldo7` 1.8 V, `vdd_ldo8` 3.0 V — all `state=enabled`.
* The kernel/DT adds nothing that would gate them: the GPS node has no supplies, the stock kernel never touched a regulator, and the only per-rail raw clear in the PMIC probe hits LDO10 (GPU companion), which is now restored and is not a GPS rail anyway.
* Therefore a **silent GPS chip is not a regulator problem**. Remaining candidates are outside this slice: the `GPS_PWR_EN` load switch driven by `gpd2-6` (is it actually high at the pin? `gpio_get_value` vs pad state), the UART0 pad mux/clock, or the chip itself / its 26 MHz TCXO.
* If someone wants a decisive hardware answer rather than the register-level one above: measure `ldo38` (2.8 V) and the `gpd2-6`-switched GPS rail with a meter while `GPS_PWR_EN=1`. Nothing in software blocks it.

## 7. Reproduction

```sh
# live rail truth (state = PMIC register read for LDOs, PMU G3D status for BUCK4)
for r in /sys/class/regulator/regulator.*; do
  echo "$(basename $r)|$(cat $r/name)|$(cat $r/state)|$(cat $r/num_users)|\
$(cat $r/microvolts)|$(cat $r/min_microvolts)|$(cat $r/max_microvolts)|\
$(ls -d $r/*/ | sed 's|.*/regulator\.[0-9]*/||' | tr '\n' ',')"
done

# DT rail inventory (kernel rev06 == stock rev06)
dtc -I dtb -O dts -q arch/arm64/boot/dts/exynos7580-j7elte_rev06.dtb | sed -n '/regulators {/,/^\t\t};/p'
# stock DTB: boot.img DTB #3 at 0x1221000, model_info-hw_rev=6
```
