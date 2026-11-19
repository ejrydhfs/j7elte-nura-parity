# Stock boot.img DT vs running DT (GPS scope)

Slice: `gps-stockdt`. Question: does the stock boot image's device tree — and the
stock kernel — enable anything for GPS (extra rail, pin, LDO enable state) before
the daemon runs, and does our running DT differ in a way that explains the silent
BCM4752?

## Verdict

- **DT: no GPS-relevant difference.** Every GPS/UART/PMIC node in the stock DTB
  is present with identical values in our running DT. Only phandle *numbers*
  and BT pinctrl labels differ.
- **Kernel: stock adds nothing extra before the daemon.** Stock's GPS code
  (`sec_gps_bcm4752.c`) only requests `GPS_PWR_EN` (gpd2-6) and drives it
  **low**, then exports it via `/sys/class/sec/gps/`. It touches **no
  regulator**. There is no GPS supply phandle, no reset gpio, no gpio-hog.
- **The one PMIC subtlety is inherited unchanged:** `s2mpu03` probe sees
  `g3d_en = <1>` and deliberately **clears** `L10CTRL` (0x34) bits [7:6] while
  setting `LDO_EFUSE` (0x5B) bit 4 ("sync buck4 and ldo10"). That code is
  byte-identical in our tree, so our kernel does the same. `vdd_ldo10` i2c
  enable readback being "disabled" is therefore *expected stock behaviour*, not
  necessarily a dead rail.
- **Consequence for bring-up:** the chip's silence cannot be attributed to DT
  pinmux/rail/power-node differences. Remaining suspects are (a) the daemon's
  power/wake sequence, (b) our added `vdd_ldo10` write fighting the hw sync
  (our deviation, not stock parity), (c) electrical (levels/clock).

## 1. Extracting the stock DT

Source: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7stock/AP.tar.md5`
→ `boot.img` (already unpacked at `/tmp/stocksb/boot.img`, 19112224 B).

Samsung/Android header (`ANDROID!`, page 2048):

| field | value |
|---|---|
| kernel_size | 15987880 (0x00F3F4A8), kernel_addr 0x10008000 |
| ramdisk_size | 2810850 (0x002AE3E2), ramdisk_addr 0x11000000 |
| second_size | 0 |
| tags_addr | 0x10000100 |
| page_size | 2048 |
| dt_size | 309248 |

Layout: kernel @0x800, ramdisk @0x00F40000 (page-aligned end 0x11EE800), then a
**DTBH v2 container at 18802688 (0x11EE800)**, 309536 B of file remaining after
it (`dt_size` field = 309248). DTBH: version 2, chip id 0x1d9c (exynos7580),
3 entries; the DTBs themselves start one page later at 0x11EF000, 0x1208000,
0x1221000 (entry offsets 0x800 / 0x19800 / 0x32800 relative to the container).

Extraction is a plain FDT-magic scan (see reproduction block at the end).

| DTB | offset in boot.img | size | `model_info-hw_rev` bucket |
|---|---|---|---|
| dt0 | 0x11EF000 | 101681 (0x18D31) | 0x00–0x03 |
| dt1 | 0x1208000 | 102042 (0x18E9A) | 0x04–0x05 |
| dt2 | 0x1221000 | 102156 (0x18F0C) | 0x06–0xFF |

All three declare `model = "Samsung J7ELTE revision 00 board based on
EXYNOS7580"`, `compatible = "samsung,exynos7580-universal7580,r01"`. They are
identical except the rev bucket and phandle numbering — verified by diffing the
decompiled trees (only `model_info-hw_rev*` and `phandle` lines change).

**Correct variant for this phone:** live DT reports
`model_info-hw_rev = <0x06> ... <0xFF>`, and its `/chosen` `bootargs` carry
`androidboot.hw_rev=8` ⇒ **dt2 / rev06**.

Our own build `/tmp/j7out/dt.img` is the same DTBH v2 (chip 0x1d9c, 3 DTBs at
0x800 / 0x19800 / 0x33000, same rev buckets in the same order), i.e. we ship
the stock device tree.

The running tree was pulled separately and is authoritative:

