# Battery / charging — userspace integration (j7elte)

**Verdict: implemented (userspace only). No kernel change was needed or made.**

The kernel side was already complete (see `parity/reports/battery.md`): the standard
`power_supply` class is registered for `battery`/`ac`/`usb`/`wireless`/`ps` plus the
SM5703 charger/fuelgauge devices. UPower therefore gets **battery level, charging state,
voltage and temperature** with no helper at all. What userspace was missing was the
stock ROM's charge daemon behaviour: the `/efs/Battery/batt_capacity_max` calibration
restore and any way to set a charge limit. Both are now provided by `j7elte-power`,
built on interfaces that exist in this 3.10 vendor kernel (verified in the driver
sources), not on mainline `charge_control_*` attributes.

Environment: static analysis + cross-compilation only. **No j7elte hardware was
available**, so no sysfs value was ever read from a real phone. Every path and value in
this report is grounded in the kernel sources, the shipped DTM/ROM artifacts, or the
UPower sources.

---

## 1. Kernel `power_supply` inventory

`drivers/power/power_supply_sysfs.c:231` (`power_supply_attr_is_visible()`) exposes an
attribute file only if the property is listed in the driver's `properties[]` array
(`type` is always present). The file set of each device is therefore exactly its
property array.

### Devices

| sysfs name | `type` string | registered by | source |
|---|---|---|---|
| `battery` | `Battery` | `sec_battery` | `drivers/battery/sec_battery.c:6431` |
| `usb` | `USB` | `sec_battery` | `sec_battery.c:6437` |
| `ac` | `Mains` | `sec_battery` | `sec_battery.c:6444` |
| `wireless` | `Wireless` | `sec_battery` | `sec_battery.c:6451` |
| `ps` | `POWER_SHARING` | `sec_battery` | `sec_battery.c:6459` |
| `sm5703-charger` | `Unknown` | `sm5703_charger` | `sm5703_charger.c:1645`; name from DT `battery,charger_name` (`exynos7580-j7elte_battery.dtsi:84`) |
| `otg` | `OTG` | `sm5703_charger` | `sm5703_charger.c:1651` |
| `sm5703-fuelgauge` | `Unknown` | `sm5703_fuelgauge` | `sm5703_fuelgauge.c:1555`; name from DT `battery,fuelgauge_name` (`dtsi:85`) |

### Properties per device

| device | properties actually exposed (`type` always present) |
|---|---|
| `battery` | `status`, `charge_type`, `health`, `present`, `online`, `technology`, `voltage_now`, `voltage_avg`, `current_now`, `current_avg`, `charge_now`, `capacity`, `temp`, `temp_ambient`, `charge_control_limit` |
| `ac`, `usb`, `wireless` | `online`, `voltage_max`, `current_max` |
| `ps` | `status`, `online` |
| `sm5703-charger` | `status`, `charge_type`, `health`, `online`, `current_max`, `current_avg`, `current_now`, `charge_otg_control`, `charging_enabled`, `energy_now` |
| `otg` | `online` |
| `sm5703-fuelgauge` | `status`, `voltage_now`, `voltage_avg`, `current_now`, `current_avg`, `charge_full`, `energy_now`, `capacity`, `temp`, `temp_ambient` |

`charge_control_limit` is present because `CONFIG_BATTERY_SWELLING=y`
(`sec_battery_props[]`, `sec_battery.c:123-125`). `time_to_full_now` is **absent**
(`CONFIG_CALC_TIME_TO_FULL` is not set, `config-nura-j7elte:3390`).

### Custom attributes on `battery` (not part of the class ABI)

`sec_bat_create_attrs()` (`sec_battery.c:4472,6587`) creates 76 driver-specific files
directly on the `battery` device with mode 0664
(`SEC_BATTERY_ATTR()`, `include/linux/battery/sec_battery.h:283`). The two that matter
here:

| attribute | access | semantics |
|---|---|---|
| `batt_capacity_max` | rw | reads `fuelgauge->capacity_max`; writes are forwarded to the fuelgauge `ENERGY_FULL_DESIGN` property (`sec_battery.c:3733,4294-4320`). Store gate: value must satisfy `800 < x < 1200` (`sec_battery.c:4299`). |
| `batt_slate_mode` | rw | 0/1. While 1 the driver stops charging: `cable_work` zeroes the charger current, and with `battery,always_enable` (set, `dtsi:180`) also clears `CHARGING_ENABLED`; it sets `cable_type = BATTERY` and reports `Discharging` (`sec_battery.c:3900-3927,3276-3290`). Cleared to 0 at driver probe (`sec_battery.c:6393`). |

