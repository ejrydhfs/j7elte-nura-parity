# Hardware bring-up: Nura on a real SM-J700M (2026-11-16)

First hardware validation of this port on an actual `samsung-j7elte`
(SM-J700M, Exynos 7580, 1.5 GB RAM, 16 GB eMMC, 720x1280 AMOLED).
Everything below was observed on the device; no emulation.

## 1. Installation: the recovery-zip method hangs, a flat install works

`pmbootstrap install --android-recovery-zip` + TWRP sideload completes, but
the initramfs then wedges the whole kernel shortly after the USB gadget is
set up (host cannot even `SET_CONFIG`; the device answers ping but nothing
else). The installer had written an **MBR partition table inside the SYSTEM
partition**, and the initramfs' `mount_subpartitions()` losetup probe is the
last thing that runs before the hang.

Flat install on SYSTEM works and is what the device runs today:

```sh
mke2fs -t ext4 -L pmOS_root /dev/block/mmcblk0p20
mount -t ext4 /dev/block/mmcblk0p20 /mnt/pmOS
unzip -p pmos.zip rootfs.tar.gz | tar -xz -C /mnt/pmOS
dd if=boot.img of=/dev/block/mmcblk0p10
```

Two install scripts must be provided by hand because the recovery zip's
installer normally does it: `/home/<user>` (skel copy, uid 10000) and a
non-empty `/etc/fstab` (the initramfs otherwise waits for a `pmOS_boot`
partition that does not exist and drops to `fail_halt_boot`):

```
# /etc/fstab — boot.img device, no separate /boot
tmpfs /tmp tmpfs rw,nosuid,nodev,mode=1777 0 0
```

## 2. S-Boot rewrites the kernel command line

`/proc/cmdline` is exactly `console=ttySAC1`, regardless of the cmdline
inside boot.img. Consequences:

* `pmos.debug-shell`, `console=tty0`, `loglevel=`, `ramoops.*` cannot be
  passed from the boot image. Debugging must be done from the initramfs
  (patch `init_2nd.sh` to force `debug_shell=y`) or by other means.
* The initramfs' debug shell (telnet port 23, 172.16.42.1) is reachable
  once it is forced; the networking itself works.

## 3. Display: the driver is fine, userspace never starts scanout

Vendor fbdev (`CONFIG_EXYNOS_DECON_EXYNOS7580`, `s3c-fb`) initialises
correctly:

```
/ea8061_wxga is founded
ea8061_probe / ea8061_read_init_info
dsim_read_hl_data read ret : 3 / 32 / 56 / 7 / 18   (panel answers)
dsim_panel_probe: done
pclk_decon_int: 100 Mhz / mipi-dsi driver(VIDEO mode) probed
decon_probe start / real_size=921600 (720.1280) / virt_size=1843200
decon window[0..2] create / decon registered successfully
dsim_panel_set_brightness / MDD : ea8061_displayon was called
```

But the panel stays black. Two independent findings explain it:

1. `decon_fb_write()` in `drivers/video/exynos/decon_7580/decon_core.c` is
   literally `return 0;` — writing to `/dev/fb0` writes nothing and returns
   0, which makes writers loop forever (this was misread as a kernel hang).
2. DECON only scans out after userspace commits a window the way Android's
   HWC does: `S3CFB_WIN_CONFIG` (`_IOW('F', 209, struct
   decon_win_config_data)`) with an **ION buffer**, followed by
   `FBIOPAN_DISPLAY`. Nothing in pmOS does this.

