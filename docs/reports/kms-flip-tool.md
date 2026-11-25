# kmsflip: atomic KMS flip / dma-buf import test

`code/gpu31/kms/tools/kmsflip.c` is the flip test for the Exynos 7580
DECON bring-up. It drives the interface a compositor (KWin, Weston) uses:
`DRM_CLIENT_CAP_ATOMIC`, `drmModeAtomicCommit()` with plane `FB_ID` and a
`DRM_MODE_PAGE_FLIP_EVENT` completion, non-blocking page flips, and
dma-bufs imported with PRIME. It no longer uses the legacy
`drmModePageFlip()`/`drmModeSetCrtc()` path for the test itself (legacy
`SetCrtc` is only a restore fallback).

Two phases run per invocation:

| Phase | Buffers | What it proves |
|---|---|---|
| `dumb` | two `CREATE_DUMB` + `ADDFB2` XRGB8888 buffers, solid colours | atomic modeset (connector `CRTC_ID`, CRTC `ACTIVE`/`MODE_ID`, plane `FB_ID`/`CRTC_ID`/`SRC_*`/`CRTC_*`) and N async page flips retire with one DRM event each |
| `dmabuf` | two dma-bufs imported into card0, `ADDFB2`, same flips | the PRIME import + scanout path KWin uses for GPU-rendered buffers |

The dma-buf source is selected in this order and reported as `source=`:

1. `panfrost-gbm` — a linear GBM BO (`GBM_BO_USE_LINEAR|GBM_BO_USE_RENDERING`)
   allocated on the panfrost render node, `gbm_bo_get_fd()` exported,
   `drmPrimeFDToHandle(card0)` imported. Requires the Mesa 20 panfrost stack
   (`env.sh kms`); the stock Mesa 26 build on the image does not initialize
   panfrost for this kernel.
2. `panfrost-dumb` — a dumb buffer on the render node, exported with
   `drmPrimeHandleToFD()`. Not available on the current 3.10 kernel
   (`CREATE_DUMB` returns `ENOSYS` on `renderD128`), so this is a fallback
   for kernels that implement it.
3. `card-prime` — a card0 dumb buffer exported and re-imported into card0.
   This is our own PRIME export/import round-trip; it works because card0
   implements `CREATE_DUMB` + `PRIME` even when the render-node source is
   unavailable.

All buffers are XRGB8888 and are written through their CPU mapping before
each commit. After each completion event the mapping is checksummed again
(FNV-1a over the active pixels), so a buffer changing under scanout is
reported as `checksum_bad`.

## Build

Native on the device is the supported build (musl userland, and the host
cross sysroot has no aarch64 libdrm/GBM/EGL):

```sh
cd /data/kms/tools
make                 # KMS + GBM source + headless (links libdrm/gbm/EGL/GLESv2)
make NO_EGL=1        # libdrm only: no GBM source, no --headless
file kmsflip         # ELF 64-bit LSB pie executable, ARM aarch64, /lib/ld-musl-aarch64.so.1
```

Prerequisites on the device: `libdrm-dev` (and `mesa-dev` for the EGL/GBM
build). `build.sh` wraps the header checks and `file`/`ldd` printing.
`make check` runs the 3-frame headless GBM/EGL smoke test; `make probe`
sources `env.sh kms` when present and runs the source probe below.

## Exact invocations

### int49-class kernel: render node only, no card0

The image currently running `nura-j7elte 3.10.108` has
`/dev/dri/renderD128` but no `/dev/dri/card0`. The meaningful check there is
the dma-buf source probe:

```sh
. /data/kms/tools/env.sh kms
/data/kms/tools/kmsflip --probe --node /dev/dri/renderD128
```

Observed on that image (Mesa 20.3.5 panfrost from `env.sh`):

