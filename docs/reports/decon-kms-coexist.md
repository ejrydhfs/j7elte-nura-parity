# DECON KMS coexistence and rollback (M3)

Date: 2026-11-23. Slice: `KmsCoexist`. Scope: how KMS is turned on and off
without the previous attempt's failure mode (`exynos7580-decon-drm` claimed the
DECON/DSIM compatibles, the vendor `decon_driver` lost its device, and the
device came up with no `/dev/fb0` and a dark panel — `patches/drm-kms-coexist/
DESIGN.md` §1, §4).

No device write, flash or reboot was performed. The kernel tree is read-only;
all `file:line` citations are from
`android_kernel_samsung_universal7580@2a746c1b` (the tree mounted under
`/run/media/.../j7k/`).

## 1. Decision

**A Kconfig display-owner choice (`iv`), with the vendor fbdev driver kept as
an ordered boot fallback.** One image can never have two active owners of
DECON; turning KMS off is a different kernel image (not a runtime switch),
because a boot parameter cannot express it on this device (§2.2). The concrete
edits are in `patches/drm-kms-coexist/` (§4).

* `EXYNOS7580_DISPLAY_FBDEV` (default, shipping): only the vendor DECON fbdev
  stack is active; `/dev/fb0`, Xorg, `fbwin` and nura-shell are byte-for-byte
  today's behaviour.
* `EXYNOS7580_DISPLAY_KMS`: the KMS driver is built in and binds the DECON
  node at `late_initcall` — after the vendor DSIM/panel driver, which it
  reuses. It registers `card0` **and** `/dev/fb0` through `drm_fbdev_cma`.
  The vendor fbdev driver remains compiled and its registration moves to
  `late_initcall_sync`, so if the KMS probe fails it binds the still-unbound
  device and the fbdev/X11 path comes back **without a re-flash**.

No device-tree change and no boot-command-line change are made in either
variant. The stock `dt.img` and the existing compatibles are used unchanged.

## 2. Constraints that decide the mechanism

### 2.1 The DT payload cannot change (kills option i)

`docs/HARDWARE-BRINGUP-2026-11-16.md:441-444`: replacing the DTB payloads in
the `DTBH` container makes S-Boot hang before USB comes up; kernel-only
flashes boot. `code/kernel/recovery/RECOVERY.md:3-6` repeats the rule, and the
known-good and M1 images carry the identical `dt` section
(`docs/reports/gpu31-m1-boot.md:38`, sha `352bda9b…`).

A DT `status` switch would require a *second* `dt.img` (and therefore a
second boot-image variant that changes the risky payload) and, worse, the KMS
driver has no DT node to bind: the only DECON compatible in the shipped DTB is
`samsung,exynos5-decon_driver` (`exynos7580.dtsi:1078-1079`), with
`status="disabled"` in the SoC dtsi (`:1152`) overridden to `"okay"` by the
board files (`exynos7580-j7elte_rev00.dts:1013-1020`, rev04/rev06
`:1009-1016`). Disabling it would disable **both** drivers, not switch owner.

### 2.2 A boot parameter cannot be the switch (kills option iii as the switch)

* The compiled command line wins: `code/pmaports/device/archived/
  linux-samsung-j7elte/config-samsung-j7elte.aarch64:403-406` has
  `CONFIG_CMDLINE="console=ttySAC1"`, `CONFIG_CMDLINE_FORCE=y`.