Other useful custom files: `batt_temp`, `batt_vol_adc`, `batt_vfocv`, `batt_vol_aver`,
`fg_capacity`, `battery_cycle`, `hv_charger_status`, `batt_charging_source`.
`batt_sw_self_discharging` and `time_to_full_now` are **absent** because
`CONFIG_BATTERY_SWELLING_SELF_DISCHARGING` and `CONFIG_CALC_TIME_TO_FULL` are not set —
the stock `lpm` daemon looks for them and tolerates their absence.

### Units / ABI deviations in this vendor kernel (important for any consumer)

| attribute | ABI says | this kernel actually returns |
|---|---|---|
| `battery/voltage_now` | µV | µV (correct: `battery->voltage_now * 1000`, `sec_battery.c:4696`) |
| `battery/current_now` | µA | **mA** (`battery->current_now` is the fuelgauge mA value, `sec_battery.c:4711`) |
| `battery/charge_now` | µAh | **not a charge**: returns `battery->charging_mode` (enum 0..4, `sec_battery.c:4717-4718`) |
| `battery/temp` | 0.1 °C | 0.1 °C (correct) |
| `battery/capacity` | % | % (correct) |
| `sm5703-fuelgauge/voltage_now` | µV | **mV** (`sm5703_get_vbat()`) |
| `sm5703-fuelgauge/{status,charge_full,energy_now}` | — | return `-ENODATA` (`sm5703_fuelgauge.c:1032-1035`) |

Sign convention: positive `current_now` while charging, negative while discharging —
`sec_battery.c:2241` uses `current_now > 0 && current_now < full_check_current` as the
"charge finished" test, and the fuelgauge sign bit is passed through unmodified.

---

## 2. UPower: what it needs vs. what j7elte provides

Reference: UPower **1.91.0-r2**, the version in Alpine edge for aarch64
(`APKINDEX`, `V:1.91.0-r2`) which is what postmarketOS/Nura builds against. UPower has
two Linux classes:

* `src/linux/up-device-supply-battery.c` — used for any `power_supply` device whose
  `type` is `battery` (`up-device-supply-battery.c:566-569`).
* `src/linux/up-device-supply.c` — generic fallback used for everything else.

### Attributes UPower's battery class reads

| attribute | needed for | j7elte `battery` | result |
|---|---|---|---|
| `present` | is-present | yes | ✓ |
| `capacity` | percentage | yes | ✓ **battery level works** |
| `status` | state | yes (`Charging`/`Discharging`/`Not-charging`/`Full`) | ✓ **charging state works** |
| `voltage_now`/`voltage_avg` | voltage | yes (µV) | ✓ |
| `temp` | temperature | yes (0.1 °C) | ✓ |
| `technology` | technology | yes (`Li-ion`) | ✓ |
| `current_now` | rate / discharge detection | yes **but in mA** | usable (negative → discharging), rate off by 1000× |
| `charge_now` | energy | yes, but it is the charging-mode enum | ✗ meaningless |
| `energy_full`, `energy_full_design` | energy-full, health | **absent** | ✗ |
| `charge_full`, `charge_full_design` | energy-full, health | **absent** | ✗ |
| `cycle_count` | charge cycles | **absent** (`battery_cycle` is a private attr) | ✗ (-1) |
| `manufacturer`, `model_name`, `serial_number` | vendor/model/serial | **absent** | ✗ (empty) |
| `voltage_max_design`, `voltage_min_design` | design voltage | **absent** | falls back to `voltage_now` (`up_device_supply_battery_get_design_voltage()`) |
| `charge_types` | charge-type support | **absent** (`charge_type` is a different, older attr) | ✗ |
| `charge_control_start/end_threshold` | charge thresholds | **absent** | ✗ |

Consequences with UPower 1.91.0, exactly as implemented upstream:

* **Percentage** comes from `capacity` (`up-device-supply-battery.c:515`). Correct.
* **State** comes from `status`, except that a negative `current_now` forces
  `DISCHARGING` (`:526-529`). Correct for this driver's sign convention and for
  `batt_slate_mode` (which sets `status=Discharging`).