Minimal proof of concept: `code/experience/fbwin.c` allocates an ION buffer
(heap `1 << 4`, falling back to `1 << 0` when the DMA heap is out of
memory), fills it white, and commits window 0
(`state=DECON_WIN_STATE_BUFFER`, `idma_type=IDMA_G0`,
`format=DECON_PIXEL_FORMAT_XRGB_8888`, `src/dst = 720x1280`,
**`fence_fd = -1`** — 0 is interpreted as a real fence fd and yields
`EINVAL`). The panel lights up white. `FBIOPAN_DISPLAY` then points the
window at the framebuffer's own memory
(`decon_pan_display()` writes `info->fix.smem_start + offset` to VIDW_ADD0),
so anything that draws into `/dev/fb0` (X, a launcher) becomes visible.

`fbwin pand` keeps issuing a pan every 200 ms; X's own pans are not enough
to (re)start the DMA after boot.

Pixel order is swapped relative to the commit format (`XRGB_8888` renders
red as blue); use `XBGR_8888` for correct colours, or keep `XRGB` and treat
it as an RGB/BGR swap.

## 4. GTK/XFCE cannot run on this kernel

`xfwm4` aborts in `libwnck` (`default_icon_at_size: assertion failed
(base)`) because every pixbuf load fails:

```
Loader process exited early ... bwrap --unshare-all --die-with-parent
  --seccomp 12 .../glycin-loaders/2+/glycin-image-rs
```

Alpine's gdk-pixbuf delegates decoding to glycin, which sandboxes the
loader with bubblewrap. `bwrap` needs user namespaces, and this kernel has
`# CONFIG_USER_NS is not set` (`bwrap: Can't open source /usr: Function
not implemented`). Enabling `CONFIG_USER_NS` does **not** compile in this
3.10 tree: `cred.h`, `fs/proc/base.c` and `kernel/cgroup.c` fail on
`kuid_t`/`uid_t` conversions — the tree predates the user-namespace
conversion.

Consequences: XFCE, LXQt-GTK, Phosh and anything else GTK-based is out.
Qt6 works at the toolkit level but its xcb platform plugin needs
`xcb-util-cursor` (`qt.qpa.plugin: From 6.5.0, xcb-cursor0 ... is needed`).

## 5. Working UI: Xlib + Cairo launcher

`code/experience/launcher.c` is a 720x1280 fullscreen Xlib/Cairo touch
launcher (no GTK, no Qt, no pixbuf): six tiles (Terminal, Keyboard,
Wi-Fi scan, Reboot, Poweroff, About) that `fork/exec` a shell. Touch input
arrives through libinput as core pointer events. `matchbox-keyboard` is the
on-screen keyboard.

The session is started by openrc:

* `/usr/local/bin/start-ui.sh` — one `fbwin` commit, then `fbwin pand` in
  the background, then `xinit /usr/local/bin/nura-launcher -- :0 vt1
  -config /etc/X11/xorg.conf.d/99-fbdev.conf -dpms -s 0 -noreset`.
* `/etc/init.d/xsession` — `supervise-daemon`, respawn forever.

`99-fbdev.conf` must be:

```
Section "Device"
    Identifier "fbdev"
    Driver "fbdev"
    Option "fbdev" "/dev/fb0"
    Option "ShadowFB" "false"
EndSection
Section "Screen"
    Identifier "screen"
    Device "fbdev"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Weight 8 8 8
        Modes "720x1280"
    EndSubSection
EndSection
```

Notes:

