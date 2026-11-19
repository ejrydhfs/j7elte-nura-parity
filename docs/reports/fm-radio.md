# FM radio — Broadcom BCM43438 combo; no Linux driver

Scope: stock `SM-J700M` (j7elte) system image + `AP.tar.md5` (Android 6.0.1,
kernel 3.10.61), the Nura kernel tree
`android_kernel_samsung_universal7580-2a746c1…` (3.10.108), and the running
device (kernel #33, stock-parity GPS). Everything below is static analysis of the
ROM/kernel plus read-only queries on the device; no kernel was rebuilt and no
write access to the FM chip was attempted.

**Verdict**

| Question | Result |
|---|---|
| FM hardware | **Present** — integrated FM receiver inside the Wi-Fi/BT combo **Broadcom BCM43438** (65–108 MHz, RDS/RBDS). No second tuner chip. |
| Device tree | No tuner node. Only an `fm_dummy` ASoC placeholder (`samsung,fm_dummy`), matching the node in stock `dt.img`. |
| Kernel driver | **None**, in stock 3.10.61 or Lineage 3.10.108. `CONFIG_MEDIA_RADIO_SUPPORT` is not set; there is nothing disabled to enable. |
| Stock access path | Android-only: `com.sec.android.app.fm` → Samsung `FmPlayer` → `FMRadioService` → Broadcom `IFmReceiverService` in the stock Bluetooth app → bluedroid `bta_fm` → **HCI vendor commands, VS opcode `0xFC15`**. |
| Linux userspace | Not implemented. |
| `/dev/radio0` / V4L2 | Absent — and the stock ROM never had one either; FM is not a char/V4L2 device on this platform. |
| Working node delivered | **No.** No BCM FM driver exists in this kernel (or mainline). Precise missing piece and implementation plan below. |

## 1. Chip identity and board wiring

Evidence for **BCM43438**:

* `/vendor/firmware/bcm43438A1_V0051.0230.hcd` — 35 307-byte Broadcom BT patch
  file, name `BCM43438A1`; already used by Nura's `hciattach bcm43xx` (see
  `bluetooth.md`).
* `/etc/wifi/nvram_net.txt` — `vendid=0x14e4`, `devid=0x43e2`,
  `prodid=0x0727`, `boardtype=0x0727`, “NVRAM file for J7 Rev0.7”,
  `xtalfreq=37400`.
* Live device tree: `bluetooth` node `compatible = "broadcom,bcm43xx"`,
  `wlan` node `compatible = "samsung,brcm-wlan"`. Live `hciconfig -a` reports
  `Manufacturer: Broadcom Corporation (15)`, HCI 4.0, hci0 `UP RUNNING`.
* Cypress/Infineon CYW43438 datasheet: “Single-Chip IEEE 802.11 b/g/n
  MAC/Baseband/Radio with Integrated Bluetooth 4.1 and **FM Receiver**”.
  §11.1: “The receiver is controlled through commands on the HCI. FM received
  audio is available as a stereo analog output or in digital form through PCM.”
  §11.2–11.3: FM PCM (master/slave, up to 3.072 MHz) and line-level analog
  stereo output. FM range 65–108 MHz, RDS/RBDS.
* Stock FM configuration is enabled, not a generic leftover:
  `/etc/floating_feature.xml` contains
  `SEC_FLOATING_FEATURE_FMRADIO_CONFIG_CHIP_VENDOR = 2`,
  `..._SUPPORT_RDS = TRUE`, `..._SUPPORT_HYBRID_RADIO = FALSE`.

There is **no separate FM tuner**:

* Live i2c inventory on the device: `k2hh-i2c`, `stk3013`, `cm36672p`,
  `rear/front-eeprom-i2c`, `sm5703-fuelgauge`, `cod3022x`, `s2mpu03-pmic`,
  `tc300-keypad`, `muic-universal`, `s2801x`, `sm5703mfd`, `mms_ts` — no
  `si470x`/`si47xx` or any tuner.
