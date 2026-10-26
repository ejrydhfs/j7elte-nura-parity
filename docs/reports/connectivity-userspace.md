# Wi-Fi / Bluetooth userspace bring-up — j7elte (Nura / postmarketOS)

Gap: `connectivity-userspace`.  Author: connectivity agent.  Date: 2026-10-24.

**Scope and limitation:** static verification and cross-compilation-free validation only.
No j7elte hardware, no Nura rootfs image and no aarch64 Alpine chroot were available in
this workspace, so nothing here was executed on the target.  Every device name, path and
package name below is traced to a source file, a config line or a package manifest; the
runtime expectations are marked as such.

The kernel-side work already exists and was **not** modified: `CONFIG_BCM4343=y` with the
firmware-path patch (`parity/patches/wifi/0001-…`), `CONFIG_BT_HCIUART(_H4)=y`
(`parity/patches/bluetooth/0002-…`), the BT pinctrl fix (`…/0001-j7elte-bt-pinctrl.patch`)
and `firmware-samsung-j7elte` (blobs at
`/lib/firmware/postmarketos/samsung-j7elte/`).  This report covers only the userspace
parts needed for the interfaces to come up.

---

## Verdict

| Item | Result |
|---|---|
| Wi-Fi interface name | **wlan0**, verified from the built-in driver (see W1) |
| Wi-Fi userspace config | `wpa_supplicant.conf` template (no network) + `/efs/wifi` MAC-state provisioner; **no udev rule needed** |
| BT UART | **/dev/ttySAC3**, verified from `BT_UPORT 3` + DT `uart3` alias (B1) |
| BT firmware name/path | **`/lib/firmware/postmarketos/samsung-j7elte/BCM43438A1.hcd`**, present in `firmware.tar.gz` (B2) |
| BT userspace | `j7elte-bt` OpenRC service + systemd unit: rfkill unblock → `hciattach … bcm43xx` → `bluetoothd`; disable flag file |
| Packages | `j7elte-wifi`, `j7elte-bt` under `parity/pmaports/device/archived/` + apply-checked patches |
| Hardware test | **not performed** (no device/rootfs); stated in every risk item |

---

## W1 — Wi-Fi interface and driver state

Evidence chain for `wlan0` (kernel tree `android_kernel_samsung_universal7580-…`):

1. `drivers/net/wireless/bcmdhd_1_77_nn/wl_android_init()` sets the interface-name seed
   when the `iface_name` module parameter is empty:
   `bcm_strncpy_s(iface_name, IFNAMSIZ, "wlan", IFNAMSIZ)` (`wl_android.c:6634-6645`);
   called from the platform-device registration path (`dhd_linux_platdev.c:831`).
2. `dhd_attach()` appends `%d` because the last character is not a digit
   (`dhd_linux.c:8793-8802`) and passes the result to `dhd_allocate_if(..., 0, if_name …)`
   (`dhd_linux.c:8805`), which copies it over the `alloc_etherdev()` default name
   (`dhd_linux.c:8322-8325`).  Result: `wlan0`.
3. `dhd_sdio.c:7783-7810`: with `dhd_download_fw_on_driverload == FALSE` the driver still
   calls `dhd_register_if(bus->dhd, 0, TRUE)` for the primary interface, i.e. **wlan0 is
   registered at driver attach (boot) even before firmware is downloaded**.
4. The driver is built in: config line 1373 `CONFIG_BCM4343=y`; the Makefile's
   `CONFIG_BCM4343` block adds `-DENABLE_INSMOD_NO_FW_LOAD -DUSE_LATE_INITCALL_SYNC` and
   sets `DRIVER_TYPE = y` (`bcmdhd_1_77_nn/Makefile:934-960`).  `wl_android_init()` then
   sets `dhd_download_fw_on_driverload = FALSE` (`wl_android.c:6638-6641`), so the firmware
   is fetched the first time the netdev is opened (`dhd_open()` →
   `dhd_bus_start()` at `dhd_linux.c:8016-8020`).  Any network manager bringing wlan0 up
   triggers the download; no userspace helper is required.

Firmware paths (already patched by the Wi-Fi parity work, verified in the working tree):

* config 1382: `CONFIG_BCMDHD_FW_PATH="/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin"`
* config 1383: `CONFIG_BCMDHD_NVRAM_PATH="/lib/firmware/postmarketos/samsung-j7elte/nvram_net.txt"`
* `firmware-samsung-j7elte/APKBUILD` installs exactly those two files (plus apsta/mfg)
  into `$_fw/postmarketos/samsung-j7elte` with `_fw="$pkgdir/lib/firmware"`.

