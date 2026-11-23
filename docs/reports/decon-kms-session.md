# Plasma Mobile wayland session on the J7 (DECON KMS slice 8)

Status: **delivered, not enabled on the device.**  The config/scripts are in
`code/gpu31/kms/session/`; this report is the install + rollback + first-boot
runbook.  Nothing was changed on the phone: the probes below were read-only
(`flock /tmp/gpsdev.lock`, SSH, no writes).

Target: after KMS milestone M1 provides `/dev/dri/card0`, boot straight into
`startplasmamobile`/`kwin_wayland`, with a documented path back to the proven
`xsession` + `nura-shell` X11 UI.

## 1. What is on the image today

Probed on the running image (kernel `3.10.108 #37`, Nura edge):

| Item | State | Evidence |
|---|---|---|
| Display nodes | **no card0**; only `renderD128` (panfrost/Mali) | `ls /dev/dri`; `/sys/class/drm` has `renderD128` only |
| Session service | `xsession` (OpenRC, `supervise-daemon`, respawn forever) is started | `/etc/init.d/xsession`, `rc-status` |
| Session command | `start-ui.sh` → `fbwin` + `xinit Xorg :0 vt1 -config /etc/X11/xorg.conf.d/99-fbdev.conf` (Xorg with `shadowfix.so` LD_PRELOAD) → `nura-shell` | `/usr/local/bin/start-ui.sh` |
| Display manager | **lightdm 1.33.0** installed, **stopped**; no sddm | `apk list -I`; `rc-service lightdm status` |
| Greeter | slick-greeter 2.2.7 (X11); `70-lightdm-pmos.conf` sets `greeter-session=slick-greeter` and `unclutter-xfixes` | `/usr/share/xgreeters`, `lightdm --show-config` |
| Wayland session files | stock `plasma-mobile.desktop` present; no `nura-*` session | `/usr/share/wayland-sessions/` |
| Plasma | plasma-mobile/plasma-workspace/kwin 6.8.3, Qt 6.12.0, `startplasmamobile`, `plasma_session`, `kwin_wayland` | `apk list -I` |
| D-Bus/elogind | `dbus` + `elogind 255.24` started; elogind is `before display-manager`; no `/etc/elogind` config, only the vendor sample `/usr/lib/elogind/logind.conf` (`HandlePowerKey=ignore`) | `/etc/init.d/elogind`, `rc-status`, `ls /etc/elogind` |
| Seat | `seat0` exists, `CanTTY=yes`, **`CanGraphical=no`** (there is no card0; fb0 is only tagged `:seat:`) | `loginctl show-seat seat0`; `udevadm info /sys/class/graphics/fb0` |
| Input | touchscreen `sec_touchscreen` on `event2` (`root:input 660`), keys `event1`/`event5`, jack `event7`, accelerometer `event3` | `/proc/bus/input/devices`; `ls -l /dev/input` |
| uaccess | `70-uaccess.rules` tags **only `drm/card*` and joysticks**, not touchscreens; `user` is in `wheel,netdev,abuild,plugdev`, **not `input`/`video`** | `/usr/lib/udev/rules.d/70-uaccess.rules`; `id user` |
| Backlight | `/sys/class/backlight/panel` (`type=raw`, `max_brightness=355`); `brightnessctl` + openrc/udev helpers installed; **powerdevil absent**; `polkit` installed but not in a runlevel | sysfs; `apk list -I`; `rc-status` |
| Rotation sensor | accelerometer is Samsung `K2HH` in `/sys/class/sensors/accelerometer_sensor` (`raw_data`) plus an `EV_REL` input device.  `/sys/bus/iio/devices/iio:device0` is the **ADC** (`13900000.adc`), not the accelerometer.  **iio-sensor-proxy absent and not applicable** | sysfs, `/proc/bus/input/devices` |
| zram | `postmarketos-zram-swap` is in the default runlevel but **stopped**; kernel `CONFIG_ZRAM=y`, `CONFIG_ZRAM_LZ4_COMPRESS=y`, `comp_algorithm=[lzo] lz4`; zramstart defaults to **zstd** (fails); **0 swap** | `apk info -L postmarketos-zram*`; `/sys/block/zram0/comp_algorithm`; `free -m` |
| zswap | **enabled (`Y`, `max_pool_percent=50`, lzo)**; `enabled` is `0444` (no runtime disable), but `max_pool_percent` is `0644` and the pool is empty (no swap device has ever existed) | `/sys/module/zswap/parameters/*` |
| Memory/disk | 1374 MiB RAM, ~564 MiB used; `/` 2.0 GiB, **69.9 MiB free**; gcc 130 MiB + binutils 14 MiB + musl-dev 11 MiB installed | `free -m`, `df -h` |
| Xwayland | **not installed** | `command -v Xwayland`; `apk policy xwayland` |

