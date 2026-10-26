# Review: j7elte userspace packages — Wi-Fi, Bluetooth, FDE, camera

Reviewer: `review-userspace-b`.  Date: 2026-10-24.  Baseline: pmaports `ba682bd`.

Scope: independent verification of `device/archived/j7elte-{wifi,bt,fde,cam}` and of
their reports (`connectivity-userspace.md`, `fde-userspace.md`,
`camera-userspace.md`).  Every claim below was re-derived from a primary source in
this workspace (kernel tree, device config, Alpine/pmaports sources fetched at the
exact versions the packages depend on) or from an executed check; nothing is taken
on trust from the reports.  Three package defects were found and fixed; one
cross-cutting integration defect was found that is **not** fixed here (shared
kernel artefacts, see §6).

No hardware, no aarch64 rootfs and no `abuild`/`apk` in this environment, so this is
static verification plus host/aarch64-GCC builds.  No runtime success is claimed.

## 1. Verdict

| Package | Verdict | Defects |
|---|---|---|
| `j7elte-wifi` | **pass** | none |
| `j7elte-bt` | **pass** (1 defect fixed) | systemd unit started `bluetoothd` **before** `hciattach` (contradicted the OpenRC service and the report) — fixed |
| `j7elte-fde` | **pass** | none |
| `j7elte-cam` | **pass** (3 defects fixed) | missing `makedepends="linux-headers"` (clean-chroot build failure), missing firmware `depends`, wrong `license` — fixed |
| Kernel artefacts used for boot | **fail** (integration, not a package defect) | every kernel built in this session lacks the BT/Wi-Fi config the packages need — §6 |

All four patches export cleanly onto `ba682bd` and reproduce the package
directories byte-for-byte (§7).

## 2. `j7elte-wifi` — pass

*APKBUILD*: `source=` lists exactly the five shipped files plus the `install=`
script; `sha512sum -c` 5/5 OK; `sh -n APKBUILD` OK.  `package()` emulated with
`srcdir`/`pkgdir` installs exactly:

```
/efs/wifi (dir 0755)  /etc/wpa_supplicant/wpa_supplicant.conf
/etc/init.d/j7elte-wifi  /etc/conf.d/j7elte-wifi
/usr/lib/tmpfiles.d/j7elte-wifi.conf
```

*No file conflict*: Alpine `main/wpa_supplicant` 2.11-r4 installs only
`/usr/share/doc/wpa_supplicant/examples/wpa_supplicant.conf` and
`/etc/wpa_supplicant/wpa_cli.sh`, never `/etc/wpa_supplicant/wpa_supplicant.conf`
(checked against its `package()`), so our config is the file Alpine's
`wpa_supplicant.initd` picks up (`default_conf=/etc/wpa_supplicant/wpa_supplicant.conf`).
Its `checkpath -d -o root:root /var/run/wpa_supplicant` matches
`ctrl_interface=/run/wpa_supplicant` because `/var/run` is a symlink to `/run`.

*Config content*: only valid global directives (`ctrl_interface`,
`ctrl_interface_group`, `update_config`, `ap_scan`); no network block.  Nothing to
parse-fail.

*Kernel/DT cross-check* (`device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`):
`CONFIG_BCM4343=y` (:1373), `CONFIG_BCMDHD_FW_PATH="/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin"`
(:1382), `CONFIG_BCMDHD_NVRAM_PATH=…/nvram_net.txt` (:1383),
`CONFIG_WIFI_BROADCOM_COB=y` (:1385).  `firmware-samsung-j7elte.package()` installs
`bcmdhd_sta.bin` and `nvram_net.txt` under exactly that directory, so the driver's
absolute opens resolve.  The `/efs/wifi` MAC-state provisioning matches
`READ_MACADDR`/`DHD_USE_CISINFO` being compiled in via `WIFI_BROADCOM_COB`.

*Init/systemd syntax*: `sh -n` OK for `j7elte-wifi.initd` and the post-install;
`depend() { need localmount; before wpa_supplicant networkmanager }` names real
services in both Alpine and pmOS images; `d /efs/wifi 0755 root root -` is valid
`tmpfiles.d` syntax and `/usr/lib/tmpfiles.d` is the systemd vendor directory.