The driver opens them with `filp_open()`/`kernel_read()` on the absolute path, so the
directory must exist verbatim; the package provides it (verified from the package
`package()` function, and the two files are present in `firmware.tar.gz`).

### Why no udev rule / rfkill step for Wi-Fi

* cfg80211 registers one `RFKILL_TYPE_WLAN` switch per wiphy
  (`net/wireless/core.c:378-382`, `rfkill_register` at `:640`), and the kernel default is
  radio-on: `rfkill_default_state = 1` ("0 = radio off",
  `net/rfkill/core.c:116-118`).  A fresh boot therefore has Wi-Fi unblocked; only an
  explicit user soft-block needs `rfkill unblock wifi`.
* The interface name is deterministic (`wlan0`) and already what every tool expects, so a
  rename rule would be counter-productive.  The only device-specific naming escape hatch
  is the driver's `iface_name` module parameter, which is unnecessary here.

Consequently `j7elte-wifi` ships a **config** and an image/boot-time provisioning step,
not a udev rule.

### The `/efs/wifi` MAC problem (fixed here)

`CONFIG_WIFI_BROADCOM_COB=y` makes `dhd_sec_feature.h:68-72` define `READ_MACADDR`, which
in turn defines `DHD_USE_CISINFO` (`dhd.h:2055-2060`).  The driver then reads
`/efs/wifi/.mac.info` (`dhd_custom_cis.c:54-58`, `:292-335`) and, when the file is
missing, generates a MAC and writes it back with `filp_open(..., O_RDWR|O_CREAT, 0664)`
(`dhd_linux.c:18106-18140`).  If the **directory** does not exist the write fails and
`dhd_set_macaddr_from_file()` returns before applying the generated address, so the shared
NVRAM MAC `00:90:4c:c5:12:38` is used on every device (the residual risk R2 of
`parity/reports/wifi.md`).

`j7elte-wifi` therefore:

* ships the empty directory `/efs/wifi` (mode 0755) in the package and also creates it
  from `post-install` (so a package installed on a running system is covered), and
* ships an OpenRC oneshot service `j7elte-wifi` (and a systemd-tmpfiles drop-in
  `d /efs/wifi 0755 root root -`) that re-creates it at every boot if it is missing,
  **before** `wpa_supplicant`/`NetworkManager`.

The service deliberately does nothing when `/efs` is a mount point (a real EFS
partition), and it never writes a MAC itself — the driver does that on first wlan0
open.  The resulting address uses the driver's own generated prefix `00:12:34` plus three
random bytes; it is unique per device but is not the factory MAC (which lives in the EFS
partition that Nura does not mount).

### The supplicant config

`/etc/wpa_supplicant/wpa_supplicant.conf` is a template with no `network=` block
(`ctrl_interface`, `update_config`, `ap_scan` only).  It is not shipped by Alpine's
`wpa_supplicant` package (verified in `main/wpa_supplicant/APKBUILD`: only the example
under `/usr/share/doc`), so there is no file conflict, and Alpine's OpenRC
`wpa_supplicant` service uses exactly this path when it exists.  On NetworkManager
images the file is inert; postmarketOS enables NetworkManager by default (pmaports
`main/postmarketos-base` post-install, "Enable NetworkManager by default"), and NM runs
its own supplicant over D-Bus.  The regulatory domain is deliberately not set: NVRAM
already carries `ccode=GB` (wifi parity report, P4/R6), and forcing `country=` could
restrict the channel set.

---

## B1 — Bluetooth UART device

* Kernel driver: `drivers/bluetooth/bcm43xx.c:43` `#define BT_UPORT 3`; the same file
  indexes `s3c2410_serial_wake_peer[BT_UPORT]` (`:45-47`, `:246`).
* DT: `aliases { uart3 = &serial_3; }` (`arch/arm64/boot/dts/exynos7580.dtsi:65`),
  `serial_3: uart@11060000` (`exynos7580.dtsi:539`), enabled in the board DTS
  (`exynos7580-j7elte_rev00.dts:1218-1221`, same in rev04/rev06).
* Serial driver: port name prefix `"ttySAC"` (`drivers/tty/serial/samsung.c:86`), port
  index from `of_alias_get_id(…, "uart")` (`:1916`), so uart3 → port 3 → **/dev/ttySAC3**.