* **Energy / energy-full / time-to-empty / time-to-full**: UPower sees
  `energy_full = 0` (both energy and charge variants missing) so `energy.cur` stays 0;
  the rate is derived from the percentage history (`up_device_battery_estimate_power()`),
  but the time estimates divide 0 by that rate. Energy values are therefore zero and
  the time estimates are unreliable. This is a kernel-ABI limitation of the vendor
  `battery` psy, not something a userspace daemon can repair: writing to a sysfs
  attribute that does not exist is not possible and UPower reads sysfs directly.
* **Health** (`capacity` of the device) and **charge cycles** are unavailable for the
  same reason.
* **Charge thresholds**: 1.91.0 either reads the sysfs `charge_control_*_threshold`
  files or a udev property `CHARGE_LIMIT` (`up-device-supply-battery.c:182,211-217`),
  and applies requests by writing those two sysfs files with
  `G_FILE_SET_CONTENTS_ONLY_EXISTING` (`:690-724`). The 3.10 vendor psy has neither
  file, so UPower correctly reports `charge-threshold-supported=false`. We deliberately
  do **not** set `CHARGE_LIMIT` in a udev rule: it would advertise a limit that UPower
  cannot write (the target files do not exist), i.e. a broken UI switch.

### What the other devices become

| device | UPower class | why |
|---|---|---|
| `ac` | line power | `type=Mains` (`up-device-supply.c:556-557`) |
| `usb` | line power | `type=USB`, no `usb_type` in 3.10 → warning + `online` heuristic (`:580,640-642`) |
| `wireless` | line power | type not recognised → `online` heuristic |
| `sm5703-charger` | line power | type `Unknown` → `online` heuristic; its `online` mirrors cable attachment (`sm5703_charger.c:856-863`) |
| `ps` | line power | type `POWER_SHARING` not recognised → `online` heuristic; `online` is 0 unless power-sharing is active |
| `otg` | line power | type `OTG` not recognised → `online` heuristic; `online` = `otg_enable_flag` (0 normally) |
| `sm5703-fuelgauge` | ignored | `type` is `Unknown` and the device has no `online` attribute, so the generic backend's fallback heuristic drops it (`up-device-supply.c:640-655`) |

The extra line-power devices are harmless for `on_ac` (they report 0 when no cable is
attached) and UPower has no udev/UPower.conf hook to filter `power_supply` devices, so
they cannot be suppressed without a kernel change. Nothing was fabricated to hide them.

### The one remaining UPower quirk at exactly 0 %

`up-device-battery.c:301-317`: if `energy.cur < 0.01` and `percentage > 0`, UPower
derives energy from `energy_full`; then if `percentage <= 0` it computes
`energy.cur / energy_full * 100`. With both values 0 this is `0/0 = NaN`. So at exactly
0 % UPower's percentage becomes NaN until the next non-zero sample. This is an upstream
edge case triggered by the missing `energy_full`; it cannot be fixed from userspace
without exposing `energy_full`/`charge_full` from the kernel (out of scope, see §6).

### Conclusion for config

**No udev rule and no UPower configuration are required or useful.** UPower enumerates
`/sys/class/power_supply` through libgudev, and every attribute its battery class needs
for level/state/voltage/temperature is already exported. Accordingly
`parity/patches/userspace/battery/` contains no udev/UPower files — adding one would be
a no-op at best and misleading at worst (`CHARGE_LIMIT`).

---

## 3. What userspace was actually missing

### 3.1 `capacity_max` restore/persistence — stock `lpm`

Evidence from the shipped ROM (`system.raw` = SYSTEM partition, 2206203904 bytes):

* `init.rc:1177` declares the battery daemon:
  `service lpm /system/bin/lpm` — `class sec-charger`, `critical`.
* `/bin/lpm` (inode 498, ELF32 ARM, Android API 23) contains
  `!@ main() : LPM Start!`, `LPMBattery`, and the sysfs templates
  `/sys/class/power_supply`, `%s/%s/capacity`, `%s/%s/status`, `%s/%s/health`,
  `%s/%s/present`, `%s/%s/voltage_now`, `%s/%s/batt_vol`, `%s/%s/temp`,
  `%s/%s/batt_temp`, `%s/%s/technology`, `%s/%s/charge_type`, `%s/%s/current_avg`,
  `%s/%s/hv_charger_status`, `%s/%s/batt_capacity_max`, `%s/%s/battery_cycle`,
  `%s/%s/time_to_full_now`, `%s/%s/batt_sw_self_discharging`, `%s/%s/camera_limit`,
  plus `!@initial Capacity max: %d`, `!@capacity_max %d, old_capacity_max %d`,
  `!@new_capacity_max %d`, and open/read/write/seek/sync errors for
  `/efs/Battery/batt_capacity_max`.