Nothing changed in this package.

## 3. `j7elte-bt` — pass, one defect fixed

*APKBUILD*: `source=`/`install=` consistent, `sh -n` OK, `sha512sum -c` 4/4 OK
(after the fix below).  `package()` emulation installs
`/etc/init.d/j7elte-bt`, `/etc/conf.d/j7elte-bt`,
`/usr/lib/systemd/system/j7elte-bt.service`.

*Dependencies exist and contain the referenced paths* (checked in Alpine aports):

* `bluez-deprecated` → `deprecated()` moves `usr/bin/hciattach` (bluez 5.87) — our
  `HCIATTACH=/usr/bin/hciattach`.
* `bluez-openrc` → generated from `bluetooth.initd`, i.e. `/etc/init.d/bluetooth`
  (command `/usr/lib/bluetooth/bluetoothd`, `need dbus localmount hostname`).
* `util-linux-misc` → `misc()` moves `usr/sbin`, and util-linux installs rfkill via
  `usrsbin_exec_PROGRAMS += rfkill` — our `/usr/sbin/rfkill` is correct.

*Device/firmware names*: `drivers/bluetooth/bcm43xx.c:45` `#define BT_UPORT 3` and
the DT alias `uart3 → serial_3` give `/dev/ttySAC3` (serial driver name prefix
`"ttySAC"`, `drivers/tty/serial/samsung.c:86`); `CONFIG_SERIAL_SAMSUNG=y`,
`CONFIG_SERIAL_SAMSUNG_UARTS=4`, `CONFIG_DEVTMPFS=y`,
`CONFIG_BT_HCIUART=y`/`CONFIG_BT_HCIUART_H4=y` (:918-919), `CONFIG_RFKILL=y` (:946)
are all set in the pmaports config.  The platform driver registers its rfkill
switch soft-blocked (`rfkill_init_sw_state(bt_rfkill, 0)` … `rfkill_set_sw_state(bt_rfkill, true)`,
`bcm43xx.c:329-342`), so "unblock first, then attach" is required and the service
does exactly that.

*`hciattach` semantics verified against BlueZ 5.87 `tools/hciattach.c` and
`tools/hciattach_bcm43xx.c`*:

* `bcm43xx` is a real table entry (`115200, 3000000, FLOW_CTL, DISABLE_PM`); the
  positional order `<tty> <type> [speed] [flow]` is respected.
* `-t 30` sets `alarm(to)`/`bcsp_max_retries` (default 10 s) — an init timeout, as
  the confd comment says.
* `-n` sets `detach = 0`: after "Device setup complete" the process stays in the
  `ppoll()` loop holding the `N_HCI` line discipline; on SIGTERM it leaves the loop,
  restores `N_TTY` and exits.  So `start-stop-daemon … --background --make-pidfile`
  + `--retry TERM/5/KILL/5` is a sound supervision model and `hci0` disappearing on
  stop is expected.
* Firmware lookup: `bcm43xx_locate_patch()` **does** recurse into subdirectories
  (`if (entry->d_type & DT_DIR) … bcm43xx_locate_patch(path, …)`) and matches files
  whose name starts with the controller's local name and ends in `.hcd`.  Alpine
  patch `004-Move-the-43xx-firmware-into-lib-firmware.patch` changes
  `FIRMWARE_DIR` to `/lib/firmware`, so
  `/lib/firmware/postmarketos/samsung-j7elte/BCM43438A1.hcd`
  (installed by `firmware-samsung-j7elte` together with the ROM filename) is
  reachable.  The report's "recursively scans" claim is confirmed by source.

### Defect fixed — systemd unit ordered `bluetoothd` before the attach

```
- After=dev-ttySAC3.device bluetooth.service
+ After=dev-ttySAC3.device
  Wants=bluetooth.service
+ Before=bluetooth.service
```

With `After=bluetooth.service` systemd started `bluetoothd` *before* `hciattach`,
the opposite of what the package documents, of what the OpenRC service does
(`depend() … before bluetooth`, then `rc-service bluetooth start` after `hci0`
appears) and of the connectivity report's own B-section description.  Now the unit
is ordered like the OpenRC path.  `systemd-analyze verify` still accepts the unit
(the only message is the expected "host has no /usr/bin/hciattach").

