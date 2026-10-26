# Sensors userspace — j7elte (`j7elte-sensors` + `net.hadess.SensorProxy`)

**Verdict: IMPLEMENTED. Host integration test PASS. Cross-compiles warning-free for
aarch64. Package/patch verified against pmaports `ba682bd`. No hardware test performed
(no device available); nothing in the shipped daemon synthesises sensor values.**

Scope: a small root daemon that exposes the two evdev sensors implemented by the Nura
kernel — K2HH accelerometer and CM36672P/STK3013 proximity — on the system bus as
`net.hadess.SensorProxy`, so existing iio-sensor-proxy consumers work on a kernel whose
sensor drivers are *not* IIO. This closes residual risk 1 of `parity/reports/sensors.md`
(“no userspace consumer”).

Reference kernel: `android_kernel_samsung_universal7580-…` @ `b5e363a` (3.10.108).
Reference pmaports: `parity/pmaports` @ `ba682bd`.

## 1. Kernel interfaces (verified against the drivers)

### 1.1 K2HH accelerometer — `drivers/sensors/k2hh.c`

| Property | Kernel evidence | Daemon use |
|---|---|---|
| input name | `MODULE_NAME "accelerometer_sensor"` (`k2hh.c:39`), `dev->name` (`:1167`), registered `:1177` | matches by `EVIOCGNAME` (`j7elte-sensors.c:366-405`) |
| event codes | `EV_REL` `REL_X/REL_Y/REL_Z/REL_DIAL/REL_MISC` (`:1170-1174`) | requires `REL_X/Y/Z` (`evdev_is_accel`), ignores `REL_DIAL/MISC` |
| payload per report | x/y/z + `REL_DIAL` (time_hi) + `REL_MISC` (time_lo) then `input_sync()` (`:646-651`) | one `SYN_REPORT` = one sample |
| units | 4 g range selected on enable (`:693`), `MAX_ACCEL_1G 8192` (`:43`) | `K2HH_LSB_PER_G 8192.0` |
| timestamps | boottime-based, split into `REL_DIAL/REL_MISC` | not used (event time is not the driver’s hi/lo timestamp; no consumer needs it) |
| control | `<inputN>/enable` (`:748-751`), `<inputN>/poll_delay` ns (`:748-751`, store `:722-747`) | writes `enable` = 1/0 and `poll_delay` = 100000000 (10 Hz, within min 5 ms / max 200 ms) |
| calibration | `/efs/FactoryApp/accel_calibration_data` read on enable (`:41`, `:475-511`), subtracted in `k2hh_work_func` (`:623-625`) | **none** — daemon receives already-calibrated values, must not apply its own |
| sysfs location | group created on `dev->dev.kobj` (`:1186`); `eventN/device` resolves to `inputN` | opens `/sys/class/input/eventN/device/{enable,poll_delay}` (`j7elte-sensors.c:421-452`) |

### 1.2 Proximity — `drivers/sensors/cm36672p.c` / `stk3013.c`

| Property | Kernel evidence | Daemon use |
|---|---|---|
| input name | `"proximity_sensor"` (`cm36672p.c:1117`, `stk3013.c:1630`) | matches by `EVIOCGNAME` |
| event code | `EV_ABS` / `ABS_DISTANCE`, range 0..1 (`cm36672p.c:1118-1120`, `stk3013.c:1632-1633`) | `EV_ABS ABS_DISTANCE` (`j7elte-sensors.c:598-607`) |
| semantics | `/* 0 is close, 1 is far */` (`cm36672p.c:456-458`); initial report 1 at enable (`:457`, `stk3013.c:617-618`) | `ProximityNear = (distance == 0)` |
| control | `<inputN>/enable`, stores accept exactly `"1"`/`"0"` (`cm36672p.c:416-420`, `stk3013.c:1106-1116`) | writes `"1"`/`"0"` |
| sysfs location | group on input device kobj (`cm36672p.c:1136`, `stk3013.c:1647`) | `/sys/class/input/eventN/device/enable` |
| calibration | `/efs/FactoryApp/prox_cal` (`cm36672p.c:61`, `:445-447`; `stk3013.c:143`, `:1531-1538`) | **none** — kernel applies offset/threshold calibration |
| no light | neither driver registers an ALS channel in this kernel | `HasAmbientLight=false`, `LightLevelUnit="lux"`, `LightLevel=0.0` |