* Config: `CONFIG_SERIAL_SAMSUNG=y` (1596), `CONFIG_SERIAL_SAMSUNG_UARTS=4` (1599),
  `CONFIG_DEVTMPFS=y` (964) → the node appears without a static /dev entry.
* The driver's rfkill switch is registered and then forced soft-blocked:
  `rfkill_init_sw_state(bt_rfkill, 0)` … `rfkill_register()` …
  `rfkill_set_sw_state(bt_rfkill, true)` (`bcm43xx.c:329-342`).  The unblock callback
  drives `BT_EN` high (`bcm43xx_bt_rfkill_set_power`, `bcm43xx.c:82-112`), so
  `rfkill unblock bluetooth` is a required, ordered first step.

## B2 — Bluetooth firmware and the attach tool

* `firmware-samsung-j7elte/APKBUILD:30-31` installs the ROM blob as
  `/lib/firmware/postmarketos/samsung-j7elte/BCM43438A1.hcd` (and under the ROM name).
  `firmware.tar.gz` contains `bcm43438A1_V0051.0230.hcd`; its sha512
  `5d563e9d…c8440365` matches `parity/firmware/MANIFEST.sha512` and
  `parity/firmware/files-bt.list`.
* BlueZ's `tools/hciattach_bcm43xx.c` (5.87) resets the chip, reads the controller's local
  name, then `bcm43xx_locate_patch(FIRMWARE_DIR, chip_name, …)` recursively scans for a
  file whose name starts with the local name and ends in `.hcd`; it then downloads it.
  Alpine patch `004-Move-the-43xx-firmware-into-lib-firmware.patch` changes
  `FIRMWARE_DIR` from `/etc/firmware` to **`/lib/firmware`**, so the recursive scan reaches
  `postmarketos/samsung-j7elte/BCM43438A1.hcd`.
* `hciattach` ships in **`bluez-deprecated`** (`/usr/bin/hciattach`, verified via the
  Alpine package index and the `bluez` APKBUILD `deprecated()` subpackage); the OpenRC
  service is in **`bluez-openrc`** (`/etc/init.d/bluetooth`, verified via the package
  index); `/usr/sbin/rfkill` is in **`util-linux-misc`**.
* Command-line semantics (BlueZ 5.87 `tools/hciattach.c`): the `bcm43xx` table entry is
  `115200, 3000000, FLOW_CTL, DISABLE_PM` (`:1079-1080`); `-s` overrides the initial
  speed; `init_uart()` — including the patch-RAM download — runs **before** the process
  detaches, so the exit status is meaningful.  With `-n` the process stays in the
  foreground, which is what the OpenRC/systemd unit supervises; `hci0` exists only while
  that process holds the `N_HCI` line discipline.
* Plain `btattach` is intentionally not used: this 3.10 kernel has no `hci_bcm`/`btbcm`
  in-kernel firmware loader (bluetooth parity report, P4), so `btattach` alone never
  downloads the patch RAM.

---

## Deliverables

### `parity/pmaports/device/archived/j7elte-wifi/` (arch `noarch`)

| File | Installed as | Purpose |
|---|---|---|
| `APKBUILD` | — | package; `depends="wpa_supplicant"` |
| `wpa_supplicant.conf` | `/etc/wpa_supplicant/wpa_supplicant.conf` | network-free template |
| `j7elte-wifi.initd` | `/etc/init.d/j7elte-wifi` | OpenRC oneshot: create `/efs/wifi` before supplicant/NM; skips if `/efs` is a mount point |
| `j7elte-wifi.confd` | `/etc/conf.d/j7elte-wifi` | `J7ELTE_WIFI_MAC_STATE_DIR` |
| `j7elte-wifi.tmpfiles` | `/usr/lib/tmpfiles.d/j7elte-wifi.conf` | systemd equivalent |
| `j7elte-wifi.post-install` | — | `mkdir -p /efs/wifi`; `rc-update add j7elte-wifi default` |
| package `package()` | `/efs/wifi` (dir 0755) | lets bcmdhd persist its generated MAC |

### `parity/pmaports/device/archived/j7elte-bt/` (arch `noarch`)