```
flock /tmp/gpsdev.lock -c "SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force \
  timeout 60 setsid -w ssh ... '/usr/local/bin/rootshell sh -c \
  \"cd /proc && tar cf - device-tree\"' > /tmp/livedt.tar"
dtc -I fs -O dts -o /tmp/live.dts /tmp/livedt/device-tree
```

## 2. GPS-relevant nodes: stock rev06 vs running DT

Phandles were resolved by scanning the decompiled trees (`linux,phandle = <N>`
→ enclosing node).

### 2.1 `gps` node — identical

Stock (`dt2.dts`, phandle 0x7E → **gpd2**):

```
gps {
    compatible = "samsung,exynos-bcm4752";
    gpios = <&gpd2 6 0x1>;   /* 0x1 = OF_GPIO_ACTIVE_LOW */
    status = "okay";
};
```

Live (`live.dts`, phandle 0x7F → **gpd2**, same pin 6, same flag):

```
gps {
    compatible = "samsung,exynos-bcm4752";
    gpios = <&gpd2 0x06 0x01>;
    status = "okay";
};
```

- `gpios` is the *only* property the stock driver reads (`of_get_gpio(np, 0)`).
- Source of both DTs: `arch/arm64/boot/dts/exynos7580-j7elte_rev06.dts:1413-1417`
  (`gps { compatible = "samsung,exynos-bcm4752"; gpios = <&gpd2 6 0x1>; ... }`)
  — our tree has the same lines (DTS diff vs pristine Samsung source is
  BT-only, see §4).
- No other gps/gps_pwr/gps_reset node exists: `grep -c gpio-hog` = 0 on both
  trees; no `gps_pwr`, `GPS_RESET`, `gps_reset`, `GPS_RST` strings in the stock
  kernel image.

Device-side confirmation (GPS_PWR_EN is exported and behaves as a plain
output):

```
/sys/class/sec/gps/GPS_PWR_EN -> ../../../139c0000.pinctrl/gpio/gpio137
/sys/class/gpio/gpio137/value = 0, direction = out
debugfs gpio: gpio-137 (GPS_PWR_EN) out lo
```

(i.e. gpd2-6 is currently **physically low**; with `ACTIVE_LOW` in DT that is
the "asserted" level. The stock driver itself ignores the flag, see §3.)

### 2.2 `uart@13800000` (ttySAC0) — identical

Defined once, in `exynos7580.dtsi:506` as `serial_0: uart@13800000`:

```
compatible = "samsung,exynos5430-uart";
samsung,separate-uart-clk;
reg = <0x0 0x13800000 0x0 0x100>;
interrupts = <0 421 0>;
pinctrl-names = "default";
pinctrl-0 = <&uart0_bus>;
clocks = <&clock 103>, <&clock 104>, <&clock 100>;
clock-names = "gate_pclk0", "gate_uart0", "sclk_uart0";
```

Decompiled stock rev06 and live are the same node with the same values
(`interrupts = <0x00 0x1a5 0x00>` = SPI 421, `pinctrl-0 = <0x12>` →
`uart0-bus`, clocks gate_pclk0/gate_uart0/sclk_uart0). Alias
`uart0 = "/uart@13800000"` is present in both.

`uart0-bus` (`exynos7580-pinctrl.dtsi:672`):

```
samsung,pins = "gpb0-7", "gpb0-6", "gpb0-5", "gpb0-4";  /* RTS,CTS,TX,RX */
samsung,pin-function = <2>;   /* UART */
samsung,pin-pud = <0>;        /* no pull */
```

Identical in stock and live. So **our UART0 pinmux is exactly stock's**
(4 pins, function 2, no pulls).

Sleep state (`exynos7580-j7elte_gpio_06.dtsi`, identical in stock/live):

```
PIN_SLP(gpb0-4, INPUT, UP);    /* GPS_RXD */
PIN_SLP(gpb0-5, OUT1,  UP);    /* GPS_TXD */
PIN_SLP(gpb0-6, INPUT, DOWN);  /* GPS_CTS */
PIN_SLP(gpb0-7, OUT0,  DOWN);  /* GPS_RTS */
PIN_SLP(gpd2-6, OUT0, DOWN);   /* GPS_EN  */
```

### 2.3 `serial@14C10000` — dead node, identical in both

Both trees carry this root-level node:

```
serial@14C10000 {
    pinctrl-0 = <0x97 0x98 0x99 0x9a>;   /* uart0-rts, uart0-cts, uart0-tx, uart0-rx */
};
```

(stock: 0x97–0x9A → uart0-rts/cts/tx/rx, verified by phandle scan; live:
0x98–0x9B → the same four groups.)

It has **no `compatible`, no `reg`**, and `exynos7580.dtsi` contains no
`14C10000` node to merge into (grep count 0). No driver can bind it, so its
pinctrl state is never applied; and even if it were, it duplicates `uart0-bus`
(same four pins, same function) already applied by `uart@13800000`. Harmless,
present identically in stock ⇒ not a differentiator.

### 2.4 `s2mpu03_pmic@66` — identical, including `g3d_en`

Both stock rev06 and live (identical text; quoted from live):

```
s2mpu03_pmic@66 {
    compatible = "samsung,s2mpu03-pmic";
    reg = <0x66>;
    ten-bit-address;
    interrupts = <0x02 0x00 0x00>;
    interrupt-parent = <0x27>;
    pinctrl-names = "default";
    pinctrl-0 = <0x3f 0x40>;
    g3d_en = <0x01>;
    wtsr_en = <0x01>;  smpl_en = <0x01>;
    ...
    regulators {
        ...
        LDO10 {
            regulator-name = "vdd_ldo10";
            regulator-min-microvolt = <800000>;   /* decompiles as "", "\f5" */
            regulator-max-microvolt = <1350000>;
            regulator-always-on;
            regulator-initial-mode = <0x01>;
        };
    };
};
```

Board source `exynos7580-j7elte_rev06.dts:601-607` confirms 800000/1350000,
`regulator-always-on`, `regulator-initial-mode = <1>`, **no**
`regulator-boot-on`.

- No GPS node anywhere references a regulator/supply.
- `vdd_ldo10` has no in-tree consumer in either kernel; the only consumer is
  our own added code in `sec_gps_bcm4752.c` (see §3).

## 3. What the stock kernel does for GPS before the daemon

Stock artifact: `/tmp/stocksb/kernel` is an **uncompressed ARM64 `Image`**
(15987880 B, `file`: "Linux kernel ARM64 boot executable Image"). Strings prove
the shipped binary contains the same code as the shipped Samsung source
(`k2.tar.gz`):

