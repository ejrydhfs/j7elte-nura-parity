# int80: nest the legacy lock_all correctly; gamma_size; one frame per state

int79 was the last mmap fix: `drm_gem_mmap ret=0`, `remap ret=0`,
`modetest -s` sets 720x1280 on connectors 20/crtc 18 with IRQ 234 climbing
(125→182), `dd` to `/dev/fb0` returns.  Then the *second* userspace modeset
blocks:

```
kmscube: drm_modeset_lock_all+0x84 <- drm_mode_setcrtc
```

## 1. Legacy `lock_all` nesting (0126, built-in)

int77's `drm_modeset_lock_all()` installs `config->acquire_ctx` on every call.
The legacy chains nest (ioctl lock_all → set_config → commit → helper paths),
and a nested lock_all **overwrote** `config->acquire_ctx`; the inner
unlock_all then released the outer locks, and the outer unlock_all (ctx now
NULL) only unlocked `config->mutex` — leaving every `crtc->mutex` locked once.
The next `drm_mode_setcrtc` blocked exactly there.

  - `struct drm_mode_config` gains `unsigned int lock_all_depth`;
  - nested lock_all: if `acquire_ctx` is already set, count and return without
    taking any lock;
  - unlock_all: decrement for nested releases, and only the outermost call
    drops the ctx, the legacy crtc mutexes and `config->mutex`;
  - both log comm/depth/ctx (`nested by`, `nested release by`), so a mismatch
    stays visible.

## 2. `gamma_size` (0127, module)

`drm_mode_gamma_set_ioctl()` requires a non-zero `crtc->gamma_size`; it was 0,
hence `failed to set gamma: Invalid argument`.  `drm_crtc_init` now sets
`gamma_size = 256` and allocates the software store; `.gamma_set`/`.load_lut`
remain no-ops (no hardware LUT, truecolor).

## 3. One frame per atomic state (0127, module)

`exynos7580_plane_commit_frame()` logs an error if the same atomic state
commits a frame twice, confirming the highest-index-plane rule (the trace
during kmscube's first commit is checked against this).

## Artifacts (image rebuild — 0126 is built-in DRM core)

| file | size | sha256 |
|---|---|---|
| `Image-int80` | 18075368 | `1a0db094c7fa8201d6558f62c6ef6983bd99a11d4132f525a9abf81c94baa8c5` |
| `kernel-boot-int80.img` | 27213824 | `840457d56ec9563887a8ebae8fafe6872e22c06a127eb5ca6724018c88b3ddaf` |
| `exynos-drm7580-int80.ko` | 1434352 | `0d28fc71a8bac456642dc86d928c277d146e53def83f7246f7cd67a0e2f3cdab` |
| `drm_kms_helper-int80.ko` | 1443520 | `5dd13fd8fa52b1e03477bbdb8d89a140a9b74ce009c387dc5c9055caf73b60ff` |

## Gates (PASS)

All int79 gates plus: the depth field, the nested lock_all/unlock logs in
vmlinux, `gamma_size = 256`, and the duplicate-frame check in the module.

## Expected

`modetest -s` and then `kmscube -D /dev/dri/card0 -c 30` in one boot:
no block at `drm_mode_setcrtc`, `failed to set gamma` gone, renderer line
shows whether Mesa picked panfrost (the int78 `prime_import`/`fb_create`
traces confirm whether our import path is used), IRQ 234 climbing, no Oops.

Log sha256s: build `4ff1214bf9374fd7907f1367fd489583ab22dbc990621d46dc0dedbe737b91a6`, gates `7f5d2041657a2d6e8233b02b7ef61e45ab8de84ee1a0736108133530c2c75cbf`, evidence `49fa886155bfea35387e59aef68028870e21d141e94cc9818be89f85db18667d`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
