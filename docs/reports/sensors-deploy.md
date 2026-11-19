# Sensors userspace deployment — j7elte on device

**Verdict: DEPLOYED and WORKING for both evdev sensors.**
`j7elte-sensors` (the `net.hadess.SensorProxy` bridge from
`patches/userspace/sensors/0001-add-j7elte-sensors-package.patch`) was compiled on the
phone, installed, enabled as an OpenRC service and verified with real readings:

| Sensor | Kernel interface | Result |
|---|---|---|
| K2HH accelerometer | `/dev/input/event3`, `accelerometer_sensor`, `EV_REL` x/y/z | **works**: 10 Hz, `|a| = 0.981–0.982 g = 9.62–9.63 m/s²` at rest, `AccelerometerTilt = "face-up"` |
| STK3013 proximity | `/dev/input/event4`, `proximity_sensor`, `EV_ABS ABS_DISTANCE` | **works**: chip answers as `PID=0x31 RID=0xc3`, PS enabled, `ProximityNear` follows the driver (0 = far at rest); near/far transition exercised (see §6), physical hand-cover not possible |
| CM36672P proximity | i2c 0x60 | **not present on this unit** (no ACK; the STK3013 driver owns `proximity_sensor`) |
| Ambient light | — | **does not exist** in this kernel: `HasAmbientLight=false`, `LightLevel=0.0` |
| Hall / flip cover | `/dev/input/event6` (`hall`) | not a `SensorProxy` sensor; not used by the daemon |

The patch's own daemon is the correct target: these drivers are *not* IIO, so the stock
`iio-sensor-proxy` cannot talk to them (`docs/reports/sensors.md` risk 1). The daemon is a
wire-protocol `net.hadess.SensorProxy` server, so `iio-sensor-proxy` consumers work
unchanged. `iio-sensor-proxy` is not installed on this device and is not needed.

Device: SM-J700M j7elte, Alpine/musl aarch64, kernel `3.10.108 #33`, OpenRC, dbus
(system bus running), no systemd. All work was done over SSH; no kernel or boot image was
touched.

## 1. Build (on the device)

The device image has **no `make`** (and no `abuild`/`pmbootstrap`), so the package sources
were copied verbatim to `/tmp/j7sens` and built with the exact command line of the
package `Makefile`:

```
$ ssh user@172.16.42.1 'cd /tmp/j7sens && gcc --version | head -1'
gcc (Alpine 15.2.0) 15.2.0          # aarch64-alpine-linux-musl

$ gcc -O2 -g -Wall -Wextra -std=gnu11 -c -o j7elte-sensors.o j7elte-sensors.c   # rc=0
$ gcc -O2 -g -Wall -Wextra -std=gnu11 -c -o dbus-wire.o dbus-wire.c             # rc=0
$ gcc -o j7elte-sensors j7elte-sensors.o dbus-wire.o -lm                        # rc=0
```

Both translation units compile **warning-free with `-Wall -Wextra`** against musl, which
closes residual risk “no musl build” of `docs/reports/sensors-userspace.md`. No source
change was needed: the shipped sources build and run as-is.

Artifact: `/tmp/j7sens/j7elte-sensors`, 158208 bytes, dynamically linked musl binary;
installed copy `sha256 = 694edf30d2f78bfe908c7a5acc0727d5913c85aa4786892e20a96f56c529dc54`.

## 2. Install

Copied from the package's `package()` file list (same paths/modes):

```
install -Dm755 j7elte-sensors            /usr/bin/j7elte-sensors
install -Dm644 j7elte-sensors.service    /usr/lib/systemd/system/j7elte-sensors.service
install -Dm755 j7elte-sensors.initd      /etc/init.d/j7elte-sensors
install -Dm644 j7elte-sensors.conf       /usr/share/dbus-1/system.d/j7elte-sensors.conf
```

The systemd unit is installed for completeness only (this image runs OpenRC). The D-Bus
policy fragment (`/usr/share/dbus-1/system.d/j7elte-sensors.conf`, root owns
`net.hadess.SensorProxy`, any local user may call it) was activated without restarting the
bus:

```
$ dbus-send --system --type=method_call --dest=org.freedesktop.DBus / \
      org.freedesktop.DBus.ReloadConfig        # rc=0
```

## 3. Service

```
$ rc-update add j7elte-sensors default
 * service j7elte-sensors added to runlevel default
$ rc-service j7elte-sensors start
 * Starting j7elte-sensors ... [ ok ]
$ rc-service j7elte-sensors status
 * status: started
$ rc-status default | grep -i sensor
 j7elte-sensors                                                    [  started  ]
$ pgrep -a j7elte-sensors
13552 /usr/bin/j7elte-sensors
$ busctl --system list | grep -i SensorProxy
net.hadess.SensorProxy   13552 j7elte-sensors  root :1.395  -  -  -
```