| offset | string |
|---|---|
| 0x9F9EC4 | `exynos-bcm4752` (DT match) |
| 0x9F9ED3 | `failed to get device node of bcm4752` |
| 0x9F9EF9 / 0x9F9F19 | `GPS_PWR_EN` (gpio_request label, sysfs link) |
| 0x99694D, 0x9AFA4E, 0x9C07C7 | `Invalied gpio pin` (driver's warn, shared literal) |
| 0x981AA7 | `buck4 gpio ctrl err` |
| 0x981ABC | `regulator sync en err` |

### 3.1 `drivers/gps/sec_gps_bcm4752.c` (pristine source, `device_initcall`)

```
root_node = of_find_compatible_node(NULL, NULL, "samsung,exynos-bcm4752");
gps_pwr_on = of_get_gpio(root_node, 0);      /* number only; ACTIVE_LOW ignored */
gpio_request(gps_pwr_on, "GPS_PWR_EN");
gpio_direction_output(gps_pwr_on, 0);        /* physical LOW at boot */
gpio_export(gps_pwr_on, 1);
gpio_export_link(gps_dev, "GPS_PWR_EN", gps_pwr_on);
```

`check_gps_op()` only reads the pin for `LPA` bookkeeping.

So before any daemon: **GPS_EN is driven low, no rail touched, no wake bytes,
no UART traffic**. First access to the UART is whoever opens `/dev/ttySAC0`.

Our tree's copy of this file differs from stock **only by our own +55 lines**
(`regulator_get(NULL, "vdd_ldo10")`, `regulator_set_voltage(.., 1050000,
1050000)`, `regulator_enable()`, and the `regmap_update_bits(pmic, 0x34, 0xC0,
0xC0)` fix). That is a **deviation from stock**, not stock parity: the stock
kernel never enables/touches a GPS rail from this driver.

### 3.2 `drivers/regulator/s2mpu03.c` — identical to stock (verified byte-for-byte)

Register addresses derived from `include/linux/mfd/samsung/s2mpu03.h` (anchor
`S2MPU03_REG_BUCK_RAMP = 0x29`): `LDO_DVS10` 0x2A, `L1CTRL` 0x2B … **`L10CTRL`
0x34** (matches our 0x34 write); explicit `L22CTRL = 0x43` … `L39CTRL` 0x54,
`LDO_DSCH1..5` 0x55–0x59, `LDO_BB` 0x5A, **`LDO_EFUSE` 0x5B**, `TCXO_CTRL` 0x5C,
`SELMIF` 0x5D; the code then writes SEQ_CTRL 0x5E and seq tables 0x5F–0x7B,
which is self-consistent.

Probe order (`s2mpu03_pmic_probe`):

1. `for` loop of `regulator_register()` → regulator core enables
   `regulator-always-on` rails, so **LDO10 is enabled here**
   (`core.c:980` `if (always_on || boot_on) _regulator_do_enable(rdev)`),
   writing `L10CTRL[7:6] = 0xC0`.
2. `if (pdata->g3d_en)` (true: DT `g3d_en = <1>`):
   - `g3d_pin_config_set()` → **no-op warning** here: DT has no
     `buck4en_addr`/`buck4en_pin`, so it prints
     `g3d pin ctrl gpio is invalid` and returns.
   - `B4CTRL1 &= ~0xC0` → "for buck4 gpio control, disable i2c control".
   - `LDO_EFUSE |= 0x10` → "sync buck4 and ldo10".
   - **`L10CTRL &= ~0xC0`** → "sync buck4 and ldo10".

That last write is why `vdd_ldo10` read "disabled" via the i2c enable bit on
stock as well: stock intentionally hands LDO10's enable to the buck4/G3D
hardware sync and clears the software bits. The same pattern exists on
`s2mps15.c:1186-1197` (buck6 + `LDO_RSVD3` + clear `L10CTRL` 0xC0), i.e. it is a
deliberate Samsung mechanism, not a bug.

Since our `s2mpu03.c` is identical and our DT has `g3d_en = <1>`, the same
sequence runs on our kernel — unless probe failed (dmesg would then contain
`buck4 gpio ctrl err` / `regulator sync en err`).

**Implication:** our `regmap_update_bits(pmic, 0x34, 0xC0, 0xC0)` re-asserts
i2c enable bits on a rail that stock leaves under hardware sync. That makes
`vdd_ldo10` read "enabled" but is not what stock does, and it does not prove the
rail was physically off beforehand. Note also that "vdd_ldo10 is the GPS rail"
comes from our earlier bring-up, not from DT or stock kernel (which has no
consumer at all); `vdd_ldo10` appearing under `/sys/kernel/debug/regulator/` on
our device is consistent with our added `regulator_get()` consumer
[INFERENCE].

### 3.3 UART

Nothing GPS-specific: the serial driver binds `uart@13800000` from DT, applies
`uart0-bus`, enables `gate_pclk0`/`gate_uart0`/`sclk_uart0`. First byte is sent
only when something opens `/dev/ttySAC0`. Stock bootargs use
`console=ttySAC1,115200n8`, so ttySAC0 is not a console and is left untouched.

## 4. Full DT diff: stock rev06 vs running DT

`diff -u <(stock dt2.dts w/o phandles) <(live.dts w/o phandles)` → 437 lines,
all hunks outside GPS scope:

1. **Our BT pinctrl fix** (the only intentional DTS change in our tree):
   `bt-en` group at `gpa1-6` added; `bt_en` renamed `bt_en_j7` at `gpc4-2`
   (`exynos7580-j7elte_gpio_00/04/06.dtsi`, `..._rev00/04/06.dts`). This shifts
   every subsequent phandle by 1, which is why stock 0x7E and live 0x7F are both
   `gpd2`. Diff of our DTS files against pristine Samsung source shows **only**
   BT changes — no UART, GPS, PMIC or regulator DTS edits.
2. Kernel-populated nodes present only in live: `/firmware/android` fstab,
   `arm-pmu`, `model_info-system_rev = "008"`, memory bank list, `bootargs`
   (incl. `androidboot.hw_rev=8`).
3. Battery/charger tuning values (ADC tables, thresholds, currents) — different
   board data for J700M, unrelated to GPS.
4. `muic-universal,uart-gpio` phandle 0x7E→0x7F (same pin `gpd2-2`).

No differences in: `uart@13800000`, `serial@14C10000`, `gps`,
`s2mpu03_pmic@66`, any `LDO*`/`BUCK*` constraints, `uart0-*` groups, gpb0/gpd2
pin configs, sleep states.

## 5. Answers to the acceptance question

- **Does stock enable anything extra (rail, pin, LDO state) before the daemon?**
  **No.** No extra rail (no GPS supply node, no regulator call in the GPS
  driver), no reset/standby gpio, no gpio-hog, no pre-daemon LDO enable beyond
  the same `regulator-always-on` constraint we already have — and stock's PMIC
  probe then *clears* the LDO10 i2c enable bits in favour of the G3D/buck4
  hardware sync.
- **Pre-daemon stock state:** gpb0-4..7 muxed to UART0 (func 2, no pulls);
  `GPS_PWR_EN` requested, output, **physically low**, exported as
  `/sys/class/sec/gps/GPS_PWR_EN` → `gpio137`; UART clocks enabled; no data on
  the wire.
- **Our DT is functionally the stock DT** for everything GPS touches, in all
  three hw-rev variants, and the phone runs the correct rev06 DTB. Therefore
  the silence is not explained by pinmux/rail/DT config.
- **Actionable follow-ups (outside this slice):**
  1. `dmesg | grep -E "buck4 gpio ctrl err|regulator sync en err"` on the
     phone — absence means the g3d sync path ran, i.e. LDO10 is hw-synced and
     our `0xC0` write is redundant; consider dropping it or verifying the rail
     electrically instead of via is_enabled.
  2. Have the daemon (j7elte-gpsd) actually drive `GPS_PWR_EN` and only then
     open `/dev/ttySAC0`; stock expects the HAL, not the kernel, to power the
     chip.
  3. If the chip still ignores frames with pins muxed and EN toggled, look at
     electrical factors (1.8 V IO rail, 32 kHz clock/TCXO, level on RX) next.

## Artifacts

- `/tmp/stocksb/boot.img`, `dt0.dtb/dts`, `dt1.dtb/dts`, `dt2.dtb/dts` (stock)
- `/tmp/j7out/dt.img` → `/tmp/our0..2.dtb/dts` (our build; identical DTs)
- `/tmp/livedt.tar`, `/tmp/livedt/device-tree`, `/tmp/live.dts` (running tree)
- `/tmp/stock_vs_live.diff`, `/tmp/a.dts`, `/tmp/b.dts` (phandle-stripped diff)
- `/tmp/k2src/...` (pristine Samsung source from `j7k/k2.tar.gz`) vs
  `j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
  (our tree): only `drivers/gps/sec_gps_bcm4752.c` (+55 lines, ours),
  `drivers/regulator/s2mpu03.c` (identical), DTS differences BT-only.

Reproduction:

```
# stock DT
python3 - <<'EOF'
import struct
d=open('/tmp/stocksb/boot.img','rb').read()
for i,o in enumerate((0x11ef000,0x1208000,0x1221000)):
    open('/tmp/stocksb/dt%d.dtb'%i,'wb').write(d[o:o+struct.unpack('>I',d[o+4:o+8])[0]])
EOF
for i in 0 1 2; do dtc -I dtb -O dts -o /tmp/stocksb/dt$i.dts /tmp/stocksb/dt$i.dtb; done

# running DT
flock /tmp/gpsdev.lock -c "SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force \
  timeout 60 setsid -w ssh -o StrictHostKeyChecking=no user@172.16.42.1 \
  '/usr/local/bin/rootshell sh -c \"cd /proc && tar cf - device-tree\"' > /tmp/livedt.tar"
mkdir -p /tmp/livedt && tar xf /tmp/livedt.tar -C /tmp/livedt
dtc -I fs -O dts -o /tmp/live.dts /tmp/livedt/device-tree

# compare (phandles normalised)
diff -u <(grep -v phandle /tmp/stocksb/dt2.dts) <(grep -v phandle /tmp/live.dts)
```
