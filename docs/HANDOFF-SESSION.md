# j7elte ROM ↔ Nura parity — full session handoff

Session root: `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session`
(23 GB). No phone/hardware was available; all runtime claims are limited to
builds, emulation and static evidence.

Read in this order:
1. `README.md` — deliverables and quick start.
2. `parity/HANDOFF-3D.md` — hardware-3D continuation (Mesa on kbase).
3. This file — whole-session state, remaining work, agent ownership.

## 1. Original question and result

Stock ROM: Samsung `SM-J700M` (`j7elte`), Android 6.0.1, Exynos 7580,
kernel `3.10.61`. Nura: downstream LineageOS `universal7580` 3.10.108, not
mainline. Upstream Linux 7.3 has **no Exynos 7580 platform support**.

Finding: the ROM's driver families are already present in the Nura fork.
The parity gaps were configuration, DT, firmware and userspace, not missing
driver code. Kernel material: 55,634 ROM kallsyms; 11 driver families at parity,
3 patched (Wi-Fi config, Bluetooth DT/config, camera firmware fallback).

## 2. What this session produced

| Workstream | Artifacts | Status |
|---|---|---|
| Kernel driver patches | `parity/patches/{bluetooth/0001,camera/0001,drm/0001,drm/0002}` (+ optional `userspace/gpu-panfrost/0001`) | applied and built |
| DRM/KMS display driver | `drivers/gpu/drm/exynos7580/` patch; DECON/DSIM/ea8061; `parity/out-drm`, `parity/final-out` | compiled; hardware modeset untested |
| Firmware package | `firmware-samsung-j7elte` (Wi-Fi/BT/FIMC-IS/MFC) | built/packaged |
| pmaports integration | 18-patch series: config, firmware, device deps, userspace packages | all apply to `ba682bd` |
| Audio | `j7elte-audio-ucm` (UCM2), `j7elte-audio-jack` (jack routing) | implemented |
| Sensors | `j7elte-sensors` D-Bus `net.hadess.SensorProxy`; UNIX_FDS fix | implemented, host-tested |
| Power | `j7elte-power` power_supply/UPower + capacity_max restore | implemented |
| Wi-Fi / BT | `j7elte-wifi`, `j7elte-bt` (firmware, services) | implemented |
| FDE | `j7elte-fde` (unl0kr/fbdev initramfs) | implemented |
| Camera | `j7elte-cam` V4L2 capture + optional AE/AWB metadata loop | basic capture + AE; no JPEG/full 3A |
| Telephony | `j7elte-cbd` CP loader; `j7elte-ril` protocol-41 SIPC5 client | core call/SMS/PDP implemented; edge gaps |
| RIL RE | `parity/userspace/ril/sipc5/` codec; `spec/{framing,messages,calls,sms,data}.md` | 34 main/415 subcommands mapped |
| GPS | `j7elte-gpsd` power/NMEA + first BRI handshake frames | partial; SW flow-control needs hardware |
| 3D (software) | `j7elte-gpu-soft` (Mesa llvmpipe + Xorg fbdev) | implemented |
| 3D (hardware) | `parity/userspace/gpu/kbase-winsys/`, Mesa patches `gpu-mesa-kbase/000{1,2,3}` | first milestone; full driver not linked |
| Reports | `parity/reports/` (46 files) | evidence and test plans |

Counts: 27 patch files, 46 reports, 13 new pmaports packages, 23 GB session.

## 3. Verification actually run

- `parity/verify.sh`: **22 OK, 5 skipped, 0 failed** (skips: GPS source patch,
  dependent sensor fix, 3 Mesa source patches).
- Full pmaports series: **18 patches apply** in order to a fresh `ba682bd`.
- Combined kernel build: **`BUILD_EXIT=0`**, `Image` **17,545,608 B** plus
  rev00/04/06 DTBs. DRM objects built; camera fallback and BT `gpc4-2` present.
- RIL: aarch64 static build, `selftest` under qemu-aarch64 passes with **44
  exact frame vectors**; `--dry-run` matches traced builders.
