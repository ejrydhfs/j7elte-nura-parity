# j7elte ROM <-> Nura driver parity session

Working directory for the Samsung Galaxy J7 (2015) `SM-J700M` (`j7elte`, Exynos 7580)
kernel driver parity work between the stock Android ROM and Nura (formerly postmarketOS).

Full session handoff: `parity/HANDOFF-SESSION.md`.
Hardware-3D continuation: `parity/HANDOFF-3D.md`.

## Device / sources

- Stock ROM: `SM-J700M` `J700MUBU2BRC2`, Android 6.0.1, kernel `3.10.61-13213628`.
- ROM kernel: raw arm64 `Image`, all drivers built in (no `.ko` modules),
  55,634 kallsyms symbols recovered.
- Nura kernel: `LineageOS/android_kernel_samsung_universal7580` @ `2a746c1`,
  downstream 3.10.108, baseline commit `b5e363a`. Nura is **not** mainline for this device.
- Upstream reference: Linux 7.3 (kernel.org), used only for the parity survey.

## Layout

```
input/SAMFW.COM_SM-J700M_..._fac.zip              original stock ROM zip
boot.img, recovery.img, system.img, system.raw   extracted ROM images
AP_J700MUBU2BRC2_..._user_low_ship.tar.md5       AP tar (extracted from the stock zip)
boot_out/                                        kernel.bin, vmlinux.elf, symbols.txt, dtbs/
android_kernel_samsung_universal7580-.../         Nura kernel tree (clean baseline)
linux-7.3/, linux-7.3.tar.xz                      upstream reference
toolchains/                                       GCC 10.3 aarch64 toolchain
config-nura-j7elte                                Nura build config (pristine)
lineage-kernel.tar.gz                             Nura kernel source archive
tools/                                            boot image / config comparison helpers
session/tmp-artifacts/                            initial /tmp scratch files
session/tmp-artifacts-late/                       late /tmp build artifacts
parity/                                           all deliverables (see below)
```

`parity/`:

```
patches/          7 exported patches (kernel + pmaports)
reports/          14 per-driver reports (P1-P4 evidence)
firmware/         extracted ROM blobs + per-driver file lists + MANIFEST.sha512
pmaports/         pmaports baseline ba682bd + uncommitted integration changes
build-src/        kernel worktree with build patches + parity kernel patches applied
out/              built Image and rev00/rev04/rev06 DTBs
build.sh          kernel build script (GCC 10.3)
verify.sh         applies every patch with git apply --check against the baselines
contract.md       parity definition and evidence rules
*.log             baseline / camera / final build logs
```

## Patches

Kernel (apply to the baseline kernel tree, commit `b5e363a`):

1. `parity/patches/bluetooth/0001-j7elte-bt-pinctrl.patch`
   - BT enable pin was `gpa1-6`; ROM uses `gpc4-2` on all three board revisions.
2. `parity/patches/camera/0001-fimc-is2-fallback-to-request_firmware-for-fw-version-check.patch`
   - FIMC-IS firmware version check hard-failed on `/system/vendor/firmware`;
     add a `request_firmware()` fallback so `/lib/firmware` works.
3. `parity/patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch`
   - new `drivers/gpu/drm/exynos7580/` KMS driver (DECON + DSIM + ea8061).
4. `parity/patches/drm/0002-arm64-dts-exynos7580-j7elte-drm-display.patch`
   - DT compatibles for the DRM driver on rev00/04/06.

pmaports (apply in this order to `parity/pmaports`, baseline `ba682bd`):

5. `parity/patches/wifi/0001-j7elte-wifi-firmware-paths.patch`
   - `BCMDHD_FW_PATH` / `BCMDHD_NVRAM_PATH` -> `/lib/firmware/postmarketos/samsung-j7elte/`.
6. `parity/patches/bluetooth/0002-j7elte-bt-hci-config.patch`
   - enable `CONFIG_BT_HCIUART` + `CONFIG_BT_HCIUART_H4` for BlueZ.
7. `parity/patches/pmaports/0003-firmware-samsung-j7elte-package.patch`
   - new `firmware-samsung-j7elte` package (Wi-Fi, BT, FIMC-IS, MFC blobs).