* The EFS partition exists and is a normal ext4 partition: stock
  `fstab.samsungexynos7580:7` mounts
  `/dev/block/platform/13540000.dwmmc0/by-name/EFS` on `/efs` (`ext4`, `wait,check`);
  the PIT entry is `EFS`, identifier 3, start block 24576, 40960 blocks (20 MiB).

So on Android the battery daemon restores the learned gauge calibration at boot and
writes the newly learned value back. **postmarketOS does not mount `/efs`**, so on Nura
the fuelgauge silently starts with the device-tree default
(`fuelgauge,capacity_max = <990>`, `dtsi:49-50`) every boot.

Why that matters: `sm5703_fg_get_scaled_capacity()` computes
`capacity = (raw_soc - capacity_min) * 1000 / (capacity_max - capacity_min)`
(`sm5703_fuelgauge.c:626-630`), and the scaling is active because
`fuelgauge,capacity_calculation_type = <28>`
(`0x1c` = `DYNAMIC_SCALE|ATOMIC|SKIP_ABNORMAL`, `dtsi:51`). With a learned
`capacity_max` of, say, 950, the DT default of 990 makes the phone report 96 % at a raw
SOC that the gauge considers full. Restoring the EFS value is therefore required for a
correct percentage, and it also shifts the point at which a charge limit triggers.

### 3.2 Charge limits

There is no `charge_control_end_threshold` in this kernel. The only userspace-writable
control that stops charging is `battery/batt_slate_mode`; everything else
(`charging_enabled`, `charge_otg_control`, …) is a read-only class attribute on this
build (`property_is_writeable` is not implemented by `sec_battery`/`sm5703_charger`).
`batt_slate_mode=1` is a real hardware stop, not a display trick:
`sec_battery.c:3917-3927` zeroes the charger current and clears `CHARGING_ENABLED`
(needed because `battery,always_enable` is set), and `cable_work` keeps the pack in
`Discharging` (`:3276-3290`). When released, `cable_work` re-evaluates the MUIC state and
charging resumes.

---

## 4. What is now provided

Package `j7elte-power` (`parity/pmaports/device/archived/j7elte-power/`):

| file | installed as | purpose |
|---|---|---|
| `j7elte-power.c` | `/usr/bin/j7elte-power` | charge-limit state machine + optional `capacity_max` persistence |
| `restore-capacity-max.sh` | `/usr/libexec/j7elte-power/restore-capacity-max` | boot restore of `/efs/Battery/batt_capacity_max` |
| `j7elte-power.conf` | `/etc/j7elte-power.conf` | configuration (also sourced by the restore script) |
| `j7elte-power.service` | `/usr/lib/systemd/system/…` | the daemon (opt-in), systemd images |
| `j7elte-efs-restore.service` | `/usr/lib/systemd/system/…` | oneshot restore, systemd images |
| `j7elte-power.initd` | `/etc/init.d/j7elte-power` | the daemon (opt-in), OpenRC images |
| `j7elte-efs-restore.initd` | `/etc/init.d/j7elte-efs-restore` | oneshot restore, OpenRC images |
| `j7elte-power.post-install` | abuild install script | enables the restore in the service manager present (OpenRC default, systemd fallback) |

Both init systems are covered because the pmaports baseline is an OpenRC port while
systemd images exist too; the sibling `j7elte-bt`/`j7elte-cbd`/`j7elte-gpsd` packages
follow the same pattern. The install script enables only the restore; the charge-limit
daemon stays opt-in on both (`rc-update add j7elte-power default` /
`systemctl enable --now j7elte-power`, after setting `charge_limit`). The package no
longer ships an `/etc/systemd/system/multi-user.target.wants` symlink: that path is
meaningless on OpenRC and is replaced by `.post-install`.

### 4.1 Boot restore (always on)

`j7elte-efs-restore.service` (`Type=oneshot`, `After=local-fs.target
systemd-udev-settle.service`, `ConditionPathExists=/sys/class/power_supply/battery/batt_capacity_max`)
or `j7elte-efs-restore` (`depend(){ need localmount; after udev-settle; }`) runs the
script. It:

