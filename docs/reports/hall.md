# Hall / flip-cover (`samsung,hall`) on j7elte

**Verdict: the driver is built in, bound, IRQ-registered and its `hall` evdev node
works; the userspace daemon now exposes the cover as `LidIsClosed`. The *physical*
flip was NOT performed (no magnet / no physical access to the phone), so
`SW_LID=1` was never captured. Section 4 gives the strongest indirect evidence
and the exact one-command test that produces the real event.**

No kernel change was made: `CONFIG_INPUT_FLIP_COVER=y` was already in the built
config and the driver is live on the phone (kernel `3.10.108 #33`).

## 1. Driver source, config and binding

| Item | Evidence |
|---|---|
| Driver | `drivers/input/flip_cover.c`, `compatible = "hall"` (line 433), built via `drivers/input/Makefile:30` |
| Config | `CONFIG_INPUT_FLIP_COVER=y` (`build/final-config:1432`, kernel tree `.config`); `CONFIG_CERTIFY_HALL`/`CONFIG_SENSORS_HALL_IRQ_CTRL` unset, as in the ROM (`docs/reports/sensors.md`) |
| Bound | `/sys/bus/platform/drivers/hall/hall.40` exists |
| Boot log | `/var/log/dmesg`: `hall_init start` → `init_hall_ic_irq : success` → `input: hall as /devices/hall.40/input/input6` |
| First hardware read | `/var/log/dmesg`: `keys:flip_cover_work #1 : 1` (open cover) |
| Interrupt | `/proc/interrupts`: `558: 0 0 0 0 exynos_wkup_irq_chip flip_cover` (registered, no edge yet) |
| Pin mux | `pin 30 (gpa3-6): hall.40 (GPIO UNCLAIMED) function /pinctrl@139B0000/hall-irq group gpa3-6` in `pinctrl/139b0000.pinctrl/pinmux-pins` |
| Pin conf | `pin 30 (gpa3-6): PUD(0x0) DRV(0x0)` in `pinconf-pins` — matches DT `pud 0, drv 0` |
| DT | `hall { compatible = "hall"; pinctrl-0 = <&hall_irq>; hall,gpio_flip_cover = <&gpa3 6 0>; status = "okay"; }` (verified identical to the stock ROM DTB in `docs/reports/sensors.md`) |
| Input node | `/dev/input/event6`, `EVIOCGNAME = "hall"`, `phys = "hall"`, `id 0000:0000:0000:0000` |
| Bits | `uevent`: `EV=100021 SW=1`; `capabilities/sw = 1` = only `SW_LID` (`EV_SW` + `EV_SYN` + `EV_REP`) |
| State sysfs | `/sys/class/sec/sec_key/hall_detect` → `OPEN` (`hall_attr_group`, same driver) |

The boot ring buffer no longer contains the probe lines (it was overwritten by an
unrelated userspace oops at uptime 1274 s); `/var/log/dmesg` is the persisted copy
of this boot and shows them.

## 2. Event mapping (from the driver and the input core)

`flip_cover_work()` does

```c
flip_cover = gpio_get_value_cansleep(ddata->gpio_flip_cover);   /* 1 = open */
input_report_switch(ddata->input, SW_LID, !flip_cover);
input_sync(ddata->input);
```

and `hall_suspend/resume` re-reports the same value.

* `EV_SW` (5), `SW_LID` (0), `value 0` → cover **open**
* `EV_SW` (5), `SW_LID` (0), `value 1` → cover **closed**

The input core keeps the switch state itself (`drivers/input/input.c:303-310`) and
drops a report equal to the current state — including the report that
`hall_open()` schedules ~500 ms after the device is opened. Consequence, verified:

* a 4 s read of `/dev/input/event6` as root while `hall_detect = OPEN` returned
  **0 events**. That is correct kernel behaviour, not a fault, and it is why the
  daemon cannot learn the initial state from evdev.

## 3. Userspace exposure

### 3.1 `j7elte-sensors` (changed)

`net.hadess.SensorProxy` has no lid property, so the existing daemon (same process,
same event loop, no fork of the package) was extended:

* discovers the input device by `EVIOCGNAME == "hall"` and `EV_SW`/`SW_LID`
  capability (no hard-coded `event6`);
* reads the initial state from `/sys/class/sec/sec_key/hall_detect`
  (`OPEN`/`CLOSE`) because of the dropped no-change report;
* parses `EV_SW/SW_LID` and updates a new read-only boolean property
  **`LidIsClosed`** (also in introspection XML and `GetAll`), emitting
  `org.freedesktop.DBus.Properties.PropertiesChanged` on every change.

Deployed on the phone (built there with the package command line, installed to
`/usr/bin/j7elte-sensors`, service restarted):