8. `parity/patches/pmaports/0005-linux-samsung-j7elte-config-sha512.patch`
   - updated config checksum after the two config changes.
9. `parity/patches/userspace/0001-device-deps.patch`
   - `device-samsung-j7elte` depends on the firmware and all userspace packages.
10. `parity/patches/userspace/audio/0002-j7elte-audio-ucm-package.patch`
11. `parity/patches/userspace/connectivity/0001-j7elte-wifi-package.patch`
12. `parity/patches/userspace/connectivity/0002-j7elte-bt-package.patch`
13. `parity/patches/userspace/sensors/0001-add-j7elte-sensors-package.patch`
14. `parity/patches/userspace/battery/0001-pmaports-add-j7elte-power-package.patch`
15. `parity/patches/userspace/gps/0001-pmaports-add-j7elte-gpsd-package.patch`
16. `parity/patches/userspace/ril/0001-j7elte-cbd-package.patch`
17. `parity/patches/userspace/camera/0001-pmaports-add-j7elte-cam-package.patch`
18. `parity/patches/userspace/fde/0001-j7elte-fde-package.patch`
19. `parity/patches/userspace/gpu/0001-j7elte-gpu-soft-package.patch`
   - software GL (Mesa llvmpipe + Xorg fbdev); pulled in by the device package.
20. `parity/patches/userspace/audio/0003-j7elte-audio-jack-package.patch`
21. `parity/patches/userspace/ril/0002-j7elte-ril-package.patch`

Dependent/optional patches:
- `parity/patches/userspace/review-a/0001-j7elte-sensors-ignore-unknown-dbus-header-fields.patch`
  (apply after `sensors/0001`; required for GDBus clients)
- `parity/patches/userspace/gpu-panfrost/0001-arm64-dts-exynos7580-add-mali-t720-compatible.patch`
  (optional kernel DT string for a future Panfrost driver; kbase still binds)
- `parity/patches/gpu-mesa-kbase/000{1,2,3}-*.patch`
  (Mesa source patches: panfrost kmod kbase backend, JM submit, screen/context
  skeleton; apply to Mesa main, not pmaports/kernel)

The GPS daemon source is also exported as
`parity/patches/userspace/gps/0002-userspace-add-j7elte-gpsd-source.patch`
(source tree; `verify.sh` skips it because it targets `parity/userspace/`).

DRM config fragment (opt-in, used for the final build):
`parity/patches/drm/config-drm-exynos7580.fragment`.

Verify everything:

```
cd parity
./verify.sh          # 22 patches OK; 5 skipped (source-tree, dependent fix, Mesa patches)
```

## Build

Base build (vendor fbdev display, camera + BT patches):

```
./parity/build.sh parity/build-src parity/out Image dtbs
```

Result: `BUILD_EXIT=0`, Image 17,365,040 B, three j7elte DTBs under `parity/out/`.

Final combined build (adds the DRM/KMS driver; opt-in config):

```
CFG=parity/final-config ./parity/build.sh parity/final-kernel-check parity/final-out Image dtbs
```

- `parity/final-kernel-check` is a clean `b5e363a` worktree with the pmaports
  build patches (GCC 10, fix-argb, sun/Solaris flag, etc.) plus the Bluetooth,
  camera and DRM kernel patches.
- `parity/final-config` is the WiFi/BT kernel config plus
  `parity/patches/drm/config-drm-exynos7580.fragment` (DRM, 32 MB CMA, vendor
  fbdev display path disabled).
- Result: `BUILD_EXIT=0`, Image **17,545,608 B**, three j7elte DTBs under
  `parity/final-out/`, `exynos7580_{drm,decon,dsim}.o` built, camera fallback
  and BT `gpc4-2` group present in the outputs.

Keep the vendor fbdev path as the fallback; do not enable both display stacks
at once.

## Firmware package

`parity/pmaports/device/archived/firmware-samsung-j7elte/` contains:

- `APKBUILD` (deterministic `firmware.tar.gz`, sha512 pinned)
- `extract-firmware.sh` (rebuilds the tarball from the stock AP tar; verifies `files.sha512`)
- `files.sha512` (per-blob checksums)

Install destinations:

- `/lib/firmware/postmarketos/samsung-j7elte/` - bcmdhd firmware/NVRAM, BT HCD
- `/lib/firmware/` - `fimc_is_fw2_3l2.bin`, `setfile_3l2.bin`, `setfile_5e3.bin`,
  `fimc_is_fd.bin`, `mfc_fw.bin`

## Kernel driver parity (first round)

- 11 driver families at parity: display, battery, audio, sensors, storage, touch,
  USB, GPS, GPU, platform (thermal/devfreq/IOMMU/cpufreq), modem.
- 3 driver families patched: Wi-Fi (config), Bluetooth (DT + config), camera (driver fallback).
- The ROM's driver set is already present in the Nura fork; no driver code was missing
  for those families.

## New DRM/KMS driver (display)

- `parity/patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch`
  adds `drivers/gpu/drm/exynos7580/` (DECON CRTC/plane, DSIM encoder/connector,
  ea8061 panel init, dumb buffers + fbdev).
- `parity/patches/drm/0002-arm64-dts-exynos7580-j7elte-drm-display.patch`
  adds `samsung,exynos7580-decon-drm` / `samsung,exynos7580-dsim` to rev00/04/06.
- `parity/patches/drm/config-drm-exynos7580.fragment` enables DRM,
  `CONFIG_DRM_EXYNOS7580`, 32 MB CMA, and disables the vendor fbdev display path.
- Verified build: `parity/final-out/arch/arm64/boot/Image` (17,545,608 B) + three
  j7elte DTBs; `exynos7580_{drm,decon,dsim}.o` are built and the DTBs contain the
  new compatibles.
- Hardware-untested. Keep the vendor fbdev driver as the fallback until a real
  modeset is validated on the phone.

## Userspace gap packages

New pmaports packages, all pulled in by `device-samsung-j7elte` depends:

| Package | Gap | Status |
|---|---|---|
| `j7elte-audio-ucm` | UCM2 routing/mixer for COD3022X (54/54 controls validated) | implemented |
| `j7elte-audio-jack` | `sec_jack` uevent -> UCM2 speaker/headphone/headset-mic routing | implemented |
| `j7elte-wifi` | wpa_supplicant template, `/efs/wifi` MAC provisioning | implemented |
| `j7elte-bt` | rfkill + hciattach + bluetoothd service, BCM43438A1.hcd | implemented |
| `j7elte-sensors` | K2HH/CM36672P/STK3013 to `net.hadess.SensorProxy`; D-Bus UNIX_FDS fix | implemented |
| `j7elte-power` | power_supply/UPower integration, capacity_max restore | implemented |
| `j7elte-fde` | unl0kr/fbdev initramfs integration for FDE | implemented |
| `j7elte-gpu-soft` | Mesa llvmpipe + Xorg fbdev software GL (installed by device package) | implemented (software only) |
| `j7elte-gpsd` | BCM4752 power + NMEA forwarder + first BRI handshake frames | partial: SW flow-control step unresolved |
| `j7elte-cam` | V4L2 capture + optional AE/AWB metadata loop | implemented (basic capture + AE; no JPEG/3A HAL) |
| `j7elte-cbd` | CP boot loader for `/dev/umts_boot0` | partial: RIL layer blocked |
| `j7elte-ril` | SIPC5 transport, IMEI, SIM lock, network state | partial: calls/SMS/data not implemented |
| `firmware-samsung-j7elte` | Wi-Fi/BT/FIMC-IS/MFC blobs | implemented |

Details and test plans are in `parity/reports/*-userspace.md`.

## Second-round verification

- 18 pmaports patches (firmware, kernel config, all userspace packages, RIL,
  audio jack, the dependent sensor D-Bus fix, and device deps) apply cleanly in
  order to a fresh `ba682bd` worktree.
- Combined kernel build with camera + Bluetooth + DRM patches and the DRM config
  fragment: `BUILD_EXIT=0`, `Image` 17,545,608 B, three DTBs.