* `busybox reboot` does not reset this device; use
  `echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger`
  (the launcher's Reboot/Poweroff tiles do exactly this).
* `lightdm` never starts: it finds no DRM seat (fbdev only, no
  `/dev/dri`).
* `msm-fb-refresher` must stay disabled; it competes with the window
  commit and the pan daemon.

## 6. Storage map and the eMMC behaviour

| Partition | Device | Size | Use |
| --- | --- | --- | --- |
| BOOT | mmcblk0p10 | 32 MiB | kernel + initramfs (`boot.img`) |
| RECOVERY | mmcblk0p11 | 38 MiB | TWRP (untouched by Nura) |
| SYSTEM | mmcblk0p20 | 2.0 GiB | flat `pmOS_root` rootfs |
| CACHE | mmcblk0p21 | 200 MiB | unused (used experimentally for kernel-log drip) |
| HIDDEN | mmcblk0p22 | 140 MiB | unused |
| USERDATA | mmcblk0p23 | 12.6 GiB | spare ext4, currently copied rootfs |

The pmOS vendor kernel occasionally wedges its dw_mmc controller after a
forced reset: every block read/write then returns EIO (`Buffer I/O error on
device mmcblk0p20`, journal aborted, remount read-only). TWRP on the same
device reads and writes the same partitions at ~150-180 MB/s, so the flash
is healthy. A cold power cycle restores normal operation.

USERDATA works normally from the running system (mounted, 1.7 GB copied
into it), but **booting with the rootfs on USERDATA wedges before
userspace** even with a stock initramfs and only `p23` carrying the
`pmOS_root` label — open issue. The 2 GiB SYSTEM is therefore the rootfs in
production and space is reclaimed by dropping development packages.

## 7. Access

* USB networking: device `172.16.42.1`, host `172.16.42.2`.
* SSH `user@172.16.42.1` (password `nura`); host internet NAT is set up via
  the host's NetworkManager shared profile.
* TWRP: `adb` as usual; recovery partition is `mmcblk0p11`.

## 8. Artifacts in this repository

* `code/experience/fbwin.c` — ION + `S3CFB_WIN_CONFIG` + pan tool
  (`fbwin`, `fbwin nopan`, `fbwin panonly`, `fbwin fbfill`,
  `fbwin single`, `fbwin dump`, `fbwin pand`).
* `code/experience/launcher.c` — Xlib/Cairo touch launcher.
* `code/experience/start-ui.sh`, `code/experience/xsession.initd` — openrc
  session wiring.
* `code/experience/lxqt-session.sh` — openbox + lxqt-panel + pcmanfm-qt.
* `code/experience/shadowfix.c` — LD_PRELOAD shim for the fbdev shadow bug
  (section 9).
* `code/experience/Xorg-preload`, `code/experience/99-fbdev.conf` — server
  wrapper and X configuration that load the shim.
* `code/experience/fbdump.c` — mmap `/dev/fb0` to a file (vendor `fb_read`
  is stubbed; `dd if=/dev/fb0` returns 0 bytes, mmap works).
* `patches/initramfs/0001-prefer-userdata-by-uuid.patch` — experimental
  initramfs change to prefer a specific root UUID (kept for reference; the
  label relabel approach is simpler).
* `patches/xorg-fbdev/0001-shadow-32to24-only-when-24bpp.patch` — driver
  patch matching the shim (section 9).

## 9. X fills the screen: the 24bpp shadow blit

Symptom: Xorg renders 720x1280, `xwd -root` has no white area, but the panel
shows a solid white band over the right 180 px (25%). `fbdump` proves it is
in the framebuffer itself: every word in x=540..719 is `0xffffffff` for the
full height; `nonzero` is 232770 of 921600 words, of which 230400 are that
band.

Cause: fbdev forces a shadow framebuffer for a 24bpp screen
(`24bpp requires shadow framebuffer, forcing`), and
`FBDevCreateScreenResources()` installs `fbdevUpdate32to24` as the shadow
blit whenever `fPtr->shadow24` is set. `shadowUpdate32to24()` writes
3 bytes per pixel, i.e. 2160 bytes per row, while the framebuffer stride is
2880 bytes: bytes 2160..2879 of every row are never written. Config options
cannot reconcile this (`DefaultFbBpp 32`, `FbBpp 32` in the Display section
both leave the 3-byte blit; `fbset -depth 24` is silently coerced back to
32bpp by the driver).

Fix used: `LD_PRELOAD` shim (`code/experience/shadowfix.c`) that redefines
`shadowUpdate32to24()` and forwards to the server's `shadowUpdatePacked()`
(32bpp copy, found with `dlopen("libshadow.so")` because `libshadow` is
loaded by `shadowAdd` *after* the shim). Preload only the server, not
`xinit`/clients, which do not export the shadow symbols:

```
/usr/local/bin/Xorg-preload:  LD_PRELOAD=/usr/local/lib/shadowfix.so exec /usr/libexec/Xorg "$@"
xinit ... -- /usr/local/bin/Xorg-preload :0 vt1 -config /etc/X11/xorg.conf.d/99-fbdev.conf ...
```

Result: `fbdump` nonzero falls 232770 -> 3087 (band gone, desktop black),
`xwd` unchanged, panel fills 720x1280.

Rebuilding the driver with the patch in `patches/xorg-fbdev/` is the
upstream-shaped fix, but a locally built 0.5.0 does not load on this
image (`fbdevHWEnterVT: symbol not found`), so the shim is what runs.

## 10. Desktop stack

* `openbox` + `lxqt-panel` + `pcmanfm-qt --desktop`, started by
  `code/experience/lxqt-session.sh` from `xsession` (openrc).
* Installing LXQt needs ~60 MiB: `apk add lxqt-session lxqt-panel
  pcmanfm-qt openbox xcb-util-cursor`.
* A full `/` (2 GiB) breaks `apk`; drop `gcc musl-dev xfce4*` first, and
  build heavy things on `/data` (USERDATA mounted at `/data` via fstab).
* The panel is unstyled without an icon theme and draws no background of
  its own (black on black); only the 4-desktop pager is visible. The config
  used on the device is `code/experience/panel.conf`
  (`~/.config/lxqt/panel.conf`); `mainmenu`/`clock` warn about missing
  sections until each plugin is added once through the GUI.
* USB networking does not survive a reboot on its own: `code/experience/usbnet.start`
  (installed as `/etc/local.d/usbnet.start`, `rc-update add local default`)
  restores the default route via `172.16.42.2` and a resolver.
* `xwd` (Alpine) is the only screenshot path; `/dev/fb0` reads are stubbed,
  `fbdump` mmaps instead.

## 11. `/usr` on USERDATA (binding before switch_root)

The 2 GiB SYSTEM partition cannot hold a desktop stack (LXQt already put `/`
at 92%; Plasma is ~1 GB). Packages install into `/usr`, so `/usr` has to live
on USERDATA (12.6 GiB). A bind mount *after* boot is too late: udev and
openrc start out of `/usr` during `switch_root`.

`patches/initramfs/0002-bind-usr-from-userdata.patch` adds, right after
`mount_root_partition` in `init_2nd.sh`:

```sh
if [ -b /dev/mmcblk0p23 ]; then
	mkdir -p /sysroot/data
	if mount -t ext4 -o rw /dev/mmcblk0p23 /sysroot/data 2>/dev/null; then
		if [ -d /sysroot/data/usr ] &&
		    mount --bind /sysroot/data/usr /sysroot/usr 2>/dev/null; then
			echo "$LOG_PREFIX /usr bind-mounted from USERDATA"
		fi
	fi
fi
```

Procedure used on the device:

```sh
cp -a /usr /data/usr                       # 1.5 GiB, rootfs untouched
# rebuild boot.img: kernel + patched initramfs + dtb, preserving the
# original cmdline, then dd it to /dev/mmcblk0p10
```

Verified after reboot: `mount | grep ' /usr '` ->
`/dev/mmcblk0p23 on /usr type ext4 (rw,relatime,data=ordered)`, `df -h /usr`
shows 11.8 GiB with 9.4 GiB free, and the X session comes up unchanged.

The old `/usr` on SYSTEM is deliberately left in place as a fallback: if the
bind fails, the system still boots on the 2 GiB copy. USERDATA is also
mounted at `/data` (fstab), so the bind source stays reachable.

## 12. X rendering: `fbScreenInit` must be 32bpp too

The shadow-blit fix alone left every image upload wrong: text and rectangles
were squeezed to 3/4 width and R/B swapped (`XPutImage` of a 720-wide image
put its right edge at x=537). `xdpyinfo` reports `depth 24, bits_per_pixel 32`,
but the server's fb layer was initialised with 24bpp: fbdev forces
`shadow24` and calls `fbScreenInit(..., 24)` on a 32bpp framebuffer, so every
fill/image path writes 3 bytes per pixel into a 4-byte stride.

`code/experience/shadowfix.c` is preloaded into the server only and now does
two things:

* interposes `fbScreenInit()` and forces `bpp = 32` (the symbol is exported
  by `/usr/libexec/Xorg`);
* routes `shadowUpdate32to24` to the server's `shadowUpdatePacked` (the white
  band from section 9).

