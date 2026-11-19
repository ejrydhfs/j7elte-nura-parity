# j7elte kernel flash + no-boot recovery

Target: SM-J700M (j7elte), Nura 3.10.108 kernel. BOOT = `/dev/block/mmcblk0p10`
(32 MiB). RECOVERY (TWRP) = `mmcblk0p11` — never flashed by this procedure.
NEVER modify DT payloads: S-Boot hangs on rebuilt `dt.img`
(`docs/HARDWARE-BRINGUP-2026-11-16.md` §16). Kernel-only flashes reuse the
stock `/tmp/bimg/dt.img`.

Sources: `docs/HARDWARE-BRINGUP-2026-11-16.md` §16 (build+flash flow,
`conv=fsync`+`sync`, sysrq-b does NOT sync), `docs/SESSION-STATE.md`
(USB-net, TWRP keys, `adb push`+`dd` path, known-good image),
`code/kernel/build.sh` + `/tmp/mkboot.py` usage.

## 1. Pre-flash checklist

- [ ] Known-good fallback image present on HOST and verified:
  `ls -l /tmp/bindboot.img` (~26 MiB) and
  `sha256sum /tmp/bindboot.img` matches the recorded value
  (at time of writing `fcec651e…f038`; re-record with
  `code/kernel/recovery/check-readiness.sh --record` — the doc value can
  go stale, the file on disk is ground truth). Keep a second copy
  (e.g. `/tmp/bindboot-known-good.img`) so a bad build can never
  overwrite your only fallback.
- [ ] New image built the supported way only:
  `code/kernel/build.sh` (Image) then
  `python3 /tmp/mkboot.py Image /tmp/bimg/ramdisk-new.gz /tmp/bimg/dt.img out.img "$(cat /tmp/bimg/cmdline)"`
  (Image + ramdisk-new.gz + STOCK dt.img + cmdline; `console=tty0 loglevel=7`).
  Confirm `out.img` size < 32 MiB and `file`/header sane before flashing.
- [ ] Battery: preferably > 50 % and on charger. A power loss mid-`dd`
  on p10 = no-boot until TWRP restore.
- [ ] TWRP availability confirmed: device can reach recovery
  (Power 20 s to force off, then VolUp+Home+Power together until TWRP).
  Host has working `adb` (`adb devices` shows the device in recovery).
- [ ] GPS-parity reminder: GPU/DRM test builds MUST NOT change GPS
  driver or config. `drivers/misc/sec_gps_bcm4752.c` stays the vendor
  file (GPIO only); no regulator/regmap writes, no UART/pinmux/config
  deltas for GPS. GPU milestones are display/GPU-config-only.
- [ ] Milestone expectation recorded (which smoke list applies, §4).

## 2. Flash command sequence (sync discipline)

sysrq-b does NOT sync — unsynced writes were once lost (firmware blobs
truncated to 0 bytes). Every flash ends with `conv=fsync` AND `sync`
BEFORE any reboot.

From the running system over ssh (USB-net 172.16.42.1):

```sh
# host -> device
scp /tmp/kernel-boot.img user@172.16.42.1:/tmp/new-boot.img
ssh user@172.16.42.1
sudo -n sh -c 'dd if=/tmp/new-boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync'
# still on device: verify, then reboot WITHOUT sysrq-b
sudo -n sh -c 'sync; reboot'
```

Notes:

- `bs=512 conv=fsync` + standalone `sync` are both required.
- Never `echo b > /proc/sysrq-trigger` without a prior `sync`; prefer
  plain `reboot` after `sync`.
- Partition note: BOOT is `mmcblk0p10` (32 MiB). If the p10 node is
  missing under TWRP/adb, prior art recreates it with
  `mknod b 259 2` — record exactly, do not guess other minors
  (verify with `ls -l /dev/block/mmcblk0p*` first).

## 3. No-boot recovery via TWRP + adb

Trigger: device does not come back on USB-net (`172.16.42.1` unpingable /
no SSH) within ~5 min, or boots with X dead / `/dev/fb0` gone (see §5).

1. Force off: hold Power ~20 s.
2. Enter TWRP: hold VolUp+Home+Power together until TWRP appears.
3. On host, confirm `adb devices` lists the device (recovery mode).
4. Push the known-good image and restore p10:

```sh
adb push /tmp/bindboot.img /data/bindboot.img
adb shell 'dd if=/data/bindboot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync; reboot'
```

5. If `/dev/block/mmcblk0p10` is missing in the recovery shell:

```sh
adb shell 'ls -l /dev/block/mmcblk0p*; mknod /dev/block/mmcblk0p10 b 259 2; ls -l /dev/block/mmcblk0p10'
```

then repeat step 4. (`mknod b 259 2` per prior art — record exactly.)
6. Device should reboot into the known-good kernel; re-verify SSH,
   then X per §4.

## 4. Post-boot smoke list

Run over `ssh user@172.16.42.1` after each flash:

```sh
sudo -n sh -c 'ls -l /dev/fb0 /dev/mali0; ls /dev/dri 2>&1'
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null   # expect 720,1280
pgrep -a Xorg; DISPLAY=:0 xrandr 2>&1 | head            # X up at 720x1280
sudo -n dmesg | tail -30
```

Expected per milestone:

- M1 (CONFIG_MODULES + CONFIG_DRM fragment, fbdev X still works):
  X up 720x1280, `/dev/fb0` present, `/dev/dri` exists (even if empty
  of render nodes), `/dev/mali0` as before. No regression in X/fb0.
- M2 (Mali bound to a DRM render node, or kbase-shim fallback):
  as M1 plus `/dev/dri/renderD128` (or documented shim node) present.
- M3 (Mesa GL on hardware, not llvmpipe):
  as M2 plus Mesa reports the hardware driver (NOT llvmpipe/swrast)
  for the GL renderer string.

## 5. When to roll back

Roll back via §3 (TWRP + known-good image) on ANY of:

- No SSH/ping on 172.16.42.1 within 5 min of flashing.
- X dead (no Xorg process, `xrandr` fails, panel black after boot).
- `/dev/fb0` gone (DECON fbdev probe failed — display path regressed).
- Milestone regression: `/dev/dri` missing at M1, render node missing
  at M2 with no documented fallback, or still llvmpipe at M3 with no
  new evidence — restore known-good first, debug on host, re-attempt.

After rollback, do not re-flash the same image; rebuild on host and
re-run `code/kernel/recovery/check-readiness.sh` first.