Consequential edits: `sha512sums` entry for `j7elte-bt.service` updated in
`j7elte-bt/APKBUILD`, canonical copy
`parity/userspace/connectivity/bt/j7elte-bt.service` synced, and
`parity/patches/userspace/connectivity/0002-j7elte-bt-package.patch` regenerated
(applies to `ba682bd`, round-trip byte-identical).

Residual (documented, not a defect): `rc-service bluetooth start` is called from
inside `start()`; OpenRC's `before bluetooth` already guarantees the ordering, so
the call is the "enable it too" half of the design.  If `bluez-openrc` were replaced
by a systemd provider this service would fail after a successful attach — the
integrator's R3 risk, unchanged.

## 4. `j7elte-fde` — pass

*APKBUILD*: `source=`/`sha512sums` 4/4 OK, `sh -n` OK, `build()` is a no-op and
`package()` emulation installs exactly:

```
/etc/unl0kr.conf.d/50-j7elte.conf
/usr/bin/j7elte-fde-check
/usr/share/mkinitfs/files-extra/40-j7elte-fde.files
/usr/share/mkinitfs/hooks/50-j7elte-fde-fb.sh
/usr/share/mkinitfs-triggers/j7elte-fde
```

`depends="unl0kr postmarketos-initramfs postmarketos-mkinitfs"` — `unl0kr` (3.2.0,
`provides="postmarketos-fde-unlocker"`), `postmarketos-initramfs` and
`postmarketos-mkinitfs` 2.6.1 all exist in pmaports `main/`.

*unl0kr config*: `[general] animations` is a real key
(`unl0kr/config.c:43-45`, `bbx_config_parse_bool`), and `unl0kr/main.c:416-419`
reads `/usr/share/unl0kr/unl0kr.conf`, `/usr/share/unl0kr/unl0kr.conf.d`,
`/etc/unl0kr.conf`, `/etc/unl0kr.conf.d` **in that order**, so the drop-in layers
on top as claimed.  pmaports' own `/etc/unl0kr.conf` uses `[general] animations=true`,
so `false` is accepted.  `quirks.fbdev_force_refresh` is correctly *not* set
(it is the DRM-simpledrm workaround; this kernel has no DRM).

*mkinitfs plumbing verified against mkinitfs source*:

* `internal/filelist/hookfiles`: each file under `/usr/share/mkinitfs/files-extra`
  is read line by line, `#`-comments and blank lines are skipped, `src[:dest]` and
  globs are supported — our list is a single absolute path, fine.
* `cmd/mkinitfs/main.go:112-121`: `/usr/share/mkinitfs/hooks` and `/etc/mkinitfs/hooks`
  are copied to `/hooks` (our script is executable, 0755).
* `postmarketos-initramfs/init_2nd.sh`: `run_hooks /hooks` at line 24, i.e. after
  `setup_udev`/`setup_framebuffer` and before `unlock_root_partition` at line 47 —
  exactly the slot the hook assumes.
* `/usr/share/mkinitfs-triggers` is in the `triggers=` list of
  `postmarketos-mkinitfs` 2.6.1, so installing the empty
  `/usr/share/mkinitfs-triggers/j7elte-fde` does run `mkinitfs` (the package's
  comment is accurate).

*Kernel prerequisites*: all 17 symbols `j7elte-fde-check` requires are `=y` in
`config-samsung-j7elte.aarch64` (`BLK_DEV_DM`, `DM_CRYPT`, `CRYPTO_{AES,XTS,CBC,SHA256}`,
`BLK_DEV_INITRD`, `RD_GZIP`, `DEVTMPFS`, `FB`, `EXYNOS_DECON_FB`, `INPUT_EVDEV`,
`VT`, `LCD_CLASS_DEVICE`, `BACKLIGHT_CLASS_DEVICE`, `TOUCHSCREEN_MELFAS_MMS438`,
`TOUCHSCREEN_IST30XXC`).  `deviceinfo` has no `deviceinfo_create_initfs_extra`, so
the files-extra merge into the single initramfs does happen.

