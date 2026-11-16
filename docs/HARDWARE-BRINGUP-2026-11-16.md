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
