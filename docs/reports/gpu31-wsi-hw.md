# gpu31-wsi-hw — hardware WSI path for the render-only node (mock-proven)

Date: 2026-11-19. Owner: G31_MesaWsiHw. Target: panfrost v5.2 on
`/dev/dri/renderD128` (Mode B, G31_Kms). No kernel tree edited, no
flash/reboot, no device writes — host-side sources + host proof only.

Companion: `gpu31-wsi.md` (software path, proven on device: EGL-X11 +
llvmpipe, `RENDER_OK`, on-screen crop). This slice prepares the
**hardware** path that replaces it once panfrost registers.

## 1. Deliverables

`code/gpu31/wsi/` (extends the existing `tri-x11` demo dir):

- `wsi-hw.c` — hardware WSI path probe. Three live probes, one pure
  decision, exact Mesa knobs:
  1. **node identity**: `DRM_IOCTL_VERSION` driver name + v5.2
     `GET_PARAM(GPU_PROD_ID)` (UAPI mirror of
     `code/gpu31/gem/panfrost_drm.h`; H1 iff driver is `"panfrost"`,
     T720 iff prod id `== 0x0720`). Full ioctl coverage (SUBMIT /
     WAIT_BO / CREATE_BO / MMAP_BO) stays with
     `code/gpu31/client/pf7580` — this tool only classifies.
  2. **GBM on the render fd**: `gbm_create_device(fd)` (fd comes from
     opening the node — never `EGL_DEFAULT_DISPLAY`), a 64×64
     `XRGB8888` `SCANOUT|RENDERING` test BO (stride readback),
     dma-buf export (`gbm_bo_get_fd`), CPU map/unmap round trip
     (LINEAR sibling retry; informational only, never in the decision).
  3. **X11 server caps**: MIT-SHM (`XShmQueryExtension`) and
     DRI3/Present (`XQueryExtension`) — decides pixmap import vs shm
     fallback. X pixmap import itself needs no new code: Mesa's
     `panfrost_dri.so` DRI3 path owns it.
- `Makefile` — extended: `all` builds `tri-x11` + `wsi-hw`
  (`-lgbm -lX11 -lXext`; device needs `mesa-gbm` — already installed
  per `gpu31-wsi.md` §4 — plus X11 dev headers), `check` runs the
  `tri-x11` smoke + `wsi-hw --selftest`.

`code/gpu31/mesa/` (new dir; scope-split agreed with G31_Mesa3, who owns
`patches/0004-0006`, `aarch64-panfrost-build.sh`, `slot-proof/`,
`build.log` — none of those names are touched here):

- `wsi-knobs.sh` — exact Mesa knobs for the HW path: unset
  `LIBGL_ALWAYS_SOFTWARE` (shipped `=1` by `j7elte-gpu-soft`, §4),
  `MESA_LOADER_DRIVER_OVERRIDE=panfrost`, unset `DRI_PRIME` (single
  GPU), unset `PAN_MESA_DEBUG` (first-HW-run validation only:
  `trace,sync`), unset `EGL_LOG_LEVEL`/`LIBGL_DEBUG` (debug runs:
  `debug`/`verbose`). `--print` / source-to-apply / `--check`
  (fails 3 while `LIBGL_ALWAYS_SOFTWARE=1` or the node is absent).

## 2. Decision (pure `wsi_decide()`, 11-case matrix in `--selftest`)

| inputs (h1 t720 bo export dri3 shm) | path |
|---|---|
| H1 T720 BO + export + DRI3 | `GBM_PIXMAP` — render on node, present via DRI3 pixmap-from-buffers |
| H1 T720 BO + SHM (no DRI3, or no export) | `GBM_SHM` — render on node, present via `XShmPutImage` fallback |
| anything else (no H1 / no T720 / no BO / no present) | `NONE` — keep `tri-x11` (EGL-X11 + llvmpipe) |

Expected on j7elte: **`GBM_SHM`**. The fbdev X (`xf86-video-fbdev`)
has no DRI3, so the pixmap-import leg cannot light; SHM is advertised
by the running server today (`gpu31-wsi.md` §1). DRI3-not-present is
harmless and recorded, same class as the earlier DRI3 warning note.

## 3. Proof (host, 2026-11-19 — no panfrost hardware exists yet)

- `--selftest`: **PASS** (11 matrix rows + `/dev/null` live negative:
  not a DRM node, never claims H1, decides `NONE`). Zero warnings
  (`-Wall -Wextra -Werror`).
- Host render node (nvidia-drm, unrelated GPU): driver=`nvidia-drm`
  h1=0 t720=0; GBM device ok, test BO ok (stride=256, export=1,
  map=1 after LINEAR retry — the non-linear BO does not CPU-map on
  this driver, which is exactly why the retry exists); X11 open ok
  (shm=1 dri3=1 present=1); decision `NONE`, exit 3. Correct: a
  non-panfrost DRM node must never select a HW present path even when
  GBM+X11 are fully working. Standard-Mesa knob line verified against
  this run: pin is `MESA_LOADER_DRIVER_OVERRIDE=panfrost` (no custom
  `LIBGL_DRIVERS_PATH`; Alpine `mesa-dri-gallium` ships
  `panfrost_dri.so`).
- `wsi-knobs.sh --print/--check`: PASS incl. `LIBGL_ALWAYS_SOFTWARE=1`
  negative (exit 3); `sh -n` clean.

## 4. Runtime expectation once panfrost registers (device runbook)

```
./wsi-hw --node /dev/dri/renderD128        # expect: driver='panfrost'
                                           # h1=1 t720=1, gbm ok, x11 shm=1
                                           # dri3=0 -> decision GBM_SHM, exit 0
. ./wsi-knobs.sh  # or: eval "$(./wsi-knobs.sh --print)"
./wsi-knobs.sh --check                     # expect READY, exit 0
```

Then run Mesa clients through the EGL GBM platform
(`eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_dev)`, headless first
via `EGL_PLATFORM=surfaceless` / `glmark-harness/run-m3.sh`); the
GLES2 draw/readback core of `tri-x11` is reusable against the GBM
surface. Modern-Mesa extra-param/`MADVISE` probes need no knob: the
v5.2 kernel answers `-EINVAL` and Mesa falls back (G31_Mesa 0005
compat — never widen the kernel UAPI). `LIBGL_ALWAYS_SOFTWARE=1`
removal (and `j7elte-gpu-soft` retirement) stays gated on M3 PASS per
`gpu31-runbook.md` A9; this slice does not change the shipped default.

## 5. What is NOT claimed / not in scope

- No on-device run (node does not exist on kernel #35/#37 yet; M1/M2
  gated, G31_M1Verify-owned). No `renderD128` on the build host was
  used as a stand-in: the host node is nvidia-drm and correctly
  decides `NONE`.
- No X pixmap-import code, no EGL client, no Mesa backend change
  (G31_Mesa), no kernel code (G31_Gem/G31_Kms/G31_DrmCore), no
  dma-buf/sync-file bridge (known H2-gap, `gpu-mesa-kbase.md` §6).
- Review (`gpu31-review.md` @ 625ed18): this slice adds no kernel
  code and touches no reviewed file; prior `gpu31-wsi` disposition
  (53f1b35: no finding, EGL client only) applies unchanged.
