# int54-diag: headless pstore rescue kernel (MODULES=y, display off)

int53 (KMS as a module) still bootloops although its module is never loaded,
so the loop is not the KMS probe.  int54-diag isolates the remaining deltas:
it is the stock kernel with **no patches at all**, `CONFIG_MODULES=y`
(+`MODULE_UNLOAD`) and **all display drivers compiled out**, plus the pstore
console so the previous boot's crash is readable over USB.

## What int53 actually had (answer to the Kconfig question)

`olddefconfig` output for the display symbols in int53:

```
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=m
CONFIG_DRM_EXYNOS7580=m
CONFIG_EXYNOS7580_DISPLAY_KMS=y
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_EXYNOS_MIPI_DSI=y
CONFIG_EXYNOS_DECON_LCD=y
CONFIG_PANEL_EA8061_DYNAMIC=y
# CONFIG_FRAMEBUFFER_CONSOLE is not set
```

`EXYNOS7580_DISPLAY_KMS` is the bool choice (not a driver) and was `=y`; the
only symbol that became a module is the DRM driver itself (plus the
promptless `DRM_KMS_HELPER` tristate it selects).  The vendor DECON fbdev
stack stayed `=y`, so `decon_fb` was built in and, with no in-tree KMS driver
competing (ours is a module that was never loaded), it registered `/dev/fb0`
at `late_initcall_sync` exactly as in int52.  **The int53 bootloop therefore
did not come from the KMS probe.**

Note also that int53 is *not* int51f + four config symbols: it carries the
whole KMS series (DRM core atomic patches, the DRM core helper fixes, the
`0200` vendor `late_initcall_sync` change + KMS sources, `0201` header), so
`CONFIG_DRM=y` and those core changes are candidates that int51f does not
have.

## int54-diag configuration (config deltas against stock)

| symbol | value | note |
|---|---|---|
| `CONFIG_PSTORE` / `_RAM` / `_PMSG` | `=y` | already in the stock config |
| `CONFIG_PSTORE_CONSOLE` | `=y` | added: console log into ramoops |
| `CONFIG_EXYNOS_SNAPSHOT*` | stock | `_PSTORE` and `_PANIC_REBOOT` already on; the vendor `exynos-ss.c` `ess_pstore_init` registers the bootloader `log_pstore` region as a ramoops device, no DT change |
| `CONFIG_MODULES` | `=y` | the int53 delta under test (was not set) |
| `CONFIG_MODULE_UNLOAD` | `=y` | |
| `CONFIG_EXYNOS_DECON_EXYNOS7580` | not set | `drivers/video/exynos/Makefile` then drops `decon_7580/`: DECON, DSIM and the panels are not compiled at all |
| `CONFIG_EXYNOS_MIPI_DSI` | not set | |
| `CONFIG_DRM` | not set | stock |

0 patches: no `0100`, no `0102`, no KMS series.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int54-diag` | 17592912 | `a978d40a3e75b6b619b694dc0d7606a242e01b893e76224ca18c72da5b6ed4d8` |
| `kernel-boot-int54-diag.img` | 26732544 | `108038d500ff9f5097f35d1e816cd0a87f77e5164c513b6326206a68bfd42fd3` |

Build: 5215 log lines, 483 warnings, 0 errors.  Gates PASS (3 groups): no
`decon_probe`/`decon_probe_common`/`decon_register_entity`/`decon_int_drvdata`/
`exynos_decon_register`/`dsim_probe`/KMS symbols in `vmlinux`, no
"decon registered successfully" in the Image; `ess_pstore_init`,
`pstore_register`, `pstore_console_write`, `ramoops_init` linked and the
`log_pstore` item name present; `load_module`/`SyS_finit_module` present and no
`.ko` produced; fresh `vmlinux`.

## Reading the crash

The ramoops region is the bootloader-reserved `log_pstore` area, so int53's
last boot is still there after flashing int54-diag.  After the first int54-diag
boot:

```
ls -l /sys/fs/pstore/
cat /sys/fs/pstore/dmesg-ramoops-0        # previous boot: int53's panic
dmesg | tail -50                          # this boot
```

If int54-diag also loops with no display, `CONFIG_MODULES=y` is the culprit;
if it boots and `/sys/fs/pstore/dmesg-ramoops-0` shows int53's panic, the loop
is display/series-dependent and the next bisect step is int51f + `MODULES=y`
(display on) versus int51f + `DRM=y`/atomic core (no KMS sources).

Log sha256s:

* `logs/int54-diag-build.log` — 19c5922e10a114f428d41976d067df5563645b27272273c2c887a4bad010b803
* `logs/int54-diag-gates.log` — bd2ac53a6dec5392096732dccc14390125a0764cf20a10729509a05624aab550
* `logs/int54-diag-evidence.log` — 23b7a96d41d3a2cf83ae715647e423ace97ddf9cfc6fe3856f5d5809dee67743
* `logs/int54-diag-config.log` — 541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e
