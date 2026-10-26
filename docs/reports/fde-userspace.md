# FDE userspace — LUKS unlock in the initramfs (j7elte)

**Verdict: implemented (generic pmOS unlock flow reused, device enablement package added).
Static verification only — no j7elte hardware, no aarch64 rootfs, no package build (abuild is
not installed in this environment); nothing here was booted.**

The unlock itself is *not* re-implemented. j7elte reuses the stock postmarketOS chain

```
init_functions.sh:unlock_root_partition()   (postmarketos-initramfs 3.4.1, :523-538)
  └─ fde-unlock <part>                      (unl0kr 3.2.0-r5, unl0kr/unlock.sh:3)
       └─ unl0kr | cryptsetup --perf-no_read_workqueue \
                   --perf-no_write_workqueue open <part> root -
```

and the kernel already has everything it needs. What was missing is device-level enablement:
nothing in the j7elte packages pulls an unlocker, nothing prepares the DECON framebuffer /
backlight for the unlock UI, and nothing lets a user check the prerequisites or the 32 MiB
`BOOT` partition limit. That is what `j7elte-fde` provides.

---

## 1. What the pinned tree contains (and what it does not)

`parity/pmaports` @ `ba682bd` is a **device-only** snapshot: `device/archived/{device,linux,
firmware}-samsung-j7elte`. There is no `main/` aports tree, so `unl0kr`,
`postmarketos-initramfs`, `postmarketos-mkinitfs` and `boot-deploy` cannot be read from the
working tree. The flow above was therefore verified against the current upstream sources:

| Component | Version / commit | What was read |
|---|---|---|
| pmaports | `817ed87` (2024-11-03) | `main/unl0kr/*`, `main/postmarketos-initramfs/*`, `main/postmarketos-mkinitfs/*` |
| unl0kr (buffybox) | 3.2.0-r5 / `c683350` | `unlock.sh`, `unl0kr.files`, `unl0kr.conf`, `main.c`, `config.c` |
| postmarketos-initramfs | 3.4.1-r0 | `init.sh`, `init_2nd.sh`, `init_functions*.sh`, file lists |
| postmarketos-mkinitfs | 2.6.1 / `d63e600` | `cmd/mkinitfs/main.go`, file listers |
| boot-deploy | `4529ed6` | `boot-deploy-functions.sh` (`create_bootimg`) |
| pmbootstrap | `7954019` | `pmb/install/format.py`, `pmb/parse/arguments.py` |

Relevant properties of that flow, all confirmed in source:

* The initramfs calls the unlocker only through `fde-unlock`, which unl0kr installs
  as `/usr/bin/fde-unlock` (`unl0kr/APKBUILD:59-60`); unl0kr `provides="postmarketos-fde-unlocker osk-sdl=…"`
  (`unl0kr/APKBUILD:37`) and `postmarketos-initramfs` depends on exactly that virtual name
  (`postmarketos-initramfs/APKBUILD:31`), plus `cryptsetup` (:17), `libinput-libs` (:27) and
  `udev` (:33).
* unl0kr's files go to `/usr/share/mkinitfs/files-extra/30-unl0kr.files`
  (`unl0kr/APKBUILD:72`). On a device **without** `deviceinfo_create_initfs_extra`, mkinitfs
  *merges* files-extra into the single initramfs (`cmd/mkinitfs/main.go:135`). j7elte has no
  such deviceinfo flag, so the merge happens — and it must, because a boot.img device has no
  boot filesystem where an `initramfs-extra` could live (`boot-deploy-functions.sh`,
  `create_bootimg` only embeds the main initramfs).
* udev is up before the unlock (`init_2nd.sh:14`), the framebuffer is set up before it
  (`init_2nd.sh:18`), hooks run at `init_2nd.sh:24`, unlock at `init_2nd.sh:47`.
* unl0kr reads `/etc/unl0kr.conf` and then `/etc/unl0kr.conf.d/*` alphabetically
  (`unl0kr/main.c:385-387`), and only the `quirks.fbdev_force_refresh` workaround is
  DRM-simpledrm specific (`main.c:413-416`) — j7elte has no DRM (`CONFIG_DRM` is not set), so
  that quirk is deliberately **not** used.
