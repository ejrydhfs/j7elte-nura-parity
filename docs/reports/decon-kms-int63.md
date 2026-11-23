# int63: KMS as a module, vendor refusal + drvdata-order fix kept

int62 panicked during the built-in bring-up and we cannot see the crash.
int63 makes the same source loadable by hand, so every attempt is observable
over SSH and a panic costs an auto-reboot, not a battery pull.

## Shape

* source series: int62's 22 patches, unchanged — including
  `0103` (vendor `decon_probe()` returns `-ENODEV` under
  `CONFIG_EXYNOS7580_DISPLAY_KMS`) and `0105` (publish the DRV drvdata only
  after `decon_kms_get()`) and `0104`'s step logging and `-EPROBE_DEFER`
  handling, but the module path registers with `module_init`, not
  `late_initcall_sync` (`0102`'s `#ifdef MODULE` branch; the built-in branch
  keeps the sync init for the non-module image)
* config: stock + KMS fragment + `CONFIG_MODULES=y`, `CONFIG_MODULE_UNLOAD=y`,
  `CONFIG_DRM_EXYNOS7580=m`; `DRM_KMS_HELPER` is the promptless tristate
  selected by our symbol and resolves to `=m`; `DRM=y`, the vendor DECON/DSIM
  stack and IOVMM stay built in; `LOG_BUF_SHIFT=18`, `PSTORE_CONSOLE=y`,
  cmdline `console=tty0 loglevel=7 ignore_loglevel`
* at boot the vendor probe refuses (`-ENODEV`), so the device stays unbound:
  headless but booting with SSH (the int60 state), no card0, the panel keeps
  the bootloader frame.  The KMS module is never auto-loaded.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int63` | 18058984 | `be96182dd4b2a3025db58a0d32922fb2958be025def908d4e5261aa4cdb102c3` |
| `kernel-boot-int63.img` | 27197440 | `6b87622a03c8ccb291223b229ef30dbbbf584f5f310146484be252a276cc2fcf` |
| `exynos-drm7580-int63.ko` | 1428184 | `9c0668a3426b8426b81d4fd6daaec8906b7a3c813949f165b79f23dd1964e6c9` |
| `drm_kms_helper-int63.ko` | 1439512 | `e96655db87821610f5940ae3ce0cd875fffdb99eaf00c389f6c51d3a0106c122` |

Load order: `drm_kms_helper.ko`, then `exynos-drm7580.ko` (`modinfo`:
`depends: drm_kms_helper`, GPL, `3.10.108 SMP preempt mod_unload aarch64`).
Loadable set is exactly those two (`find drivers -name '*.ko'` = 2).

Build 5346 lines, 488 warnings, 0 errors, 0 `undefined!`, no modpost warnings.

## Gates (all PASS)

* `module_init`/`module_exit`/`#ifdef MODULE` present; the built-in
  `late_initcall_sync` line only inside the `#else` branch; no
  `__initcall_exynos7580_drm_init*` and no `exynos7580_*` code symbols in
  `vmlinux` (KMS symbols live only in the `.ko`);
* vendor refusal gates: marker + `#ifdef CONFIG_EXYNOS7580_DISPLAY_KMS`,
  `return -ENODEV` in `decon_probe()`, normal `decon_probe_common(pdev, true)`
  still compiled for the non-KMS case, **and an object-level check**
  `objdump -d vmlinux --disassemble=decon_probe` must show
  `mov w0, #0xffffffed` (verified `12800240` on the identical vendor object);
* vendor at stock `late_initcall`, no `late_initcall_sync` in vendor code;
* 0105 regression gate: `decon_kms_get()` line precedes
  `platform_set_drvdata(pdev, ex)`, published exactly once; the new log string
  is in the module;
* all probe/load log strings in the `.ko` (probe entry, decon_kms_get ok,
  drvdata published, drm_platform_init, dsi_init, FRAME callback, fbdev_init,
  initial modeset, deferral);
* 130 module imports and 119 helper imports all present in `Module.symvers`;
  the nine `decon_kms_*`, `iovmm_map`/`iovmm_unmap` exports present;
  `decon_kms_get` in `vmlinux` `__ksymtab`; modpost clean;
* `LOG_BUF_SHIFT=18`, `PSTORE_CONSOLE`, `log_pstore`, single-line cmdline;
* minimal vendor remove (no `decon_disable`, no psr teardown, no
  `kfree(decon…)`, probe `fail_kfree` devm fix); stock-dt boot image.

## Device procedure

```
insmod /home/user/drm_kms_helper.ko
insmod /home/user/exynos-drm7580.ko
dmesg | tail -80
ls /dev/dri
```

From insmod context `decon_kms_get()` sees `platform_get_drvdata()==NULL`
(vendor refused at boot, never probed), so it takes the same
`decon_probe_common(pdev, false)` path as the built-in case; `0105` guarantees
our own drvdata is not installed before that call.  The step log names the
first failing step with its errno.  A `-EPROBE_DEFER` prints the deferring
line (if mdev/DSIM were missing); a panic reboots into the same SSH-capable
headless state.

Log sha256s: build `30ac91f101187a98aa1f3eccbe69457f6865afbcdd5ab7efa5dc8649d749f08b`, gates `e6e34cf20aaab15d04a4c6b8f44cae92320930fbfa4934159843ed173d952c65`, evidence `adc83ba2ddd549044f593abfc771b42b0aaf9c1cb3c0d5ec3e40f75e94decbb4`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
