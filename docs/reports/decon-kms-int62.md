# int62: KMS probe publishes its drvdata only after `decon_kms_get()`

## What int61's failure actually was

Main saw `DECON already owned by the fbdev driver` (`-EBUSY`) from
`decon_kms_get()`, which looks like the vendor probe ran
`decon_probe_common(pdev, true)`.  It did **not**.  The evidence:

int61 build `.config` / `include/generated/autoconf.h` (inspection build of
the identical tree, scratch kept):

```
CONFIG_EXYNOS7580_DISPLAY_KMS=y
# CONFIG_EXYNOS7580_DISPLAY_FBDEV is not set
CONFIG_DRM_EXYNOS7580=y
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_DRM=y
#define CONFIG_EXYNOS7580_DISPLAY_KMS 1
#define CONFIG_DRM_EXYNOS7580 1
```

(the `choice` block is anonymous — Kconfig gives it no symbol of its own; its
members are exactly the two `EXYNOS7580_DISPLAY_*` bools.)

`aarch64-linux-gnu-objdump -d vmlinux --disassemble=decon_probe`:

```
ffffffc0004f8f28 <decon_probe>:
ffffffc0004f8f28:	12800240 	mov	w0, #0xffffffed   	// #-19
ffffffc0004f8f2c:	d65f03c0 	ret
```

`0103`'s guard is active and the vendor probe cannot reach
`decon_probe_common()`.  The `-EBUSY` came from **our own probe**:

```c
	ex->dev = dev;
	platform_set_drvdata(pdev, ex);   /* struct exynos7580_device */
	...
	ex->decon = decon_kms_get(dev);   /* reads platform_get_drvdata() and
	                                     expects struct decon_device */
```

`decon_kms_get()` therefore interpreted `struct exynos7580_device` as
`struct decon_device`, read a bogus `kms_owned` from it and rejected the
hand-off.  That is also why int60 booted headless with no `card0` (same order,
no logging yet).

## Fix (`0105-kms-probe-drvdata-order.patch`)

* `decon_kms_get()` is now called while the platform drvdata is still NULL;
* `platform_set_drvdata(pdev, ex)` happens right after the handle is obtained,
  before `drm_platform_init()` (whose `->load()` needs it);
* on a failed hand-off the error path still clears the drvdata, so a deferred
  retry starts from NULL;
* gates: the `decon_kms_get` call line must precede the
  `platform_set_drvdata(pdev, ex)` line, and the latter must appear exactly
  once; the new log "probe: decon ok, drvdata published" must be in source and
  Image.

## Artifacts (int62 = int61 + 0105, everything else unchanged)

| file | size | sha256 |
|---|---|---|
| `Image-int62` | 17500448 | `ded8b83b5269cfb7899215a9dfecfdfd8cdce9fcfbe8201944d2817916863444` |
| `kernel-boot-int62.img` | 26640384 | `58116f69e22d4f312de33573f1361858e9e14abf466e5cec20de0a76cbf6df15` |

Series: 22 patches.  Kernel command line in the image is one line:
`console=tty0 loglevel=7 ignore_loglevel`.  Build 5304 lines, 0 errors.

Gates PASS (same 10 groups as int61 plus the drvdata-order guard):
`late_initcall_sync(exynos7580_drm_init)` / `__initcall_exynos7580_drm_init7s`
and no plain init7; vendor probe disassembly source guard active
(`#ifdef CONFIG_EXYNOS7580_DISPLAY_KMS`, `return -ENODEV`, normal
`decon_probe_common(pdev, true)` still compiled), vendor at stock
`late_initcall`; `-EPROBE_DEFER` handling and step logs in source and Image;
`CONFIG_LOG_BUF_SHIFT=18`, `PSTORE_CONSOLE`, `log_pstore`; single-line cmdline;
minimal remove (no `decon_disable`, no psr teardown, no `kfree(decon…)`,
probe `fail_kfree` devm fix); 13 slice symbols + `decon_kms_get` in vmlinux,
zero `.ko`, bind strings in Image; stock-dt boot.

## Expected boot log (in order)

```
decon: probe of 14830000.decon_fb failed with error -19      (vendor refused; unchanged)
exynos7580: probe entry (DECON node /soc/…), binding card0
exynos7580: probe: calling decon_kms_get()
exynos7580: probe: decon_kms_get ok (N windows, default M)
exynos7580: probe: decon ok, drvdata published               (new in int62)
exynos7580: probe: calling drm_platform_init()
exynos7580: load: dsi_init()
exynos7580: load: registering DECON FRAME callback
exynos7580: load: fbdev_init() (card0 fb0)
exynos7580: load: initial modeset
exynos7580: probe: success, card0 registered
```

If the hand-off still fails it can now only be after a successful
`decon_kms_get`; the next log line and errno name the failing step, and a
`-EPROBE_DEFER` prints "DECON/DSIM not ready, deferring probe" (retry via the
deferred-probe workqueue).

int61 (`63c6bbd0…`) and int60 (`06f74064…`) are superseded; their reports stay
for the bisect record.

Log sha256s: build `185c66c3850cc4092438f638b1420b8d2a7f2fcf46ee609b6e1a47ace8f8288d`, gates `a8e4e03b31461273870ab5667b49870d663b4626718de9c5b2e3e01d55a2e72a`, evidence `b9fb199b9f8cfc2f7456cf8d128641d1da2865a9dd673500a5132de3750082f9`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
