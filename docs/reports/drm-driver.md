# DRM/KMS driver: exynos7580

Status: **implemented and compiled; hardware modeset untested**.

## What was added

New driver `drivers/gpu/drm/exynos7580/`:

- `exynos7580_drm.c` - DRM device, mode config, CMA GEM/dumb buffers, fbdev
  emulation, vblank, legacy CRTC helpers.
- `exynos7580_decon.c` - DECON CRTC + primary plane; register access, clocks,
  power domain, scanout setup.
- `exynos7580_dsim.c` - MIPI DSIM master/D-PHY, encoder and connector; panel
  enable/disable sequencing.
- `exynos7580_drm.h` - shared state and prototypes.
- `Kconfig`/`Makefile` integration under `drivers/gpu/drm/`.

DT binding (rev00/04/06):

```
decon_fb@14830000 { compatible = "samsung,exynos7580-decon-drm"; ... };
dsim@14800000     { compatible = "samsung,exynos7580-dsim"; ... };
```

The driver finds the DSIM node via `of_find_compatible_node()` and uses the
existing `ea8061_wxga` panel, `decon_board_list` reset/power sequences,
clocks, regulators and power domains already present in the vendor DT.

## Configuration

`parity/patches/drm/config-drm-exynos7580.fragment`:

```
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_KMS_CMA_HELPER=y
CONFIG_DRM_EXYNOS7580=y
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_CMA_SIZE_MBYTES=32
# vendor fbdev path disabled
```

The vendor fbdev display stack and this driver claim the same DECON/DSIM
hardware; they must not both be enabled.  The fragment is opt-in so the vendor
fbdev path remains the fallback until the DRM modeset is validated.

## Build verification

Applied to a clean `b5e363a` worktree together with the pmaports build patches,
the camera patch, the Bluetooth DT patch and the DRM config fragment:

```
parity/final-out/arch/arm64/boot/Image   17,545,608 bytes
parity/final-out/arch/arm64/boot/dts/exynos7580-j7elte_rev00.dtb
parity/final-out/arch/arm64/boot/dts/exynos7580-j7elte_rev04.dtb
parity/final-out/arch/arm64/boot/dts/exynos7580-j7elte_rev06.dtb
```

Built objects:

```
drivers/gpu/drm/exynos7580/exynos7580_drm.o
drivers/gpu/drm/exynos7580/exynos7580_decon.o
drivers/gpu/drm/exynos7580/exynos7580_dsim.o
```

The built Image contains `samsung,exynos7580-decon-drm`; the built DTB contains
the DECON/DSIM compatibles and the BT `gpc4-2` group.

Patches (apply-checked against `b5e363a`):

- `parity/patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch`
- `parity/patches/drm/0002-arm64-dts-exynos7580-j7elte-drm-display.patch`

## Design notes

- 3.10 has no atomic helpers; the driver uses the legacy CRTC/encoder/connector
  helpers and a single primary plane.  This is sufficient for a KMS compositor
  using the legacy modeset path; modern atomic-only compositors may need
  additional work.
- CMA is used for dumb buffers (`drm_gem_cma_*`).  The stock config had
  `CONFIG_CMA_SIZE_MBYTES=0`; the fragment reserves 32 MB.
- The driver does not implement HDMI, overlays, rotation or the vendor MDNIE
  color path; the ea8061 panel init sequence and DECON register values are
  taken from the vendor `decon_7580` driver.
- The vendor fbdev driver remains available if the DRM path is disabled.

## Hardware test plan

1. Build with the DRM fragment; flash the resulting boot image.
2. Check `dmesg` for `exynos7580-drm` probe, DECON/DSIM clocks and panel init.
3. Confirm `/dev/dri/card0`; run `modetest -M exynos7580` or `weston`.
4. Verify 720x1280 modeset, fbdev console, suspend/resume and backlight.
5. If the panel stays black, capture the DSIM/DECON register state and compare
   with the vendor fbdev driver; the fallback is to re-enable the vendor path.

## Risks

- No hardware validation of the modeset; the panel power/reset timing is the
  most likely failure point.
- Legacy non-atomic KMS may not satisfy compositors that require atomic modeset.
- 32 MB CMA may be tight for large buffers; tune for the compositor used.
- Disabling the vendor fbdev driver also removes its framebuffer console until
  the DRM fbdev emulation takes over.