* pmbootstrap formats the root with `cryptsetup luksFormat -q --cipher aes-xts-plain64
  --iter-time …` (`pmb/install/format.py:56-66`), i.e. XTS+AES, nothing exotic.

---

## 2. Device-side prerequisites — all already satisfied by the kernel

`config-nura-j7elte` (Nura kernel `2a746c1`, baseline `b5e363a`), line numbers in that file:

| Requirement | Symbol | Line |
|---|---|---|
| initramfs | `CONFIG_BLK_DEV_INITRD=y`, `CONFIG_RD_GZIP=y` | 135, 137 |
| device nodes | `CONFIG_DEVTMPFS=y` | 963 |
| dm-crypt | `CONFIG_BLK_DEV_DM=y`, `CONFIG_DM_CRYPT=y` | 1197, 1200 |
| LUKS cipher | `CONFIG_CRYPTO_AES=y`, `CONFIG_CRYPTO_XTS=y`, `CONFIG_CRYPTO_CBC=y`, `CONFIG_CRYPTO_SHA256=y` | 3936, 3904, 3898, 3928 |
| framebuffer | `CONFIG_FB=y`, `CONFIG_EXYNOS_DECON_FB=y` | 2312, 2358 |
| backlight | `CONFIG_LCD_CLASS_DEVICE=y`, `CONFIG_BACKLIGHT_CLASS_DEVICE=y` | 2384, 2398 |
| input | `CONFIG_INPUT_EVDEV=y`, `CONFIG_VT=y` | 1426, 1568 |
| touch | `CONFIG_TOUCHSCREEN_MELFAS_MMS438=y`, `CONFIG_TOUCHSCREEN_IST30XXC=y` | 1510, 1514 |

Two consequences worth recording:

* **The kernel is fully static** — `grep -c '=m$' config-nura-j7elte` = **0**. Nothing has to be
  modprobed before the root filesystem exists, so dm-crypt is usable from the first initramfs
  process. (`CONFIG_CRYPTO_ESSIV` is absent, but pmbootstrap's default `aes-xts-plain64` does
  not need it; only a hand-made `aes-cbc-essiv:sha256` volume would.)
* `/dev/fb0` is created unconditionally when DECON probes
  (`drivers/video/exynos/decon_7580/decon_core.c:4591`, `register_framebuffer` at the end of
  `decon_probe`; see also `parity/reports/display.md` :178-183), and
  `/sys/class/backlight/panel` is registered by the ea8061 panel driver. Touch firmware is
  built into the kernel (`parity/reports/touch.md` P4), so the touchscreen is functional in the
  initramfs without any file in `/lib/firmware`.

So the kernel needs **no patch and no config change**. `CONFIG_FRAMEBUFFER_CONSOLE` is not set
and `CONFIG_IKCONFIG` is not set: the initramfs debug shell is therefore only visible on the USB
network/telnet, not on the panel — relevant when troubleshooting an unlock failure.

### The one real device constraint: `BOOT` is 32 MiB

Parsed from the device PIT (`session/tmp-artifacts/modemcp/j7elte.pit`, entry 13):

```
BOOT  sectors=65536  = 32768 KiB = 32.0 MiB
RECOVERY sectors=77824 = 38.0 MiB   USERDATA = rest of eMMC
```

For comparison: the stock boot.img is 19.1 MB (kernel 16.0 MB + ramdisk 2.8 MB + dt 0.3 MB),
and the Nura kernel Image built in this session is **17.37 MB = 16.56 MiB**
(`parity/out/arch/arm64/boot/Image`). boot-deploy passes the kernel payload **uncompressed**
(`boot-deploy-functions.sh`, `_kernelfile="$work_dir/$kernel_filename"`), so the merged
initramfs must fit into roughly `32 MiB − 16.56 MiB − 0.30 MiB ≈ 15 MiB` (inflated by the
unl0kr/libinput/libdrm/udev/cryptsetup set). That is expected to fit, but it is the one thing
that cannot be proven without building the image — hence the `boot.img` size check in
`j7elte-fde-check` and the 90 % warning threshold. `deviceinfo_create_initfs_extra` is **not**
the escape hatch here (boot.img devices have no boot filesystem); if a merged image ever
overflowed, the initramfs itself would have to shrink.

### Minimal configuration delta (exact answer)

