# int65: power pd_disp and check clocks before the first DECON register read

## The crash and the `pd-disp can't add device` line

int64 loaded the modules and the KMS probe reached DECON stand-up, then died on
the first register read (`decon_reg_get_stop_status`, x0 = mapped `regs`).

`PM DOMAIN: pd-disp can't add device 14830000.decon_fb` is **not** the failure:
`arch/arm64/mach-exynos/pm_domains.c` performs the genpd attach from a platform
bus notifier on `BUS_NOTIFY_BIND_DRIVER`, and `drivers/base/dd.c` sends that
from `driver_sysfs_add()` on *every* probe attempt.  The vendor probe (which
0103 makes return `-ENODEV`) therefore attached the device on its attempt, and
a failed probe does not send `BUS_NOTIFY_UNBOUND_DRIVER`, so the KMS bind's
second `__pm_genpd_add_device()` returns `-EBUSY` and prints that line.  The
domain is attached (`dev->pm_domain != NULL`); the working vendor boot shows
the successful `PM DOMAIN: pd-disp, Device : 14830000.decon_fb Registered`
(`docs/reports/decon-kms-power.md`).

The real cause: `decon_probe_common()` reads `VIDCON0` (the "DECON already
running" check) **before** its `pm_runtime_enable()`/`pm_runtime_get_sync()`
block further down.  On the vendor boot path the bootloader leaves pd_disp and
the DECON clocks on, so that read works.  In the KMS path the vendor probe
attempt has already been through the driver core's `pm_request_idle`, pd_disp
can be off, and `readl()` aborts — with clocks off as well, since the clock
enable happens in `decon_runtime_resume()`, i.e. only once runtime PM is
enabled and resumed.

## Fix (`0107-kms-decon-power-before-regs.patch`), all in `decon_probe_common()`

* `decon_int_get_clocks()`'s return is checked (the vendor probe ignored it;
  an `ERR_PTR` clock aborts later); failure logs and returns `-ENODEV`.
* After the IRQ-mutex init and **before any register access**:
  * publish `platform_set_drvdata(pdev, decon)` (the runtime PM callbacks
    resolve the device from it),
  * verify `dev->pm_domain` is attached; if not, log
    `DECON has no power domain attached (pd_disp)` and return `-ENODEV`,
  * `pm_runtime_enable()` + `pm_runtime_get_sync()`; the latter runs
    `decon_runtime_resume()`, which programs the clocks and lets genpd power
    pd_disp.  Failure logs `failed to power DECON power domain: N` and returns.
  * log `decon: regs powered (pd attached), reading VIDCON0` immediately before
    the first read.
* The later duplicate `platform_set_drvdata()`/`pm_runtime_enable()`/
  `pm_runtime_get_sync()` block is removed, so the existing teardown
  (`pm_runtime_put_sync()` + `pm_runtime_disable()` + `decon_put_clocks()`)
  stays balanced.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int65` | 18058984 | `f4c52ab79b82b90c09ff9f182998e5c3833969d00d573d15e24f407ec6fac56f` |
| `kernel-boot-int65.img` | 27197440 | `fee77c1feba62637fa2aafe07988df2f24b7d8565905dec2cee90974704f5975` |
| `exynos-drm7580-int65.ko` | 1430120 | `c268d6a00df53bf3737459dda0249940482203d815f30284091deb048ae50110` |
| `drm_kms_helper-int65.ko` | 1443368 | `df2c14aac834d40ef0a23fed3c9f6e7d9099de330ae6cbf2d2b8d9f22b8227cd` |

Load order: helper first, then `exynos-drm7580.ko`.  The Image is unchanged in
size from int64/int63; the fix is in the built-in vendor core, so the module
only differs by the build.

## Gates (all PASS)

Everything from int64 (vendor refusal incl. objdump, drvdata order in the KMS
probe, no KMS symbols in vmlinux, unwind-free modules with only supported
relocation types, imports resolved, pstore/log-buffer/single-line cmdline,
minimal remove, stock-dt) plus:

* `regs powered (pd attached), reading VIDCON0`, the pd-missing and both
  failure logs present in the built-in core and in `vmlinux` strings;
* `ret = decon_int_get_clocks()` check present;
* exactly one `platform_set_drvdata(pdev, decon)` and one
  `pm_runtime_enable(dev)`; ordering gates: drvdata (line 4738) <
  `pm_runtime_enable` (4744) < first `VIDCON0` read (4854).

## What the next dmesg will say

On `insmod` the log should now read:

```
exynos7580: probe: calling decon_kms_get()
decon: regs powered (pd attached), reading VIDCON0
... (DECON stand-up continues)
exynos7580: probe: decon_kms_get ok (...)
exynos7580: probe: decon ok, drvdata published
exynos7580: probe: calling drm_platform_init()
exynos7580: load: dsi_init() ...
```

If it stops earlier, the log names the step: `DECON has no power domain
attached`, `failed to power DECON power domain: N`, or `failed to get DECON
clocks: N` — each returns an errno instead of faulting.

Log sha256s: build `f8c9a5583c25e99ab8289de423e5f55d5ceab42c7a3222a2f4bb8c823a716b4d`, gates `0a65ae9269c2c995b0cd3e0708284477fa7d058557a49c4813b1f7db931b0dc9`, evidence `da24bdf612ddbe7f6925a066d262f1463570861c54205f9c497f7813d26c2de3`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