*BOOT size constant*: parsed the device PIT
(`session/tmp-artifacts/modemcp/j7elte.pit`) directly — `BOOT` = 65536 × 512 B =
**32 MiB**, `RECOVERY` = 77824 × 512 B = 38 MiB.  The constant and the 90 % warning
threshold in `j7elte-fde-check` are therefore correct.  Current kernel
`parity/out/arch/arm64/boot/Image` = 17,365,040 B (16.56 MiB), leaving ≈15 MiB for
the merged initramfs.

*`j7elte-fde-check`*: `sh -n` OK; executed on the host (55 checks, 6 expected
failures for the host kernel/unl0kr absence, exit 1); `--help`, `--quiet` and the
`/proc/config.gz` fallback path all behave.

Nothing changed in this package.

## 5. `j7elte-cam` — pass, three defects fixed

The package is under active development (`camera-ae` added the `--ae`/`--awb` 3A
work while this review ran).  Everything below was re-checked against the state at
22:40 (`j7elte-cam.c` = 2214 lines, sha512 `190c55de…`).

### 5.1 Defects fixed in `j7elte-cam/APKBUILD`

1. **`makedepends="linux-headers"` (build-breaking).**  `j7elte-cam.c` includes
   `<linux/videodev2.h>`, `j7elte-cam-ioctl.h` includes it too, and the source
   optionally `<linux/media.h>`.  abuild installs `build-base` =
   `binutils file gcc g++ make libc-dev fortify-headers patch`, and Alpine's
   `musl-dev` depends only on `musl` (checked in aports and on
   pkgs.alpinelinux.org), so `linux-headers` is **not** pulled implicitly — the
   build fails with "linux/videodev2.h: No such file or directory" in a clean
   chroot.  Same reason bluez/wpa_supplicant/util-linux list it explicitly.
2. **`depends="firmware-samsung-j7elte"`.**  The tool cannot start the pipeline
   without `fimc_is_fw2_3l2.bin`/`setfile_3l2.bin`/`setfile_5e3.bin`/`fimc_is_fd.bin`;
   the dependency was only documented in a comment.
3. **`license="MIT AND Apache-2.0"`.**  `j7elte-cam-meta.h` is Samsung's
   `fimc-is-metadata.h`, dual-licensed GPL-2.0-only/Apache-2.0 (its own header says
   so, and the copy states "Do not edit"), so `MIT` alone misdescribes the work.

`camera-ae` kept all three hunks and regenerated
`parity/patches/userspace/camera/0001-pmaports-add-j7elte-cam-package.patch` on top;
that patch applies to `ba682bd` and round-trips byte-identically.

### 5.2 Camera ABI/interface cross-check (all pass)

* *ioctl/metadata constants* re-derived from the kernel tree:
  `V4L2_CID_FIMC_IS_BASE = (V4L2_CTRL_CLASS_CAMERA|0x1000)`
  (`videodev2_exynos_camera.h:150`); `IS_END_OF_STREAM +58`, `IS_S_STREAM +14`,
  `IS_SET_SETFILE +51`, `IS_S_BNS +52`, `IS_HAL_VERSION +902`;
  `enum is_set_stream {0,1}`; `enum v4l2_is_hal_version {1_0=0, 3_2=1, MAX}`;
  `INPUT_*` shifts 24/16/8/4/0 with the masks in `fimc-is-video.h:23-32`;
  `enum fimc_is_video_dev_num` 1/2/10/11/20/21/30/31/40/41/50/51/52;
  `SENSOR_MODULE_SHIFT=0`/`SENSOR_SCENARIO_SHIFT=28`
  (`fimc-is-device-sensor.h:28-31`, so `(scenario<<28)|sensor_id` is right);
  `SENSOR_NAME_S5K3L2=8`, `SENSOR_NAME_S5K5E3=17`.
* *Node names*: `FIMC_IS_VIDEO_NAME(name) = "exynos-fimc-is-"name` used by
  `FIMC_IS_VIDEO_{SSX,3XS,3XC,IXS,SCP}_NAME` — matches `J7NODE_*` exactly.