| Layer | Needed on j7elte | Why |
|---|---|---|
| pmbootstrap | `install --fde --add j7elte-fde` (cipher stays the default `aes-xts-plain64`) | `--fde` creates and formats the LUKS root (`pmb/install/format.py:56-66`); the `--add` puts the unlocker into the image the initramfs is generated from |
| kernel config | **nothing** | all of §2 is already `=y` |
| device tree | **nothing** | DECON/DSIM/ea8061 and the Melfas touch node are already correct (`display.md`, `touch.md` P2) |
| `deviceinfo` | **nothing** — no `deviceinfo_create_initfs_extra`, no `deviceinfo_no_framebuffer` | boot.img device with a 32 MiB `BOOT`; extra files are merged into the one initramfs, and fb0 exists |
| initramfs module list | **none** | the kernel is static (`=m` count 0), dm-crypt/XTS/AES and the panel/touch drivers are built in |
| kernel cmdline | **none** | `pmos.root_uuid=` is emitted by boot-deploy; the initramfs finds the LUKS container by `crypto_LUKS` signature / `pmOS_root` label |
| services | **none** | the unlocker is not a daemon: `init_2nd.sh:47` calls `fde-unlock` before mount; udevd is started by `setup_udev` (`init_2nd.sh:14`) |
| extra files | unl0kr drop-in + one `/hooks` script + trigger | provided by `j7elte-fde` (§3) |

---

## 3. What is provided

### `j7elte-fde` package (`parity/pmaports/device/archived/j7elte-fde/`)

| File | Purpose |
|---|---|
| `APKBUILD` | `depends="unl0kr postmarketos-initramfs postmarketos-mkinitfs"`, installs the four files below plus an empty `/usr/share/mkinitfs-triggers/j7elte-fde` (installing anything there makes the postmarketos-mkinitfs trigger run `mkinitfs`, so the initramfs is regenerated by `apk add` alone) |
| `50-j7elte.conf` | unl0kr drop-in at `/etc/unl0kr.conf.d/50-j7elte.conf`: `[general] animations=false` (a full 720x1280x4 = 3.5 MiB framebuffer copy per animated frame on a Cortex-A53), with a comment explaining why `quirks.fbdev_force_refresh` is *not* set. It layers on top of `/etc/unl0kr.conf` instead of replacing user settings. |
| `40-j7elte-fde.files` | mkinitfs `files-extra` entry for the drop-in; merged into the single initramfs because j7elte does not set `deviceinfo_create_initfs_extra` |
| `50-j7elte-fde-fb.sh` | boot-time `/hooks` script (`init_2nd.sh:24`, i.e. after udev + framebuffer setup, before `unlock_root_partition()`): logs `/dev/fb0` size, unblanks fb0 if `blank != 0` (VT console sleep is enabled in this config), restores `/sys/class/backlight/panel` to `max_brightness` if it is 0, and logs every `/dev/input/event*` name so the libinput view is visible in `dmesg`/telnet. Always exits 0. |
| `j7elte-fde-check` | read-only prerequisites checker (see below); `--regenerate` additionally runs `mkinitfs` as root; `--quiet`, `--help` |

The `BOOT` size constant (32 MiB) and the 90 % warning threshold live in `j7elte-fde-check`
and are cross-checked against the PIT by `parity/userspace/fde/validate.sh`.

### `j7elte-fde-check` checks

Kernel config (from `/boot/config-samsung-j7elte`, falling back to `/proc/config.gz`): every
symbol of the table above, plus "touchscreen driver built in". Runtime: `/dev/fb0` present and
its `virtual_size`, fb blank state, `/sys/class/backlight/panel` brightness, at least one
`/dev/input/event*`, `unl0kr` + `fde-unlock` present, the drop-in installed, whether the running
root is a dm-crypt mapping (and if not, that FDE requires a reinstall), and
`/boot/boot.img` against the 32 MiB `BOOT` partition. Exit code 1 on any hard failure.

### `parity/userspace/fde/validate.sh`

Host-side static harness (no hardware): `sh -n` on both shipped scripts, `shellcheck -s sh -S
warning` when available, APKBUILD `source=` / `sha512sums=` consistency against the files on
disk, presence of the mkinitfs trigger and `/hooks` install lines, `40-j7elte-fde.files` → real
file mapping, every key in `50-j7elte.conf` checked against the key list parsed by
`unl0kr/config.c:parsing_handler`, the 15 kernel symbols checked as `=y` in
`config-nura-j7elte`, "kernel is static" (`=m` count 0), and the PIT `BOOT` partition compared
to the constant hard-coded in `j7elte-fde-check`.