```
j7elte-sensors: found accelerometer at /dev/input/event3 (control: enable=yes poll_delay=yes)
j7elte-sensors: found proximity at /dev/input/event4 (control: enable=yes poll_delay=no)
j7elte-sensors: found lid at /dev/input/event6 (state: open, initial state from /sys/class/sec/sec_key/hall_detect)
j7elte-sensors: owning net.hadess.SensorProxy as sensor proxy
$ busctl get-property net.hadess.SensorProxy /net/hadess/SensorProxy \
      net.hadess.SensorProxy LidIsClosed
b false
$ busctl introspect … | grep Lid
.LidIsClosed  property  b  false  emits-change
```

`GetAll` now returns 9 properties including `"LidIsClosed" b false`, next to the
unchanged accelerometer/proximity ones.

The evidence that the new evdev→property path is wired correctly is the package's
host integration test, run in a privileged Alpine container (uinput devices named
exactly like the kernel ones, private D-Bus). It passes, including
`LidIsClosed: b false → b true → b false` driven by `SW_LID 1/0`. Those are
**test data** from `tests/test-emu.c`, never used on hardware, and do not replace
the real capture of §4:

```
ok: HasAccelerometer = b true
ok: HasProximity = b true
ok: LidIsClosed = b false
...
ok: LidIsClosed = b true
ok: LidIsClosed = b false
PASS
```

### 3.2 What already existed: elogind

elogind (OpenRC service, running) already opens `/dev/input/event6` itself and
exposes the standard logind property:

```
$ busctl introspect org.freedesktop.login1 /org/freedesktop/login1 | grep -i lid
.HandleLidSwitch          property  s  "suspend"  const
.HandleLidSwitchDocked    property  s  "ignore"   const
.LidClosed                property  b  false      emits-change
```

`/proc/*/fd` shows event6 open by both `/usr/libexec/elogind/elogind` and
`/usr/bin/j7elte-sensors`; evdev allows both. Note that the default
`HandleLidSwitch="suspend"` means a real close will suspend the phone unless a
`handle-lid-switch` inhibitor is held — the test script below holds one.

## 4. The physical flip was not performed

Nobody could place a magnet on the top bezel over SSH, so the transition
`SW_LID 1` (closed) has **not** been observed on this unit. What is established
instead:

* the driver probed, the threaded IRQ `flip_cover` is registered on the Exynos
  wakeup interrupt controller (`/proc/interrupts`, IRQ 558);
* the pin is muxed by `hall.40` to the `hall-irq` EINT function with the DT's
  pull/drive setting;
* the driver has read the pin for real at boot (`flip_cover_work #1 : 1`) and
  `hall_detect` reads `OPEN`, i.e. the sensor currently reports the physically
  correct state;
* the evdev node advertises exactly `EV_SW`/`SW_LID`, and the daemon decodes
  `EV_SW/SW_LID` and publishes it (host integration test with the same input-core
  semantics).

### Exact one-command test (needs a magnet at the top bezel)

From the repository root, run:

```sh
cat code/userspace/sensors/tests/hall-flip-test.sh | \
  ssh user@172.16.42.1 'cat > /tmp/hall-flip-test.sh && chmod +x /tmp/hall-flip-test.sh && rootshell sh /tmp/hall-flip-test.sh 60'
```

(`rootshell` is the phone's setuid root helper; the script itself holds an
`elogind-inhibit --what=handle-lid-switch` for the duration so elogind does not
suspend the phone, prints `hall_detect`, elogind `LidClosed` and the daemon's
`LidIsClosed`, then reads the raw events.)

Expected output on a real flip — the cover is open at rest:

```
hall input device: /dev/input/event6
hall_detect:  OPEN
elogind LidClosed: b false
SensorProxy LidIsClosed: b false

move a magnet near the top bezel now (60s)...
EV_SW code=0 (SW_LID) value=1 -> LidIsClosed=b true     # magnet applied
EV_SW code=0 (SW_LID) value=0 -> LidIsClosed=b false    # magnet removed
```

A `PropertiesChanged` for `LidIsClosed` can be watched in parallel with
`dbus-monitor --system "type='signal',path='/net/hadess/SensorProxy'"`.

The script was executed on the device with a 4 s window (no magnet) and printed
the four state lines and exited cleanly; it has not seen a transition for the
reason above.

## 5. Files

* `code/userspace/sensors/j7elte-sensors.c` — hall discovery, initial state from
  `hall_detect`, `EV_SW/SW_LID` decoding, `LidIsClosed` property.
* `code/userspace/sensors/tests/test-emu.c`, `tests/run-host-test.sh` — emulated
  `hall` uinput device and lid assertions (test only).
* `code/userspace/sensors/tests/hall-flip-test.sh` — real-flip capture helper.
* `code/pmaports/device/archived/j7elte-sensors/` — synced sources, APKBUILD
  checksum updated.
* `patches/userspace/sensors/0001-add-j7elte-sensors-package.patch` — regenerated;
  applies cleanly to an empty tree and reproduces the package byte-for-byte.
