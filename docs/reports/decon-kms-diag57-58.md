# int57 / int58: KMS config vs `0200` initcall move

Both are built from int56's proven base (M0 atomic backport + the four core
helper fixes, stock vendor display, `CONFIG_DRM=y`, modules off,
`PSTORE_CONSOLE=y`), display on, stock `dt.img`.

## int57 — KMS fragment config only

int56 + the KMS ownership fragment keys, with 0200's **vendor Kconfig hunk
alone** (the choice) extracted and applied so `EXYNOS7580_DISPLAY_KMS` exists;
no initcall move, no KMS sources.

The fragment's `CONFIG_DRM_EXYNOS7580=y` and the `DRM_KMS_HELPER`/
`DRM_KMS_CMA_HELPER` lines have no symbol/selector in this tree (those symbols
come from 0200's `drivers/gpu/drm/exynos7580/Kconfig`, which is not applied),
so they are no-ops; **no KMS driver exists in the image**.  What int57 really
changes against int56: the ownership choice `=y` (and `DISPLAY_FBDEV` unset),
`CONFIG_DRM_EXYNOS=n`, `CONFIG_FRAMEBUFFER_CONSOLE` off, `CMA_SIZE_MBYTES=32`.

| file | size | sha256 |
|---|---|---|
| `Image-int57` | 17400992 | `7f975c2d8debf60bb57e57727dd00c46ef809bf62b38749e03391fafe4bf20fc` |
| `kernel-boot-int57.img` | 26540032 | `456b8fe026081e38a0e2e90314781c2fbf32197cd7987f1a7be7e090eaa8bf9c` |

Gates PASS: config closure (`DISPLAY_KMS=y`, CMA 32, fbcon off,
`DRM_EXYNOS` off, vendor display on, modules off); no `exynos7580/` sources
and no KMS symbols; vendor probe still at `late_initcall(exynos_decon_register)`
(the initcall move must not leak in); DRM core/atomic + four source fixes as
int56; pstore/ramoops; fresh Image/vmlinux.  Build 5278 lines, 0 errors.

## int58 — `0200` vendor initcall move only

int56 + only 0200's `decon_core.c` hunk (`late_initcall ->
late_initcall_sync`), extracted alone.  Stock config (no fragment), no KMS
Kconfig symbol anywhere, no KMS sources.

| file | size | sha256 |
|---|---|---|
| `Image-int58` | 17400992 | `57d3c57abf063bc9d55adb6b6820aa5dba09f0650fa2731990d8e934de14fab9` |
| `kernel-boot-int58.img` | 26540032 | `dd6c2da0c37ef868ecdf2339a7c40ab512ef55aea606534bab15a9d8078dc1df` |

Gates PASS: `late_initcall_sync(exynos_decon_register)` present and the stock
`late_initcall(...)` gone; no KMS Kconfig/choice/source/symbol; DRM core as
int56; vendor display on; pstore/ramoops; fresh Image/vmlinux.  Build 5278
lines, 0 errors.

## Which series patches touch the vendor DECON stack

Full audit of the 19-patch int53 series (`diff --git` path scan):

| patch | vendor files | change |
|---|---|---|
| `0100` | `decon_7580/Makefile` | adds `decon_kms_api.o` to `decon-y` |
| `0100` | `decon_7580/decon-int_drv.c` | vblank trampoline: local `kms_frame`, calls `decon->kms_vblank_cb` outside `decon->slock` |
| `0100` | `decon_7580/decon.h` | KMS struct fields (`kms_owned`, kms vblank cb/data), new prototypes, `decon_unregister_entity`/`decon_probe_common` made non-static |
| `0100` | `decon_7580/decon_core.c` | probe split into `decon_probe_common(pdev, register_fb)`, registration kept in place gated by `register_fb`, `fb_registered`, remove() conditional unregister, `register_fb == false` skips the fb/window section |
| `0100` | `decon_kms_api.c`, `include/linux/exynos_decon_kms.h` | new shim + contract header |
| `0102` | `decon_7580/decon_core.c` | remove(): unregister V4L2 subdev, clear platform drvdata + `decon_int_drvdata` before `kfree` (unbind/rmmod safety) |
| `0200` | `decon_7580/Kconfig` | adds the display-owner `choice` and its two symbols |
| `0200` | `decon_7580/decon_core.c` | `late_initcall -> late_initcall_sync` |

Nothing else in the series touches `drivers/video/exynos/`; all other patches
are `drivers/gpu/drm/**` only.  So besides 0100/0102 the only vendor-side
patches are exactly those two 0200 hunks, and int57/int58 test them one at a
time.

`drm-kms-coexist/0001-display-owner-kconfig.patch` (subsumed by 0200) carried
three things: the same `choice` (with a *double* default
`EXYNOS7580_DISPLAY_FBDEV if FB` / `EXYNOS7580_DISPLAY_KMS if DRM=y` and
`DISPLAY_FBDEV depends on FB`), the same `late_initcall -> late_initcall_sync`
move, and the `exynos7580/Kconfig` bool + `EXYNOS7580_DISPLAY_KMS` dependency
rewrite.  `0200` subsumes all three with two documented deltas: a single
`default EXYNOS7580_DISPLAY_FBDEV` without `if FB`, `DISPLAY_FBDEV` without
`depends on FB` (the dual form is a recursive dependency: FB is selected by
`DRM_KMS_HELPER`, which `DRM_EXYNOS7580` selects, whose choice would then
depend on FB), plus `EXYNOS_IOVMM_V6` added to the driver's dependencies.

## Reading the results

* int57 loops -> the KMS ownership *config* alone is fatal (choice/fbcon/CMA),
  independent of any KMS code.
* int57 boots, int58 loops -> the initcall move alone is fatal; the vendor
  fbdev driver registering at `late_initcall_sync` misses something that must
  happen at `late_initcall` (e.g. panel/DSIM ordering or the fb0 registration
  being late relative to another initcall_sync user).
* both boot -> the earlier loops come from the KMS code/wiring (or the
  combination), and the next step is int52 + only the KMS main/sources without
  the config, or the reverse.

## Logs

* int57: `logs/int57-build.log` `eedae370c7bb2450871283e01079913e1a7acad583f6938fc2c990ad6b27d93e`, `logs/int57-gates.log` `25a0e441529460c37d8cd5db5f75329039b024958916f9a004187a4d29f5d385`,
  `logs/int57-evidence.log` `cfc737b1a0325448ee4a71bf50ac6a3f391f676b880fe36126cf7e4f44888c95`
* int58: `logs/int58-build.log` `b8d5cdb61801a2ed04c26122e695985cff0c1be4fe134f4db9cb78a6203566af`, `logs/int58-gates.log` `bc6120b98a2779e643a1a1df546cbaef6c4f1224c44234b68065c165799ab81d`,
  `logs/int58-evidence.log` `f7a0e12b1c3fdb0f92335256f7bae073d95a7c1be7b2d8f2950343754a940c6c`
