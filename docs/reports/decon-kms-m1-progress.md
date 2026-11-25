# DECON KMS — M1 progress (2026-11-25)

## Working on hardware (int79–int81)

| Capability | Evidence |
|---|---|
| Driver loads, `card0`+`controlD64`+KMS `fb0` persist | `/dev/dri`, `/sys/class/graphics/fb0` |
| DECON frame IRQ | `/proc/interrupts` 234 rising continuously (`VIDINTCON0=0x8803`) |
| libdrm enumeration | `modetest -c`: `DSI-1` connected, 720x1280@59.92; `-p`: CRTC 18 ACTIVE |
| Mode set + pattern | `modetest -s 20@18:720x1280` → "setting mode …" with no error |
| GEM mmap | `drm_gem_mmap ret=0`, `remap ret=0` (64-bit offset fix) |
| fbdev writes | `dd /dev/fb0` returns (~66 MB/s) |
| Locking | owner/depth `lock_all`; no leaks with a single client |

Fixed to get here: `late_initcall_sync` boot loop, devm double-free, drvdata-before-`decon_kms_get`,
module `R_AARCH64_PREL32` relocations, RPM_ACTIVE no-op clocks, NULL encoder `.mode_set`,
unguarded helper callbacks, NULL `load_lut`/destroy, ESD IRQ double-enable, vsync-thread stop,
fbdev postcondition fields, 32-bit mmap offset overflow, and `lock_all` nesting.

## Open

1. **`kmscube` still renders on llvmpipe** (`renderer: llvmpipe`) and then dies inside our
   `SETCRTC` path while holding `lock_all`; when that task exits the mutex is leaked and all
   later DRM ioctls block. Two separate issues: Mesa device pairing (kmsro should hand the
   scanout device to panfrost's render node) and the crash/leak in the atomic commit that
   `drm_mode_setcrtc` drives (capture the oops next).
2. **kwin_wayland not yet attempted** — needs (1) plus a stable flip path.
3. Userspace hygiene for tests: this image is OpenRC; `rc-service xsession stop` (not
   `systemctl`), otherwise `fbwin` holds the modeset lock across its own set_par path.

## Test harness (reusable)

- `/home/user/wdpet.sh` — hardware watchdog pet (30 s DT timeout): any full kernel hang resets
  the SoC instead of needing a battery pull.
- `/home/user/logd.sh` — persists filtered dmesg to `/home/user/kms.trace` every 2 s.
- `/home/user/kmstest.sh <rev>` — stops the UI, arms watchdog+logd, loads
  `drm_kms_helper-<rev>.ko` then `exynos-drm7580-<rev>.ko`, prints nodes/traces/IRQs.
- Rollback image: `kernel-boot-int49.img` (sha256 `62377e6573ed5045…`), UI + 155 fps GPU.

## Next steps (in order)

1. Capture the `drm_mode_setcrtc` oops (kmscube) with the persisted trace; fix the atomic
   commit path it hits; make a dead client unable to leak `lock_all` (release on file close /
   check owner liveness).
2. Make Mesa pick panfrost for GBM on `card0`: verify `DRIVER_PRIME` + `PRIME_FD_TO_HANDLE`
   on card0, `kmsro` driver-name match (`exynos`), and check whether the render node must be
   opened via `KWIN_RENDER_NODES`/`GBM_BACKEND`. Test with `kmscube -D /dev/dri/card0` and
   `EGL_PLATFORM=gbm`.
3. Run `kwin_wayland` against `card0` (no compositor yet), then the Plasma Mobile session.

## Update after int80–int82

Also working now:
- `modetest -s 20@18:720x1280` sets the mode cleanly (no gamma error, no hang) — DECON IRQ climbs.
- `dd /dev/fb0` returns (~66 MB/s).
- `kmscube` reaches our commit path (`exynos7580_atomic_commit: async=1`) and no longer oopses.

Remaining, with the latest evidence:
- **Page flip carries no buffer**: `atomic_commit: async=1 crtc->fb=0 primary->state->fb=0 by kmscube`
  → the 3.10 `drm_atomic_helper_page_flip` path does not install the flip fb into the plane state
  before our commit; `drm_mode_page_flip_ioctl`'s legacy `crtc->fb` guard and the atomic state must
  be reconciled in the adapter (next: set the plane state fb from the flip ioctl args in
  `exynos7580_atomic_commit`/page_flip wrapper, and verify `state->planes[i]` + `plane_states[i]`).
- **Mesa still picks llvmpipe** for GBM on card0 (no `fd_to_handle` trace → kmsro never imports into
  our device). Needs a Mesa-side pairing investigation (exynos kmsro ↔ panfrost) once flips work.
- A dead DRM client can leak `lock_all` (owner task exits while holding it) — release on file close
  or check owner liveness before blocking.

Test harness and rollback image are unchanged (`wdpet.sh`, `logd.sh`, `kmstest.sh <rev>`,
`kernel-boot-int49.img`). Note this image is OpenRC: stop the UI with
`rc-service xsession stop` (not `systemctl`).

## 2026-11-25 — Plasma Mobile is running

With int93 (blob MODE_ID, panfrost DRM 1.1 + modern CREATE_BO flags, dma-buf SEEK_END,
platform driver renamed `panfrost`, commit re-enable, ion unmap guard) plus the system Mesa 26:

- `kwin_wayland --drm` runs as an atomic compositor (markers: atomic-ams, planes-enumerated,
  gpu-added, gl-compositing, output-added; no commit failures; DECON frame IRQs rising).
- `startplasmamobile`-equivalent session (kwin + kactivitymanagerd + plasmashell with
  `PLASMA_PLATFORM=phone:handset`, `PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell`) shows the
  **“Welcome to Plasma Mobile”** initialization screen on the panel.
- Session recipe: `/home/user/plasma-boot.sh` (detached under `dbus-run-session`), env
  `XDG_RUNTIME_DIR=/data/plasma-run`, `KWIN_DRM_DEVICES=/dev/dri/card0`,
  `KWIN_RENDER_NODES=/dev/dri/renderD128`, `KWIN_COMPOSE=O2ES`.

Remaining for a usable phone: touch/input in the session, boot integration (load KMS modules and
start Plasma instead of X11/nura-shell), brightness/rotation, PipeWire (media), and the known
non-blocking issues (rmmod cleanup oops, occasional ion unmap trace, legacy page-flip adapter).