Both proximity drivers claim the same `gpa1-4` interrupt in the DT; only the fitted chip
probes successfully. Discovery picks the first `proximity_sensor` event node, so it works
with either chip. No poll_delay exists for proximity, therefore none is written.

Hall/flip-cover (`drivers/input/flip_cover.c`) has no SensorProxy property and is not used.

## 2. Mismatches found and fixed

1. **Zero-valued `EV_REL` events are dropped by the kernel.**
   `drivers/input/input.c:318-322` in the Nura tree only passes `EV_REL` when
   `value` is non-zero. The daemon previously required `REL_X` **and** `REL_Y` **and**
   `REL_Z` before accepting a report, so any stationary axis reading exactly 0 made the
   sample (and the orientation update) disappear. Reproduced on the host kernel with a
   uinput probe: writing `REL_X=0, REL_Y=8192, REL_Z=0` delivers only `REL_Y` + `SYN_REPORT`.
   Fixed in `j7elte-sensors.c:556-596`: accumulate per report, treat a missing axis as 0
   (the driver reads all three axes for every sample), accept the report when at least one
   `EV_REL` arrived.
2. **`NameOwnerChanged` released the wrong claim owner.**
   `handle_name_owner_changed()` copied `new_owner` (empty on disconnect) instead of `name`,
   so claims of a vanished client were never released and the sensors stayed enabled.
   Fixed at `j7elte-sensors.c:865-878`; the host test log now shows
   `released claims of vanished client :1.2`.
3. **Makefile installed a non-existent file.** `make install` referenced
   `net.hadess.SensorProxy.conf` while the shipped file is `j7elte-sensors.conf`; fixed in
   `parity/userspace/sensors/Makefile` (the APKBUILD already installed the right file).

No other interface mismatch: names, event codes, sysfs attribute names, unit scaling and
0/1 proximity semantics all match the drivers.

## 3. Cross-compilation

Available toolchain (only aarch64 toolchain in the session):
`parity/../toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc`
(GNU Toolchain 10.3-2021.07, gcc 10.3.1).

Command and output (`parity/userspace/sensors/build-aarch64.log`):

```
make CC=<toolchain>/aarch64-none-linux-gnu-gcc \
     CFLAGS='-O2 -g -Wall -Wextra -std=gnu11' LDFLAGS='-static'
  … -c -o j7elte-sensors.o j7elte-sensors.c
  … -c -o dbus-wire.o dbus-wire.c
  … -static -o j7elte-sensors j7elte-sensors.o dbus-wire.o -lm
exit=0
j7elte-sensors: ELF 64-bit LSB executable, ARM aarch64, statically linked
(no dynamic section)
```

Zero warnings with `-Wall -Wextra`. Artifact `parity/userspace/sensors/j7elte-sensors-aarch64`
(sha256 `4104ecd8b903d416a7342ec4fc9c3f51c25fff70695d66d6016f66c36bd249ee`).

**Limitation:** this is a glibc-targeted static binary; the deployment target is
postmarketOS aarch64 **musl**. No musl cross toolchain, `abuild`, `pmbootstrap` or arm64
binfmt is available in this environment (an `alpine:arm64` container cannot execute), so the
musl/abuild build was not run. The sources use only libc/POSIX and `<linux/input.h>`; there
is no glibc-specific API, so the Alpine build is expected to be equivalent. The glibc binary
must **not** be installed on the device.

## 4. Packaging and install

