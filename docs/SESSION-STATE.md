# Session state (survives context compaction)

Updated: 2026-11-19 (GPS/BCM4752 BRI handshake verified on hardware; NMEA not chip-generated). Target: **samsung-j7elte**, SM-J700M, Nura (pmOS) 3.10.108.

## Audio / UCM (2026-11-18)
- ADMA prealloc: `sound/soc/samsung/dma.c` `preallocate_dma_buffer_of()`
  ignored a failed `ioremap()` of the DT `samsung,tx-buf`; it now falls back to
  `dma_alloc_coherent()`. Built image `/tmp/kernel-boot.img`; auto-flash TWRP
  zip `/home/asdf/kernel-audiofix.zip` (dd p10 + reboot).
- UCM: playback verbs only disabled the `ADCL/ADCR` analog-bypass mixers and
  never enabled the `DACL/DACR` DAC mixers. Speaker also required PCM 1
  (`hw:${CardId},1`, the secondary DAI), S2801 `CH1 DOUT Select=AIF4IN`,
  `SRC1/MCKO/HQ En`, `MIX1_LVL=3`, and DAC soft-mute off. Fixed in
  `code/userspace/audio/ucm2` and the pmaports package; speaker and earpiece
  confirmed on hardware.
- Camera RE: dropped per user; no further camera work.

## Device access
- USB net: host `172.16.42.2/24`, device `172.16.42.1`; SSH `user@172.16.42.1` (password `nura`).
- Host tools: `SSH_ASKPASS=/tmp/ap.sh` (echoes `nura`), `setsid -w ssh ...`.
- Reboot: `echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger` (**sync first** — sysrq-b does not sync; unsynced writes were lost once).
- Recovery: TWRP = Power 20 s, then Vol Up+Home+Power; then
  `adb push boot.img /data/ && adb shell 'dd if=/data/boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync; reboot'`.
  Known-good boot image: `/tmp/bindboot.img` (host).

## Working state on the phone
- Kernel: our build of `LineageOS/android_kernel_samsung_universal7580@2a746c1b`
  with patches `patches/camera/0001` (fw fallback), `0002` (FD optional),
  `0003` (ISP pin handover + rails re-enable at `fimc_is_ischain_power`).
  Rebuild: `code/kernel/build.sh` (GCC 16.2, `KCFLAGS=-Wno-error`), then
  `mkboot.py Image /tmp/bimg/ramdisk-new.gz /tmp/bimg/dt.img out.img "$(cat /tmp/bimg/cmdline)"`.
  **Never change the DTB payloads** — S-Boot hangs.
- `/usr` is bind-mounted from USERDATA in the initramfs (`patches/initramfs/0002`).
- Session: `rc-service xsession` -> `start-ui.sh` -> Xorg (with
  `LD_PRELOAD=/usr/local/lib/shadowfix.so`) -> `nura-shell` (phone UI).
- Firmware blobs in `/lib/firmware` (from stock J700MUBU2BRC2).
- `udev` v4l probe disabled: `/lib/udev/v4l_id.disabled` and
  `/etc/udev/rules.d/60-persistent-v4l.rules -> /dev/null`. **Re-apply after any
  apk transaction that reinstalls v4l-utils/udev.**

## Camera status (2026-11-18)
Green: sensor power sequencing, rails at voltage, reset high, MCLK 26 MHz
toggling at the pad, module EEPROM+cal CRC over I2C, signed firmware load,
ischain open, FD skipped.

Red: **both sensors are electrically silent on I2C.** A bit-bang master in
`fimc_is_ischain_power` (AP GPIOs gpf0-0/1 for ch0, gpf0-2/3 for ch1) scanned
0x08-0x77 at 100 kHz while the module was powered:
- rear: `VDD_CAM_SENSOR_A2P8 2.8 V, VDD_CAM_1P2 1.2 V, VDDIO 1.8 V,
  VDDAF 2.8 V`, reset gpe0-4 high, MCLK gate/rate/pad verified, full scan
  `none`;
- front: `VDD_VTCAM_A2P8 2.8 V, VDD_VTCAM_1P2 1.2 V, VDDIO_1.8V_VT 1.8 V`,
  `gpios_cam_en` toggling, reset gpe0-3 high, full scan finds **only the
  EEPROM at 0x10**, sensor at 0x20/0x5a never ACKs;
- rail power-cycle + 9-clock bus recovery + 100 ms reset delay changed
  nothing.
- The same NACK occurs with the **stock 3.10.61 vendor kernel** (boots the
  Nura rootfs), so this is not our kernel build.

Conclusion: the ISP-I2C/pin-mux theory is dead; the buses, EEPROMs, rails,
reset and MCLK are all verified good. The sensor dies themselves do not
respond, on both modules. Next step is hardware: reseat/measure the camera
connectors and the sensor core rails at the module.

### HIC_ISP_I2C_CONTROL (corrected)
Firmware command 28 is `HIC_SET_ISP_I2C_CONTROL` (index 28, not 27; kernel
enum agrees). Sending it with `param1=ch` is accepted but does not change the
NACK. Firmware uses fixed `g_I2C_SCLK=26 MHz`; I2CHAL computes
406 kHz/32 prescaler. Command 28's handler copies host data into a per-sensor
firmware buffer (AP->ISP I2C shadow), it does not initialise the ISP I2C block.

### Front flash + proximity (2026-11-18)
- Front flash (KTD2692, `front_torch_flash`) works. Patched for 5 levels
  (`patches/camera/0004`): laptop camera means 118.7/123.3/128.1/133.8/
  136.5/138.6 for off/L1..L5, p99 158->214. Confirms the front module is
  seated and powered.