* `si4709`, `si470x`, `si47xx`, `fm_si47`, `bcm_fm`, `bcmfmc`, `/dev/fm` occur
  **zero** times in the stock `system.raw` and zero times in the stock kernel
  image. The only Samsung FM marker in the stock kernel is the audio-routing
  property `samsung,fm-uses-bt1`; the other FM strings are the generic V4L2
  tuner ioctl names, and no radio driver symbol is present.
* Stock `dt.img` strings contain `fm_dummy`, `samsung,fm_dummy`,
  `broadcom,bcm43xx`; no `si470*`, no radio node.
* The kernel tree agrees: `si4709` has no hit in any `.c`/`.h`/`.dts`.

## 2. Kernel state — nothing to enable

Device tree (`arch/arm64/boot/dts/`):

* `exynos7580-j7elte_rev00.dts:1100-1103` and
  `exynos7580-j7elte_rev06.dts:1097-1100`:
  `audio_fm_dummy: fm_dummy { compatible = "samsung,fm_dummy"; status = "okay"; };`
* The node is only referenced as the 5th entry of the sound card's
  `samsung,audio-cpu` list (`rev00:1114`, `rev06:1111`). It has no address, no
  bus, no GPIO and no control interface.
* `samsung,fm-uses-bt1` exists in the codec machine driver
  (`sound/soc/samsung/universal7580-cod3022x.c:745-746`) but is **not** set in
  the j7elte DTS; the card still has the `universal7580 FM` DAI
  (`.../universal7580-cod3022x.c:1033`).

Drivers/config:

* `drivers/media/radio/` contains only the generic mainline tuner drivers
  (`si470x` I2C/USB, `wl128x`, ISA/PCI/USB radios). None can bind to this board;
  there is also no `s5p`/Samsung FM tuner driver (`s5p.*fm` matches only
  unrelated MFC/TV/crypto files).
* `# CONFIG_MEDIA_RADIO_SUPPORT is not set` in both
  `arch/arm64/configs/lineageos_j7elte_defconfig:2085` and `.config:2099`.
  `CONFIG_VIDEO_DEV=y` is for the camera only; no `radio` class.
* `drivers/bluetooth/bcm43xx.c` is the HCI transport only; it has no FM code.
* The stock 3.10.61 `Image` (extracted from `boot.img` in `AP.tar.md5`) has zero
  hits for `fmradio`, `si470*`, `si47xx`, `bcm_fm`, `sec_fm`, `/dev/fm`.
* Running kernel: `/sys/class/radio` and `/dev/radio*` do not exist; `dmesg`
  has no FM/radio line. `/proc/config.gz` is not built.

There is therefore **no disabled driver to turn on**. A rebuild with
`CONFIG_MEDIA_RADIO_SUPPORT=y` would only add generic drivers that cannot bind
(no Si470x, no I2C tuner) and would perturb the stock-parity GPS kernel. No
kernel change was made.

The chip itself is reachable: hci0 is already up on `/dev/ttySAC3`
(`hciattach -n -t 30 -s 115200 … bcm43xx 115200 flow`) and reports Broadcom.

## 3. Stock userspace path — how the chip is actually accessed

```
com.sec.android.app.fm  (HybridRadio2016_M.apk)
  -> com.samsung.android.media.fmradio.FmPlayer        (framework)
  -> com.android.server.FMRadioService                 (system_server)
  -> com.android.server.FMPlayerNative                 (system_server)
  -> com.broadcom.fm.fmreceiver.IFmReceiverService     (stock Bluetooth app)
  -> libbluetooth_jni.so : register_com_broadcom_fm_service
  -> lib/hw/bluetooth.default.so : bta_fm_* / btif_fm_*
  -> HCI vendor command 0xFC15 over hci0/ttySAC3
  -> BCM43438 FM receiver
```