## 2. Hard findings that shape the design

### 2.1 Installed kwin is ABI-broken against Qt 6.12; `kwin=6.8.3-r1` fixes it

`kwin 6.8.3-r0` was built for Qt 6.8.  Qt 6.12 changed the private
`QWindowSystemInterface` tablet signatures, so `libkwin.so.6` cannot resolve
two symbols:

```
Error relocating /lib/libkwin.so.6: ...handleTabletEnterLeaveProximityEvent...ddddi... symbol not found
Error relocating /lib/libkwin.so.6: ...handleTabletEvent...dddddi...             symbol not found
```

`nm -D /lib/libQt6Gui.so.6` exports the new forms (`...ddddd...` and
`...dddddd...`).  The Alpine edge package `kwin-6.8.3-r1` is a Qt 6.12
rebuild: downloading `kwin-6.8.3-r1.apk` and reading
`usr/lib/libkwin.so.6.8.3` shows exactly those new undefined signatures.
A pinned install touches only two packages:

```
apk add --no-cache 'kwin=6.8.3-r1' 'kwin-lang=6.8.3-r1'   # 2 packages, 7.4 MB
```

Do **not** run `apk add --upgrade kwin`: the resolver wants 76 packages
because it also upgrades KF6/Qt.  Pin the version.

`plasma_session --version` is not a valid health check: `plasma_session`
starts the session and `kwin_wayland_wrapper` itself
(`plasma-workspace-6.8.3/startkde/plasma-session/startup.cpp:145`), so it
prints the libkwin relocation errors in a respawn loop.  `ksmserver`,
`kded6` and `plasmashell` load; `ldd plasma_session` has no libkwin.  The
session gate therefore checks `kwin_wayland --version`, which is a pure
loader check.

### 2.2 LightDM 1.33 autologin goes through the greeter

LightDM 1.33 has no direct autologin: the seat only forwards the hints when
`autologin-user-timeout > 0` (`lightdm-1.33.0/src/seat.c:1300-1318`), and
slick-greeter performs the login.  `autologin-user-timeout=0` disables
autologin entirely.  The drop-in uses `1` second.

Wayland sessions are supported (`wayland-session.c:62` sets
`XDG_SESSION_TYPE=wayland`; `XDG_VTNR` comes from `seat-local.c:197`), so
after autologin LightDM creates a Wayland display server (no X server) and
KWin gets the VT.  The greeter itself is X11; it is forced onto the KMS
fbdev emulation so it cannot hold DRM master when KWin starts.

LightDM waits for `CanGraphical=yes` because `logind-check-graphical`
defaults to true (`lightdm.c:832`).  `71-seat.rules:34` tags
`drm/card[0-9]*` `master-of-seat`, so card0 is what flips seat0 to
graphical.  That is the "once card0 exists" gate at the DM level.

### 2.3 The session needs `input` group membership

KWin 6 uses **libinput**, not evdev directly (`libinput.so.10` is linked;
`LibinputBackend`).  On this image `70-uaccess.rules` does not tag
touchscreens, and `event2` is `root:input 660`.  Xorg/nura-shell worked only
because Xorg runs as root.  The Plasma session runs as `user`; `install.sh`
adds `user` to `input` (and `video`, for `brightnessctl`/fb0).

