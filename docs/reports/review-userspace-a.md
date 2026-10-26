# Review A — j7elte-audio-ucm, j7elte-sensors, j7elte-power

Independent review of the three userspace packages, baseline pmaports `ba682bd`
(kernel @ `2a746c1`). No phone hardware was available; every claim below is backed by
a command run in this session. Nothing here is a hardware result.

Tooling actually used: host GCC 16.2.1, session GCC 10.3 aarch64 cross compiler, `patch`/
`git apply`, `sh -n`, `bash -n`, `xmllint`, `systemd-analyze` 262, `alsaucm`/alsa-lib
1.2.16.1, `dbus-daemon`/`gdbus`/`busctl`, `qemu-aarch64` (not needed), Docker
(`alpine:latest`, privileged host-chroot container for `/dev/input` access only).
**`abuild`, `apk`, `pmbootstrap`, an aarch64-musl compiler and `openrc-run` are not
installed**; `package()` was therefore simulated with the APKBUILD's own shell code, and
the OpenRC init scripts were syntax-checked, not executed.

## Verdict

| Package | As received | After my patch | Summary |
|---|---|---|---|
| `j7elte-audio-ucm` | **PASS** (with a licence caveat) | n/a | Checksums, `package()` tree, UCM parse, control/enum cross-check all pass. No invented control or path. |
| `j7elte-sensors` | **FAIL** (real defect) | **PASS** | Any GDBus/GLib client's first method call made the daemon log `malformed D-Bus message` and exit. Fixed; patch exported. |
| `j7elte-power` | **PASS** | n/a | Checksums, host + aarch64 GCC 10.3 builds, `package()` tree, 12 functional checks, restore-script cases, service syntax and all sysfs paths verified against `sec_battery.c`. |

Details and evidence per package follow.

---

## 1. `j7elte-audio-ucm`

### 1.1 APKBUILD / packaging

* `pkgname`, `pkgver=0.1`, `pkgrel=0`, `arch="aarch64"`, `depends="alsa-ucm-conf"`,
  `options="!check !archcheck"` — all syntactically valid; `!archcheck` is a real abuild
  option. No `makedepends` needed (data-only package).
* `source=` lists the four files that are present; `sha512sum -c` on the APKBUILD block:
  **4/4 OK**.
* `package()` simulated with `srcdir`/`pkgdir` set and the APKBUILD sourced; exits 0 and
  installs exactly the four intended paths with mode 0644:
  `usr/share/alsa/ucm2/conf.d/Universal7580-I2S/Universal7580-I2S.conf`,
  `usr/share/alsa/ucm2/j7elte/{HiFi,VoiceCall}.conf`,
  `usr/share/j7elte-audio/jack_alsa_mixer.json`.
* Package files are byte-identical to the canonical copies under
  `parity/userspace/audio/ucm2/` and `parity/userspace/audio/rom/` (`diff` empty).

### 1.2 UCM correctness vs. the kernel

* Card naming checked directly in the kernel, not just in the report:
  `universal7580-cod3022x.c:1129` sets `card->name = "Universal7580-I2S"`; the card sets
  no `driver_name`/`long_name`, and `soc-core.c:1793-1811` copies the name into
  `shortname`/`longname`/`driver` **while explicitly preserving `'-'` and `'_'`**. So
  `conf.d/Universal7580-I2S/Universal7580-I2S.conf` is the correct UCM2 lookup path
  (verified against `/usr/share/alsa/ucm2/ucm.conf`, `UseCasePath.confd2`).
* UCM2 tree parsed with host `alsaucm` 1.2.16 under `ALSA_CONFIG_UCM2` (virtual-card
  path, `${CardId}` substituted because no matching card exists):
  `alsaucm -c testsnd list _verbs` → `HiFi`, `VoiceCall`; both files and all `Value`
  blocks parsed. `File "/j7elte/HiFi.conf"` resolves below the UCM2 root (confirmed by
  the parse).
