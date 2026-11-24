# int67: DSI encoder `mode_set` (module-only), plus the ESD IRQ warning

int66 reaches a working modeset (`fb0: drmfb frame buffer device`) and then
faults on a NULL encoder op.  int67 fixes that with a module-only change, so
the int66 kernel stays flashed and only the two `.ko` files change.

## 1. NULL encoder op (fatal) — fixed

`drm_atomic_helper`'s `crtc_set_mode()` in this tree calls
`encoder->funcs->mode_set(encoder, mode, adjusted_mode)` **unconditionally**
(`drivers/gpu/drm/drm_atomic_helper.c:753`).  The DSI encoder only had
`.disable` and `.mode_fixup`, so the first modeset jumped to `0` with
`crtc_set_mode.isra.0+0x144 [drm_kms_helper]` as the LR.

`0109-kms-dsi-encoder-mode-set.patch` adds
`exynos7580_dsi_encoder_mode_set()` (a `DRM_DEBUG_KMS` no-op; the DSI link and
DECON programming live in the CRTC's `enable_link()`) and wires it as
`.mode_set`.

Audit of every encoder/CRTC helper callback the 3.10/4.0 atomic helper calls
around a modeset, so the next one is not hit blind:

| call site | guarded? | our state |
|---|---|---|
| encoder `.mode_set` (crtc_set_mode) | **no** | added by 0109 |
| encoder `.prepare`/`.disable`/`.dpms` (disable_outputs) | if/else-if, `.dpms` only when both absent | `.disable` exists → `.dpms` never taken |
| encoder `.enable`/`.commit` (commit_modeset_enables) | `if (funcs->enable) else if (funcs->commit)` | both optional, neither set |
| encoder `.mode_fixup` (check_modeset) | called when `.atomic_check` absent | exists |
| CRTC `.mode_set`/`.mode_fixup`/`.disable`/`.enable` | guarded / implemented | implemented |
| CRTC `.mode_fixup` (check_modeset) | **no** | `exynos7580_crtc_mode_fixup` exists |

So the only unguarded hooks are the encoder `.mode_set` (fixed) and the CRTC
`.mode_fixup` (present).

## 2. Unbalanced `enable_irq(558)` (WARN) — built-in only, not fixed here

This one cannot be fixed from the module: it is produced entirely inside the
built-in vendor stack.

* `decon_probe_common()` reaches `decon_init_done` (our `register_fb == false`
  skip lands there) and calls `decon_esd_enable_interrupt(decon)`, which
  `enable_irq()`s the ESD IRQs (`pcd_irq`/`err_irq`/`disp_det_irq`, requested
  and initially `disable_irq_nosync()`d by the ESD-IRQ setup).
* the first modeset runs `exynos7580_crtc_enable_link()` →
  `decon_kms_enable()`; the shim drops `DECON_STATE_INIT` to `OFF` and calls
  the vendor `decon_enable()`, which calls `decon_esd_enable_interrupt()`
  again (`decon_core.c:1007`) → `enable_irq()` on an already-enabled IRQ.

In the vendor fb0 path this cannot happen because `decon_enable()` short-
circuits `DECON_STATE_INIT` and the fbdev blank/set_par sequence keeps
enable/disable balanced.  The warning is non-fatal — `enable_irq()` warns and
returns, the IRQs stay enabled — so the modeset continues.

Proposed fix for a later image (built-in, one of):
* skip `decon_esd_enable_interrupt()` in `decon_init_done` when
  `register_fb == false` (the KMS stand-up; `decon_enable()` enables them), or
* make `decon_esd_enable_interrupt()`/`decon_esd_disable_interrupt()`
  idempotent with an `esd_irq_on` flag set/cleared by both.

Not applied: int67 keeps the int66 image.

## Artifacts

Module-only iteration: **keep the int66 image**, replace the two `.ko`:

| file | size | sha256 |
|---|---|---|
| `exynos-drm7580-int67.ko` | 1431424 | `846944dca8fdc73656fc7e19a6b093cfc769a8ae7aa10a46055b5db08f99b98f` |
| `drm_kms_helper-int67.ko` | 1443368 | `3cda2674016bbadbdfe85310447e6730cdc23c6ec7457d807287e2ab2d235f75` |

(helper code is unchanged from int66's; the rebuild's sha differs only through
build layout.  Either helper loads, but use the pair built together.)

int67 image (code-identical — 0109 touches only
`drivers/gpu/drm/exynos7580/`, which is `obj-m`; the size is the same
18058984 bytes; a gate checks the patch's file list):

| file | sha256 |
|---|---|
| `Image-int67` | `11ce8428fe66cda13e980568ab46f5d113a0fbca51aa3006fb54303225554199` |
| `kernel-boot-int67.img` | `32f97a6f1af971b0fdaea98f15a26eaf0c4b9ccc13cba97fdbe25ea9cb6088e7` |

Load order: helper, then driver.  Gates PASS: all int66 gates plus the encoder
hook (implementation, `.mode_set` wiring, log string in the `.ko`) and the
module-only patch-path check.

Log sha256s: build `c76939da168a964083a89195b1f35167d93f08ae4561e69a24830baadb084261`, gates `3ce26975594c6fc1874c1b82ec845084666a18999545b7b955d66a5aeeb4a7d8`, evidence `6d137422ca5dcfc32d2f7886a3976c92a17bfbe75838d089c3d3029dbc74ba51`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