Evidence:

* App: `/priv-app/HybridRadio2016_M/HybridRadio2016_M.apk` has manifest package
  `com.sec.android.app.fm`, `minSdk 23`, permission
  `android.permission.ACCESS_FM_RECEIVER`, `MainActivity` and test activities
  such as `IntennaTestActivity`, `HwTunningActivity_Qcom`,
  `SoftmuteTestMrvlActivity`; its odex contains
  `com/sec/android/app/fm/RadioApplication` and uses
  `com.samsung.android.media.fmradio.FmPlayer`. (`Fmm.apk` is Find My Mobile,
  unrelated.)
* Framework: `framework/arm/boot.oat` contains
  `com/android/server/FMRadioService` (plus inner classes) and
  `com/android/server/FMPlayerNative`. `FMPlayerNative.connectService()`
  creates an `Intent` from `IFmReceiverService.class.getName()` and resolves it
  with `PackageManager.resolveSystemService()`.
* Provider: `app/Bluetooth/Bluetooth.apk` declares and contains
  `com.broadcom.fm.fmreceiver.FmService` (`FmNativehandler`,
  `FmReceiverServiceStub`). The Broadcom service has no separate APK and there
  is no `libfmradio.so` in the ROM; the FM app is the separate
  `HybridRadio2016_M.apk` above.
* JNI: `/lib/libbluetooth_jni.so` exports
  `register_com_broadcom_fm_service`, `enableFmNative`, `disableFmNative`.
* Stack: `/lib/hw/bluetooth.default.so` contains the full Broadcom BTA FM
  implementation with exported symbols: `bta_fm_enable`, `BTA_FmTuneFreq`,
  `BTA_FmSearchFreq`, `BTA_FmSetRegion`, `BTA_FmConfigAudioPath`,
  `btif_fm_get_interface`, etc.

Transport/protocol (derived from the stock binary):

* `bta_fm_snd_vendor_cmd` builds
  `payload = [cmd_byte, arg_byte, data…]` and calls
  `BTM_VendorSpecificCommand(0xFC15, 2+len, payload)`; the disassembly contains
  `movw r0, #0xfc15`, `strb r8,[r0]`, `strb r7,[r0+1]`.
* So FM control is **HCI VS opcode 0xFC15**, first payload byte = FM
  sub-command. Command bytes observed at call sites include `0x00` (enable,
  arg 1/3), `0x02`, `0x05` (2-byte audio/config bitmask), `0x08`, `0x09`,
  `0x0a`, `0x14`/`0x16` (search thresholds), `0x4d` (audio path, 1 byte
  `0x00`/`0x80`), `0xde`, `0xfc`, `0xfd`, `0xfe`. These are call-site
  constants; the complete request/response/event table still has to be
  recovered (see §5).

Audio (the board path):

* `/etc/audio_policy.conf:45` declares `AUDIO_DEVICE_IN_FM` in the primary
  capture devices; the audio HAL (`audio.primary.universal7580.so`) has
  `fm_mode`, `pcm_fm_out`, `fm_radio_mute`, `fm_radio_volume` strings.
* `/etc/mixer_paths.xml`: `pcmdai fmradio_link="4"`; `route-ap-fm`
  (`S2801 Mixer En`, `S2801 SRC3 En`); `route-ap-fm-rx` (record path);
  `dev-fm-input` (codec `LINELL`/`LINELR` + `LINERL`/`LINERR` cross-switches);
  `fm_radio-speaker`, `fm_radio-headset`, `fm_radio-fm-recording`.
* The running kernel already exposes the hardware pieces:
  `aplay -l` shows card 0 device 4 `FM audio cod3022x-aif-4`, capture device 0
  `i2s0-pri cod3022x-aif-0`, and `amixer -c0` has
  `ADCL/ADCR Mixer LINELL/LINERL/LINELR/LINERR`, `LINEIN LINEIN On`,
  `S2801 SRC3 En` (the FM route controls exist; the sampled ones are off).