Both fixes verified from `/dev/fb0` (`fbdump`): a 4px border drawn at the
window edges appears at x=0..3 and x=716..719, and known colours come back
byte-exact (accent `#3b82f6` -> `(59,130,246)`).

## 13. `nura-shell`: phone UI without GL or a compositor

Plasma Mobile is packaged (`plasma-mobile 6.8.3`) but its shell is
Wayland-only: `plasmashell` exits with `Failed to create wl_display`, and
`kwin_wayland` has no backend for a fbdev-only device (no `/dev/dri`, no
usable GL: `HANDOFF-3D.md`). Lomiri has the same constraint.

`code/experience/nura-shell.c` is a WM-less phone shell for this display:

* override-redirect 720x1280 window, Cairo `CAIRO_FORMAT_RGB24` image surface
  pushed with `XPutImage` (the Cairo Xlib surface compresses 3/4 here);
* status bar (clock, battery from `/sys/class/power_supply`), 2x3 tile grid,
  tap-to-launch, pressed feedback;
* touch is read straight from `/dev/input/event2` (ABS_MT_POSITION_X/Y,
  BTN_TOUCH); the physical Home key (`/dev/input/event5`, KEY_HOMEPAGE/KEY_BACK)
  kills the child app and raises the shell;
* launched apps run without a WM, i.e. fullscreen, and `Home` returns.