* Control names/types/ranges: re-ran `parity/userspace/audio/validate_controls.py` →
  `RESULT: PASS` (54/54 ROM controls, 53/53 UCM controls, 0 range/enum violations).
  Spot-checked independently in the tree:
  `SOC_SINGLE_TLV("DNC Max Gain", …)`, `SOC_ENUM("Chargepump Mode", …)` with
  `VDD/HALF-VDD/CLASS-G-D/CLASS-G-A`, `s2801x.c` `"RMIX1_LVL"/"MIX1_LVL"`,
  `"CH4 Mixer En"`, `"DMIX_OUT"/"AIF4IN"/"RMIX_OUT"`, `s2801x_off_on_text = {"Off","On"}`
  — all match the UCM text. No invented control, mixer path or ALSA device node found.
* Jack: the package ships the ROM's Android HAL `jack_alsa_mixer.json` as reference data;
  no Linux control is claimed for it. This is consistent with `sec_jack.c` registering
  input/switch devices, not a kcontrol.

### 1.3 Defects / flags (not fixed here)

1. **Licence is wrong (packaging/IP, not code).** The package is `license="MIT"` but its
   `HiFi.conf`/`VoiceCall.conf` are translations of Samsung's proprietary
   `/etc/mixer_paths.xml` and `jack_alsa_mixer.json` is shipped verbatim from the ROM.
   MIT is not accurate for that content. A maintainer must pick the real licence (e.g.
   `unknown`/`proprietary`-style handling per pmaports policy) before merge; I did not
   change it because the correct value is a project decision, not a mechanical fix.
2. **Stale paths in `parity/reports/audio-userspace.md`.** §P5 references
   `parity/patches/userspace/audio/0001-device-samsung-j7elte-depend-on-audio-ucm.patch`,
   which does not exist. The device dependency is now in the shared
   `parity/patches/userspace/0001-device-deps.patch` (it already contains
   `j7elte-audio-ucm`), and the package itself is
   `audio/0002-j7elte-audio-ucm-package.patch`. Only the report is stale; the patches
   apply correctly (see §4).

---

## 2. `j7elte-sensors`

### 2.1 Real defect found and fixed

**Symptom (reproduced):** with a private `dbus-daemon`, `gdbus introspect` succeeded but
the first properties call killed the daemon:

```
$ gdbus call … org.freedesktop.DBus.Properties.GetAll net.hadess.SensorProxy
Error: GDBus.Error:org.freedesktop.DBus.Error.NoReply: Message recipient disconnected
$ cat sens.log
j7elte-sensors: malformed D-Bus message
j7elte-sensors: stopped
```

**Root cause:** GLib/GDBus puts the standard D-Bus header field **9 (`UNIX_FDS`,
signature `u`)** on every method call. `dbus-wire.c:parse_header_fields()` handled only
fields 1–8 and returned `-1` for anything else; `dw_next_message()` turned that into a
fatal "malformed message" and `main()` stopped the daemon. Every GTK/GNOME/iio-sensor-
proxy client (GDBus) would therefore have found the service dead after its first call.
The package's own host test only used `busctl` (libsystemd does not add `UNIX_FDS`), which
is why it passed and this was missed.

**Fix (implemented):**
* `HF_UNIX_FDS = 9` added and parsed as a `u` field.
* Unknown header fields are now skipped (per the D-Bus spec) via a new
  `skip_variant_value()` that understands `u`, `b`, `y`, `s`, `o`, `g`; an unknown
  scalar *type* still fails closed.
* APKBUILD `sha512sums` entry for `dbus-wire.c` updated to the new file's sum.
* Canonical copy `parity/userspace/sensors/dbus-wire.c` synced (the aport is generated
  from it by `sync-to-aport.sh`).
* Exported as
  `parity/patches/userspace/review-a/0001-j7elte-sensors-ignore-unknown-dbus-header-fields.patch`
  (pmaports-relative; **apply after** `sensors/0001-add-j7elte-sensors-package.patch`).

