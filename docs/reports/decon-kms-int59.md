# int59: KMS as a module, vendor DECON kept at stock `late_initcall`

int58 proved the `late_initcall(exynos_decon_register) ->
late_initcall_sync(...)` move from `0200` is fatal on its own (everything else
tested boots: int51d, int51f, int55, int56, int57).  int59 drops that move and
keeps the rest of the series: fixed shim (`0100`), slices, `0201`, `0102`
module plumbing, and `0200` **without** its `decon_core.c` hunk (its vendor
Kconfig choice is kept — int57 proved it harmless).

## Build

* series: 19 patches; `0200` applied with
  `--exclude=drivers/video/exynos/decon_7580/decon_core.c`
* config: stock + KMS fragment (`EXYNOS7580_DISPLAY_KMS=y`, `DRM=y`,
  `FRAMEBUFFER_CONSOLE` off, `CMA_SIZE_MBYTES=32`, `DRM_EXYNOS=n`, vendor
  display `y`) + `CONFIG_MODULES=y`, `CONFIG_MODULE_UNLOAD=y`,
  `CONFIG_DRM_EXYNOS7580=m`, `DRM_KMS_HELPER=m` (promptless tristate follows
  the selector)
* vendor DECON registers at the stock `late_initcall`, so vendor fb0 owns the
  display at boot; KMS is not built in and not auto-loaded
* build: 5346 log lines, 488 warnings, 0 errors, 0 `undefined!`, no modpost
  warnings, no warnings in `drivers/gpu/drm/exynos7580/`

| file | size | sha256 |
|---|---|---|
| `Image-int59` | 18058856 | `db0e89915438459d9bcf13ca84099f1cfb2da04479b73c69a360c49267ff746c` |
| `kernel-boot-int59.img` | 27197440 | `c3f2a73fbc4b69cf19d9a91e5ee154ff88b76cfbd6b082d8a7c6558e85e77d6b` |
| `exynos-drm7580-int59.ko` | 1425208 | `1d7369f1c9491c863d9566cbf5f0d17e05deff17f57b1da60dd604e92dcbd80d` |
| `drm_kms_helper-int59.ko` | 1439512 | `88d53f1d24232085f95fafbf478cf964c11f1899d76b3ad0f927ab62a35c74dc` |

`modinfo exynos-drm7580.ko`: GPL, `depends: drm_kms_helper`, `aarch64`
vermagic, `fbdev`/`fbdev_bpp` params, `alias: of:N*T*C…` (manual modprobe only).
The loadable set is exactly the two `.ko` (`find drivers -name '*.ko'` = 2);
load order helper first.  Module imports: 130, all resolved in
`Module.symvers`, including the nine `decon_kms_*`, `iovmm_map`/`iovmm_unmap`
(added as GPL exports by `0102`) and the DRM core/CMA helpers.

## Gates (8 groups, all PASS)

layout/single wiring point; atomic core + driver features; fixed-shim
ordering; **unbind leaves the DECON re-bindable** (no `kfree(decon…)`, psr/
vsync threads stopped, lpd workqueue + ion client destroyed, drvdata +
`decon_int_drvdata` cleared, iovmm exports present); no core gem-mmap; module
symbols/metadata (KMS symbols only in the `.ko`, absent from `vmlinux`/Image);
module chain and imports; KMS objects clean.  Vendor probe string
"decon registered successfully" still in the Image (display on).

## Device procedure

```
systemctl stop lightdm                 # or kill Xorg
echo 14830000.decon_fb > /sys/bus/platform/drivers/decon/unbind
insmod /home/user/drm_kms_helper.ko
insmod /home/user/exynos-drm7580.ko
dmesg | tail -60                       # "DECON handed to KMS", "… bound, card0 ready"
ls /dev/dri/                           # card0
rmmod exynos-drm7580                   # no DRM client holding card0
echo 14830000.decon_fb > /sys/bus/platform/drivers/decon/bind   # vendor fb0 back
```

## Do the vendor probe/unbind assumptions hold for an `insmod`-time probe?

Yes — the module's probe is the same code path the KMS-only boot used, with
the state left by `unbind` instead of `probe never ran`.  Per resource:

* **platform drvdata** — stock `decon_remove()` freed the `decon_device` but
  left `platform_get_drvdata()` pointing at it; `decon_kms_get()` would have
  read freed memory (and `decon && !decon->kms_owned` would have returned
  `-EBUSY` for a live handle).  `0102` now clears it before returning, so
  `decon_kms_get()` takes the `!decon` branch and calls
  `decon_probe_common(pdev, false)`, exactly as when the vendor probe never
  ran.
* **`decon_int_drvdata`** (exported global used by the IRQ handlers and
  `get_decon_drvdata()`) — cleared by `0102` if it pointed at the removed
  device; `decon_probe_common()` sets it again before requesting any IRQ.
* **devm lifetime / double free** — `decon` and `decon->pdata` are
  `devm_kzalloc()` allocations (vendor code), and the stock code `kfree()`d
  them in `decon_remove()` and in the probe fail paths.  Driver-core devres
  release then freed them a second time.  `0102` removes all three `kfree()`
  calls, so devres owns the lifetime: devm IRQs are released before the
  structs (reverse registration order), and an `unbind` or a failed module
  probe cannot double free.
* **threads/workqueues/ion/entity** — the stock remove stopped only the update
  thread, leaving the psr thread, the vsync thread, the lpd workqueue, the ion
  client and the V4L2 subdev alive against the freed `decon`.  `0102`'s
  remove now mirrors the probe unwind (`fail_*` labels) and
  `decon_kms_teardown()`: pipe disabled when on, update/psr/vsync threads
  stopped, lpd workqueue and ion client destroyed, framebuffers released,
  media entity unregistered.  `probe_common()` re-creates all of them, so the
  second probe starts clean.
* **IOVMM** — `decon_remove()` deactivates it; `probe_common()` re-activates
  it (`iovmm_activate`), and the dmabuf slice maps through the exported
  `iovmm_map`.
* **media device** — `v4l2_device_unregister_subdev()` at remove removes the
  entity and its links, so `decon_register_entity()`/`find_subdev_mipi()`
  during the second probe do not duplicate links.  Watch dmesg for v4l2/media
  warnings; a failure there makes `decon_probe_common()` return an error and
  the load fails cleanly.

Nothing else in the series depends on the `late_initcall_sync` ordering:
after dropping `0200`'s `decon_core.c` hunk, `late_initcall_sync` appears only
in `0200`'s Kconfig help text and comments, and the KMS sources never bind at
initcall time in this image (module not loaded at boot).  The one ordering
requirement — "vendor fb0 must exist before anything takes DECON" — is
satisfied because the module is loaded by hand after boot.

## Notes

* int53's image is superseded: it carried the sync move (hence its bootloop)
  and the older `0102` that still `kfree()`d the devm `decon` in `remove()` /
  probe-fail paths, so an unbind would have crashed.  int59 is the runnable
  variant; int53's bootloop happened before any unbind, so those fixes are not
  a bootloop cause.
* After `rmmod` the DECON stays unbound (display dark) until the manual bind
  above or a reboot; `rmmod` returns `-EBUSY` while a DRM client holds card0.

Log sha256s: build `f4ee1bfd1dc13328a50bbc7752068336ca16ab0208d471d60c4b69746acd960d`, gates `35037efb08d6833f9985dfd5450aa7e26b0d2636d41c9cf1651702212b6badb9`, evidence `887e290c82067c2eee1bf94496fdcf118d162d5f08e54149cad3ce262861a9f8`,
config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