Build: `gcc -O2 -o /usr/local/bin/nura-shell nura-shell.c $(pkg-config
--cflags --libs cairo x11) -lm`. Started by `code/experience/start-ui.sh`
under the openrc `xsession` service, with `LD_PRELOAD` set for the server.

## 14. Shell input wiring and app lifecycle

Verified on the device:

| Node | Device | Values |
|---|---|---|
| `/dev/input/event2` | `sec_touchscreen` | ABS_MT_POSITION_X 0..720, ABS_MT_POSITION_Y 0..1280 (already screen units; the shell scales by the reported range anyway) |
| `/dev/input/event5` | `gpio_keys.21` | KEY code **172** (`KEY_HOMEPAGE`) — the physical Home key |

The shell finds the touchscreen by capability (`EVIOCGBIT(EV_ABS)` has
`ABS_MT_POSITION_X`) instead of hardcoding an event node, and handles both
Protocol-B release (`ABS_MT_TRACKING_ID` = -1) and `BTN_TOUCH`.

App lifecycle bug found on hardware: the first launch worked, every later tap
was a no-op. `child` was only ever set, never cleared, so `launch()` returned
early forever. Fixed by reaping on `SIGCHLD`, raising the shell and redrawing
when a child exits, and treating `kill(child, 0) != 0` as "already gone".

Taps and launches are logged to `/var/log/xsession.log`:

```
touch: /dev/input/event2 range x 0..720 y 0..1280
down 117,696 hit 4
up 114,681 hit 4
launch megapixels -> pid 11486
sh: line 0: megapixels: not found
```

`megapixels` (Camera tile) is not installed; replace the tile command or
install the app. Tiles run through `sh -c`, so any command works.

## 15. Camera (FIMC-IS): firmware fixed, driver blocks capture

