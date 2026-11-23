# int60: built-in KMS with a vendor driver that refuses to bind

The runtime `unbind` test is dropped: `decon_remove()` hung (watchdog reset,
no pstore record) — most likely `decon_disable()` waiting for a frame/vsync
after X was stopped, or `kthread_stop()` on the vsync thread.  int60 removes
the need to unbind at all:

* vendor DECON stays compiled in and at the **stock `late_initcall`** (no
  `late_initcall_sync` anywhere; int58 proved that move bootloops),
* its `decon_probe()` returns `-ENODEV` at the top under
  `CONFIG_EXYNOS7580_DISPLAY_KMS`, before taking any resource, so the device
  stays unbound,
* the KMS driver is **built in** (`DRM_EXYNOS7580=y`) and at its own
  `late_initcall` (drivers/gpu, after drivers/video) registers its
  `platform_driver`; the bus probes the unbound device,
  `decon_kms_get()` finds no drvdata and calls
  `decon_probe_common(pdev, false)`, so DECON is stood up without a vendor
  fb0, then `card0` + the CMA fbdev are created.

Failure is a probe error, not a panic: the KMS late_initcall's
`platform_driver_register()` returns 0 regardless (the probe failure is
logged), the kernel keeps booting with USB, and if it does panic the reboot
already preserves the log (`PSTORE_CONSOLE`) for the next boot.

## Change set

* series: 20 patches = the int52 series (18) + `0102-kms-module.patch` +
  `0103-kms-builtin-vendor-refuse.patch`
* link order makes the vendor late_initcall run first (`drivers/Makefile`:
  `video/` at line 18, `gpu/` at line 50), so the refusal is observed before
  the KMS probe; if the order ever changed, KMS would simply bind first and
  the vendor probe would never see the device
* `0200` applied with
  `--exclude=drivers/video/exynos/decon_7580/decon_core.c` (no initcall move)
* `0103` adds the vendor refusal in `decon_probe()` and reverts
  `decon_remove()` to the minimal stock sequence (see §3); the devm
  double-free fix from `0102` stays, `iovmm_map`/`iovmm_unmap` exports stay
* config: stock + KMS fragment (`EXYNOS7580_DISPLAY_KMS=y`, `DRM=y`,
  `DRM_KMS_HELPER=y`, `DRM_KMS_CMA_HELPER=y`, `DRM_EXYNOS7580=y`,
  `DRM_EXYNOS=n`, `FRAMEBUFFER_CONSOLE` off, `CMA_SIZE_MBYTES=32`, vendor
  display stack `y`) + `PSTORE_CONSOLE=y`; modules off

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int60` | 17500448 | `06f74064f592da6f37f311d9e3909a612a125c7beaa2523d8401957c9761b0e2` |
| `kernel-boot-int60.img` | 26640384 | `c2525bcd9fc550154d14cbce3dd148e5b663c8030e5ce9b30a906a56c93e2f95` |

Build 5304 lines, 487 warnings, 0 errors, 0 `undefined!`, no modpost warnings,
no warnings in `drivers/gpu/drm/exynos7580/`.  10 gate groups PASS.  (The gate
log's final banner still says "21 patches": it was emitted by the script text
before the count was corrected; the build applied 20 patches as listed above.)

## Gates

* layout/wiring: 7 sources in `exynos7580/`, Makefile wiring, single
  `DRIVER_ATOMIC`;
* atomic core + driver features, no core gem-mmap paths;
* fixed shim ordering (`register_framebuffer` before `output_lock`,
  `register_fb == false` skip before the `fbinfo` window branch, no hand-off);
* **built-in KMS owns DECON**: refusal marker + `#ifdef
  CONFIG_EXYNOS7580_DISPLAY_KMS` inside `decon_probe()` with the original
  `decon_probe_common(pdev, true)` still compiled for the non-KMS case;
  vendor `late_initcall(exynos_decon_register)` and KMS
  `late_initcall(exynos7580_drm_init)` both stock, **no
  `late_initcall_sync` line** in either;
* remove minimal: no `decon_disable()`/psr teardown in `decon_remove()`, no
  `kfree(decon…)` of the devm allocation, probe `fail_kfree` double-free fix
  present;
* KMS linked built-in: 13 slice symbols once each in `vmlinux`,
  `__initcall_exynos7580_drm_init7`, `decon_kms_get`, zero `.ko`,
  `card0 ready (atomic)` and `DECON handed to KMS` strings in the Image;
* pstore: `ess_pstore_init`, `PSTORE_CONSOLE`, `log_pstore` region;
* config closure, stock-dt boot image, freshness.

## What to look for on the device

```
dmesg | grep -Ei 'decon|drm|exynos7580|probe.*-19'
#   "decon: probe of 14830000.decon_fb failed with error -19"  <- vendor refused
#   "DECON handed to KMS (720x1280, N windows)"
#   "exynos7580: ... bound, card0 ready (atomic)"
ls /dev/dri/            # card0
modetest -M exynos
```

If the KMS probe fails instead, dmesg shows the exact `DECON KMS bring-up
failed: -N` / `drm_platform_init failed (-N)` line; the kernel keeps running
(no display) and the next boot's `/sys/fs/pstore` holds the log if it
panicked.

## Why remove() stays minimal

With the vendor probe refusing, `decon_remove()` only runs if someone
force-unbinds the (unbound) device or if the KMS driver is unloaded — neither
is part of this test.  The int59 experiment showed the full teardown hangs in
`decon_disable()`/vsync waits after X is stopped, so `0103` restores the stock
sequence (keep fb0 conditional, stop the update thread, release windows,
remove debugfs) minus the `kfree()` of the devm `decon_device`.  The probe
`fail_kfree` double-free fix from `0102` stays: the KMS probe calls
`decon_probe_common()` and a failure must return an error, not free devm
memory twice.

Log sha256s: build `ca1c3fa55a65056f54f41e2748fe1009034d9e5dcb15973119a286dcbf84dc88`, gates `9b0faef2e6391f45947b472e6ef242f0706b976c8a8878b029ba8670c73be731`, evidence `e5f7eb5c33d3127140630ea5a65b8f8b8d4e87d4c440d032ef7651304f4680e5`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