### 2.4 Rotation is not available from this sensor

The accelerometer is the Samsung `K2HH` driver, exposed through
`/sys/class/sensors/accelerometer_sensor/raw_data` and a relative (`EV_REL`)
input device.  It is not an IIO accelerometer, so `iio-sensor-proxy` (which
KWin 6.8 and Plasma Mobile's rotation plugin use via
`net.hadess.SensorProxy`) would expose nothing.  Automatic rotation needs a
kernel IIO shim for K2HH or a userspace evdev→SensorProxy bridge; neither is
in this slice.  Manual rotation is possible with the optional `kscreen`
package (`kscreen-doctor`); Plasma Mobile's rotation quicksetting uses
libkscreen directly and does not need the `kscreen` daemon merely to apply a
rotation.

### 2.5 zswap is on and immutable at runtime

`/sys/module/zswap/parameters/enabled` is `0444`; `zswap.enabled=0` must be
added to the kernel command line (or `CONFIG_ZSWAP_DEFAULT_ON` disabled) by
the KMS boot-image slice.  The session install ships a stopgap service,
`nura-zswap-off`, which runs before `postmarketos-zram-swap` and writes `0`
to the writable `max_pool_percent` (`mm/zswap.c:93-95`).  With no swap device
yet, `zswap_is_full()` is then true and no pages are stored; the pool is
empty today, so nothing leaks.  zram itself must use **lz4**: this kernel's
zram only advertises `[lzo] lz4`, while `zramstart` defaults to zstd.

### 2.6 Xwayland is missing

`plasma_session` starts `kwin_wayland_wrapper --xwayland`
(`startup.cpp:145`).  KWin starts without Xwayland, but X11 apps will not
run and the log will carry warnings.  `xwayland 24.1.15` is available
(1.8 MiB) and is in the recommended install list.

## 3. Boot flow

```
OpenRC default
  postmarketos-zram-swap   (lz4 via /etc/deviceinfo)
  nura-session             (supervise-daemon, this slice)
        |
        | /usr/local/bin/nura-session-run reads /etc/nura/session-mode,
        | writes /run/nura-session/{decision,reason,probe}, then execs:
        |
        | mode = nura  or gate fails
        +--> start-ui.sh          -> Xorg fbdev -> nura-shell
        |
        | mode = auto/plasma and: card0 is atomic
        |                          and kwin_wayland --version loads
        +--> /usr/bin/lightdm     -> slick-greeter (X, fb0)
                                  -> autologin 1 s
                                  -> Wayland session nura-plasma
                                  -> nura-plasma-wrapper
                                  -> plasma-dbus-run-session-if-needed
                                  -> nura-plasma-session (KWIN_*/Qt/Mesa env)
                                  -> startplasmamobile
                                  -> startplasma-wayland
                                  -> plasma_session -> kwin_wayland (card0)
                                                     + panfrost (renderD128)
```

The stock `xsession` and `lightdm` services are removed from the default
runlevel; `nura-session` is the only provider of `display-manager` and
`exec`s exactly one backend.  Xorg/nura-shell and KWin can never own the
display at the same time.  (Starting one OpenRC service from another with
`rc-service` was avoided; the selector is the supervised process.)

## 4. Files to install

All sources are in `code/gpu31/kms/session/`.  `install.sh` copies them and
builds the probe; it does not enable `nura-session` or switch modes.