Hardware path: the vendor FIMC-IS stack is present (`/dev/video101`
`exynos-fimc-is-ss0`, `ss1`, `30s/30c/30p`, `i0s`, `dis`, `scp`), the DT sensor
node is `fimc-is_sensor_3l2@0` (`compatible = samsung,exynos5-fimc-is-sensor-3l2`,
`status = okay`), and the sensor sits on an `hsi2c` controller.

**Missing firmware.** `firmware-samsung-j7elte` is not in the repo index, so
`/lib/firmware` contained only `regulatory.db`. Captures failed before the
sensor was even touched. The blobs were extracted from the exact stock ROM
`J700MUBU2BRC2` (archive.org `fwatlas-samsung-sm-j700m-j700mubu2brc2`), by
streaming `system.img` out of the AP tar, `simg2img`, loop-mounting it and
copying out of `/vendor/firmware`:

```
fimc_is_fw2_3l2.bin  2813502
setfile_3l2.bin       161172
setfile_5e3.bin       108876
fimc_is_fd.bin        500640
mfc_fw.bin            356088
```

**Boot-time open leak.** With firmware installed, every capture failed with
`fimc_is_sensor_open:1181: already open` -> `-EMFILE`. `fimc-is-device-sensor.c`
guards on `test_bit(FIMC_IS_SENSOR_OPEN)`; the boot log showed
`fimc_is_ischain_close_wrap:3559: close is invalid on opening` at ~32 s, i.e.
something opens both sensors during boot and the close never clears the bit.
udev's `v4l_id` was the suspect: masking
`60-persistent-v4l.rules` **and** renaming `/lib/udev/v4l_id` to
`v4l_id.disabled` produces a boot with zero FIMC-IS messages, after which the
sensor opens cleanly.

**DT verified, not the cause.** Extracted the three DTBs from the boot image
(`DTBH` container, FDTs at 0x800/0x19800/0x33000) and decompiled them with
`dtc`: every one carries `sensor_i2c_addr = <0x5a>`, `sensor_i2c_ch = <0x00>`,
`mclk_ch = <0x00>` for `fimc-is_sensor_3l2@0`, matching the stock ROM DT. The
`/proc/device-tree` read that looked empty was a shell/od artefact.

**Current blocker (kernel-side).** With a clean boot the open succeeds, but a
kworker then hangs in `exynos5_i2c_xfer` and the driver loops
`[@][WRN]sensor is not yet power off`, leaving the caller in D-state in
`msleep`. That is sensor I2C/power sequencing in the vendor driver, not
userspace: `j7elte-cam` never reaches VIDIOC_STREAMON. Fixing it needs a kernel
patch (bounded I2C transfer plus a correct power-off/close path) and a rebuild
of `linux-samsung-j7elte`; reusing the parity tree
(`patches/camera/0001...` is only the firmware-path fallback).

Until then the Camera tile should not launch a capture tool. Field state:
`/lib/udev/v4l_id.disabled`, masked v4l rule, firmware blobs installed
(proprietary, not committed).

## 16. Building and flashing the kernel (works), camera state

**Build.** `code/kernel/build.sh` reproduces it:

* source `LineageOS/android_kernel_samsung_universal7580@2a746c1b`
  (tarball ~133 MB; codeload is slow, ~50 kB/s here),
* config `code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`,
* all patches from the same directory, `yes "" | make oldconfig`,
* `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j2 Image`
  (GCC 16.2 needs `-Wno-error`: `arch/arm64/kernel/setup.c` trips
  `-Warray-bounds`, which is fatal otherwise; build ~10 min),
* boot image via `mkboot.py` with the existing ramdisk and **the stock
  `dt.img`**.

**Do not replace the DTBs.** Replacing the DTB payloads inside the `DTBH`
container (same slots, padded) makes S-Boot hang before USB comes up; the
rebuilt DTBs are not needed because the DT content is unchanged. Kernel-only
flashes boot fine (`3.10.108 #1 SMP ... Mon Nov 16 15:34`).

