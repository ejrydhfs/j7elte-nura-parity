# Session state (survives context compaction)

Updated: 2026-11-17. Target: **samsung-j7elte**, SM-J700M, Nura (pmOS) 3.10.108.

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

## Camera status
Green (verified): sensor open/S_INPUT, module EEPROM+cal CRC, signed firmware
load, ischain open, rails on before FW boot, ISP pin mux (`pin1`), FD skipped.
Red: firmware `OPEN_SENSOR` -> `I2C ch(0/1) NACK status 0x5100` for **both**
sensors -> not a rear-module fault.

**Lead (docs §23):** the firmware's ISP I2C stack is configured by host command
`HIC_ISP_I2C_CONTROL`, which exists in `include/fimc-is-cmd.h` but is neither
sent nor handled anywhere in the driver. Implement the host side / find the
V4L2 control that the vendor HAL uses. Firmware strings of interest:
`ISP_I2C_WaitForBusReady`, `[I2CHAL_CalcInit] g_I2C_SCLK`, `HIC_SET_ISP_I2C_CONTROL`.

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
