# Audio parity — COD3022X codec, Universal7580 machine driver, I2S (j7elte)

**Driver set:** `sound/soc/codecs/cod3022x.c`, `sound/soc/codecs/s2801x.c`,
`sound/soc/samsung/universal7580-cod3022x.c`, `sound/soc/samsung/i2s.c`,
`sound/soc/samsung/eax-dai.c`, `sound/soc/samsung/eax-dma.c`,
`drivers/misc/sec_jack.c`, `sound/soc/samsung/exynos_regmap_fw.c`.

**ROM:** SM-J700M `J700MUBU2BRC2`, kernel 3.10.61-13213628 (Exynos 7580).
**Nura:** `android_kernel_samsung_universal7580` @ 2a746c1 (3.10.108), baseline b5e363a.
**ROM DT compared:** `boot_out/dtbs/000_800.dts` (decompiled, 000_800).
**Nura DT compared:** `arch/arm64/boot/dts/exynos7580-j7elte_rev00.dts` +
`exynos7580.dtsi`, built DT decompiled to `/tmp/nura_j7.sorted.dts`.

## Verdict

**PARITY.** No kernel gap found; no patch produced. The Nura audio kernel stack
(codec, machine driver, I2S, EAX, S2801X aux codec, sec_jack) is the same
vendor fork at the same source revision for every ROM-visible function, and the
j7elte device tree is byte-for-byte equivalent for all audio nodes/properties.
The Nura wiki "Audio = Partial" is a **userspace ALSA/UCM state issue**, not a
kernel-driver gap (evidence in P5/Residual risks).

## P1 — Symbol coverage

`boot_out/symbols.txt` (55,634 kallsyms symbols) vs. definitions/uses in the
Nura source tree. Compiler suffixes `.isra.N`, `.constprop.N`, `.part.N`,
`.cold` were stripped before matching.

| Group (ROM prefix) | ROM symbols | Present in Nura source | Missing |
|---|---|---|---|
| `cod3022x_*` (+ `g_cod3022x`, `soc_codec_dev_cod3022x`) | 64 | 61 | 3 |
| `universal7580_cod3022x_*` (card/dai/of_match) | 3 | 3 | 0 |
| `s2801x*` | 106 | 103 | 3 |
| `i2s_*` | 20 | 20 | 0 |
| `eax_*`, `check_eax_dma_status` | 55 | 51 | 4 |
| `sec_jack_*`, `jack_*`, `earjack_*` | 25 | 25 | 0 |
| generic `snd_soc_*` | 168 | 168 | 0 |
| **Total** | **441** | **431 (97.7 %)** | **10** |

All 10 name-level misses are **macro-generated module init/exit wrappers**,
not driver functions; the macros expand to exactly these symbol names, so a
build produces them:

- `cod3022x_i2c_driver_init`, `cod3022x_i2c_driver_exit`,
  `__initcall_cod3022x_i2c_driver_init6` — from
  `module_i2c_driver(cod3022x_i2c_driver)` at
  `sound/soc/codecs/cod3022x.c:3614`. Expansion in
  `include/linux/i2c.h:539` → `include/linux/device.h:1145-1155`
  (`module_driver`: `static int __init <drv>_init` / `static void __exit <drv>_exit`).
- `s2801x_i2c_driver_init`, `s2801x_i2c_driver_exit`,
  `__initcall_s2801x_i2c_driver_init6` — `module_i2c_driver(s2801x_i2c_driver)`
  at `sound/soc/codecs/s2801x.c:1847`.
- `eax_dai_driver_init`, `eax_dai_driver_exit`, `eax_driver_init`,
  `eax_driver_exit` — `module_platform_driver(eax_dai_driver)` and
  `module_platform_driver(eax_driver)` at `sound/soc/samsung/eax-dai.c:509,540`.

Vendor-specific coverage is therefore **273/273 (100 %)** once generated
wrappers are accounted for; `snd_soc_*` generic helpers are 168/168.

### Vendor driver identity (stronger than name matching)

To rule out a source-level drift between 3.10.61 and 3.10.108, the codec's
static tables from the Nura source were compared against string constants in
`boot_out/vmlinux.elf`:

- DAPM widgets in `cod3022x_dapm_widgets[]`: 19/19 names present in ROM
  (`SPK Mixer`, `HPL Mixer`, `EPDRV`…`AIF4IN`, `VMID`, …).
- Routes in `cod3022x_dapm_routes[]`: 47/47 route tuples (sink, control, source)
  present in ROM (e.g. `{"HPL Mixer", "DACL Switch", "DAC"}`, `{"EP","EP On","EPDRV"}`).