```
KMSFLIP: probe node=/dev/dri/renderD128 driver=panfrost7580 source=gbm buffers=2 prime_export=2 prime_import=2 import_err=0
KMSFLIP: RESULT PASS probe node=/dev/dri/renderD128 driver=panfrost7580 source=gbm buffers=2 prime_export=2 prime_import=2 import_err=0 commits=0 events=0 timeouts=0 ebusy=0 einval=0 other=0
```

Exit 0. `prime_import=2` is the render-node round-trip; the real card0
import is exercised by the `dmabuf` phase on a KMS kernel. Without
`env.sh kms`, GBM BO allocation fails and the render node has no
`dumb_create`, so `--probe` exits 1 with a hint to source `env.sh` (expected,
not a kernel regression).

A KMS invocation on a render-only kernel is a clean skip, exit 3:

```sh
/data/kms/tools/kmsflip --device /dev/dri/card0
KMSFLIP: RESULT SKIP reason=no-card0 device=/dev/dri/card0 hint="render-only kernel; run: kmsflip --probe --node /dev/dri/renderD128" commits=0 events=0 timeouts=0 ebusy=0 einval=0 other=0
```

Run both under the shared device lock and without touching the kernel:

```sh
flock /tmp/gpsdev.lock sh -c '. ./env.sh kms; ./kmsflip --probe'
```

### KMS kernel: card0 present

Free the framebuffer/compositor first if `xsession`/Weston owns the DRM
master (`rc-service xsession stop` on the OpenRC image), then:

```sh
. /data/kms/tools/env.sh kms
/data/kms/tools/kmsflip --device /dev/dri/card0 \
    --frames 10 --timeout-ms 250 --pattern solid
```

Useful variants:

```sh
kmsflip --device /dev/dri/card0 --no-dmabuf          # dumb phase only
kmsflip --device /dev/dri/card0 --prime-self         # skip render node, card PRIME round-trip
kmsflip --device /dev/dri/card0 --hold-ms 1000       # leave the last frame up 1 s
kmsflip --device /dev/dri/card0 --dump /tmp/f.ppm    # last frame artifact
kmsflip --device /dev/dri/card0 --list               # connectors/modes
```

`--timeout-ms` bounds each commit retry loop and each event wait.
`--hard-timeout-ms` overrides the automatic whole-run watchdog.

## Expected counters

A healthy run prints one line per phase and one summary line. The exact
numbers below are **expected** from the atomic property set the driver
registers; they have not been observed in this ticket because the device is
running the int49-class render-only image (no card0). Capture them on the
next KMS image.

