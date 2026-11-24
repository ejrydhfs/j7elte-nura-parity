# int69: fbdev LUT hooks and object destroy hooks (module-only)

int68 advances the modeset (fb0 registered, the screen changes) and then hits
two NULL calls:

```
LR drm_fb_helper_setcmap+0x1ac [drm_kms_helper]   PC 0
LR drm_mode_config_cleanup+0x4c                   PC 0
```

## 1. `drm_fb_helper_setcmap()` → `crtc_funcs->load_lut()` (unguarded)

`setcmap()` ends with `crtc_funcs->load_lut(crtc)` where `crtc_funcs` is
`crtc->helper_private` (`drm_crtc_helper_funcs`); our table had no
`.load_lut`.  The fbdev framebuffer is truecolor (XRGB8888) so `setcolreg()`
only fills the pseudo-palette and returns early; there is no hardware LUT.
Added:

* `exynos7580_crtc_load_lut()` — DRM_DEBUG_KMS no-op, wired as `.load_lut`;
* `exynos7580_crtc_gamma_set()` — DRM_DEBUG_KMS no-op, wired as
  `crtc->funcs->gamma_set` (used by `drm_fb_helper_restore_lut_atomic()` and
  checked by `drm_mode_gamma_set_ioctl()`; the void signature of this 3.10
  struct).  `.gamma_get` does not exist in this tree's `drm_crtc_funcs` (the
  get-gamma ioctl reads `crtc->gamma_store`).

## 2. `drm_mode_config_cleanup()` → object `.destroy()` (unguarded)

Cleanup iterates encoders → connectors → framebuffers → planes → CRTCs and
calls each `funcs->destroy()` without a NULL check.  Our encoder table and
connector table had no `.destroy` (plane and CRTC already had one), so the
encoder iteration jumped to NULL first.  Added:

* encoder funcs `.destroy = drm_encoder_cleanup`;
* connector funcs `.destroy = drm_connector_cleanup`.

Both are exported by the built-in drm.ko (the import gate resolves them).
Plane `.destroy = exynos7580_plane_destroy`, CRTC
`.destroy = exynos7580_crtc_destroy`, and `.atomic_destroy_state` for CRTC,
plane and connector were already set.

## 3. Audit of the 3.10 fbdev/cleanup `funcs->` surface

| path | call | status |
|---|---|---|
| `drm_fb_helper_setcmap` | crtc helper `.load_lut` | added (was unguarded) |
| `setcolreg` (non-truecolor only) | fb_helper `gamma_set`/`gamma_get` | CMA helper leaves them NULL; our fb is truecolor so `setcolreg` returns early — documented, not guarded |
| `drm_fb_helper_restore_lut_atomic` | crtc `.gamma_set` | guarded; no-op added |
| `drm_mode_gamma_set_ioctl` / `_get_ioctl` | crtc `.gamma_set` (NULL-checked) / `gamma_store` | no-op added / core |
| `drm_mode_config_cleanup` | encoder/connector/plane/CRTC `.destroy` | all set now |
| fbdev restore/mode set | CRTC `.set_config`, `.page_flip` | present |
| cursor/mode ioctls | plane `.update_plane`, `.disable_plane` | present |
| probe | connector `.detect`, `.fill_modes` | present |
| atomic state | `.atomic_duplicate_state`/`.atomic_destroy_state` (CRTC/plane/connector) | present |

## Artifacts (module-only, keep the int66 image)

| file | size | sha256 |
|---|---|---|
| `drm_kms_helper-int69.ko` | 1443520 | `ddc02a7e68fa2ff93da83bf3ebaedcfc5a40f4dc608bf2b798a2c43436a31510` |
| `exynos-drm7580-int69.ko` | 1433832 | `ed9f40a0b6297885990bc5e72d6c03fc129be68a92dfc94d2e80c79f0e5b5529` |

Load helper, then driver.  `Image-int69` is for completeness only
(`01d0c72b…`, same 18058984 bytes; 0111 touches obj-m sources).

## Still open (built-in, non-fatal)

Unbalanced `enable_irq(558)` from `decon_init_done` + the first
`decon_enable()` after the shim's `INIT→OFF`; folded into the next image
rebuild (skip the ESD enable when `register_fb == false`, or make it
idempotent).

Log sha256s: build `c977de4232583d144cd99554798e914942efcc5454fce99a3046708737fbd4c2`, gates `20120a0a4d53e7a2df53b07b71709f225d35ecdab41a2056196c0ea6d845b29d`, evidence `bbccd7e5f302d77c86ed4590f8badddb608f27abc2870798b8d89a9a9275713c`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