| Source | Target | Mode |
|---|---|---|
| `session.conf` | `/etc/nura/session.conf` | 644 |
| `nura-session-mode` | `/usr/local/bin/nura-session-mode` | 755 |
| `nura-plasma-session` | `/usr/local/bin/nura-plasma-session` | 755 |
| `nura-plasma-wrapper` | `/usr/local/bin/nura-plasma-wrapper` | 755 |
| `nura-session-run` | `/usr/local/bin/nura-session-run` | 755 |
| `nura-session.initd` | `/etc/init.d/nura-session` | 755 |
| `nura-zswap-off.initd` | `/etc/init.d/nura-zswap-off` | 755 |
| `nura-plasma.desktop` | `/usr/share/wayland-sessions/nura-plasma.desktop` | 644 |
| `60-nura-session.conf` | `/etc/lightdm/lightdm.conf.d/60-nura-session.conf` | 644 |
| `kms-atomic-probe.c` | `/usr/local/src/nura-session/kms-atomic-probe.c`, compiled to `/usr/local/sbin/kms-atomic-probe` | 644 / 755 |
| `deviceinfo-zram` | `/etc/deviceinfo` (or a marked block appended if it exists) | 644 |
| `logind-lid.conf` | `/etc/elogind/logind.conf.d/60-nura-lid.conf` | 644 |

`install.sh` and `revert.sh` are the repo-side install/rollback drivers; they
are not copied to the device.

Install sequence on the phone (later, not now):

```sh
# free space if needed: apk del gcc binutils musl-dev libdrm-dev meson
cd /path/to/code/gpu31/kms/session
sh install.sh

# required: kwin rebuilt for Qt 6.12 (two packages, pinned)
apk add --no-cache 'kwin=6.8.3-r1' 'kwin-lang=6.8.3-r1'

# recommended: X11 apps, brightness slider, manual rotation
apk add --no-cache xwayland powerdevil kscreen

/usr/local/bin/nura-session-mode status
```