- Winsys: host/aarch64 builds; `make check-abi` passes; no-device selftest
  returns non-zero (no `/dev/mali0`).
- No hardware validation of display, camera, audio, GPS, RIL, or GPU.

## 4. Repository map

```
j7elte-session/
  README.md                     deliverables / quick start
  parity/
    HANDOFF-SESSION.md          this file
    HANDOFF-3D.md               Mesa-on-kbase continuation
    patches/                    kernel, pmaports, userspace, Mesa patches
    reports/                    46 evidence/test-plan reports
    pmaports/                   pmaports baseline + all new packages
    userspace/                  RIL, GPS, camera, sensors, audio, GPU code
    final-out/                  combined DRM kernel Image + 3 DTBs
    build.sh, verify.sh, contract.md
  android_kernel_samsung_universal7580-.../   clean kernel baseline
  boot_out/                     ROM kernel, symbols, DTBs
  system.raw, AP tar, linux-7.3/, toolchains/
```

## 5. Reproduce

Kernel (clean baseline `b5e363a` + pmaports build patches + parity patches):

```
./parity/build.sh parity/final-kernel-check parity/final-out Image dtbs
```

Full driver config (DRM opt-in):

```
CFG=parity/final-config ./parity/build.sh parity/final-kernel-check parity/final-out Image dtbs
```

pmaports: apply the 18-patch series in the order listed in `README.md` to a
`ba682bd` tree; `./parity/verify.sh` checks all patches.

Firmware: `parity/pmaports/device/archived/firmware-samsung-j7elte/` contains
`APKBUILD`, `firmware.tar.gz`, `files.sha512`, `extract-firmware.sh`.

RIL:

```
cd parity/pmaports/device/archived/j7elte-ril
aarch64-none-linux-gnu-gcc -std=c11 -D_GNU_SOURCE -O2 -static \
  -o j7elte-ril j7elte-ril.c
qemu-aarch64 ./j7elte-ril selftest
```

## 6. Remaining work (priority order)

1. **Hardware 3D (Mesa kbase).** Follow `parity/HANDOFF-3D.md`. First unblock
   the build (`libpan`/`mesa_clc`, enable EGL/GLES2), then fan out five slices:
   build/link, winsys/sync, Midgard submit, fbdev WSI, integration/tests.
2. **RIL edge messages.** Multi-part SMS/UDH/UCS2, TPDU field decoding,
   `IpcHistory` (main,sub,type)+msg_seq matching, LTE profile 13/0x14,
   call-list W4/E911 RAT/state-word, SIM record semantics. Core call/SMS/PDP
   is implemented in `j7elte-ril`; patch `0002` is the integrated package.
3. **GPS SW flow-control.** The first BRI handshake frames are byte-exact; the
   next step searches escape-byte pairs at runtime and needs a hardware UART
   capture (`--init --raw-log`).
4. **Camera JPEG/full 3A.** Capture + AE exist; JPEG encoder and full 3A HAL
   remain.
5. **Display hardware validation.** DRM/KMS compiles; modeset/panel/backlight
   untested; vendor fbdev is the fallback.
6. **Runtime validation of all userspace packages** on a phone.

## 7. Ownership and acceptance

- One integration owner per repository (kernel, pmaports, Mesa). Agents must
  not edit the same Mesa/APKBUILD files concurrently; use worktrees.
- A patch is accepted only if `git apply --check` passes against the stated
  base and the package builds with the aarch64 GCC 10.3 toolchain.
- No fake hardware results. "Code complete" is not "works".
- New packages go under `parity/pmaports/device/archived/<pkg>/`; patches under
  `parity/patches/<area>/`.

## 8. Do not redo

- Do not re-run the driver survey; symbol/DT/config coverage is in
  `parity/reports/*.md`.
- Do not use Panfrost/DRM for the Nura kernel; it is mainline-only.
- Do not re-add RIL patches `0003/0004/0005`; they were folded into the current
  `0002-j7elte-ril-package.patch`.
- Do not enable the vendor fbdev display and a DRM display stack together.
- Do not claim GL/rendering, telephony, GPS, or display without hardware tests.