- Proximity STK3013 on i2c-0 0x48 works: after
  `echo 1 > /sys/class/input/event4/device/enable`, `raw_data`/`state` read
  2 (far) and drop to 1 when covered. The input event only reports on
  transitions; polling the sysfs node is reliable.
- The camera sensors themselves still do not ACK; see Camera status above.

## Test loop (one attempt per boot!)
```
ssh user@172.16.42.1 'sudo -n sh -c "dmesg -c >/dev/null; timeout -s KILL 40 /usr/local/bin/j7elte-cam6 -t 25 -o /tmp/x.ppm; dmesg | grep -iE \"Sensor I2C|NACK|OpenSensor|handed to ISP|rails on\" | tail -15"'
```
Front camera: `/usr/local/bin/j7cam7 -f` (uses ss1 with the shared 30s/30c group).

## Key files
- `code/kernel/build.sh`, `code/userspace/camera/j7elte-cam/`,
  `code/experience/{nura-shell.c,shadowfix.c,start-ui.sh,...}`
- `patches/{camera,initramfs,xorg-fbdev,pmaports}/`
- `docs/HARDWARE-BRINGUP-2026-11-16.md` §1-23 (all evidence),
  `docs/SESSION-STATE.md` (this file).
- Host scratch: `/tmp/bimg/` (kernel, ramdisk, dt, cmdline), `/tmp/rom.zip`
  (stock ROM), `/tmp/fw.bin` (ISP firmware), `/tmp/cam2/` (tool source tree),
  kernel tree on `/run/media/asdf/db69437b-.../j7k/`.

### Latest firmware RE (2026-11-17, continued)
- `fimc_is_fw2_3l2.bin` stores its message-name table as **file offsets** (base 0),
  e.g. `HIC_OPEN_SENSOR` string at `0x2a293e`, referenced at `0x2acd48`;
  `HIC_SET_ISP_I2C_CONTROL` at `0x2a2841`, referenced at `0x2acd70`.
- Index distance between them is 10 entries = exactly the kernel enum
  (`HIC_OPEN_SENSOR` 17 -> `HIC_ISP_I2C_CONTROL` 27), so **firmware and kernel
  agree on command numbering**; no version mismatch. The command is simply
  never sent by the host.
- Next: recover the parameter block for command 27 (handler code near the
  table users), then emit it from `fimc_is_ischain_power` before the firmware's
  `OPEN_SENSOR`, mirroring `HIC_I2C_CONTROL_LOCK`
  (`fimc-is-interface.c:3418`).

### HIC_ISP_I2C_CONTROL sent (2026-11-17)
Implemented `fimc_is_hw_isp_i2c_control()` + call in `fimc_is_ischain_power`
right after `set_bit(FIMC_IS_ISCHAIN_LOADED)` (channel from the module pdata).

Result on hardware: `isp i2c control(instance 0, ch 0)` -> `-> 0`, firmware
accepts it and advances (`OpenStream`, `MAP VERSION check: SUCCESS`,
`CRC error check: SUCCESS`), but `OPEN_SENSOR` **still NACKs** on 0x5a
(`PushData I2C NACK ... ERROR_SENSOR_I2C_FAIL`).
So the command id is right and the command is accepted, but the payload
(param1=ch, rest 0) is not what configures the ISP I2C. Next: recover the real
parameter block for command 27 from the firmware handler, or try
param1 = ch|0x0100-style encodings used elsewhere in this driver.

## GPS / BCM4752 (2026-11-19) — chip answers BRI; NMEA not obtainable from it
Full evidence: `docs/reports/gps-bringup.md`, tool
`code/userspace/gps/src/gpsprobe.c`.

- The chip is powered and the UART works. The old "chip silent" result was an
  **ordering bug in `--init`**, not hardware: `RequestHwVersion` is ignored
  until the NVRAM exchange has run.
- Verified prologue on `/dev/ttySAC0` 921600 8N1, no flow control, after
  `GPS_PWR_EN` 0 (1 s) -> 1 (0.25 s):
  1. `80` x16 autobaud -> no announce (normal);
  2. `fe00fd001b07000000fc` -> no reply, but stores the word;
  3. `ff00fd001bfc` -> `fe00fd001b 07000000 fc`;
  4. `ff00fd401afc` -> `fe00fd401a 00000000 fc` (`020010a4` on a cold boot);
  5. `ff00fd0000fc` -> `fe00fd0000 05000230 fc` (version `0x30020005`);
  6. `ff00fd1f95fc` -> `fe00fd1f95 00000000 fc` (sub 0x95 = asic-ready ACK).
  `GetOutOfLegacy` is **not** answered here; after step 6 the chip is silent
  for 6 s and polls stay constant — no measurement stream, no NMEA.
- Flow control is irrelevant: `CRTSCTS`, manual RTS, both, and both after a
  power cycle gave zero rx over 10 s x3. 14-baud sweep (4800..4M) after a power
  pulse: zero rx. Pinmux pins 87-90 (gpb0-4..7) -> `13800000.uart`; gpio137
  writes move the pin `out lo`/`out hi`.
- NMEA is produced by `glgps`, never by the chip; the remaining vendor stages
  (flow-control search, ESW upload, RF/clock plan, measurement start) are
  run-time computed and the ROM has no ESW blob.
- Next experiment: one cold-start capture of the stock stack (pad tap or stock
  boot log) and replay; alternatively disassemble
  `Bri_ProgramSwFlowControl` (`0x2a6aa0`) against the ACK parser (`0x2a789c`).
- `j7elte-gpsd --init` must send the NVRAM pair before `RequestHwVersion` and
  must not abort when autobaud is unannounced (GpsToolchain is implementing).
