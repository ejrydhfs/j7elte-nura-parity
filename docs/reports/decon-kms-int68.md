# int68: guard the optional atomic-helper callbacks (module-only)

int67 dies inside our M0 helper backport:

```
PC is at 0x0
LR is at drm_atomic_helper_commit_post_planes+0x144/0x144 [drm_kms_helper]
```

`commit_post_planes` used `if (funcs->enable) … else funcs->commit(x);` for both
the CRTC and the encoder; `commit` is optional upstream and our DSI encoder has
neither `.enable` nor `.commit` (the link enable is in the CRTC), so the else
branch jumped to NULL.

## Fix (`0110-kms-atomic-helper-guard-optional-ops.patch`)

Call sites changed (all in `drivers/gpu/drm/drm_atomic_helper.c`, which is
`drm_kms_helper` = module here, so the image is untouched):

| function | before | after |
|---|---|---|
| `commit_post_planes` (CRTC) | `else funcs->commit(crtc)` | `else if (funcs->commit)` |
| `commit_post_planes` (encoder) | `else funcs->commit(encoder)` | `else if (funcs->commit)` |
| `disable_outputs` (encoder) | `else funcs->dpms(...)` | `else if (funcs->dpms)` |
| `disable_outputs` (CRTC) | `else funcs->dpms(...)` | `else if (funcs->dpms)` |
| `crtc_set_mode` (encoder) | `funcs->mode_set(...)` | `if (funcs->mode_set)` |
| plane commit | `else funcs->atomic_update(...)` | `else if (funcs->atomic_update)` |

Already guarded before: `prepare_fb`/`cleanup_fb`, `atomic_check`
(plane/CRTC/encoder), `atomic_begin`/`atomic_flush`, `mode_set_nofb`/CRTC
`mode_set`, `atomic_disable`.  Remaining unguarded calls are the ones upstream
makes mandatory (documented in the patch header):

* connector `.best_encoder` (`check_modeset`);
* encoder `.mode_fixup` when `.atomic_check` is absent, and CRTC `.mode_fixup`
  (both implemented here);
* `.prepare`/`.disable` are inside the if/else-if chain, `.dpms` only as the
  last guarded fallback.

The build gate forbids any bare `else` immediately followed by
`funcs->commit/dpms/mode_set/atomic_update`, counts the six guards, checks
`drm_atomic_helper_commit_post_planes` is in the `.ko` and absent from
`vmlinux`, and verifies 0110 touches only `drm_atomic_helper.c`.

## Artifacts (module-only, keep the int66 image)

| file | size | sha256 |
|---|---|---|
| `drm_kms_helper-int68.ko` | 1443520 | `95e77701ffd5d30b9ff4e2be294668c9b12b174a38f0d09616b6bf920955ac5f` |
| `exynos-drm7580-int68.ko` | 1431424 | `4ecc28a33253419a8b92dfbf243f351c42b51c4dcf4cf848a8d7d1a224dcf533` |

Load order: helper, then driver.  `Image-int68` exists for completeness
(`d390015a…`, same 18058984 bytes): 0109/0110 touch only module sources, so the
image code is unchanged.

## Still open (built-in, non-fatal)

The unbalanced `enable_irq(558)` remains: `decon_init_done` enables the ESD
IRQs and the first `decon_enable()` enables them again after the shim forces
`INIT→OFF`.  `enable_irq()` only warns and the IRQs stay enabled.  Fix folded
into the next image when we rebuild it: skip the `decon_esd_enable_interrupt()`
in `decon_init_done` when `register_fb == false`, or make the ESD enable/disable
idempotent.

Log sha256s: build `c628328370896eb907f9fd2e69b9d7c94d5ffe1430b49fb029128a41ffe2451d`, gates `34928e7536f96dbfcf711abf060aa26c5665a4f75e6b7881f2ff6210c817bc86`, evidence `653544df69c4214002c941953fc5aa8d5959982a8d409d7d3b8584f7c63350e0`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