* *Buffer types*: `fimc_is_video_probe()` sets
  `type = (vfl_dir == VFL_DIR_RX) ? CAPTURE : LEADER`, and the registrations are
  SS0/SS1 = `VFL_DIR_RX`, 30c = `VFL_DIR_RX`, scp = `VFL_DIR_RX`, 30s = `VFL_DIR_M2M`,
  i0s = `VFL_DIR_M2M`.  The client opens ss/30c/scp as `CAPTURE_MPLANE` and
  30s/i0s as `OUTPUT_MPLANE` — correct.  (Also confirmed `S_PARM` exists only on the
  SSX ioctl ops, as used.)
* *Plane model*: `fimc_is_queue_set_format_mplane()` overwrites the user's
  `num_planes` with the driver's format-table value (`SBGGR10` = 1+SPARE,
  `YUV420M` = 3+SPARE, `SPARE_SIZE` = 32 KiB) and `fimc_is_queue_buffer_queue()`
  takes `spare = vb->num_planes - 1`, mapping the shot at `spare`+0 and setting
  `shot = kvaddr + sizeof(shot_ext)-sizeof(shot)`.  The client's "last plane is the
  spare, ≥32 KiB holds `camera2_shot_ext`" model is correct, as is its
  `SPARE_PLANE_SIZE`.
* *Metadata ABI*: compiled `sizeof`/`offsetof` from the kernel header and from the
  client header and diffed them — **identical** (`shot_ext` 30000, `node_group` 200
  @4, `shot` 29624 @376, `magicNumber` 29992, leader/capture field offsets equal).
  Stronger: stripping comments and tokenising both headers gives 4589 tokens with
  a difflib ratio of **1.000000 and zero differing blocks** — the vendored header is
  an exact ABI copy, so every field the new AE code writes
  (`ctl.aa.aeMode/awbMode/aeTargetFpsRange`, `ctl.sensor.exposureTime/frameDuration/sensitivity`,
  `dm.*`, `udm.internal.vendorSpecific2`) exists with the kernel's layout.
* *Sensor modes*: `get_sensor_mode()` in `fimc-is-device-sensor.c:284-327` matches
  width/height and takes an exact frame rate, else the smallest one above the
  request — the client's `mode_supported()` mirrors it.
* *SCP output*: `fimc_is_subdev_s_format()` rejects `YUV420M` unless `width % 16 == 0`
  (`fimc-is-subdev-ctrl.c:432-439`); the client enforces the same.
* *Group wiring*: `fimc_is_group_init()` stores `source_vid = video_id`; 30s gets the
  sensor vid and i0s gets 30c's — matches the client's `encode_group_input()`.
* *Build*: `aarch64-none-linux-gnu-gcc` 10.3 with the package's own
  `-std=gnu11 -Wall -Wextra -Wshadow -Wformat=2 -Wpointer-arith -Wstrict-prototypes
  -Wmissing-prototypes -Wvla` → **zero warnings**, both for the 1655-line version and
  the current 2214-line AE version.

Nothing else changed in the sources or Makefile (owned by `camera-ae`).

## 6. Cross-cutting defect (not fixed here): no built kernel has the BT/Wi-Fi config

The connectivity report asserts the kernel already has `CONFIG_BT_HCIUART=y` and the
`/lib/firmware/postmarketos/...` bcmdhd paths.  That is true of the **pmaports**
config (`config-samsung-j7elte.aarch64`, whose sha512 `4351f862…` matches the
`linux-samsung-j7elte/APKBUILD`, so the package is consistent) but **not** of any
kernel actually built in this session:

```
$ for d in parity/out parity/out-drm parity/out-drm-clean; do grep -n 'BT_HCIUART\|BCMDHD_FW_PATH=' $d/.config; done
918:# CONFIG_BT_HCIUART is not set
1381:CONFIG_BCMDHD_FW_PATH="/system/etc/wifi/bcmdhd_sta.bin"      # × 3 build trees
$ diff -q config-nura-j7elte parity/out/.config   → identical
```

Consequences on such an image: `hciattach`'s `TIOCSETD`/`N_HCI` fails because the
HCI-UART line discipline only exists with `CONFIG_BT_HCIUART` (the `j7elte-bt`
service correctly aborts with "hciattach did not create hci0"), and `bcmdhd` opens
`/system/etc/wifi/…` instead of the pmOS firmware directory, so the first `wlan0`
open fails its firmware download.