The service is in the `default` runlevel, so it starts at boot. The init script only needs
`dbus` (`need dbus`), matching the package.

Foreground run log (captured with `--verbose`):

```
j7elte-sensors: found accelerometer at /dev/input/event3 (control: enable=yes poll_delay=yes)
j7elte-sensors: found proximity at /dev/input/event4 (control: enable=yes poll_delay=no)
j7elte-sensors: owning net.hadess.SensorProxy as sensor proxy
```

## 4. Interface check

```
$ busctl --system introspect net.hadess.SensorProxy /net/hadess/SensorProxy
net.hadess.SensorProxy              interface -  -            -
.ClaimAccelerometer                 method    -  -            -
.ClaimLight                         method    -  -            -
.ClaimProximity                     method    -  -            -
.ReleaseAccelerometer               method    -  -            -
.ReleaseLight                       method    -  -            -
.ReleaseProximity                   method    -  -            -
.AccelerometerOrientation           property  s  "undefined"  emits-change
.AccelerometerTilt                  property  s  "undefined"  emits-change
.HasAccelerometer                   property  b  true         emits-change
.HasAmbientLight                    property  b  false        emits-change
.HasProximity                       property  b  true         emits-change
.LightLevel                         property  d  0            emits-change
.LightLevelUnit                     property  s  "lux"        emits-change
.ProximityNear                      property  b  false        emits-change
```

`for p in HasAccelerometer …; do busctl --system get-property net.hadess.SensorProxy \
/net/hadess/SensorProxy net.hadess.SensorProxy $p; done` → `b true`, `b true`, `b false`,
`s "undefined"`, `s "undefined"`, `b false`, `d 0`. The introspection XML and property
`emits-change` flags match iio-sensor-proxy 2.x, so existing clients (`gnome-settings`,
`iio-sensor-proxy`-aware apps) accept the object.

## 5. Accelerometer — real values

Claim from a client (this keeps the sensor enabled only while the client is on the bus):

```
$ python3 - <<'EOF'
import dbus
b = dbus.SystemBus()
p = b.get_object('net.hadess.SensorProxy', '/net/hadess/SensorProxy', introspect=False)
p.ClaimAccelerometer(dbus_interface='net.hadess.SensorProxy')
EOF
```

After the claim: `cat /sys/class/input/event3/device/enable` → `1`,
`…/poll_delay` → `100000000` (the daemon programs 10 Hz before enabling, which this
driver requires — see §7), `AccelerometerTilt` → `face-up`.

Reading the same event node while the daemon holds the claim (phone lying flat,
screen up):

```
accel sample x=-5 y=454 z=8027 |a|=0.9814 g = 9.62 m/s2
accel sample x=-4 y=453 z=8029 |a|=0.9817 g = 9.63 m/s2
accel sample x=-7 y=452 z=8026 |a|=0.9813 g = 9.62 m/s2
…
accel samples in 2s: 19
magnitude min=0.981 max=0.982 mean=0.981 g ; mean=9.62 m/s2
```

Driver-level confirmation (`stk3013`/`k2hh` bind correctly); on the same node with the
daemon stopped and the identical sysfs sequence (`poll_delay=100000000`, `enable=1`),
2 s gave 20 samples at 9.63 m/s². Magnitude is 0.98 g rather than 1.00 g because there is
no `/efs/FactoryApp/accel_calibration_data` on a converted device (kernel logs
`k2hh_open_calibration: No Calibration`) and the chip's zero-g offset is visible
(`y ≈ +454` LSB). It is a genuine, live measurement: it tracks the physical device (z
dominant while flat) and is derived from the driver's `EV_REL REL_X/Y/Z` stream, not
synthesised by the daemon.

`AccelerometerOrientation` stays `"undefined"` because the phone is flat; the orientation
is only defined for the four upright positions, while `AccelerometerTilt="face-up"`
confirms real gravity data. No value was manufactured: before any claim the properties are
`"undefined"`.

## 6. Proximity — real values

```
$ busctl call net.hadess.SensorProxy /net/hadess/SensorProxy \
      net.hadess.SensorProxy ClaimProximity        # while the daemon holds it
ProximityNear = 0                                   # far
raw_data = 0        (PS ADC count)   prox_avg = 0,0,0
state = 0           thresh_high = 18  thresh_low = 13
```

Driver log while the claim is held (chip is powered only while enabled — see §7):

