# j7elte / SM-J700M — ROM ↔ Nura parity

Public snapshot of a driver-parity investigation for the Samsung Galaxy J7
(2015) **SM-J700M** (`j7elte`, Exynos 7580): stock Android 6.0.1 (kernel
3.10.61) vs Nura 3.10.108 and upstream Linux 7.3.

The short answer from the investigation: the stock ROM's drivers are the
usual Samsung/CAF downstream stack; upstream 7.3 has no Exynos 7580 platform
support. Nura's vendor 3.10.108 tree is the closest working base. Most driver
families are at parity; the gaps were in config/DT/firmware and userspace
bring-up, not missing vendor kernel code. The ROM was obtained from SamFW https://samfw.com/firmware/SM-J700M/PCW/J700MUBU2BRC2 

No phone was connected. Runtime claims below are limited to builds,
cross-compilation, static analysis and emulated selftests.

## Start here

| Path | Contents |
| --- | --- |
| [`docs/HANDOFF-SESSION.md`](docs/HANDOFF-SESSION.md) | Full session handoff: state, commands, next work, ownership |
| [`docs/HANDOFF-3D.md`](docs/HANDOFF-3D.md) | Mesa/kbase hardware-GL continuation |
| [`docs/SESSION-README.md`](docs/SESSION-README.md) | Original session README (deliverables and detailed status) |
| [`docs/reports/`](docs/reports/) | 46 per-subsystem reports with evidence and limits |
| [`patches/`](patches/) | 27 patches + series apply/header files |
| [`code/userspace/`](code/userspace/) | RIL, GPS, camera, sensors, audio, connectivity, GPU sources |
| [`code/pmaports/`](code/pmaports/) | 13 package recipes under `device/archived` |
| [`build/`](build/) | `Image`, 3 DTBs, `System.map`, kernel config, build/verify scripts |
| [`session/`](session/) | Full and no-artifacts omp session archives |

## Result

| Area | State |
| --- | --- |
| Kernel drivers | 11 families verified at parity; 3 patched (Wi-Fi config, Bluetooth DT/config, camera firmware fallback) |
| DRM/KMS | New DECON/DSIM/ea8061 stack; compiled; hardware modeset untested |
| pmaports | 18-patch series; 13 packages; 18/18 apply |
| RIL | Protocol 41 client, call/SMS/PDP core, SIPC5 codec, 415-subcommand catalog; edge messages remain |
| GPS | Power/NMEA + first BRI frames; software flow-control capture blocked |
| Camera | V4L2 capture + AE/AWB metadata loop; no JPEG/full 3A |
| Audio | UCM2 routing + automatic jack routing |
| Sensors / power / Wi-Fi / BT / FDE | Implemented userspace packages |
| GPU | Software GL; Mesa-on-kbase first milestone, not linked |
| Verification | `verify.sh`: 22 OK / 5 skipped / 0 failed; kernel build exit 0, `Image` 17,545,608 B; RIL selftest 44 exact frames |

## Reproduce

```bash
# Session checks
bash build/verify.sh

# Kernel build (paths inside the script point at the original workspace)
bash build/build.sh

# RIL selftest (aarch64 static build)
cd code/pmaports/device/archived/j7elte-ril
aarch64-none-linux-gnu-gcc -std=c11 -D_GNU_SOURCE -O2 -static \
  -o j7elte-ril j7elte-ril.c
qemu-aarch64 ./j7elte-ril selftest
```

## What is not published

To keep this repository code/documentation-only and free of proprietary
Samsung material, it excludes:

- the stock ROM zip, AP tar, `boot.img`, `recovery.img`, `system.img`/`raw`;
- extracted Samsung firmware (`firmware.tar.gz`) and any stock blobs;
- proprietary Mali/EGL (`libGLES_mali.so`) and RIL (`libsec-ril*.so`) binaries;
- full kernel and Mesa source trees, and compiler/build trees.

The patches, reports and userspace sources reference those inputs but do not
contain them.

## Session archives

Both archives are in [`session/`](session/) and attached to the
[`session-archive-2026-10-25`](https://github.com/ejrydhfs/j7elte-nura-parity/releases/tag/session-archive-2026-10-25)
release:

- `omp-archive-full.zip` (40 MB): main transcript, 63 subagent sessions,
  311 artifact/tool logs, viewer, manifest.
- `omp-archive-noartifacts.zip` (27 MB): transcript + viewer only.

Unzip and open `viewer.html`. The archives are unredacted: they contain local
paths, shell history, research context and personal filenames. They are
published by the owner's request.

## Provenance and licensing

Kernel patches are against GPL-2.0 kernel trees and are GPL-2.0. Upstream code
retains its original license. No additional license is granted for the
session-specific reports, notes or userspace code. Stock Samsung firmware and
proprietary binaries are not included. See [`NOTICE.md`](NOTICE.md).