| File | Installed as | Purpose |
|---|---|---|
| `APKBUILD` | — | `depends="bluez bluez-deprecated bluez-openrc util-linux-misc"` |
| `j7elte-bt.initd` | `/etc/init.d/j7elte-bt` | unblock rfkill → `hciattach -n -t 30 -s 115200 /dev/ttySAC3 bcm43xx 115200 flow` with retries (each attempt waits up to `J7ELTE_BT_TIMEOUT` s for `hci0`) → `rc-service bluetooth start`; ordered `before bluetooth`, `need localmount` |
| `j7elte-bt.confd` | `/etc/conf.d/j7elte-bt` | UART, type, speeds, timeout, retries (plain `KEY=value`, readable by OpenRC and systemd) |
| `j7elte-bt.service` | `/usr/lib/systemd/system/j7elte-bt.service` | same steps for systemd images (`EnvironmentFile`, `rfkill` in `ExecStartPre`, `-n` foreground attach) |
| `j7elte-bt.post-install` | — | enable in default runlevel (OpenRC) or `systemctl enable` (systemd) |

Disable mechanism: create `/etc/j7elte-bt.disabled` (checked by both service managers;
the OpenRC service then exits successfully without attaching) and/or
`rc-update del j7elte-bt default` / `systemctl disable j7elte-bt`.

Balanced line speed: 115200 is the default (`J7ELTE_BT_SPEED`); set it to `3000000` after
`hciconfig -a` proves a stable link — Alpine patch 002 resets the UART speed after the
firmware download, and the BCM43438 supports 3 Mbit/s.

### Patches (`parity/patches/userspace/connectivity/`)

| Patch | Contents | Verification |
|---|---|---|
| `0001-j7elte-wifi-package.patch` | adds the six files above under `device/archived/j7elte-wifi/` | `git apply --check` against pmaports baseline `ba682bd` → **APPLY_OK** |
| `0002-j7elte-bt-package.patch` | adds the five files above under `device/archived/j7elte-bt/` | same → **APPLY_OK** |

Canonical source copies are kept under `parity/userspace/connectivity/{wifi,bt}/`; the
package directories contain byte-identical copies (checksums in the APKBUILDs verify
this).

### Integration still required (parent-owned file, intentionally not edited)

`device/archived/device-samsung-j7elte/APKBUILD` must pull the two packages in, e.g.:

```
depends="
	linux-samsung-j7elte
	firmware-samsung-j7elte
	j7elte-wifi
	j7elte-bt
	mkbootimg
	postmarketos-base
"
```

The file is shared and already modified by the integration branch, so no patch for it is
included here.  Without those two lines the packages are built but not installed.

---

## Validation performed

All commands were run in this workspace; nothing was run on the target.

| Check | Command | Result |
|---|---|---|
| Shell syntax | `sh -n` on both APKBUILDs, both init scripts, both conf.d, both post-install | **OK** |
| Shell lint | `shellcheck -s sh` on both init scripts and both post-install (v0.11.0) | **clean** (SC2034 for OpenRC `description*` variables silenced inline) |
| APKBUILD checksums | `sha512sum -c` over each `sha512sums` block | **all OK** |
| APKBUILD source list | every `source=` entry exists in the package dir | **OK** |
| Patch applicability | `git apply --check` in an extracted `ba682bd` baseline tree | **APPLY_OK** for both |
| wpa_supplicant config | `wpa_supplicant -c <file>` (parse-only, no interface) | **Parsing succeeded**, rc 0; a deliberately broken control file returns “Parsing failed”, rc 255, proving the test is sensitive |
| systemd unit | `systemd-analyze verify j7elte-bt.service` | only expected host-only warning: `/usr/bin/hciattach` is not installed on the build host; no unit syntax/ordering errors |
| BT service logic | simulated OpenRC run with stubbed `start-stop-daemon`, `hciattach`, `rfkill`, `rc-service` and `J7ELTE_BT_TIMEOUT=2` | success path: rfkill → hciattach (`-n -t 30 -s 115200 /dev/ttySAC3 bcm43xx 115200 flow`) → `rc-service bluetooth start`, rc 0, pidfile + hci0 present; failure path: 2 attempts, rc 1 with both error lines; disabled flag: rc 0, no commands invoked; missing UART: rc 1 before rfkill; rfkill failure: rc 1, no hciattach; stop: pidfile removed, rc 0 |
| Wi-Fi provisioner | simulated run with `J7ELTE_WIFI_MAC_STATE_DIR` in /tmp and a fake `/proc/mounts` | creates the directory, idempotent on second run, skips when `/efs` is mounted |
| Firmware cross-check | `tar tzf firmware.tar.gz`; `sha512sum` of the extracted HCD vs `MANIFEST.sha512`; path in `firmware-samsung-j7elte/APKBUILD` vs `CONFIG_BCMDHD_*` | blob present, hash matches, install path equals the configured Wi-Fi paths; BT HCD install path is under the recursive `/lib/firmware` scan root |
| Kernel prerequisites | grep of the pmaports kernel config | `CONFIG_BT=y`, `CONFIG_BT_HCIUART=y`, `CONFIG_BT_HCIUART_H4=y`, `CONFIG_RFKILL=y`, `CONFIG_SERIAL_SAMSUNG=y`, `CONFIG_SERIAL_SAMSUNG_UARTS=4`, `CONFIG_DEVTMPFS=y`, `CONFIG_WLAN=y`, `CONFIG_CFG80211=y`, `CONFIG_BCM4343=y` |