`commits` counts every `drmModeAtomicCommit()` call, including EBUSY
retries; `commit_ok` counts accepted commits; `events` counts completion
events belonging to that phase; `flips` counts page flips that retired
(the modeset's own completion is an event but not a flip).

```
KMSFLIP: dev=/dev/dri/card0 connector=<n> crtc=<m> plane=<p> mode=720x1280 (720x1280@60) atomic=1 saved_fb=<f>
KMSFLIP: phase=dumb source=dumb modeset=1 flips=10/10 commits=11 commit_ok=11 events=11 timeouts=0 ebusy=0 einval=0 other=0 checksum_bad=0 addfb_fallback=0 elapsed_ms=~180 PASS
KMSFLIP: dmabuf source=panfrost-gbm path=gbm:/dev/dri/renderD128
KMSFLIP: phase=dmabuf source=panfrost-gbm modeset=0 flips=10/10 commits=11 commit_ok=11 events=11 timeouts=0 ebusy=0 einval=0 other=0 checksum_bad=0 addfb_fallback=0 elapsed_ms=~180 PASS
KMSFLIP: restore=atomic saved_fb=<f>
KMSFLIP: RESULT PASS device=/dev/dri/card0 connector=<n> crtc=<m> plane=<p> mode=720x1280 frames=10 phases=2 commits=22 commit_ok=22 events=22 flips=20/20 timeouts=0 ebusy=0 einval=0 other=0 checksum_bad=0 restore=ok elapsed_ms=~400
```

Expected relationships:

| Counter | Healthy value | Meaning of a deviation |
|---|---|---|
| `commits` | `flips + phases` (+ EBUSY retries) | fewer than `frames+1` means the tool aborted early |
| `commit_ok` | `flips + phases` | a mismatch is a lost/retried commit |
| `events` | `frames + 1` per phase | fewer: no flip completion event (dead vblank/flip path); more: events leaked across commits |
| `flips` | `frames/frames` | first failure stops that phase |
| `timeouts` | `0` | an event did not arrive inside `--timeout-ms` |
| `ebusy` | `0` | previous flip still pending; retried inside the timeout. A handful is tolerable, sustained EBUSY is a flip that never retires |
| `einval` | `0` | property/format rejected (missing/immutable prop, bad `ADDFB2`); the failing ioctl is logged |
| `other` | `0` | any other errno (EACCES/ENOMEM/…); logged |
| `checksum_bad` | `0` | CPU mapping changed while the buffer was on screen |
| `addfb_fallback` | `0` | `ADDFB2` rejected and legacy `ADDFB` used; the atomic path still works but the driver’s format handling should be checked |
| `restore` | `ok` | `fail` is a warning: flips may still PASS, but the panel is left on the last test frame |

Exit status: `0` PASS, `1` FAIL, `2` usage, `3` SKIP (no card0, or no
render node for `--probe`).

## Why it cannot wedge

Every blocking operation has a deadline:

* `drmModeAtomicCommit()` runs `NONBLOCK` for page flips; the initial
  modeset is blocking but is followed by the same event deadline.
* EBUSY commit retries stop at `--timeout-ms` (the attempt that exhausts
  the budget is counted in `commit_fail`, not `timeouts`).
* Every completion wait is `poll()` in <=20 ms slices with a monotonic
  deadline; on expiry the phase counts a timeout, drains stale events and
  stops.
* A whole-run `setitimer(ITIMER_REAL)` watchdog fires after
  `--hard-timeout-ms` (auto: ~5 s + 4x the worst-case phase time, capped at
  30 min). The first tick sets the abort flag; if a libdrm ioctl is stuck
  in an EINTR-retry loop, the repeating 2 s tick calls `_exit(124)`.
* Restore and `RmFB` are bounded: restore gets its own 5 s watchdog and
  `RmFB` is retried for at most 500 ms on EBUSY.
* `SIGINT`/`SIGTERM` set the same abort flag, so the run still restores the
  saved CRTC (or disables it if the CRTC was off before).

The tool saves the CRTC (`drmModeGetCrtc`) before the first commit and
restores its fb/mode with an atomic commit, falling back to `drmModeSetCrtc`
only if the atomic restore fails.

## Checking the flip path without a display

`--headless` remains available for the GPU userspace path (`run-headless.sh`).
Observed after this change with `env.sh egl` on the int49 image:

```
KMSFLIP-HEADLESS: GL_RENDERER="Mali T720 (Panfrost)" GL_VERSION="OpenGL ES 3.0 Mesa 20.3.5"
KMSFLIP-HEADLESS: node=/dev/dri/renderD128 size=720x1280 frames=3 stable=1 gbm=1 gbm_map=1 csum=0x016f30f2da294819 PASS
```

## Files

| File | Role |
|---|---|
| `code/gpu31/kms/tools/kmsflip.c` | atomic flip tool, dma-buf source probe, headless test |
| `code/gpu31/kms/tools/Makefile` | native build; `check` (headless), `probe` (source) targets |
| `code/gpu31/kms/tools/run-kms.sh` | card0 gate: list + modetest + `kmsflip` dumb/dmabuf + optional kmscube + dmesg delta |
| `code/gpu31/kms/tools/run-soak.sh` | 300 flips per phase + memory/thermal stamps + dmesg delta |