`install.sh` also starts `postmarketos-zram-swap` with the lz4/50 % override,
enables `nura-zswap-off` before it, and enables `polkit` (needed by
powerdevil's brightness helper).  The session
itself is not enabled: `xsession` stays in the default runlevel, the running
UI is untouched, and `nura-session` is not in a runlevel until
`nura-session-mode` is run.

### Toggle and gate

```sh
nura-session-mode status     # card, atomic, kwin, groups, seat, swap, services
nura-session-mode nura       # Xorg + nura-shell (opts nura-session in)
nura-session-mode auto       # plasma when the gate passes, else nura
nura-session-mode plasma     # refuses if the gate fails; --force overrides
```

`plasma` gate (also checked by `nura-session-run` at boot):

1. `/dev/dri/card0` exists and `kms-atomic-probe` reports `atomic=1` with
   ≥1 CRTC/connector/plane.
2. `kwin_wayland --version` loads (catches the Qt 6.12 ABI break).
3. `/usr/share/wayland-sessions/nura-plasma.desktop` exists.
4. `user` is in `input`.

If the gate fails in `plasma` mode, the selector logs the reason and starts
nura-shell, so a kernel rollback cannot leave a black screen.  The requested
mode stays in `/etc/nura/session-mode`; `/run/nura-session/{decision,reason,probe}`
record what happened.

## 5. Session environment

Set in `nura-plasma-session` (all inherited through
`startplasma-wayland → plasma_session → kwin_wayland_wrapper`):

| Variable | Value | Why |
|---|---|---|
| `KWIN_DRM_DEVICES` | `/dev/dri/card0` | Display is the DECON KMS card, not the Mali render node.  `drm_backend.cpp:118` parses this list and skips the seat filter for explicit devices. |
| `KWIN_RENDER_NODES` | `/dev/dri/renderD128` | Rendering is panfrost on a *different* platform device.  KWin 6.8's `GpuManager` (`gpumanager.cpp:85`) discovers render nodes separately; platform-bus devices are treated as compatible (`gpumanager.cpp: findCompatibleRenderDevice`). |
| `KWIN_COMPOSE` | `O2ES` | Forces OpenGL compositing (`compositor.cpp:267`); KWin exits instead of falling back to QPainter.  KWin 6.8 always binds `EGL_OPENGL_ES_API` and creates a GLES 2 context (`eglcontext.cpp:181,242`), so this is the GLES2 path on panfrost. |
| `KWIN_DRM_USE_MODIFIERS` | `0` | First bring-up: DECON KMS has no validated tiling modifiers.  Remove after the dma-buf milestone proves format/modifier scanout. |
| `KWIN_DRM_DISABLE_TRIPLE_BUFFERING` | `1` | 1374 MiB RAM; two buffers until memory is measured. |
| `QT_QPA_PLATFORM` | `wayland` | Deterministic platform for Plasma clients.  Safe ahead of KWin: `main_wayland.cpp:320` overwrites it with its own QPA and unsets it at `:334`. |
| `QT_WAYLAND_DISABLE_WINDOWDECORATION` | `1` | Phone/handset. |
| `GALLIUM_DRIVER` | `panfrost` | Prevents a silent llvmpipe fallback that cannot sustain the shell on this SoC.  Unset only when debugging a panfrost failure. |

Not set on purpose: `MALLOC_ARENA_MAX` (musl has a single arena),
`KWIN_DRM_NO_AMS` (KWin 6 requires atomic), `LIBGL_ALWAYS_SOFTWARE`
(debug-only), `KWIN_FORCE_SW_CURSOR` (no cursor on this device).

`startplasmamobile` itself sets `QT_QPA_PLATFORMTHEME=KDE`,
`EGL_PLATFORM=wayland`, `QT_QUICK_CONTROLS_MOBILE=1`,
`PLASMA_PLATFORM=phone:handset` and runs
`plasma-mobile-envmanager --apply-settings` with the offscreen QPA (verified
to return 0 on this image).

## 6. zram

`/etc/deviceinfo` (sourced after the packaged device file) adds:

```
deviceinfo_zram_swap_algo="lz4"
deviceinfo_zram_swap_pct="150"
```

This makes `postmarketos-zram-swap` create a **687 MiB** zram0 (50 % of
1374 MiB, the memory-budget assumption) with lz4.  The service was already in
the default runlevel but stopped; `install.sh` starts it, `revert.sh` stops it
again.  `nura-zswap-off` is enabled in the same runlevel and ordered before
it, blocking zswap stores through `max_pool_percent=0`; the permanent fix
remains `zswap.enabled=0` on the boot cmdline.  `install.sh` records the
original `max_pool_percent` in `/etc/nura/zswap-max-pool`, which `revert.sh`
restores.

## 7. Revert path

Runtime switch (no reboot):

```sh
nura-session-mode nura
```

This restarts `nura-session` in `nura` mode: LightDM/KWin/Plasma are stopped
and the proven `start-ui.sh → Xorg fbdev → nura-shell` path runs instead.
Verify with `rc-service nura-session status`, `cat /run/nura-session/decision`
and `ps -o args -C Xorg,nura-shell`.

Full removal (files, groups, zram override, runlevel):

```sh
sh code/gpu31/kms/session/revert.sh
```

`revert.sh` stops `nura-session`/LightDM, re-adds `xsession` to `default`,
removes the installed files and `/etc/nura`, removes only the groups recorded
in `/etc/nura/installed-groups`, stops zram (the runlevel entry was already
there), restores the recorded zswap `max_pool_percent`, removes
`nura-zswap-off`, removes `polkit` from the runlevel it was not in before, strips the
marked zram block from `/etc/deviceinfo`, and removes its
`/etc/elogind/logind.conf.d/60-nura-lid.conf` drop-in.  Package changes are
deliberately not downgraded; `apk del` lines for the optional packages are
printed.

Boot-image rollback: with the pre-KMS kernel there is no card0, so
`nura-session-mode auto` (or `nura-session-run`) selects nura-shell.  The explicit
`plasma` mode also falls back at boot when the gate fails.

## 8. First-boot checklist

Run as root via SSH (`user@172.16.42.1`, `flock /tmp/gpsdev.lock`, root via
`/usr/local/bin/rootshell`).

Before switching to Plasma:

| Check | Command | Expected |
|---|---|---|
| Disk space | `df -h /` | ≥ 100 MiB free after cleanup |
| card0 | `ls -l /dev/dri/card0 /dev/dri/renderD128` | both present; card0 root:video |
| Atomic KMS | `kms-atomic-probe /dev/dri/card0` | `... atomic=1 crtcs=... connectors=... planes=...`, rc 0 |
| KWin loads | `kwin_wayland --version` | `kwin 6.8.3` (after r1); no relocation errors |
| Seat | `loginctl show-seat seat0 -p CanGraphical` | `CanGraphical=yes` once card0 is tagged |
| Input access | `id user`; `su - user -c 'head -c0 /dev/input/event2'` | `input` group present; command succeeds |
| zram | `swapon --show`; `zramctl` | `/dev/zram0` ~687 MiB, lz4 |
| zswap | `cat /sys/module/zswap/parameters/enabled`; `cat .../max_pool_percent` | `N` if the boot cmdline fix landed; otherwise `Y` with `max_pool_percent=0` (stopgap) |
| Session files | `ls /usr/share/wayland-sessions/nura-plasma.desktop /etc/lightdm/lightdm.conf.d/60-nura-session.conf` | present |
| Gate | `nura-session-mode status` | all required lines OK |

Switch and verify:

```sh
nura-session-mode plasma
rc-service nura-session status   # started
rc-service lightdm status        # stopped (lightdm runs under nura-session)
rc-service xsession status       # stopped
cat /run/nura-session/decision   # plasma
cat /run/nura-session/reason
pgrep -a kwin_wayland            # kwin_wayland --xwayland
pgrep -a plasmashell
loginctl list-sessions           # one session on seat0
SID=$(loginctl list-sessions --no-legend | awk '$3=="user"{print $1; exit}')
loginctl show-session "$SID" -p Type -p Active -p Seat -p TTY   # Type=wayland
```

| Interactive check | Expected |
|---|---|
| Home screen | Plasma Mobile shell, 720×1280, no black/white band |
| Touch | taps/drags follow the finger (libinput); `libinput debug-events` as user sees `sec_touchscreen` |
| Rotation | auto-rotate absent (K2HH is not IIO); test manual rotation with `kscreen-doctor -o` if installed |
| Brightness | Settings slider changes `/sys/class/backlight/panel/brightness`; `brightnessctl -d panel info` works as user |
| Wi-Fi/Bluetooth | unchanged; `networkmanager`/`bluez` services still running |
| Memory (30 min) | `free -m` stable, no OOM in `dmesg`; `swapon --show` has zram |
| Display errors | `~/.xsession-errors` (LightDM session log), no `0x08` DECON timeouts, no atomic commit failures |
| Expected warnings | KWin logs `net.hadess.SensorProxy... failed` (no accelerometer bridge) and possibly "Xwayland not found" before `xwayland` is installed; both are non-fatal |
| VT switch | `chvt 2` / back to the session VT does not wedge KWin |

If the screen stays dark or LightDM never starts:
`nura-session-mode nura` (SSH still works), then inspect
`/var/log/lightdm/lightdm.log` (greeter/X), `~/.xsession-errors` (KWin/
Plasma) and `/run/nura-session/reason`.

## 9. Open items / owned elsewhere

| Item | Owner / action |
|---|---|
| `zswap.enabled=0` on the kernel cmdline | KMS boot slice (`KmsBoot`); runtime sysfs is 0444 |
| Accelerometer → IIO or SensorProxy bridge for auto-rotation | sensor/kernel work; K2HH is not an IIO device |
| DECON KMS format modifiers | dma-buf/KMS milestone; drop `KWIN_DRM_USE_MODIFIERS=0` after validation |
| fb0 under KMS (used by the LightDM greeter) | KMS fbdev emulation milestone; if fb0 is dropped, remove `xserver-command` from the LightDM drop-in |
| slick-greeter on this GTK/gdk-pixbuf image | verify at first boot; if it cannot start, the direct-session alternative is a greetd `initial_session` (not installed now) |
| `powerdevil`/`kscreen`/`xwayland` package installs | runbook step, not done on the device |