Not performed: `abuild`/`apk` build (no Alpine chroot here), kernel build, and any
runtime test (`ip link`, `iw`, `hciconfig`, `bluetoothctl`) — no hardware.

---

## Residual risks

* **R1 — no hardware test.**  The exact `Read Local Name` string is only inferred.  The
  blob reports `BCM43438A1` and `hciattach` matches `<local name>*.hcd`
  case-sensitively; if the controller returns a name that is not a prefix of
  `BCM43438A1.hcd`, hciattach logs “Patch not found, continue anyway” and the link comes
  up unpatched.  Runtime action: read the name from the hciattach log and add an
  exact-name symlink in `/lib/firmware/postmarketos/samsung-j7elte/`.
* **R2 — line speed.**  The ROM vendor library did not reveal its baud rate.  115200 is
  the safe default; test 3000000 only after hci0 enumerates reliably.
* **R3 — init-system choice.**  The dependency set targets OpenRC (postmarketOS default
  and the only realistic option for this archived 3.10 device): `bluez-openrc` supplies
  `/etc/init.d/bluetooth`.  A systemd image must use `bluez-systemd` instead of
  `bluez-openrc`; the shipped `j7elte-bt.service` covers the service-manager side, but the
  `depends=` line would have to be adjusted by the integrator.
* **R4 — MAC semantics.**  `/efs/wifi` changes the effective MAC from the shared NVRAM
  address to a driver-generated `00:12:34:xx:xx:xx` address persisted in the rootfs.
  This is intentional (fixes duplicate MACs), but it is not the factory MAC; if Nura ever
  mounts the real EFS partition, the init service skips and the driver will use whatever
  that partition contains.  A read-only rootfs would regenerate the MAC every boot.
* **R5 — rfkill state persistence.**  `util-linux-openrc` ships an `rfkill` service that
  restores the saved soft-block state at boot; `j7elte-bt` runs later
  (`after udev-settle`) and unconditionally unblocks Bluetooth, so a saved block does not
  prevent attach — but it also means the service overrides a user's “Bluetooth off”
  preference at every boot while enabled.  Disable the service (flag file) to respect it.
* **R6 — network manager.**  No NetworkManager drop-in is shipped: the driver needs no
  device-specific NM configuration, and NM already manages rfkill and brings wlan0 up
  (which triggers the firmware load).  The wpa_supplicant template is for images without
  NM; running it alongside NM is not useful.
* **R7 — firmware path assumption.**  `/lib/firmware` is assumed to be the kernel
  firmware root on Nura (it is on Alpine/postmarketOS); the bcmdhd paths are
  absolute VFS opens, so they only require the directory to exist verbatim.  No rootfs
  image was available to confirm the directory.

## File inventory

```
parity/pmaports/device/archived/j7elte-wifi/{APKBUILD,wpa_supplicant.conf,
    j7elte-wifi.initd,j7elte-wifi.confd,j7elte-wifi.tmpfiles,j7elte-wifi.post-install}
parity/pmaports/device/archived/j7elte-bt/{APKBUILD,j7elte-bt.initd,
    j7elte-bt.confd,j7elte-bt.service,j7elte-bt.post-install}
parity/userspace/connectivity/wifi/{wpa_supplicant.conf,j7elte-wifi.initd,
    j7elte-wifi.confd,j7elte-wifi.tmpfiles,j7elte-wifi.post-install}
parity/userspace/connectivity/bt/{j7elte-bt.initd,j7elte-bt.confd,
    j7elte-bt.service,j7elte-bt.post-install}
parity/patches/userspace/connectivity/{0001-j7elte-wifi-package.patch,
    0002-j7elte-bt-package.patch}
parity/reports/connectivity-userspace.md
```