**Verification after the fix (all run in this session):**
* `sha512sum -c` on the APKBUILD block → 7/7 OK.
* `make CC=gcc CFLAGS='-O2 -g -Wall -Wextra -Werror -std=gnu11'` → clean;
  GCC 10.3 aarch64 `-static` build → clean, `ELF 64-bit LSB executable, ARM aarch64`.
* GDBus: `GetAll` returns the full dict, `Get` returns `'undefined'`, `Set` returns
  `PropertyReadOnly`, `ClaimAccelerometer`/`ReleaseAccelerometer` return `()`,
  `Ping` and `GetMachineId` answer; `busctl introspect` still works.
* End-to-end with real evdev objects: two uinput devices named `accelerometer_sensor`
  and `proximity_sensor` were created in a privileged host chroot; the daemon found
  `/dev/input/event28`/`event29`, and `gdbus monitor` received the correct
  `PropertiesChanged` signals: `HasAccelerometer=true`, `HasProximity=true`,
  `AccelerometerOrientation='normal'`, `AccelerometerTilt='vertical'`,
  `ProximityNear=true`. (Test-only uinput emulation; no hardware claim.)
* Patch stack on a pristine `ba682bd` export:
  `0001-device-deps.patch` → `sensors/0001` → my patch applies with **no offsets**, and
  the resulting `device/archived/j7elte-sensors` is byte-identical to the working
  package directory.

### 2.2 APKBUILD / config checks

* Checksums 7/7 OK after the fix; `bash -n APKBUILD` OK; `sh -n` initd OK.
* `package()` simulated: `/usr/bin/j7elte-sensors` 0755,
  `/usr/lib/systemd/system/j7elte-sensors.service` 0644,
  `/etc/init.d/j7elte-sensors` 0755, `/usr/share/dbus-1/system.d/j7elte-sensors.conf`
  0644 — exactly the four declared paths.
* `xmllint --noout` on the bus policy OK; the policy parses under a real `dbus-daemon`
  and the daemon can own/emit on the bus (proven above).
* `systemd-analyze verify` on the unit: the only diagnostic is `Unit dbus.service not
  found` in the minimal test root (expected; `depends="dbus"` provides it).
* `depends="dbus"` is correct (the daemon speaks the wire protocol itself; no libdbus
  linkage). The unit's `ProtectSystem=strict` does not block the `/sys` writes (systemd
  exempts `/dev`, `/proc`, `/sys` from `strict`; `ProtectKernelTunables` is not set).

### 2.3 Other notes

* `/sys/class/input/eventN/device/{enable,poll_delay}` was checked against the drivers:
  `k2hh.c` creates the attribute group on `dev->dev.kobj` (the input device) and the
  event node's `device` link resolves there; names `accelerometer_sensor` /
  `proximity_sensor` match `k2hh.c:39` and `cm36672p.c:1117` / `stk3013.c:1630`.
  No invented path.
* The report's claim that the test passed is true but its coverage was `busctl` only;
  it cannot detect the GDBus defect. If the regression test is meant to guard real
  clients, add one `gdbus call` (or `GetAll` via GLib) to `tests/run-host-test.sh`.
* Nothing in the package synthesises sensor values; the `HasAmbientLight=false` and
  calibration choices match the drivers (re-checked in `k2hh.c`/`cm36672p.c`).

---

## 3. `j7elte-power`

### 3.1 APKBUILD / packaging

* `sha512sum -c`: **8/8 OK**. `sh -n` on both `.initd`, the post-install and
  `restore-capacity-max.sh`: OK. `bash -n APKBUILD`: OK.
* `package()` simulated: exactly the declared tree — `/usr/bin/j7elte-power` 0755,
  `/etc/j7elte-power.conf` 0644, `/usr/libexec/j7elte-power/restore-capacity-max` 0755,
  both initd 0755, both systemd units 0644. `install="$pkgname.post-install"` is
  present, executable and checksummed; it only enables the EFS restore (OpenRC, systemd
  fallback) and disables both services on deinstall.
* `systemd-analyze verify` on both units: no parse/ordering errors (only the expected
  missing exotic units in the fake root).