```
[SENSOR] proximity_enable_store: Enable PS : 1
[SENSOR] stk3013_regulator_onoff: on
[SENSOR] stk3013_check_pid: PID=0x31, RID=0xc3
[SENSOR] stk3013_read_otp25: otp25=0x0
[SENSOR] stk3013_enable_ps: ps input event=1, ps code = 0     # 1 = far, initial report
[SENSOR] stk_work_func: ps input event=1, ps code = 10
```

Near/far transition exercised end-to-end through the real chain (chip INT flag → driver →
`ABS_DISTANCE` → daemon → property). The device could not be physically covered over SSH,
so the driver's own high threshold was lowered (this is a sysfs calibration knob of the
driver, no code was changed) and restored afterwards:

```
echo 0  > /sys/class/sensors/proximity_sensor/thresh_high
… t=2.0 ProximityNear=1        # near
… t=2.5 ProximityNear=0
echo 18 > /sys/class/sensors/proximity_sensor/thresh_high   # restored
[SENSOR] stk_work_func: ps input event=0, ps code = 1        # near flag from the chip
[SENSOR] stk_work_func: ps input event=1, ps code = 10       # back to far
```

After the restore `ProximityNear = false` with the default thresholds 18/13 and the PS ADC
reading 0–10 counts (nothing in front of the window) — i.e. the sensor is measuring, and
“far” is the physically correct answer for a phone lying face-up on a desk.

## 7. Kernel-driver behaviour found while validating (no kernel change made)

1. **`k2hh` needs `poll_delay` before `enable`.** `k2hh_enable_store()` writes `CTRL1` as
   `hr | data->odr | axes`; `data->odr` is only initialised by `k2hh_delay_store()`. With
   the default (never-written) delay the ODR field stays `1000`, the chip's output
   registers stay at zero and `STATUS=0x00` (verified by reading the chip through
   `i2c-dev`). After `poll_delay=100000000` the driver writes `CTRL1=0xb7` and samples
   flow (`STATUS=0xff`). The daemon already writes the delay first
   (`j7elte-sensors.c:sensor_set_enabled`), which is why it works; a manual
   `echo 1 > enable` without touching `poll_delay` correctly yields nothing.
2. **`k2hh`'s `lowpassfilter`/`reactive_alert` attributes are unsafe after a runtime
   rebind.** Writing `/sys/class/sensors/accelerometer_sensor/lowpassfilter` on a driver
   instance that was re-bound with `…/drivers/k2hh-i2c/{unbind,bind}` dereferences a stale
   `sensors_class` drvdata and oopses in `k2hh_i2c_read` (`NULL pointer dereference`,
   `PC is at k2hh_i2c_read+0x1c`). Nothing in the daemon touches those attributes; they
   were only used during diagnosis and are not needed.
3. **The STK3013 is powered down while idle.** Its `vdd`/`vio` regulators are switched by
   the driver on `enable`, so reading `enable`, `thresh_*`, `raw_data` while the sensor is
   disabled returns `-EIO`/`-EINVAL` (`ack was not received at write`). That is expected,
   not a fault: after `ClaimProximity` the same attributes read back normally. The daemon
   handles the failed writes (logs `failed to enable …` only when the chip is really
   absent).
4. **CM36672P is absent.** Both proximity nodes exist in the DT on the same `gpa1-4`
   interrupt, but only one chip is fitted: `i2c-0` address 0x48 (STK3013) answers with
   `PID=0x31`, address 0x60 (CM36672P) returns `ENXIO`. `proximity_sensor` therefore binds
   the STK3013, and the daemon's “first `proximity_sensor` node” discovery is correct for
   this unit.

## 8. Limitations / notes

- `AccelerometerOrientation` cannot be made to change over SSH (the phone does not move);
  `AccelerometerTilt="face-up"` and the live magnitude are the real-data evidence.
- Proximity “near” was produced by the driver's threshold register, not by a hand over the
  window; the resulting `ABS_DISTANCE`/`ProximityNear` values are still produced by the
  sensor chip and the driver (no userspace synthesis). With default thresholds the sensor
  reports far, which matches the physical situation.
- No ALS exists in this kernel, so `HasAmbientLight=false` and `LightLevel` stays 0.0;
  `ClaimLight` is accepted as a no-op for API compatibility.
- The device package `device-samsung-j7elte` still does not depend on `j7elte-sensors`
  (shared file, out of scope here). Installing the APKBUILD alone does not start the
  daemon; a device image must add the dependency and the `default` runlevel entry (both
  applied manually on this device as shown in §2–§3).
- Standard clients work unmodified: `busctl introspect`,
  `gdbus introspect --system --dest net.hadess.SensorProxy --object-path
  /net/hadess/SensorProxy` and `gdbus call … Properties.GetAll net.hadess.SensorProxy`
  all parse the daemon's XML/properties (`rc=0`), as does `dbus-python`.