1. returns immediately if `battery/batt_capacity_max` is missing (driver not probed);
2. if `/efs` is not already a mount point, resolves the partition from
   `/dev/disk/by-partlabel/EFS` (GPT label, created by udev; the same name as the PIT
   entry, see §6.5 for the hardware caveat) or the Android-style `by-name/EFS` path, and
   mounts it **read-only**;
3. parses `/efs/Battery/batt_capacity_max` (digits only — the ROM's format is a plain
   decimal, optionally with a newline);
4. refuses values outside `801..1199`, the same gate the kernel store uses, writes the
   value to `batt_capacity_max`, then reads it back and logs the effective value. Note
   the driver itself then clamps: `sm5703_fg_check_capacity_max()` keeps values in
   `[990-70-10, 990+70] = [910, 1060]`, mapping anything below 910 to 920 and anything
   above 1060 to 1060 (`sm5703_fuelgauge.c:664-687`);
5. unmounts `/efs` if it mounted it; always exits 0, so a missing/corrupt EFS cannot
   break boot or block the daemon.

`EFS_MOUNT`, `EFS_DEVICE` and a read-write mount can be overridden with
`J7ELTE_EFS_MOUNT`, `J7ELTE_EFS_DEVICE`, `J7ELTE_FORCE_RW`.

### 4.2 Charge limit (opt-in)

Set `charge_limit=80` (50..100) in `/etc/j7elte-power.conf` and enable the daemon:
`rc-update add j7elte-power default && rc-service j7elte-power start` (OpenRC, the
pmOS default here) or `systemctl enable --now j7elte-power` (systemd image). The daemon:

* reads `battery/capacity`, `battery/status` and the `online` of `ac`/`usb`/`wireless`;
* when plugged and `capacity >= charge_limit`: writes `batt_slate_mode=1` and logs it;
* when `capacity <= charge_limit - resume_margin` (1..20, default 5): writes
  `batt_slate_mode=0`;
* if the battery driver re-enables charging while above the limit (temperature/health
  recovery paths call `sec_bat_set_charge(true)`), re-applies the latch, rate-limited to
  one write per 30 s;
* is event driven: it subscribes to the kernel uevent netlink group and filters
  `SUBSYSTEM=power_supply`; a monotonic-clock safety poll (default 60 s) covers a missed
  uevent and unrelated-uevent storms;
* never writes `batt_slate_mode` when limiting is disabled. With limiting enabled it
  adopts (rather than clears) a latch left behind by a previous instance so the resume
  path can release it; it never clears a foreign `batt_slate_mode` when
  `charge_limit=0`;
* on SIGTERM/SIGINT releases the latch (so stopping the service cannot leave the phone
  unable to charge) and exits 0. With both features disabled it exits immediately
  instead of idling.

`persist_capacity_max=1` (default 0) additionally writes the value the fuelgauge learned
at runtime back to the EFS file, exactly what `lpm` does — atomically
(`tmp`+`fsync`+`rename`+dir `fsync`), only when the value changed and only when `/efs` is
mounted read-write. The aging algorithm itself is inside the closed-source `lpm` and is
not reproduced; the kernel's own dynamic-scale learning is already active
(`DYNAMIC_SCALE` bit), so only the persistence step was missing. It is off by default
because it is the only code path that writes to the EFS partition.

---

## 5. Validation (no hardware)

Re-run on 2026-10-24 after the second review; every item below was executed in this
session.

* **Compilation** — `-std=c11 -Wall -Wextra -Werror`:
  * host x86-64 GCC 16.2.1: clean;
  * `toolchains/gcc-arm-10.3-…/aarch64-none-linux-gnu-gcc` (the same GCC 10.3 line as
    the kernel build) `-static`: clean, ELF aarch64;
  * host `aarch64-linux-gnu-gcc` 16.1.0 `-static`: clean, ELF aarch64;
  * **aarch64 musl** — `aarch64-linux-musl-gcc` 11.2.1 (musl.cc
    `aarch64-linux-musl-cross`), the ABI the APKBUILD actually targets:
    * `-static` → `ELF 64-bit LSB pie executable, ARM aarch64, static-pie linked`;
    * default dynamic link → `interpreter /lib/ld-musl-aarch64.so.1`,
      `NEEDED libc.so`;
    * both clean under the same `-Werror`. No API used by the daemon is glibc-only
      (`fopen(..., "re")` is accepted by musl; `clock_gettime`, `SOCK_CLOEXEC`,
      `O_DIRECTORY`, `NETLINK_KOBJECT_UEVENT` are all available).