- The built Image contains the camera `request_firmware` fallback and the
  `samsung,exynos7580-decon-drm` compatible; the built DTB routes `bluetooth`
  pinctrl to `gpc4-2` and contains the DRM display compatibles.
- Verification is static plus cross-build; no phone was available for runtime tests.

## Remaining blockers

- **GPU / 3D (hardware)**: the target is the Nura 3.10 **kbase** kernel, not
  mainline. Panfrost is therefore not the Nura path: it requires mainline DRM
  (absent in 3.10) and mainline has no Exynos7580 support. The Panfrost M0/LPAE
  work is a feasibility artifact only. Chosen path: **Mesa kbase backend** for
  UK10.6 (estimated ~3.5-6 person-months to full GL, vs 6-18 for mainline
  Exynos+Panfrost). First milestone delivered: standalone kbase winsys
  (handshake/SAME_VA/soft-job/event/submit), a panfrost kmod kbase backend, and
  a Gallium screen/context skeleton; the Mesa patches apply to main. Remaining:
  WSI/dma-buf/sync_file, full Mesa link (needs LLVM/mesa_clc), Midgard chain
  validation and hardware bring-up. No GL rendering is claimed.
  This avoids the r7p0->r15p0 UAPI shim, the 32-bit bionic runtime, libhybris,
  gralloc/HWC and the Android blob. The archived alternatives were ARM's r15p0
  Linux DDK (closed/absent) and the Android-blob libhybris path; a blob
  decompilation inventory remains as reference. The blob track proved the blob
  loads under a freestanding ELF32 loader (106,993 relocations, constructors,
  EGL display creation) and its submit path is 32 B/56 B atom (r7p0) vs the
  kernel's 40 B/64 B (r15p0), but it stops at gralloc/ion//dev/mali0.
  Software GL is installed by the device package (llvmpipe + Xorg fbdev); it is
  not Mali acceleration. Continuation handoff for the next agent:
  `parity/HANDOFF-3D.md` (state, build blockers, 5-agent split, test plan).
- **Telephony (calls/SMS/data)**: `j7elte-cbd` boots the CP and `j7elte-ril`
  implements the protocol-41 SIPC5 client: IMEI, SIM/PIN status, network
  registration, SMS device-ready, radio/modem power. The SIPC5 link/FMT framing
  and transport codec are reverse-engineered in `parity/userspace/ril/sipc5/`.
  The 415-subcommand catalog is in `parity/userspace/ril/spec/messages.md`.
  Implemented: full protocol-41 client; calls (originate A/B, answer, release,
  list, hold, DTMF, flash, voice privacy, modify/time, E911/SRVCC/block/
  transfer/manage-call, call-list upload, SND audio); SMS (SMSC, text send via
  GSM 03.40 encoder, read/delete/ack, auto-ack, incoming parser, SIM write);
  GPRS (define-PDP, PDP activate/deactivate/actions, PS-attach, IPv6,
  mobile-data).
  Remaining edge: multi-part SMS/UDH/UCS2, TPDU field decoding, IpcHistory
  response matching, LTE profile, call-list W4/E911 RAT/state-word. No hardware
  validation of any frame.
- **GPS**: power/NMEA forwarding and the first BRI handshake frames (autobaud,
  `Bri_GetOutOfLegacy`, `Bri_RequestHwVersion`, `Bri_RequestDebugSfr`) are
  implemented byte-exact from the ROM binary; the sequence stops at Broadcom's
  SW flow-control search, which needs a hardware UART capture.
- **Camera**: basic capture + optional AE/AWB metadata loop; JPEG and the full
  3A HAL are not implemented.
- **Display**: the DRM/KMS driver compiles; a hardware modeset is still untested.

## Notes

- The original stock zip is at `input/SAMFW.COM_SM-J700M_PCW_J700MUBU2BRC2_fac.zip`
  (moved into the session folder).
- All `/tmp` session scratch is under `session/tmp-artifacts*/` (initial 218
  entries plus the later build artifacts); no session files remain in `/tmp`.
- Verification is static plus cross-build; no runtime test on the phone was possible here.