* Host build and aarch64 GCC 10.3 `-static -Werror` build both clean.
* `parity/userspace/battery/build-check.sh` run end-to-end (with `TMPDIR` on the session
  disk because `/tmp` is full): host build, GCC 10.3 cross build, uevent parser test,
  checksums, `build()`/`package()` tree, service syntax, six restore-script cases and
  the 12 functional checks — **`all build checks passed`**, independently of the
  package's report.

### 3.2 Kernel-interface cross-check (no invented paths)

Checked in `drivers/battery/sec_battery.c`, not only in the report:

* `batt_slate_mode` store (`sec_battery.c:3903-3930`) parses `%d\n`, accepts exactly 0/1,
  and on 1 zeroes `POWER_SUPPLY_PROP_CURRENT_NOW` and (with `always_enable`, set in the
  DT) clears `CHARGING_ENABLED`. The daemon writes `"%d\n"` — matches.
* `batt_capacity_max` store (`sec_battery.c:4294-4313`) gates on `800 < x < 1200` and
  `!fg_reset`; the daemon/restore script use the same 801..1199 window.
* `capacity`/`status` come from the standard `sec_battery_props[]`;
  `ac`/`usb`/`wireless/online` exist and are read via the parent of `battery_dir`.
* Uevent payload parsing (NUL-terminated key=value fields) is correct and regression-
  tested by the package's own test; the daemon's latch adoption on restart and its
  release on SIGTERM were exercised.

No defect found. The two unit tests that matter were re-run from a clean binary, not
taken from the report.

### 3.3 Residual (not fixable without hardware)

* OpenRC init scripts are syntax-checked only (`openrc-run` absent); `openrc-run` was
  available in a container but running these services needs the real rootfs and
  `localmount`/`udev-settle`, so this remains unexecuted. Note the `pidfile=` in
  `j7elte-efs-restore.initd` is superfluous for a foreground oneshot; it should not
  break boot, but a maintainer with OpenRC can drop it.
* The report's apply-order note mentions
  `0002-device-samsung-j7elte-depend-on-j7elte-power.patch`, which **no longer exists**;
  the dependency now lives in the consolidated `parity/patches/userspace/0001-device-deps.patch`.
  This is a documentation staleness, not a packaging error.

---

## 4. Patch application on pristine `ba682bd`

All applied sequentially with `git apply` to a fresh `git archive ba682bd` export:

```
0001-device-deps.patch                                                   APPLIED
audio/0002-j7elte-audio-ucm-package.patch                                APPLIED
sensors/0001-add-j7elte-sensors-package.patch                            APPLIED
review-a/0001-j7elte-sensors-ignore-unknown-dbus-header-fields.patch     APPLIED
battery/0001-pmaports-add-j7elte-power-package.patch                     APPLIED
```

The files created by the audio/sensors/power patches are byte-identical to the working
package directories; after my patch the sensors tree (including the corrected
`sha512sums`) is byte-identical and verifies. `0001-device-deps.patch` already contains
`j7elte-audio-ucm`, `j7elte-power` and `j7elte-sensors` (plus the other slices'
packages), so no extra device-dependency patch is needed for these three.

## 5. Changes made by this review

```
M device/archived/j7elte-sensors/dbus-wire.c
M device/archived/j7elte-sensors/APKBUILD          (dbus-wire.c sha512sum)
M parity/userspace/sensors/dbus-wire.c             (canonical copy, kept in sync)
A parity/patches/userspace/review-a/0001-j7elte-sensors-ignore-unknown-dbus-header-fields.patch
A parity/reports/review-userspace-a.md
```

Nothing else was edited; no shared device/kernel config was touched.

## 6. What still needs checking (explicitly not claimed)

* Any on-device audio play/capture, jack detection, sensor orientation/tilt signs,
  SM5703 charge-limit behaviour and EFS mount discovery.
* A real `abuild`/musl build (`apk`/`abuild` are absent here); the C sources are
  libc/POSIX only and cross-compile with GCC 10.3, so this is expected to be equivalent.
* OpenRC execution of the four init scripts.
