# int55 / int56: separating MODULES from DRM core

Two cheap separators for the int53 bootloop (int53 = full KMS series + modules,
display path never entering the KMS probe because the module was not loaded).
Both are display-on, stock `dt.img`, `PSTORE_CONSOLE=y`, no `.ko`, and read
the previous boot's panic from ramoops after flashing.

## int55 — int51f + module loader only

Tree: pristine + fixed `0100` shim only (identical to int51f).  Config: stock
+ `PSTORE_CONSOLE=y` + `CONFIG_MODULES=y` + `CONFIG_MODULE_UNLOAD=y`.  Nothing
else.  Nothing is `=m`, so no module is produced.

| file | size | sha256 |
|---|---|---|
| `Image-int55` | 18026152 | `ba26780dced49119664f610a20c1f60d70ccd308499a42df2cbfae113d6cb7ba` |
| `kernel-boot-int55.img` | 27164672 | `f30ac4160d5482d850e088078c6930c51c1132715b95a8b7d04c949c6c14ec29` |

Gates PASS (3 groups): 7 config expectations exact; `decon_probe_common` /
`decon_kms_get` linked and the fixed-shim ordering present; `load_module` and
`SyS_finit_module` in `vmlinux`, zero `.ko`; pstore/ramoops present; fresh
Image/vmlinux.  Build 5282 lines, 486 warnings, 0 errors.

Interpretation: boots -> the module loader delta is harmless and int53's loop
comes from the series (DRM core / `0200`), not from `CONFIG_MODULES`.  Loops ->
`CONFIG_MODULES=y` changes early boot on this tree and int53 is explained.

## int56 — DRM core + atomic backport only

Tree: pristine + `drm-atomic/0001..0005` + the four core helper fixes
(`drm-kms/0001` crtc-helper, `0002` drmfb id, `0003` CMA DMA attrs, `0005` CMA
import free).  **No** `0100`, **no** `0200`/`0201`, no KMS sources, no shim.
Config: stock + `PSTORE_CONSOLE=y` + `CONFIG_DRM=y`; `CONFIG_MODULES` stays
off, the vendor display stack stays on (`EXYNOS_DECON_EXYNOS7580=y`,
`EXYNOS_MIPI_DSI=y`, `PANEL_EA8061_DYNAMIC=y`).

| file | size | sha256 |
|---|---|---|
| `Image-int56` | 17400992 | `a2470df6db9a6dc9cc4ec5bfee0b294215b0bf34d339f47c4f67f5ff051171ea` |
| `kernel-boot-int56.img` | 26540032 | `f4c84e41c424cc87a33ee7d256c1a6b5b9decb05ccfc4eeef7c9244a666e5134` |

Gates PASS (3 groups): `drm_atomic.o` built and `drm_mode_atomic_ioctl` in
`vmlinux`, `DRM_IOCTL_MODE_ATOMIC` wired, all four core-fix markers present in
source, no `decon_kms_api.c`/`decon_probe_common`/`exynos7580`, vendor
`decon_probe` and its boot string still in the Image; pstore/ramoops present;
fresh Image/vmlinux.  Build 5278 lines, 487 warnings, 0 errors.

Caveat: `DRM_KMS_HELPER` is a promptless tristate selected by driver Kconfigs
and nothing selects it here, so `drm_atomic_helper.o`, `drm_plane_helper.o`
and the helper only objects are **not** built; what int56 exercises at boot is
the DRM core initialisation plus `drm_atomic.o`/`drm_modeset_lock`/`drm_plane`/
`drm_framebuffer` wired into `drm.ko`, which is the part that runs without a
KMS driver.  The four source fixes are verified in source, not in the Image.

Interpretation: boots -> DRM core + atomic backport is not the bootloop cause;
the suspects left are `0200`'s vendor `late_initcall -> late_initcall_sync`
change, the KMS sources themselves (they can only matter if something binds),
or the `=m` KMS choice interacting with the vendor probe.  Loops -> DRM core
is the culprit and can be bisected further inside the nine patches.

## Decision table

| int54-diag | int55 | int56 | conclusion |
|---|---|---|---|
| loops | — | — | `CONFIG_MODULES=y` breaks early boot; displays/DRM irrelevant |
| boots | loops | boots | modules fine, DRM fine -> `0200`'s vendor late_initcall change or the KMS series wiring |
| boots | boots | loops | DRM core/atomic backport is the culprit |
| boots | loops | loops | both deltas independently fatal; fix modules first |

## Logs

* int55: `logs/int55-build.log` `98a1633b6ef5aa7ca7ae7c915221f8b3908bdb4184872e9b0eb910b41309f0b3`, `logs/int55-gates.log` `11fdc82991721595fd06f0c2ab0e432172e927d0456066136acc4bf7e0b6a090`,
  `logs/int55-evidence.log` `ceef47c8da2c871b5ffb849f2b6b9a677d0ead8e9bf5252f5198c7245445099f`
* int56: `logs/int56-build.log` `ecfd7b59e4e3816b74a4e67b14d7d850f4d76f5d7ec4b8e11cffd8d8d8b01d0e`, `logs/int56-gates.log` `bb8eada05f28d32541499b9ed252f2b1fc6e76c97ad72e1b09905f6f003cca4b`,
  `logs/int56-evidence.log` `b9d076c96fc95f6d6cfa5c467b3b3fac204cdc8ac6a3817f2797de5893f353ed`

## int56b (deferred, only if int56 boots and the helper code must be ruled in/out)

Not built.  If int56 boots and the pstore log points at an atomic/helper
symbol that int56 did not link (`drm_atomic_helper_*`, `drm_plane_helper_*`,
`drm_fb_helper_*`, `drm_crtc_helper_*`, `drm_fb_cma_helper_*`), the variant is:

* same tree/config as int56 (`DRM=y`, vendor display on, modules off, no
  shim/KMS), plus a **scratch-only** Kconfig edit that gives the hidden
  `DRM_KMS_HELPER` tristate a prompt:

  ```
  config DRM_KMS_HELPER
  -	tristate
  +	tristate "Diagnostic: DRM KMS helper"
  ```

  (edited in the disposable scratch copy, never committed to `patches/`),
* `CONFIG_DRM_KMS_HELPER=y` forced, so `drm_kms_helper.o` is built in and the
  M0 helper objects (`drm_atomic_helper.o`, `drm_plane_helper.o`, `drm_rect.o`)
  plus the four patched helper files are linked,
* extra gates: `drm_kms_helper.o`/`drm_atomic_helper.o` exist, `vmlinux` has
  exactly one `drm_atomic_helper_check`/`drm_atomic_helper_commit_planes` and
  `drm_plane_helper_update`, and the Image contains `drm_atomic_helper` symbols
  (i.e. the helper code is in the boot image, not only in source).

Until then int56 stays the "DRM core + atomic core init" test, as accepted.