* **[INFERENCE]** the FM audio is the BCM43438 line-level analog output feeding
  the codec line input; Android captures it as `AUDIO_DEVICE_IN_FM` and plays
  it back through `pcm_fm_out`. The mixer paths, the capture device list and
  the `dev-fm-input` switch names all fit this; the exact analog-vs-PCM wiring
  must still be confirmed on hardware.

## 4. The precise missing piece

Linux is missing the entire FM backend, not a kernel option:

1. A client for Broadcom FM HCI VS `0xFC15` (enable/tune/seek/mute/RDS/audio
   path) — the stock implementation lives in Android's bluedroid
   (`bluetooth.default.so`), not in any Linux package, and mainline has no BCM
   FM driver.
2. The ALSA PCM loopback / mixer setup that turns the codec FM input into
   audible output (`hw:0,0` capture → `hw:0,4` playback).
3. UCM integration so the route is selected automatically when FM is opened.

No `/dev/fm`, no `/dev/radio0`, no `/sys/class/radio` exists in the stock ROM
either; stock uses binder + HCI + ALSA, so a char device is not a prerequisite.

## 5. Implementation plan

### Phase 0 — recover the exact command set

Preferred: enable Android's “Bluetooth HCI snoop log” on a stock ROM, run
`com.sec.android.app.fm` (tune, seek, volume, RDS) and parse
`btsnoop_hci.log`. This yields the authoritative `0xFC15` request/response and
vendor-event bytes.

Fallback: static reverse engineering of `/lib/hw/bluetooth.default.so`. It is
stripped but exports the `bta_fm_*` symbols; use
`llvm-objdump -d --triple=thumbv7-linux-gnueabi`. Start at
`bta_fm_snd_vendor_cmd`, `bta_fm_vnd_cmd_write_1b/2b/read`,
`bta_fm_enable`, `bta_fm_disable`, `bta_fm_tune_freq`, `bta_fm_scan_freq`,
`bta_fm_set_audio_path`, `bta_fm_set_aud_mode`, `bta_fm_mute_aud`,
`bta_fm_set_step`, `bta_fm_set_region`. This report already establishes the
opcode and the `[cmd, arg, data…]` layout.

Required coverage: enable, disable, tune, seek up/down/abort, mute, volume,
audio path, region/step, RDS on/off, plus command-complete/status and vendor
event parsing.

### Phase 1 — smallest working path: userspace `bcmfm` daemon (recommended)

No kernel change, reuses the already-working hci0 and ASoC:

1. Open a raw HCI socket:
   `socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)`, bind
   `{ hci_dev = 0, channel = HCI_CHANNEL_RAW }` (needs `CAP_NET_ADMIN`; run as
   root or grant the capability).
2. Send commands as `01 <opcode LE16> <plen> <payload>` with opcode `0xFC15`
   and payload `[cmd, arg, data…]`; read HCI events (command complete for
   `0xFC15`, vendor events for seek/RDS).
3. Bring-up: FM enable → audio path matching the board → band/step/region →
   tune → unmute.
4. Audio: set the codec input
   (`LINEIN LINEIN On`, `ADCL Mixer LINELL`/`ADCR Mixer LINELR`, ADC gain),
   open capture `hw:0,0` (S16_LE stereo 44.1/48 kHz) and playback `hw:0,4`
   (`FM audio`), select speaker/headset route, and loop PCM blocks. The exact
   control order is the stock `dev-fm-input` and `fm_radio-*` path lists.
5. Expose a CLI/MPRIS control; no `/dev` node.
6. Validation: tune a known strong station, read RSSI/SNR via VS read
   commands, confirm audio on speaker and headset, confirm the headset antenna
   / internal-antenna behavior.

### Phase 2 — optional V4L2 `/dev/radio0`