- Controls in `cod3022x_snd_controls[]`: 25/25 control names present
  (`Headphone Volume`, `Chargepump Mode`, `ADC Left Gain`, `DNC Max Gain`, …).
- Machine driver: `Universal7580-audio`, `samsung,universal7580-cod3022x`,
  `codec_bclk`, `mic-bias-mode`, `mic-bias-gpios`, `ext-spk-gpios`,
  `samsung,uses-ext-spk`, `samsung,use-externel-jd`, `JD select gpio request failed`
  all present in the ROM image; card/aux-dev/DAI-link code at
  `sound/soc/samsung/universal7580-cod3022x.c:1116` (aux dev), `:1128` (card),
  `:1320` (`module_platform_driver`).

The previous "142/249" observation is explained by raw name matching against
optimised/static kernel symbols (inlined `snd_soc_*`, macro wrappers); no
vendor function is absent.

## P2 — Device tree

Comparison method: full sorted decompiled DT diff (`/tmp/nura_j7.sorted.dts` vs
`/tmp/rom_000_800.sorted.dts`) plus per-node extraction. Every removed line in
the whole 1,273-line diff is either `linux,phandle` (dtc version artifact emitted
only by the ROM's older dtc) or one of the non-audio nodes `arm-pmu`,
`firmware{android{fstab}}`, `ese-cs-func`, `bt-en`.

| Node | ROM (000_800.dts) | Nura rev00 | Match |
|---|---|---|---|
| `sound` | `boot_out/dtbs/000_800.dts:5119-5135` | `exynos7580-j7elte_rev00.dts:1108-1136` | identical |
| ↳ `compatible` | `samsung,universal7580-cod3022x` | same | ✅ |
| ↳ `samsung,audio-cpu` | `<i2s0 i2s0 cp_dummy bt_dummy fm_dummy eax eax>` (phandles 0x8a..0x8e) | same 7 phandles | ✅ |
| ↳ `samsung,audio-codec` | `<cod3022x ×3, dummy-codec, cod3022x ×4>` (0x8f/0x90) | same | ✅ |
| ↳ `samsung,auxdev` | `<s2801x>` (0x91) | `<&s2801x>` | ✅ |
| ↳ `samsung,use-externel-jd`, `samsung,codec-uses-i2scdclk` | present | present | ✅ |
| ↳ `clocks`/`clock-names` | `<&clock 0x97>` / `codec_bclk` | `<&clock 151>` / `codec_bclk` | ✅ |
| ↳ `mic-bias-mode` | `<1 2 0>` | `<1 2 0>` | ✅ |
| ↳ `io-channels` | `<&exynos_adc 2>`, `adc-jack` | same | ✅ |
| `earjack` | `000_800.dts:5079-5089` | `j7elte_rev00.dts:1068-1088` | identical (11 lines) |
| ↳ `compatible`,`detect-gpio`,`key-gpio`,`det-dbtime` | `sec_jack`, gpa3-1, gpa0-4, 50 | same | ✅ |
| ↳ `det-zones-list`,`but-zones-list` | 4 + 4 zones (codes 226/582/115/114) | same | ✅ |
| ↳ `pinctrl-0` | `jack-det`, `key-det` | same groups/values | ✅ |
| `cod3022x@04` | `000_800.dts:3133-3147` | `j7elte_rev00.dts:459-472` | identical |
| ↳ `compatible`,`reg`,`ten-bit-address` | `codec,cod3022x`, 0x04, yes | same | ✅ |
| ↳ `vdd1/vdd2-supply` | LDO22 / LDO23 (0x36/0x37) | `<&ldo22_reg>`/`<&ldo23_reg>` | ✅ (both LDOs identical: 0x243d58 max, initial-mode 3) |
| ↳ `gpios`,`interrupt-parent`,`pinctrl-0` | gpa0-4 flags 0xf, `<&gpa0>`, `cod3022-irq` (gpa0-4, pud 3, drv 3) | same | ✅ |
| ↳ `mic-bias1-voltage`,`mic-bias2-voltage` | `<1>`,`<1>` | `<1>`,`<1>` | ✅ |
| `i2s@11040000` | `000_800.dts:3806-3836` | `exynos7580.dtsi:1027-1058` + `j7elte_rev00.dts:1089-1091` | identical |
| ↳ `dmas`/`dma-names` | `<&adma 0>`,`<&adma 2>` tx/rx | same | ✅ |
| ↳ `interrupts` | `<0 69 0>` | same | ✅ |
| ↳ clocks `0x96,0x97,0x97` | pclk/sclk/sclk | `<&clock 150>,<&clock 151>,<&clock 151>` | ✅ |
| ↳ `supports-6ch/-rstclr/-secdai/-tdm/-low-rfs`,`amixer=<2>`,`lpass-subip` | all present | all present | ✅ |
| ↳ `i2s-sec` child | tx-sec adma 1, `tx-buf 0x3000000`, `tx-size/deepbuf 0x9000`, `tx-uhqa-buf <0x40000000 0x40000>` | same | ✅ |
| `s2801x@7a` (i2c@13830000) | `000_800.dts:2918-2932` | `j7elte_rev00.dts:318-343` | identical |
| ↳ clocks 154,155,156,157,152,149 + names | `audmixer_sysclk..audmixer_dout` | same | ✅ |
| ↳ `sysreg-reset`,`sysreg-i2c` | `0x110f2000`,`0x110f3000` | same | ✅ |
| ↳ `pinctrl-0/1/2` | i2s-pmic-bus, i2s-codec-clk, pcm-bt-bus(+idle) | same groups/values | ✅ |
| `eax` | `samsung,exynos-amixer` | `samsung,exynos-amixer` (dtsi) | ✅ |
| `cp_dummy`/`bt_dummy`/`fm_dummy`/`dummy-codec` | all okay | all okay, same compatibles | ✅ |
| `lpass@11000000` (AUDSS dmac/clks) | 11 clocks incl. `mi2s_aud_bclk` | identical | ✅ |
| `adma@11020000` | `arm,pl330` (phandle 0x59) | identical | ✅ |
| pinctrl `cod3022-irq`, `jack-det`, `key-det`, `i2s-pmic-bus`, `i2s-codec-clk`, `i2s2-codec-clk`, `pcm-bt-bus`, + `_idle` variants | present | present, same pins/functions | ✅ |

**DT deltas: none.** Phandle numbers and `linux,phandle` presence differ only as
dtc-version artifacts (as with the rest of the board DT).

## P3 — Kernel configuration

Nura config (`config-nura-j7elte`) is the identical file shipped by pmaports
(`config-samsung-j7elte.aarch64`, sha256
`ac864d5c19c564c52e7229cc863ee438a7fc3b1abbb1d83d04ce4de8a1e61538`
for both).

| Item | Value / line | Evidence |
|---|---|---|
| `CONFIG_SOUND`, `CONFIG_SND`, `CONFIG_SND_SOC` | y (2417, 2452) | config |
| `CONFIG_SND_SOC_SAMSUNG` | y (2455) | config |
| `CONFIG_SND_SAMSUNG_I2S` | y (2456) | selects I2S offline DAI (`sound/soc/samsung/Makefile`) |
| `CONFIG_SND_SAMSUNG_AUDSS` | y (2457) | AUDSS |
| `CONFIG_SND_SOC_SAMSUNG_UNIVERSAL7580_COD3022X` | **y (2476)** | machine driver `sound/soc/samsung/Makefile:111` |
| `CONFIG_SND_SOC_COD3022X` | **y (2483)** | codec `sound/soc/codecs/Makefile:154` |
| `CONFIG_SND_SOC_SRC2801X` | y (2487) | aux codec, required by machine driver |
| `CONFIG_SND_SOC_DUMMY_CODEC` | y (2488) | cp/bt/fm dummy links |
| `CONFIG_SND_SOC_REGMAP_FIRMWARE` | y (2479) | `snd-soc-exynos-regmap-fw.o` (`samsung/Makefile:40`) |
| `CONFIG_SND_SOC_I2C_AND_SPI` | y (2481) | codec bus |
| `CONFIG_REGMAP_I2C` | y (978) | codec regmap |
| `CONFIG_I2C` | y (1632) | hsi2c |
| `CONFIG_SAMSUNG_JACK` | y (1064) | sec_jack built (`drivers/misc/Makefile:75`), deps INPUT, INPUT_GPIO, SWITCH all y (1412, 1541, 2923) |
| `CONFIG_SND_JACK` | y (2425) | `sound/core/jack.c` |
| `CONFIG_FW_LOADER` | y (967) | only if firmware update requested (not, see P4) |

Dependency chain `sound/soc/samsung/Kconfig:435-449` selects
`SND_SAMSUNG_I2S`, `SND_SAMSUNG_AUDSS`, `SND_SOC_DUMMY_CODEC`,
`SND_SOC_COD3022X`, `SND_SOC_SRC2801X`; `sound/soc/codecs/Kconfig:255-257`
selects `SND_SOC_REGMAP_FIRMWARE`. **P3 passes; the machine driver is built for
j7elte and binds via `samsung,universal7580-cod3022x` in the board DT.**

## P4 — Firmware / blobs

Audio is the only subsystem here whose firmware path does **not** need a blob on
this device:

- Both codec drivers name the same optional firmware
  `cod3022x-s2801x-aud-fw.bin` (`sound/soc/codecs/cod3022x.c:42`,
  `sound/soc/codecs/s2801x.c:52`; also `MODULE_FIRMWARE` at `cod3022x.c:3622`,
  `s2801x.c:1853`).
- Loading is gated on the DT property `update-firmware`
  (`cod3022x.c:3240` → `cod3022x.c:3314`; `s2801x.c:1535`).
  Neither the ROM DT (`boot_out/dtbs/000_800.dts`) nor the Nura rev00 DT
  contains `update-firmware` (0 occurrences in both). Therefore both drivers
  take the `cod3022x_post_fw_update_failure()` / `post_update_fw(NULL)` path and
  never call `request_firmware()`.
- The file is consequently absent from the ROM too: not in
  `parity/firmware/` (MANIFEST.sha512 lists only bcm/fimc/mfc/nvram/setfile
  blobs) and not in `/system/vendor/firmware` (debugfs listing). The string
  `cod3022x-s2801x-aud-fw.bin` exists in `vmlinux.elf` only as the driver
  constant.

`parity/firmware/files-audio.list` records that no audio blob needs mapping.
**P4 passes.**

## P5 — Patch

None. P1–P4 pass, so no patch exists under `parity/patches/audio/`.

## Why "Audio = Partial" is not a kernel gap

The kernel exposes the exact same card, DAPM graph and control names as the ROM.
What the ROM adds is the Android userspace mixer graph:
`/system/etc/mixer_paths.xml` (96,675 bytes, 908 `<path>` entries) and
`/system/etc/jack_alsa_mixer.json`, which drive controls whose names match the
Nura driver 1:1 (`Headphone Volume`, `SPK Mixer ADCL Switch`, `MIC1 Mic1 On`,
`Chargepump Mode`, `DNC Max Gain`, `Earphone Volume`, `S2801 CH3 Rec En`, …).

The Nura side ships no equivalent:

- `device/archived/device-samsung-j7elte/APKBUILD` has no ALSA/UCM dependency or
  config; `deviceinfo` has no audio entries (only `linux-samsung-j7elte`,
  `mkbootimg`, `postmarketos-base`).
- The archived pmaports tree in this repo contains no `alsa-ucm-conf`/j7elte
  mixer file.

So the fix for playback routing / headset buttons / mic paths is a
UCM profile (or `alsactl` state) built from the ROM's `mixer_paths.xml`, not a
kernel change.

## Residual risks / tests

- **Static verification only.** No kernel build or runtime audio test was
  performed (no aarch64 build in this task; the shared baseline build stopped in
  host `scripts/dtc`, unrelated to audio). Symbol, DT, config and ROM-string
  evidence above is complete for this scope.
- **Kernel version drift (3.10.61 → 3.10.108):** generic `snd_soc_*` and
  `snd_soc_dapm` behaviour could differ subtly; all 168 generic symbols and the
  entire codec DAPM/control table are present, so no API/semantic loss is
  visible, but only runtime testing can confirm clock/PM behaviour.
- **Userspace is the actual gap** (see above). Recommended follow-up outside
  kernel ownership: derive a UCM profile from `/system/etc/mixer_paths.xml`
  (paths `headset`, `speaker`, `earpiece`, `main-mic`, `headset-mic`,
  `voice-call`, …) and ship it in `device-samsung-j7elte`.
- **Buttons/jack:** the ROM `det-zones-list`/`but-zones-list` are consumed by
  `drivers/misc/sec_jack.c` (same driver, `CONFIG_SAMSUNG_JACK=y`); if buttons
  fail on Nura, check `CONFIG_SWITCH`, `INPUT_GPIO` (both y) and the
  `earjack` DT zones — all identical here.
- Runtime tests: `aplay -l` (expect 7 links: primary/secondary/CP/BT/FM/EAX×2),
  playback HPL/HPR/SPK/EP, record from MIC1/MIC2/headset mic, jack-detect IRQ
  (`gpa3-1`) and key ADC buttons (`gpa0-4`).