---

## 4. How to use it

**Fresh install with FDE** (the only way to get an encrypted root; there is no in-place
conversion):

```sh
pmbootstrap install --fde --add j7elte-fde
# or, equivalently for the unlock itself:
pmbootstrap install --fde --add unl0kr
```

`j7elte-fde` pulls `unl0kr` explicitly instead of relying on the
`postmarketos-fde-unlocker` provider resolution, and adds the drop-in + hook. The initramfs
generated during `pmbootstrap install` already contains them because the package is installed
before mkinitfs runs.

**Existing FDE system that boots into an initramfs without an unlocker** (or after installing
unl0kr by hand):

```sh
sudo apk add j7elte-fde          # trigger regenerates the initramfs
sudo j7elte-fde-check            # expects 0 failures on j7elte
# flash the regenerated /boot/boot.img to BOOT, from the host:
pmbootstrap flasher flash_kernel
```

**Existing unencrypted system:** FDE cannot be enabled in place; reinstall with `--fde`.

---

## 5. Verification performed

Ran in this environment (no j7elte hardware):

```
$ sh -n j7elte-fde-check 50-j7elte-fde-fb.sh            # both parse
$ shellcheck -s sh -S warning <both scripts>            # clean (0.10.0)
$ parity/userspace/fde/validate.sh                      # 36 passed, 0 failed
$ j7elte-fde-check                                      # exercised on the host:
   #   --help, kernel-config fallback via /proc/config.gz, fb/input/backlight
   #   probing, LUKS detection fallback, boot.img absence path, exit code 1
$ sh -c '. ./APKBUILD; srcdir=…; pkgdir=…; build; package'   # package() emulation
   #   installs exactly /etc/unl0kr.conf.d/50-j7elte.conf,
   #   /usr/bin/j7elte-fde-check, /usr/share/mkinitfs/{files-extra/40-j7elte-fde.files,
   #   hooks/50-j7elte-fde-fb.sh}, /usr/share/mkinitfs-triggers/j7elte-fde
```

Not performed (and not claimed): no aarch64 build of the package (`abuild` absent), no
initramfs/boot.img was generated, no LUKS volume was created or unlocked, no boot test. The
`/hooks` script is syntax/lint-checked only — it cannot be executed without hardware, and its
`fb0`/backlight writes were deliberately not run against the host's framebuffer.

Residual risks:

1. **boot.img size** — see §2. Verify with `j7elte-fde-check` after the first `mkinitfs`; the
   script fails if the image exceeds 32 MiB and warns above 28.8 MiB.
2. **LUKS2 + argon2 unlock time** — pmbootstrap only passes `--iter-time 200` (ms, calibrated in
   the host chroot) and lets cryptsetup choose the LUKS format; on this Cortex-A53 the unlock
   PBKDF can take several seconds. Not a functional blocker; `--iter-time` can be raised and
   `cryptsetup luksChangeKey` re-run, or a LUKS1 volume can be used.
3. **The pin is a snapshot.** The j7elte packages live in `device/archived`; `j7elte-fde` was
   written against unl0kr 3.2.0 (keyscript `fde-unlock`) and postmarketos-initramfs 3.4.1. An
   older channel that still expects `osk-sdl -d <dev> -k <keyfile>` would need the osk-sdl
   invocation instead — on such a channel `j7elte-fde-check` warns that `fde-unlock` is missing.
4. **No screen console** (`CONFIG_FRAMEBUFFER_CONSOLE` unset): initramfs errors after the splash
   are only visible via `pmos.debug-shell` over USB networking, or by adding a debug hook.

No shared pmaports file was modified (`device-samsung-j7elte/APKBUILD`, `deviceinfo`,
`linux-samsung-j7elte/config-*` are untouched, and the kernel tree is unmodified). No patch
under `parity/patches/userspace/fde/` is required: every prerequisite was already present in
the kernel and the device package; the gap was purely the missing userspace/initramfs
enablement shipped as the new `j7elte-fde` package.