* **Packaging** — the APKBUILD's `build()`/`package()` were executed in a copy of the
  package directory (so no binary lands in the source tree); `sha512sum -c` on the
  APKBUILD sums passes for all eight sources. The installed tree contains exactly
  `/usr/bin/j7elte-power`, `/etc/j7elte-power.conf`,
  `/usr/libexec/j7elte-power/restore-capacity-max`, `/etc/init.d/j7elte-efs-restore`,
  `/etc/init.d/j7elte-power`, both systemd units, and the abuild install script; there
  is no `/etc/systemd/system` `.wants` tree any more.
* **Behaviour** — `parity/userspace/battery/test-j7elte-power.sh` drives the daemon
  against a fake `power_supply` tree (**12 checks**):
  latch at 85 % with limit 80; resume at 70 %; release on SIGTERM; release of a stale
  latch left by a killed instance; no write when limiting is disabled; persistence of a
  learned value; no rewrite when unchanged; no persistence of an out-of-range value;
  `charge_limit=120` rejected; unknown config key reported; missing config falls back to
  defaults; missing battery device exits 1. The same 12 checks pass against the host
  binary, the aarch64 musl static binary, and the aarch64 musl **dynamic** binary under
  `qemu-aarch64` (the dynamic run needs a prefix containing
  `lib/ld-musl-aarch64.so.1`; a symlink to the host `/lib/libc.so` from the musl.cc
  toolchain does not work). QEMU verifies instruction-set and libc behaviour only — it
  does not exercise the sec_battery driver.
* **Uevent parsing** — a netlink kobject uevent payload is a sequence of
  **NUL-terminated** `key=value` fields, so the original `strstr()` stopped at the first
  NUL and never matched; `uevent_is_power_supply()` walks the fields over the received
  length. `build-check.sh` compiles the real source with the parser under test and
  asserts that the NUL-separated power_supply message matches, a `SUBSYSTEM=usb` message
  does not, and a truncated payload behaves correctly.
* **Restore script** — `build-check.sh` now exercises it in six cases without mounting
  (`J7ELTE_EFS_MOUNT=/proc`, which is already a mount point, plus a fixture for
  `/efs/Battery/batt_capacity_max`): valid value applied and read back; out-of-range
  `1500` refused; non-numeric file ignored; absent file ignored; missing
  `battery/batt_capacity_max` skipped; unresolvable EFS device skipped (mount fails).
  Every case exits 0 and leaves the attribute untouched on the failure paths.
* **Config / service syntax** — `sh -n` on `restore-capacity-max.sh`, both `.initd`
  scripts and `.post-install`; `systemd-analyze verify` on both units reports only the
  two executables that live in the package and are therefore absent on the build host
  (no parse or dependency errors). `openrc-run` is not installed on the build host, so
  the `.initd` files were only syntax-checked, not run.
* **Patches** — verified on a freshly exported pristine `ba682bd` tree (`git archive`,
  temporary repo):
  * `0001-pmaports-add-j7elte-power-package.patch` applies cleanly (no offsets) and
    creates exactly the nine package files with the right modes (`0755` for the two
    init scripts, the restore helper and the install script);
  * `0002-device-samsung-j7elte-depend-on-j7elte-power.patch` applies cleanly (no
    offsets) and adds `j7elte-power` to `depends`;
  * `diff -r` between the patched `device/archived/j7elte-power` and the working
    package directory is empty (byte-identical);
  * `0002` also applies on a tree with
    `0004-device-samsung-j7elte-depend-firmware.patch` applied.