Only if standard V4L2 userspace is required:

1. New `drivers/media/radio/radio-bcm43438.c`; Kconfig entry depends on
   `BT && MEDIA_RADIO_SUPPORT`, selects `VIDEO_DEV`; enable
   `CONFIG_MEDIA_RADIO_SUPPORT` plus the new symbol. Rebuild with
   `code/kernel/build.sh`.
2. Bind to the controller: `hci_get_route(NULL, NULL)` / `hci_dev_get(0)` +
   `hci_dev_hold()`. Send VSC with the exported
   `__hci_cmd_sync(hdev, 0xFC15, plen, param, timeout)`
   (`net/bluetooth/hci_core.c:189-196`) and parse the returned skb; register an
   HCI notifier for asynchronous vendor events if required.
3. Register a `video_device` with
   `V4L2_CAP_RADIO|V4L2_CAP_TUNER|V4L2_CAP_HW_FREQ_SEEK`; implement
   `vidioc_querycap`, `g_tuner/s_tuner`, `g_frequency/s_frequency`,
   `s_hw_freq_seek`, `g_audio/s_audio`, and `v4l2_ctrl` mute/volume;
   `video_register_device(vdev, VFL_TYPE_RADIO, -1)` creates `/dev/radio0`.
4. Keep audio out of the driver: on `open()` signal a userspace helper (uevent
   or a control) to run the UCM/ALSA loopback. Do not couple ASoC DAPM into the
   radio driver.
5. Risks: sharing hci0 with BlueZ (do not steal `0xFC15` completions from
   other clients); FM enable may require BT idle; no DT node exists, so bind by
   HCI index via module parameter rather than a fabricated compatible.
6. Acceptance: `v4l2-ctl -d /dev/radio0 --info` shows Radio/Tuner;
   `--list-frequencies`, `--set-freq`, `--get-tuner` work; audio is audible.

### Open questions to resolve during implementation

* Complete `0xFC15` command/response table (Phase 0).
* Analog line-out vs PCM interface actually wired on j7elte (the plan assumes
  analog capture via codec line-in; verify with the schematic or a probe).
* Antenna: the app has `IntennaTestActivity`/`checkIntennaRssi` and headset
  detection; confirm whether the internal antenna is populated and whether the
  headset wire is required.
* Whether the BT firmware accepts FM VS commands while BlueZ is connected;
  test with BT idle first.

## 6. Reproduction commands

```sh
# DT: only fm_dummy, no tuner
grep -rn 'fm' arch/arm64/boot/dts/exynos7580-j7elte_rev0*.dts
strings -a dt.img | grep -iE 'fm|si47|radio'    # stock dt image

# no kernel driver / config
grep -n 'MEDIA_RADIO' .config arch/arm64/configs/lineageos_j7elte_defconfig
find drivers -iname '*fm*' | grep -v '\.o'
strings -a stock_Image | grep -icE 'fmradio|si470|bcm_fm'   # from AP.tar.md5 boot.img

# stock userspace evidence
debugfs -R 'cat /etc/floating_feature.xml' system.raw | grep FMRADIO
unzip -l .../HybridRadio2016_M.apk ; strings -a HybridRadio2016_M.odex | grep 'com/sec/android/app/fm'
unzip -l Bluetooth.apk ; strings -a classes.dex | grep 'com/broadcom/fm'
strings -a libbluetooth_jni.so | grep -E 'com_broadcom_fm|enableFmNative'
llvm-objdump -d --triple=thumbv7-linux-gnueabi --disassemble-symbols=bta_fm_snd_vendor_cmd bluetooth.default.so

# device
ssh ... 'hciconfig -a; aplay -l; arecord -l; amixer -c0 scontrols'
```

## 7. Changes

No kernel, DT, firmware or userspace changes were made. The kernel remains
stock-parity for GPS; no rebuild/flash was performed. This report is the only
commit.