**Flashing.** `dd` the image to `/dev/mmcblk0p10` from the running system with
`conv=fsync` + `sync`, or from TWRP via `adb`. `sysrq-b` does *not* sync —
always `sync` before rebooting or the last writes are lost (this truncated
the firmware blobs to 0 bytes once).

**Camera progress with the new kernel.** The `request_firmware` fallback from
`patches/camera/0001` now works:

```
Camera: read cal data from Rear EEPROM, headerOnly(0)
Camera: EEPROM Cal map_version = V001
Camera: I2C read cal data
Camera: /system/vendor/firmware/fimc_is_fw2_3l2.bin is not available, trying request_firmware
```

so module EEPROM, sensor power, MCLK and firmware load are all functional.
The remaining failure is the driver's ischain state machine driven by
`j7elte-cam`:

* `VIDIOC_S_CTRL(0x9a103a = V4L2_CID_IS_END_OF_STREAM, 1)` returns `-EMFILE`
  (`fimc_is_ischain_open_wrap`: `already open` / `open_cnt > ENTRY_END`);
* issuing EOS *after* the `S_INPUT`s instead makes `S_INPUT` fail `-EMFILE`
  (`fimc_is_ischain_init_wrap` needs the ischain open);
* each failed attempt leaves the ischain half-open, so the next node open
  fails and a reboot is required between attempts (a clean boot is essential:
  udev's `v4l_id` must stay disabled, see section 15).

The Android FIMC-IS3 HAL that defines the correct order is not public
(`hardware/samsung_slsi/exynos5` only ships the pre-FIMC-IS HAL), so the
sequence has to be derived from the driver sources
(`fimc-is-video*.c`, `fimc-is-device-ischain.c`): the HAL opens every node,
issues `S_INPUT` with the leader encoding, and only then EOS — but which
node carries the leader encoding and what `open_cnt` accounting the driver
expects still needs to be traced.

## 17. Camera: FD library and the sensor-I2C handover

With `patches/camera/0001` (firmware fallback) and `patches/camera/0002`
(FD optional) in the kernel, a **clean boot** (udev `v4l_id` disabled) gets the
pipeline much further:

```
fimc_is_sensor_open():0 ... fimc_is_sensor_s_input(8, 0):0
fimc_is_ischain_loadfd:1333: FD library loading is fail
[@][WRN] FD library unavailable (-22), continuing without FD
fimc_is_ischain_config_secure : PA_FIMC_IS_GIC_C : 0x000000fc
fimc_is_ischain_power(1):0
fimc_is_ischain_open():0
<TS:...> CALDATA_BASE/SIZE = 0x01fd0000/0x00014000
<TS:...> [INFO][FROM HEADER, STREAM ID:0] CRC error check: SUCCESS!
<TS:...> [FD  ] FdTaskEntry
<TS:...> [ERROR]Sensor I2C doesn't work. I2C ch(0),Addr:0x5a -
         Interrupt Status Register: 0x5100
<TS:...> [ERROR][send] Sensor I2C doesn't work. I2C ch(0) m_I2CNackErrCount(1)
```

So: sensor power/EEPROM, Secure-OS signature of the ISP firmware, firmware
boot and ischain open all work. The current blocker is the **sensor I2C
ownership** between the AP and the ISP firmware: the kernel's thema read of the
module EEPROM (address 0x5a, ch 0) succeeds, but the firmware's own transfer
to the same address gets a NACK (status 0x5100). On stock the HAL never touches
that bus from the kernel side; the ISP owns it after handover, and the DT's
`pinctrl-0/1` ("pin0"/"pin1") plus the driver's pin-control scenario table are
what flips the routing (`fimc-is-dt.c: get_pin_lookup_state`,
`exynos_fimc_is_module_soc_pin_control`). The next step is to make the driver
select the ISP-side state (or quiesce the AP i2c controller) before the
firmware's first transfer.

Note: each failed attempt leaves the ischain half-open (VRA/interface
"already open", `open_cnt`), so **one attempt per boot**; the tool itself is
otherwise correct up to this point (`j7elte-cam` reordered EOS/S_INPUT
variants were tried and are not the issue).

## 18. Camera: the ISP firmware's sensor I2C NACK

Skipping the AP-side EEPROM read (tried, reverted) does **not** help: the ISP
firmware's own transfer still fails.

```
[I2CHAL_SetI2CSpeedInfo](Ch:0,DevNum:1) Addr(0x5A), Speed(400000)
[SENS][00|00][08|08] [OpenSensor] is called. I2c Ch:0
[ERROR]Sensor I2C doesn't work. I2C ch(0),Addr:0x5a - Interrupt Status Register: 0x5100
[ERROR][read:1] ... m_I2CNackErrCount(4)
[ERROR][PushData] I2C NACK error when opening sensor !!!
[ERROR][EC__][S0][SEN0] 524296 can not open
```

The kernel's raw-I2C EEPROM read of the *same* address succeeds, so the sensor
is powered and the bus works from the AP. The driver's pin-control log shows
it selects the AP state:

```
pin_ctrl(act(4), pin(0), val(0), nm(pin)     # PIN_FUNCTION -> pinctrl state
```

i.e. `pinctrl-names = "pin0","pin1"` with **pin0 = AP I2C**, and nothing in this
path selects **pin1 = ISP I2C** before `fimc_is_ischain_power()`. The sensor
node has no `pin_ctrls` table in the (stock, verified) DT, so
`get_pin_lookup_state()` has nothing to install and the state is never
switched. Next step: select `pin1` (or quiesce the AP i2c controller) right
before the firmware boots, then re-test.

Kernel currently flashed: FD-optional build (`patches/camera/0001/0002`), boots
and reaches this stage.

## 19. Where the ISP/AP sensor-I2C switch actually lives (RE result)

The pinctrl switch to the ISP side is **board-conditional**, not DT:

```c
/* sensor/module/fimc-is-device-3l2.c */
#if defined(CONFIG_MACH_SMDK7580)
	SET_PIN(..., GPIO_SCENARIO_ON,  gpio_none, "pin", PIN_FUNCTION, 0, 0);
	...
	SET_PIN(..., GPIO_SCENARIO_OFF, gpio_none, "pin", PIN_FUNCTION, 1, 0);
#endif
```

Only the SMDK7580 development board flips `pin0`/`pin1`; retail board data
(what j7elte builds) contains no `PIN_FUNCTION` entry for the sensor, so the
state is never changed by the kernel. The `pin_ctrl(... nm(pin) ...)` line seen
in the boot log comes from the SMDK branch being compiled **out** on this
config; what remains is the regulator sequence.

Consequence: on retail hardware the sensor I2C must be handed to the ISP by
the **firmware handshake** (the ISP firmware drives the sensor bus and expects
MCLK/power arranged through the ischain messages), not by a pinctrl state
change. The failure
`[OPEN_SENSOR] I2C ch(0) Addr(0x5A) ... NACK(4)` therefore points at that
handshake: the firmware is talking on the bus while the sensor is not yet
clocked/reset from the ISP's point of view.

Config sanity check on the built kernel: `CONFIG_MACH_SMDK7580` is **not** set
(`arch/arm64/configs/exynos7580-j7elte_defconfig` has no such symbol; the
retail defconfigs only list the board DTs), which matches the code path above.

Next: trace the ischain message handler for the sensor-open sequence
(`IHC_OPEN_SENSOR` / `fimc_is_hw_sensor_mode`, `fimc_is_hw_set_sensor_mclk`)
and compare which of those messages the AH `j7elte-cam` sequence is missing.