* **Interface review (re-checked against the v3.10 sources)** — the daemon opens only
  `battery/{capacity,status,batt_capacity_max,batt_slate_mode}` and
  `{ac,usb,wireless}/online`.
  * `battery` psy names: `psy_bat.name = "battery"` (`sec_battery.c:6431`),
    `"usb"`/`"ac"`/`"wireless"` immediately after; the DT supplies
    `battery,charger_name = "sm5703-charger"` and `"sm5703-fuelgauge"`
    (`exynos7580-j7elte_battery.dtsi:84-85`).
  * `capacity` returns integer percent (`sec_bat_get_property`, `sec_battery.c:4716+`);
    `status` returns the standard strings (`"Charging"` etc.); the daemon compares
    `"Charging"` exactly.
  * `batt_slate_mode` is created unconditionally by `SEC_BATTERY_ATTR()`
    (`sec_battery.c:36`); the store accepts exactly `0`/`1`, sets
    `battery->slate_mode`, queues `cable_work`, and on `1` zeroes the charger current
    and clears `CHARGING_ENABLED` when `battery,always_enable` is set
    (`sec_battery.c:3900-3930`; DT `dtsi:180`). Probe initialises it to 0
    (`sec_battery.c:6393`).
  * `batt_capacity_max` (`sec_battery.c:82`, `#if !defined(CONFIG_DISABLE_SAVE_CAPACITY_MAX)`;
    config has it unset) reads the fuelgauge's `ENERGY_FULL_DESIGN`
    (`:3733-3737`) and its store forwards only `800 < x < 1200` (`:4294-4313`). The
    fuelgauge clamps to `[capacity_max - margin - 10, capacity_max + margin]`, mapping
    below `910` to `920` and above `1060` to `1060` with the j7elte DT
    (`sm5703_fuelgauge.c:664-687`); the DT values are `capacity_max=990`,
    `margin=70`, `capacity_min=0`, `capacity_calculation_type=28` (`dtsi:49-52`).
  * `CONFIG_BATTERY_SAMSUNG=y` builds `sec_battery.o` because `CONFIG_CAMERA_S5NEO` is
    not set (`drivers/battery/Makefile:1-5`, `config-nura-j7elte:2174,3384`), so the
    attribute set above is the one that exists on this device — not the `s5neo`
    variant.
  * The `capacity_max` store is additionally gated on `!fg_reset`; `fg_reset` is the
    global set only by the `fg_reset=` kernel command line
    (`drivers/staging/samsung/sec_batt.c:34-43`), so it is 0 on a normal boot. If a
    bootloader ever passed `fg_reset=1`, the restore would be silently ignored — the
    script logs the read-back value, which exposes that case.
* **Daemon bug found by this review and fixed** — the latch state was process-local and
  the service restarts on failure (`Restart=on-failure`) or can be restarted manually.
  After a SIGKILL (or crash) while latched, `batt_slate_mode` stays 1, the new instance
  did not know it owned the latch and would never clear it, leaving the phone unable to
  charge until a reboot (probe resets the flag). The daemon now reads
  `batt_slate_mode` at startup **only when `charge_limit > 0`** and adopts an existing
  latch, so the normal resume path releases it; with limiting disabled the attribute is
  still never read for state nor written. Test 3b covers this.
* **Reproduce everything**: `parity/userspace/battery/build-check.sh`. It runs the host
  build, the GCC 10.3 and host aarch64 cross builds, the optional aarch64 musl build and
  qemu functional test, the uevent parser test, APKBUILD checksums, the
  `build()`/`package()` tree check, service/script syntax, the restore-script cases and
  the full functional test.

### No fake paths

Every sysfs path used by the code is either a class attribute present in the driver's
`properties[]` or a custom `SEC_BATTERY_ATTR` (table above). The only conditional paths
are the EFS device candidates, which are used only if they exist, and `/efs` (stock
fstab path). No mainline-only attribute is assumed; in particular the daemon does not
reference `charge_control_*_threshold`, `energy_full` or `charge_full`, which this
kernel does not export.

---

## 6. Residual risks / on-device checks

1. **No on-device runtime test.** All execution was off-device; `qemu-aarch64` proves
   the aarch64/musl binaries and daemon logic but cannot emulate the SM5703, MUIC or
   their sysfs. On a device, verify:
   `cat /sys/class/power_supply/battery/{capacity,status,present,temp,voltage_now}`,
   `cat /sys/class/power_supply/battery/batt_capacity_max`, then start the restore
   (`rc-service j7elte-efs-restore start` or `systemctl start j7elte-efs-restore`) and
   check the log/kmsg shows the restored value (proves the EFS mount path), and
   `upower -d` shows the battery with the expected percentage and state. Then test the
   limit: with `charge_limit=80`, cross 80 % on the charger and confirm
   `batt_slate_mode` reads 1, `status` is `Discharging` and charge current drops; drop
   below 75 % and confirm it returns to 0.