* S-Boot also discards the boot-image command line:
  `docs/HARDWARE-BRINGUP-2026-11-16.md:35-44` ("`/proc/cmdline` is exactly
  `console=ttySAC1`, regardless of the cmdline inside boot.img");
  `docs/reports/gpu31-m1-boot.md:26,83` records the same.
* Therefore a `module_param` value can only be changed by rebuilding the
  `Image` (or the initramfs, which is also part of the boot image). It is a
  compile-time switch with extra steps and two drivers claiming one node.
  This replaces the plan's M3 assumption that KMS would be "enabled by a boot
  parameter" (`docs/reports/decon-kms-plan.md:75`).

The driver core *does* fall through to the next matching driver after a failed
probe (`drivers/base/dd.c:278,317-334` clears `dev->driver` and, for
`-ENODEV`/`-ENXIO`, does not defer; `device_attach()` walks every driver at
`:405-447`). That mechanism is used here as the **safety net**, not as the
switch: the vendor fbdev driver registers later (`late_initcall_sync`) and
binds only if KMS is not bound.

### 2.3 Probe/registration order is deterministic

* Platform devices are populated at `arch_initcall_sync`
  (`arch/arm64/kernel/setup.c:495-500`).
* The vendor DSI driver is `late_initcall(dsim_init)`
  (`dsim_drv.c:1645`) and matches `samsung,exynos5-mipi-dsi`
  (`:1569-1572`).
* The vendor fbdev driver is `late_initcall(exynos_decon_register)`
  (`decon_core.c:4890`), matches `samsung,exynos5-decon_driver`
  (`:55-58`) and calls `register_framebuffer()` (`:4591`).
* `drivers/Makefile:18` links `video/` before `drivers/Makefile:50` `gpu/`,
  so within `late_initcall` (level 7, `include/linux/init.h:223`) the DSI
  driver always runs before any DRM driver in `gpu/`.

The edited order is therefore: level 7 `video/` DSIM+panel → level 7 `gpu/`
KMS (binds DECON, creates `card0`+`fb0`) → level 7s
(`late_initcall_sync`, `init.h:224`) vendor DECON fbdev (fallback only).
No deferred-probe race is involved.

### 2.4 `/dev/fb0` sources

* Vendor path: `register_framebuffer()` in `decon_probe` (`decon_core.c:4591`).
* KMS path: `drm_fbdev_cma_init()` (`drm_fb_cma_helper.c:344-392`, used by the
  existing KMS driver at `patches/drm/0001-…:1800`) registers an fb whose
  `fb_ops` provide `fb_fillrect/copyarea/imageblit/check_var/set_par/blank/
  pan_display/setcmap` (`drm_fb_cma_helper.c:236-245`), with
  `fix.smem_start` set from the CMA object (`:315`). That is enough for the
  Xorg fbdev DDX (mmap + generic ioctls); it is not enough for the vendor
  `S3CFB_WIN_CONFIG` ioctl used by `code/experience/fbwin.c:59,201` (see §5).

The KMS driver must fail its probe if fb0 cannot be registered; otherwise the
fallback would not trigger.

## 3. Option comparison

| | (i) DT status/second DTS | (ii) one driver registers fb0+card0 | (iii) boot param/module_param | (iv) Kconfig choice **(chosen)** |
|---|---|---|---|---|
| Owner clash | none, but requires a new compatible/DT | one driver, two scanout consumers on one DECON — window/vsync/clock policy must be invented; concurrent X+KWin corrupts | two drivers on one node; active owner depends on initcall order/param | one active owner per image; driver core enforces one binding |
| S-Boot DT rule | **violated** (new `dt.img`) | safe | safe | safe (stock `dt.img`) |
| Rollback cost | reflash a boot image with a different, risky DT payload | no runtime switch; a crash in always-on KMS still needs a reflash | rebuild `Image`; param not settable at boot | reflash a kernel-only boot image; fbdev image and pre-KMS image both documented |
| Same-image A/B | no | yes (both nodes), but unsafe to use concurrently | no (CMDLINE_FORCE) | no |
| Xorg/nura-shell | KMS variant needs DRM fb0; vendor `fbwin` gone | unchanged | unchanged in off mode; KMS mode as (iv) | off: unchanged; on: DRM fb0 works with Xorg, `fbwin` gone; probe failure restores the vendor path automatically |
| fb0 guarantee | depends on KMS probe | always (vendor) | depends | KMS probe failure falls back to vendor fb0; KMS success has DRM fb0 |
| Verdict | rejected | rejected for M3 (post-M2 option) | rejected as the switch (kept as fallback semantics) | **chosen** |

`(ii)` remains the long-term "both nodes at once" goal, but it needs a window
partitioning and vsync ownership contract that does not exist yet; it is not a
safe M3 switch.

## 4. Concrete edits

### 4.1 Patch: `patches/drm-kms-coexist/0001-display-owner-kconfig.patch`

Three files (validated with `patch -p1 --dry-run` against the pre-state):

1. **`drivers/video/exynos/decon_7580/Kconfig:1`** — insert the new choice
   before `config EXYNOS_DECON_EXYNOS7580` (31 added lines; the vendor symbol
   and its `depends on FB` at `:1-3` are left untouched):

```
choice
	prompt "Exynos 7580 display stack"
	default EXYNOS7580_DISPLAY_FBDEV if FB
	default EXYNOS7580_DISPLAY_KMS if DRM=y
	...
config EXYNOS7580_DISPLAY_FBDEV
	bool "Legacy DECON fbdev only (X11 / nura-shell)"
	depends on FB
config EXYNOS7580_DISPLAY_KMS
	bool "DECON DRM/KMS on top of the vendor stack (Plasma Mobile)"
	depends on DRM=y
endchoice
```

2. **`drivers/video/exynos/decon_7580/decon_core.c:4890`** — move the vendor
   fbdev driver behind the KMS owner and before `module_exit`:

```
-late_initcall(exynos_decon_register);
+/* KMS binds first; this is the fallback if its probe failed. */
+late_initcall_sync(exynos_decon_register);
```

3. **`drivers/gpu/drm/exynos7580/Kconfig:1-3`** (the file created by
   `patches/drm/0001-…`) — make the KMS driver built-in and choice-gated:

```
-config DRM_EXYNOS7580
-	tristate "DRM/KMS support ..."
-	depends on DRM && OF && ARM64 && ARCH_EXYNOS
+config DRM_EXYNOS7580
+	bool "DRM/KMS support ..."
+	depends on DRM=y && OF && ARM64 && ARCH_EXYNOS && EXYNOS7580_DISPLAY_KMS
```

Apply order: `patches/drm/0001-…` first (it creates the KMS driver and its
Kconfig), then this patch. If `KmsAssemble` renames the symbol to
`DRM_EXYNOS_DECON7580` in `drivers/gpu/drm/exynos/Kconfig`, the same two lines
(`bool`, `depends on … && EXYNOS7580_DISPLAY_KMS`) are the only change.

`bool` is mandatory: `module_platform_driver` is `device_initcall` (level 6,
`include/linux/init.h:313`), i.e. *before* the vendor DSI driver at level 7.
A module would load after the fallback bound DECON.

### 4.2 Kconfig fragments

* `patches/drm-kms-coexist/display-owner-fbdev.fragment` — selects the default
  and deselects the KMS driver; the stock display symbols are not touched.
* `patches/drm-kms-coexist/display-owner-kms.fragment` — selects the KMS
  driver, keeps the vendor stack on (DSIM/panel/window code + fallback), turns
  off the in-tree `DRM_EXYNOS` (its M1 G2D-only `card0` would steal the name),
  sets `CMA_SIZE_MBYTES=32` and keeps `FRAMEBUFFER_CONSOLE` off.

Both are merged onto `config-samsung-j7elte.aarch64` followed by
`olddefconfig`, like the M1 fragment (`patches/drm-m1/README.md`).

### 4.3 KMS-driver requirements (interface for `DeconCrtc`/`DsiEncoder`/`KmsFbdev`/`KmsAssemble`)

These are not optional; they are what makes the switch safe:

1. **Match the existing nodes only**: `samsung,exynos5-decon_driver`
   (`exynos7580.dtsi:1079`). Do **not** add `samsung,exynos7580-decon-drm`,
   and do **not** claim `dsim@14800000` (`:1170`) — the vendor DSIM driver at
   level 7 already owns it and the KMS encoder uses `dsim0_for_decon`
   (`dsim_drv.c:72-76`) plus the vendor `dsim_reg_*`/panel helpers. Note that
   the window API is currently `static`: `decon_set_win_config`
   (`decon_core.c:2185`), `decon_update_regs` (`:2066`) and
   `decon_set_win_buffer` (`:1516`). `DeconApi`'s shim must expose them
   (non-static wrappers in the vendor file, built-in, no `EXPORT_SYMBOL`
   needed).
2. **Register at `late_initcall`**, not `module_platform_driver`: level 7,
   `gpu/` link order after `video/`. The probe then runs after the panel is up.
3. **Create both nodes in the same probe**: `card0` (`drm_platform_init`) and
   `/dev/fb0` (`drm_fbdev_cma_init`, preferred bpp 32, 1 CRTC, 1 connector).
   `drm_fbdev_cma_init` only registers the fb; it does **not** commit a mode
   (`drm_fb_helper_single_fb_probe()` stops at `register_framebuffer()`,
   `drm_fb_helper.c:848-963`). With `FRAMEBUFFER_CONSOLE` off, the KMS driver
   must then, holding the modeset locks, do
   `drm_modeset_lock_all(); drm_fb_helper_restore_fbdev_mode(&fbdev->fb_helper);
   drm_modeset_unlock_all();` (`drm_fb_helper.c:285-300`,
   `drm_crtc.c:47-56`) so the CRTC actually scans the fb0 buffer out. If fb0
   init or the initial modeset fails, return an error from the probe.
4. Set the DECON context used by the vendor helpers (`decon_int_drvdata` is
   the existing global, `decon_core.c:65-66`) before calling them; the vendor
   `decon_probe` does not run in KMS mode.
5. No DT edits, no new compatibles, no `arch/arm64/boot/dts` hunks.

### 4.4 Device tree: the no-edit list

Nothing under `arch/arm64/boot/dts/` changes. The exact nodes that must stay
as-is:

| File | Lines | Content |
|---|---|---|
| `exynos7580.dtsi` | 1078-1079, 1152 | `decon_0` node, `samsung,exynos5-decon_driver`, `status="disabled"` |
| `exynos7580.dtsi` | 1169-1170, 1175 | `dsim_0` node, `samsung,exynos5-mipi-dsi`, `status="disabled"` |
| `exynos7580-j7elte_rev00.dts` | 1013-1020, 1022-1029 | board `status="okay"` for both nodes |
| `exynos7580-j7elte_rev04.dts` | 1009-1016, 1018-1025 | same |
| `exynos7580-j7elte_rev06.dts` | 1009-1016, 1018-1025 | same |

Build gate: the repacked boot image's DT section must hash to the stock value
(`352bda9b…`, `docs/reports/gpu31-m1-boot.md:38`); `mkboot.py` is called with
`/tmp/bimg/dt.img` unchanged (`RECOVERY.md:24-28`).

### 4.5 Boot parameter: no edit

`CONFIG_CMDLINE="console=ttySAC1"` and `CONFIG_CMDLINE_FORCE=y` stay
(`config-samsung-j7elte.aarch64:403-406`). The KMS and FBDEV images boot with
the identical command line; the only payload difference is the kernel.

## 5. What happens to Xorg / nura-shell

* **FBDEV image (KMS off)** — unchanged: `card0` absent (unless the separate
  M1 fragment is also applied), `/dev/fb0` from DECON, `code/experience/
  start-ui.sh` runs `fbwin` (ION + `S3CFB_WIN_CONFIG`, `fbwin.c:59,201`) and
  the pan daemon, then Xorg fbdev DDX + nura-shell. This is the rollback
  target and must stay bit-identical in behaviour.
* **KMS image, KMS probe succeeds** — `/dev/fb0` exists (DRM fbdev, 720x1280,
  32 bpp) and, after the driver's initial `drm_fb_helper_restore_fbdev_mode()`
  (§4.3 item 3), the CRTC scans it out, so Xorg's fbdev DDX can open
  and mmap it and nura-shell (plain X11, `docs/HARDWARE-BRINGUP-2026-11-16.md:
  323-341`) still works as a debug fallback. `fbwin`'s vendor
  `S3CFB_WIN_CONFIG` is **not** available (ENOTTY) and `start-ui.sh` must skip
  that step if X is used in this variant. Xorg and KWin must not run together:
  one CRTC, one master.
* **KMS image, KMS probe fails** — the vendor DECON driver binds at
  `late_initcall_sync`, recreating today's `/dev/fb0`; `fbwin`, Xorg and
  nura-shell behave exactly as in the FBDEV image. This is the property the
  previous attempt lacked.

## 6. Rollback runbook

### 6.1 Images

| Role | Path | Identity / check |
|---|---|---|
| **Primary rollback** (exact image) | `/tmp/pre-kms-boot.img` | raw `dd` copy of the working p10 taken immediately before the KMS flash (exactly 32 MiB, the p10 size); `sha256sum` recorded on host; writes back with the same `dd` command |
| **Last-resort known-good** | `/tmp/bindboot.img` | sha256 `fcec651e8af6f23e5b6d4ff552c2c6e4cf1336d8fc37a7c7ab08d3dd36f0f038` (`code/kernel/recovery/known-good.sha256`); keep a second copy |
| KMS test image | `/tmp/kms-boot.img` | built as §6.3 |
| FBDEV variant (rebuild, not a rollback until boot-verified) | `/tmp/int50-fb-boot.img` | choice FBDEV fragment |

"Do not use an unverified rebuild as the rollback image." The primary
rollback is the pre-existing working image; it was built the supported way
with the stock `dt.img` and `ramdisk-new.gz` (`RECOVERY.md:24-28`), and the
KMS image reuses exactly those sections, so both restore the fbdev path.

### 6.2 Before flashing (host, read-only device access)

Use the host SSH wrapper from `docs/SESSION-STATE.md` if password auth is not
already cached (`SSH_ASKPASS=/tmp/ap.sh setsid -w ssh …`); every device command
in this runbook runs under `flock /tmp/gpsdev.lock` and is executed by
`KmsBoot`, never by this slice.

```sh
# 1. Fallback present and verified on host
sha256sum /tmp/bindboot.img          # expect fcec651e…f038
ls -l /tmp/bindboot.img              # ~26 MiB, < 32 MiB

# 2. Save the exact working image from the phone (read-only; flock per policy,
#    binary-clean ssh, no device-side temp file)
flock /tmp/gpsdev.lock sh -c \
  'ssh user@172.16.42.1 "sudo -n dd if=/dev/block/mmcblk0p10 bs=512 2>/dev/null"' \
  > /tmp/pre-kms-boot.img
ls -l /tmp/pre-kms-boot.img            # expect 33554432 bytes (p10 = 32 MiB; confirm with ls)
sha256sum /tmp/pre-kms-boot.img > /tmp/pre-kms-boot.sha256
```

### 6.3 Build and pack the KMS image

```sh
# patch + choice fragment, then the normal build (code/kernel/build.sh)
#   patches/drm/0001-…            (KMS driver)
#   patches/drm-kms-coexist/0001-display-owner-kconfig.patch
#   patches/drm-kms-coexist/display-owner-kms.fragment
# repack with the STOCK dt.img and existing ramdisk:
python3 /tmp/mkboot.py /tmp/kms/Image /tmp/bimg/ramdisk-new.gz \
    /tmp/bimg/dt.img /tmp/kms-boot.img "$(cat /tmp/bimg/cmdline)"
```

Accept the image only if it is < 32 MiB (p10 is 32 MiB,
`RECOVERY.md:62-64`), has `ANDROID!` magic, and its `dt` section hash equals
the stock `352bda9b…`.

### 6.4 Flash and verify

```sh
scp /tmp/kms-boot.img user@172.16.42.1:/tmp/kms-boot.img
ssh user@172.16.42.1
sudo -n sh -c 'dd if=/tmp/kms-boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync'
sudo -n sh -c 'sync; reboot'
```

Post-boot gates (KMS image):

```sh
sudo -n dmesg | grep -iE 'exynos7580|decon|dsim|drm'   # KMS probe line present,
                                                       # no vendor "decon_probe start"
ls -l /dev/dri/                                        # card0 + controlD64
cat /sys/class/graphics/fb0/virtual_size               # 720,1280
modetest -M <kms-driver>                               # CRTC+plane+connected DSI connector
pgrep -a Xorg                                          # optional X fallback: X up at 720x1280
```

Also verify the fallback once with a throw-away KMS image whose probe returns
`-ENODEV` before touching hardware: `dmesg` must show the vendor
`decon_probe start`/`decon registered` and `/dev/fb0` must exist. That single
boot proves the safety net works.

### 6.5 Roll back

Trigger any of: no SSH/ping on `172.16.42.1` within 5 min; no `/dev/fb0` and
no `card0`; panel black with no compositor; `card0` present but `modetest`
reports no connected 720x1280 connector; oops/panic in `dmesg`.

```sh
# Running system still reachable:
scp /tmp/pre-kms-boot.img user@172.16.42.1:/tmp/rollback.img
ssh user@172.16.42.1
sudo -n sh -c 'dd if=/tmp/rollback.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync'
sudo -n sh -c 'sync; reboot'
```

```sh
# No boot / no USB (TWRP path, RECOVERY.md §3):
#   Power 20 s, then VolUp+Home+Power until TWRP
adb push /tmp/pre-kms-boot.img /data/pre-kms-boot.img
adb shell 'dd if=/data/pre-kms-boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync; reboot'
```

If the pre-KMS copy is lost, flash `/tmp/bindboot.img` with the same commands
(sha `fcec651e…f038`); never flash a DT payload. To keep KMS off permanently,
flash the FBDEV-variant image (`int50-fb`) after it has been boot-verified, or
simply stay on the pre-KMS image.

### 6.6 Switching back to KMS

Repeat §6.3-§6.4 with the KMS image; no DT, ramdisk or command-line change is
needed in either direction.

## 7. Risks and residual work

| Risk | Control |
|---|---|
| KMS probe succeeds but the DRM device fails later (no `card0`) | The standalone probe runs `drm_platform_init` synchronously; any `load()`/fb0 error returns from the probe, the device is unbound and the 7s vendor fallback binds. Gate: `card0` missing while the KMS platform device is bound fails the boot. |
| KMS probe fails after partially programming DECON/DSIM | Most failures (ioremap/clk/IRQ/power) happen before panel init; the vendor `decon_probe` performs a full re-init. The deliberate-failure boot (§6.4) is mandatory before trusting the fallback. |
| Ordering depends on link order | `video/` before `gpu/` (`drivers/Makefile:18,50`) and `.initcall7.init` order is link order; assert in dmesg that DSIM probes before the KMS bind and the vendor `decon_probe start` is absent. |
| fb0 in KMS mode is not the vendor fb0 | Xorg fbdev DDX + `FBIOPAN_DISPLAY` work (`drm_fb_cma_helper.c:236-245`); `S3CFB_WIN_CONFIG` does not. Do not run `start-ui.sh` unmodified in the KMS image. |
| KMS driver built as a module | Kconfig makes it `bool`; a module loads after the fallback and cannot own DECON. |
| CMA 32 MB on a 1.4 GB device | `MemBudget` slice to confirm; 32 MB is the minimum for the fb0/dumb buffers, not a final value. |
| KMS probe failure leaves the vendor driver as the owner but `DRM_EXYNOS` (M1) disabled | Intended: one card node only; the M1 card0 is not part of either variant. |

## 8. Hand-off

Files this slice commits:

* `docs/reports/decon-kms-coexist.md` (this report)
* `patches/drm-kms-coexist/0001-display-owner-kconfig.patch` (validated with
  `patch -p1 --dry-run` and content-matched against the post-state)
* `patches/drm-kms-coexist/display-owner-fbdev.fragment`
* `patches/drm-kms-coexist/display-owner-kms.fragment`

Not touched: any `.dts/.dtsi`, `config-samsung-j7elte.aarch64`, the vendor
`decon_7580` source tree, `patches/drm/`, `patches/drm-m1/`, and every other
slice's files. `KmsAssemble` must apply the patch after the KMS driver series
and enforce §4.3; `KmsBoot` owns the actual flash.