APKBUILD: `parity/pmaports/device/archived/j7elte-sensors/APKBUILD`
(`arch="aarch64"`, `depends="dbus"`, `options="!check"`; no libdbus/libsystemd linkage —
the daemon speaks the D-Bus wire protocol itself).

Installed files (`package()`):

| Path | Mode | Source |
|---|---|---|
| `/usr/bin/j7elte-sensors` | 755 | built binary |
| `/usr/lib/systemd/system/j7elte-sensors.service` | 644 | `Type=dbus`, `BusName=net.hadess.SensorProxy`, `ExecStart=/usr/bin/j7elte-sensors`, `ProtectSystem=strict`, `RestrictAddressFamilies=AF_UNIX` |
| `/etc/init.d/j7elte-sensors` | 755 | OpenRC service (`command_background`, pidfile, `need dbus`) |
| `/usr/share/dbus-1/system.d/j7elte-sensors.conf` | 644 | bus policy: only root may own `net.hadess.SensorProxy`; any local user may call the four interfaces |

All seven source files in `source=` are present and their `sha512sums` were recomputed after
the source fixes (`sha512sum -c` on all seven: OK); `sh -n APKBUILD` passes. `make install
DESTDIR=/tmp/... PREFIX=/usr` produces exactly the four paths above.

The package is not listed as a dependency of `device-samsung-j7elte` (that shared file is
out of scope for this task); a device image must add `j7elte-sensors` to its dependency
list to get it installed.

Default state after install: the unit is installed but not enabled; enable with
`systemctl enable --now j7elte-sensors` (systemd) or `rc-update add j7elte-sensors default`
(OpenRC). The daemon runs as root (required by the bus policy and by the 0640 root:input
event nodes).

Patch: `parity/patches/userspace/sensors/0001-add-j7elte-sensors-package.patch`.
Verified against a clean `git archive ba682bd` export:

```
git apply --check …/0001-add-j7elte-sensors-package.patch   → OK
git apply … && diff -r pmaports/device/archived/j7elte-sensors → identical
```

## 5. Interface / config validation

- **Introspection XML**: the daemon was run against a private dbus-daemon and the XML was
  fetched with `gdbus introspect --xml`; `xmllint --nonet --dtdvalid
  /usr/share/xml/dbus-1/introspect.dtd` passes (needs the local DTD path; the DOCTYPE URL
  is not fetched). Interface matches iio-sensor-proxy: read-only properties
  `HasAccelerometer`, `AccelerometerOrientation`, `AccelerometerTilt`, `HasAmbientLight`,
  `LightLevelUnit`, `LightLevel`, `HasProximity`, `ProximityNear`; methods
  `Claim{Accelerometer,Light,Proximity}` / `Release{…}`; `PropertiesChanged` signal.
  `Get`/`GetAll` answered, `Set` rejected with `PropertyReadOnly`.
- **Bus policy**: the fragment is well-formed XML, and wrapping it in a minimal system-bus
  config parses cleanly with the real `dbus-daemon` (`--config-file`, rc=0). The distribution
  `busconfig.dtd` shipped with this host is the obsolete 1.0 version and rejects even
  `/usr/share/dbus-1/system.conf` (it lacks `send_destination` and friends), so DTD
  validation of the policy is not meaningful; the parser check is authoritative.
- **systemd unit**: `systemd-analyze verify` against a minimal install root (binary and
  unit installed at `/usr/bin` and `/usr/lib/systemd/system`) accepts the unit and
  `ExecStart`; the only diagnostic is `Unit dbus.service not found`, because that minimal
  root has no dbus package (on the device `dbus` is a dependency). Without the root it
  additionally reports the missing installed binary, as expected. Runtime requirements:
  `/dev/input` reads, `/sys` writes (`/sys` is writable under `ProtectSystem=strict`),
  `/run/dbus/system_bus_socket` (AF_UNIX).

## 6. Test plan

### Executed (host, emulated evdev — no hardware)