2. **`batt_slate_mode` side effects while latched**: status is `Discharging` and
   `ac`/`usb` online are 0 although the cable is attached; OTG is disabled by the same
   flag. This is the kernel's own semantics, documented in the config file.
3. **SIGKILL while latched** (the release runs on SIGTERM/INT only) now self-heals on a
   service restart: a new instance with `charge_limit > 0` adopts an existing latch and
   releases it at the resume point. Two cases still need a manual `echo 0 >
   /sys/class/power_supply/battery/batt_slate_mode` or a reboot (probe resets
   `slate_mode`, `sec_battery.c:6393`): the daemon is not restarted at all, or it was
   running with `charge_limit=0` when the latch was set by something else.
4. **Persistence writes to EFS** and is off by default. If enabled, a corrupted write
   would be limited to `/efs/Battery/batt_capacity_max` by the atomic replace, but the
   EFS partition also holds modem NV data; keep it disabled unless the value is needed
   across reboots. It also only works while `/efs` is mounted read-write.
5. **EFS discovery is inferred, not hardware-verified.** The script tries
   `/dev/disk/by-partlabel/EFS` (GPT label; the bootloader/PIT names the partition EFS
   and `CONFIG_EFI_PARTITION=y` makes the kernel expose the GPT) and then the
   Android-style `by-name/EFS`. If neither exists the restore is skipped (logged) and
   the DT default 990 applies, so the SOC offset from §3.1 is back. Confirm the udev
   link on a device before relying on the calibration.
6. **The restore runs once per boot.** If the kernel command line ever contains
   `fg_reset=1` (`drivers/staging/samsung/sec_batt.c:34-43`), the `batt_capacity_max`
   store is a no-op (`sec_battery.c:4295`); the logged read-back value reveals it, but
   nothing retries. Normal boots have `fg_reset=0`.
7. **Charge limits above the driver's full threshold are pointless.** With
   `battery,full_condition_soc = <93>` (`dtsi:168`), `sec_battery` reports `Full` and
   stops charging at ~93 % anyway; a configured limit of 94..100 therefore never
   triggers the latch, but the kernel still stops charging. Limits up to 93 % behave as
   configured.
8. **OpenRC scripts are syntax-checked only.** `openrc-run` is not installed on the
   build host, so `j7elte-power.initd`/`j7elte-efs-restore.initd` were validated with
   `sh -n` plus review against the sibling packages, not executed.
9. **UPower energy/time/health remain unavailable** (§2): `energy_full`,
   `charge_full`, `charge_full_design`, `energy_full_design`, `cycle_count` are not
   exported by `sec_battery`. Fixing that requires a kernel change (add those
   properties to `sec_battery_props[]` and return real values) — out of scope for this
   userspace gap, and the parity report found no kernel change necessary. Percentage,
   state, voltage and temperature are correct without it.
10. **The exact-0 % NaN** (§2) is an upstream UPower consequence of (9).
11. **Duplicate line-power devices** from `sm5703-charger`/`ps`/`otg` are cosmetic; no
    userspace filter exists.
12. **`CONFIG_CALC_TIME_TO_FULL` is off**, so even tools that read `time_to_full_now`
    find nothing. Turning it on is a kernel config decision, not part of this gap.

---

## 7. Deliverables

```
parity/pmaports/device/archived/j7elte-power/
    APKBUILD
    j7elte-power.c
    j7elte-power.conf
    j7elte-power.service
    j7elte-efs-restore.service
    j7elte-power.initd
    j7elte-efs-restore.initd
    j7elte-power.post-install
    restore-capacity-max.sh

parity/patches/userspace/battery/
    0001-pmaports-add-j7elte-power-package.patch        (new package, all nine files)
    0002-device-samsung-j7elte-depend-on-j7elte-power.patch

parity/userspace/battery/
    test-j7elte-power.sh     (12 functional checks against a fake sysfs tree)
    build-check.sh           (host+cross/musl builds, checksums, package tree,
                              service/script syntax, restore cases, tests)
parity/reports/battery-userspace.md
```


Apply order: `patches/pmaports/0003`–`0005`, then `0001`, then `0002`. Both 0001 and
0002 also apply to the pristine baseline (0002's context is independent of the firmware
dependency line).

No shared file was modified: `device-samsung-j7elte/APKBUILD` and
`linux-samsung-j7elte/config-*` are untouched; the dependency is supplied as patch
0002 only. The kernel tree was not modified.