`parity/build.sh` already defaults `CFG` to the pmaports config with the comment
"config-nura-j7elte is the pre-patch copy and no longer matches the package"
(mtime 22:25), but the newest build tree is from 21:09 and the newest log from 21:51
— **no rebuild has happened since**.  This is a shared kernel artefact, outside my
write ownership; the fix is to rebuild with the default `CFG` before any
connectivity test, and to stop using `config-nura-j7elte` for these packages.

Also stale: `connectivity-userspace.md` "Integration still required" says
`device-samsung-j7elte/APKBUILD` must gain `j7elte-wifi`/`j7elte-bt`.  It already
has both (and `j7elte-fde`, `j7elte-cam`, …), and the file is byte-identical to
`parity/patches/userspace/0001-device-deps.patch` applied to `ba682bd`.

## 7. Checks executed (evidence)

| Check | Command | Result |
|---|---|---|
| APKBUILD parse | `sh -n` on all four APKBUILDs | OK |
| Shell syntax | `sh -n` on both initd, both post-install, the hook and `j7elte-fde-check` | OK |
| Checksums | `sha512sum -c` over each `sha512sums` block | 5/5, 4/4, 4/4, 4/4 OK |
| `source=` completeness | every entry exists in the package dir | OK |
| `package()` emulation | `sh -c '. ./APKBUILD; …; package'` with `srcdir`/`pkgdir` | install paths per §2–§5 |
| Patch applicability | `git apply --check`/apply in a pristine `ba682bd` tree | all 4 patches APPLY_OK |
| Patch ⇄ package sync | `diff -r` applied tree vs package dir | all four byte-identical |
| Device deps patch | apply `userspace/0001-device-deps.patch`, diff `device-samsung-j7elte/APKBUILD` | identical |
| Camera build | `aarch64-none-linux-gnu-gcc` 10.3, package WARN flags | 0 warnings, 0 errors |
| Camera metadata ABI | host compile of `sizeof`/`offsetof` from both headers + token diff | identical (ratio 1.0) |
| systemd unit | `systemd-analyze verify j7elte-bt.service` | no unit errors (only host-missing `hciattach`) |
| `j7elte-fde-check` | run on host, `--quiet`, `--help` | behaves, exit 1 on hard failures |
| PIT | parsed `session/tmp-artifacts/modemcp/j7elte.pit` | BOOT 65536 blk = 32 MiB, RECOVERY 77824 blk |
| Kernel symbols | grep `=y` in `config-samsung-j7elte.aarch64` | all FDE/BT/Wi-Fi symbols present |
| Alpine package contents | aports `bluez`, `util-linux`, `wpa_supplicant`, `build-base`, `musl`; pkgs.alpinelinux.org for `musl-dev` | paths/deps as cited |
| BlueZ behaviour | BlueZ 5.87 `tools/hciattach.c`, `hciattach_bcm43xx.c` + Alpine patch 004 | `-n`/`-t`/recursive firmware scan confirmed |
| unl0kr behaviour | `unl0kr/config.c`, `main.c` (postmarketOS/buffybox, main) | config order/keys confirmed |
| mkinitfs behaviour | mkinitfs `main.go`, `hookfiles.go`, `postmarketos-mkinitfs/APKBUILD`, `init_2nd.sh` | dirs/trigger/hook order confirmed |

## 8. Not verified (and why)

* No `abuild`/`apk` in the environment: the APKBUILDs were validated by parsing,
  checksum verification, `package()` emulation and source-level cross-checks
  against abuild/Alpine semantics; a real `abuild -r` was not run.  In particular
  the empty `/efs/wifi` directory in the `.apk` payload is verified only by the
  `package()` emulation (apk's directory handling was not exercised).
* No j7elte hardware and no aarch64 rootfs: no `wlan0`/`hci0`/frame capture, no
  initramfs was generated, no LUKS volume unlocked.  All runtime statements in this
  report stay expectations.
* The BT local name returned by the controller (`BCM43438A1` prefix match) remains
  an inference; the report's R1 stands unchanged.
* `j7elte-cam` is being modified concurrently by `camera-ae`; this review covers the
  22:40 state and the APKBUILD fixes only.