`parity/userspace/sensors/tests/run-host-test.sh` creates two **uinput** devices with the
kernel drivers’ names/capabilities, runs the daemon against a private dbus-daemon, claims
the sensors and drives test vectors through the emulator. The emulator
(`tests/test-emu.c`) is test-only, is not part of the package, and every value it emits is
TEST DATA.

Because `/dev/input/event*` is mode 0640 root:input on the build host and the user is not
root, the suite was executed in a privileged, host-root chroot container
(`docker run --platform linux/amd64 --privileged -v /:/hostroot alpine chroot /hostroot …`).
Result:

```
ok: HasAccelerometer = b true
ok: HasProximity = b true
ok: HasAmbientLight = b false
ok: AccelerometerOrientation = s "undefined"
ok: AccelerometerOrientation = s "normal"
ok: AccelerometerTilt = s "vertical"
ok: ProximityNear = b false
ok: AccelerometerOrientation = s "right-up"
ok: AccelerometerOrientation = s "bottom-up"
ok: AccelerometerTilt = s "face-up"
ok: ProximityNear = b true
PASS
```

This includes the zero-axis regression (upright vector `(0, 8192, 0)`) and the client
disconnect/claim-release path.

### On-device (not performed)

1. Install `j7elte-sensors` (and enable the unit); `journalctl -u j7elte-sensors` (or
   `rc-service j7elte-sensors status`) must log `found accelerometer at
   /dev/input/eventN` and `found proximity at /dev/input/eventM` and `owning
   net.hadess.SensorProxy`.
2. `busctl introspect net.hadess.SensorProxy /net/hadess/SensorProxy` and
   `busctl get-property … HasAccelerometer HasProximity` → both `true`.
3. `busctl call … ClaimAccelerometer`, then check
   `cat /sys/class/input/eventN/device/enable` = 1 and `poll_delay` = 100000000.
4. Rotate the phone / turn screen up/down: `AccelerometerOrientation` and
   `AccelerometerTilt` must change (`normal` at rest upright, `face-up`/`face-down` when
   laid flat); `PropertiesChanged` must be emitted. No values are invented by userspace:
   if the driver is disabled or the event node disappears, the property stops updating.
5. Cover/uncover the proximity window: `ProximityNear` must become `true` when covered
   (driver reports 0). `ClaimProximity` also writes `enable=1`; releasing it writes 0.
6. Disconnect test: `busctl call … ClaimAccelerometer` from a short-lived process; after it
   exits the log must show the claim release and `enable` must return to 0.
7. Suspect/dual-proximity check: `dmesg | grep -i 'cm36672p\|stk3013'` identifies which chip
   probed; only one `proximity_sensor` node is expected.

## 7. Limitations / residual risks

- **No hardware**: nothing was run on a j7elte. Orientation/tilt signs and the 8192 LSB/g
  scale come from the driver source and the DT (axis map / negate flags); the host test
  only proves the plumbing and the algorithm, not the physical axis orientation.
- **No musl build**: only a glibc aarch64 cross build was possible (section 3); the actual
  package build must be checked with `abuild` in the pmOS build environment.
- **No `/efs` data on a converted device**: the kernel then uses zero calibration
  (`k2hh.c:485-491` on open failure and `:559-562` when no calibration is measured,
  `cm36672p.c:445-447` tolerates `-ENOENT`, `stk3013.c:1531-1538` only logs).
  Readings still work but without factory offset; the daemon deliberately does not
  compensate on its own.
- **Proximity chip ambiguity**: cm36672p and stk3013 share `gpa1-4`; if both ever probed,
  the daemon would bind the first event node. This is inherited from the ROM DT and the
  shipped HAL behaved the same way.
- **No ALS / no light sensor**: `HasAmbientLight=false`; `LightLevel` is fixed 0.0 and
  `ClaimLight` is accepted as a no-op for API compatibility (iio-sensor-proxy clients do not
  require a light sensor).
- The service is not enabled by the package, and the device package does not yet depend on
  it (shared file out of scope), so installing the APKBUILD alone does not start the daemon.
