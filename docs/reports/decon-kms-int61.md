# int61: KMS binds at `late_initcall_sync`, defers on missing DSIM, logs every step

int60 booted headless: the vendor DECON refused correctly (`-ENODEV`), but the
KMS probe ran at the plain `late_initcall` level and produced no `card0`; the
early log was gone.  int61 keeps int60's shape and changes three things, all in
our driver:

1. **bind at `late_initcall_sync`** (level 7s) — after every `late_initcall`
   (vendor DSIM driver, panel, exynos-mdev).  This is *our* driver, not the
   vendor DECON driver, so the int58 hazard (moving the vendor fbdev driver to
   `late_initcall_sync`) does not apply.  The vendor DECON stays at the stock
   `late_initcall` and still refuses.
2. **deferred probe**: `-ENODEV`/`-EPROBE_DEFER` from `decon_kms_get()` is
   turned into `-EPROBE_DEFER`, and a `-EPROBE_DEFER` from the DSI init inside
   `drm_platform_init()`/`->load()` propagates unchanged.  The driver core puts
   the device on the deferred-probe list and the deferred workqueue retries it
   after the provider shows up (3.10 `really_probe()` →
   `driver_deferred_probe_add()` + `driver_deferred_probe_trigger()`).  With the
   sync init the providers (all `late_initcall` or earlier) are already up, so
   this is a second net, not the primary mechanism.
3. **step logging**: `dev_info`/`dev_err` with errno for probe entry,
   `decon_kms_get()` result + caps, deferral, `drm_platform_init()` result,
   probe success, and inside `->load()`: `dsi_init`, the DECON FRAME callback,
   `fbdev_init` ("card0 fb0") and the initial modeset.  With
   `CONFIG_LOG_BUF_SHIFT=18` (256 KB) and `ignore_loglevel` in the kernel
   command line, the failure is in `dmesg` and in pstore even when it happens
   early.  (`PSTORE_CONSOLE=y` was already in int60.)

The kernel command line is a single line:
`console=tty0 loglevel=7 ignore_loglevel` (the previous attempt appended after
a newline, which would have made `loglevel=7\n` an unknown token; the build now
flattens the file and gates that it has no newline).

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int61` | 17500448 | `63c6bbd0c08a3f17b2ba5d3358e66235dbb2cee03df2cc7618f87e34ea23d82d` |
| `kernel-boot-int61.img` | 26640384 | `a05de58890b31758896fa32403803bf74c6a52e3f88db341cf791a269cf7ded4` |

Series: 21 patches = int60's 20 + `0104-kms-sync-init-and-defer.patch` (which
also carries the step logging).  `0200` is still applied without its vendor
`decon_core.c` initcall hunk; vendor probe refuses under the KMS choice; config
unchanged from int60 except `LOG_BUF_SHIFT=18`.

Build 5304 lines, 487 warnings, 0 errors, 0 `undefined!`, no modpost warnings,
no warnings in `drivers/gpu/drm/exynos7580/`.

## Gates (10 groups, PASS) — int60's plus

* KMS bind is `late_initcall_sync(exynos7580_drm_init)`; no plain
  `late_initcall(exynos7580_drm_init)`; `__initcall_exynos7580_drm_init7s`
  present in `vmlinux` and `…init7` absent;
* vendor DECON still `late_initcall(exynos_decon_register)`, no
  `late_initcall_sync` line in the vendor file, refusal marker and `#ifdef
  CONFIG_EXYNOS7580_DISPLAY_KMS` inside `decon_probe()` with the normal
  `decon_probe_common(pdev, true)` still compiled;
* `-EPROBE_DEFER` handling present and logged;
* all six probe/load log strings in the source **and** in the Image;
* `CONFIG_LOG_BUF_SHIFT=18`, `CONFIG_PSTORE_CONSOLE=y`, `log_pstore` present;
* kernel command line single-line and contains `ignore_loglevel`;
* minimal `decon_remove()` (no `decon_disable`, no psr teardown, no
  `kfree(decon…)`, probe `fail_kfree` devm fix present);
* 13 KMS slice symbols + initcall + `decon_kms_get` in `vmlinux`, zero `.ko`,
  `card0 ready (atomic)` / `DECON handed to KMS` strings in the Image.

## What to look for on the device

```
dmesg | grep -Ei 'exynos7580|decon|drm|defer|probe.*-19'
# expected order:
#   decon: probe of 14830000.decon_fb failed with error -19     (vendor refused)
#   exynos7580: probe entry (DECON node /soc/...), binding card0
#   exynos7580: probe: calling decon_kms_get()
#   exynos7580: probe: decon_kms_get ok (N windows, default M)
#   exynos7580: probe: calling drm_platform_init()
#   exynos7580: load: dsi_init()
#   exynos7580 load: registering DECON FRAME callback
#   exynos7580: load: fbdev_init() (card0 fb0)
#   exynos7580: load: initial modeset
#   exynos7580: probe: success, card0 registered
ls /dev/dri/          # card0
modetest -M exynos
```

If the probe still cannot stand DECON up, the last line before the error names
the failing step and errno (`decon_kms_get failed (-N)`, `DSI encoder/connector
init failed: -N`, `fbdev init failed: -N`, `initial modeset failed (-N)`), and a
`-EPROBE_DEFER` prints "DECON/DSIM not ready, deferring probe" and is retried.

Log sha256s: build `aed96e8b43a9e70d97a558042081926f0aae084541f247f9a37aae0dd08cb3e3`, gates `3ad759461094823a14e7b9fffd1642852a5bd0b14d27c36399dba014772f871b`, evidence `d356269bd91222ac1f9b99861299a77f63ebcd2158e9f785d93ae3d8ca15c301`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
