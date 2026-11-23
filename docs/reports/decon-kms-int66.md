# int66: force the runtime-PM resume and enable/verify the DECON clocks

int65 aborted on the same first register read, after its own "regs powered"
log — so the domain was attached but the **clocks were still off**.

`CONFIG_PM_RUNTIME=y` is set, so `pm_runtime_get_sync()` is real.  The device
can nevertheless be left `RPM_ACTIVE` (the driver core/genpd path during the
failed vendor probe attempt), and then `get_sync()` does not call
`decon_runtime_resume()`; that callback is the only place the five clocks are
`clk_prepare_enable()`d.  The vendor boot path never notices because the
bootloader leaves the clocks on.

## Fix (`0108-kms-clocks-on-before-regs.patch`)

In `decon_probe_common()`, before the first register access:

* `pm_runtime_set_suspended(dev)` → the following
  `pm_runtime_enable()` + `pm_runtime_get_sync()` really runs
  `decon_runtime_resume()`;
* log `decon: pm_runtime suspended=%d` (state after the get);
* `decon_kms_clocks_on()`: verify the five clock pointers
  (`pclk/aclk/eclk/vclk/aclk_disp`); an ERR_PTR/NULL one logs
  `DECON clock <name> missing` and fails with `-ENODEV`; log their
  `clk_get_rate()` values, then `clk_prepare_enable()` each (rollback on
  error) and log `clocks enabled (pclk=…, aclk=…, eclk=…, vclk=…,
  aclk_disp=…)`;
* log `decon: regs powered (pd attached, clocks on), reading VIDCON0` right
  before the read.

Balanced teardown: `decon_remove()` now does `pm_runtime_put_sync()` +
`pm_runtime_disable()` + `decon_kms_clocks_off()` before `decon_put_clocks()`,
and `decon_kms_teardown()` (shim) disables the same five clocks (with a
`#include <linux/clk.h>`) before `decon_put_clocks()`.  The explicit enables
and the resume callback's enables are both accounted for.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int66` | 18058984 | `9d08f78ed50ea53b41ae1861f3173d1f503dbae3c6dc97445c473e7f496d22ad` |
| `kernel-boot-int66.img` | 27197440 | `abcaf426a824f5801ece59ea89d9b45b917ed35de9078d1626b0be1ad3d15014` |
| `exynos-drm7580-int66.ko` | 1430120 | `f80b1a9522adf470e4de63cadf40b35f70054371a83a2d8920421f4e2296b927` |
| `drm_kms_helper-int66.ko` | 1443368 | `6053aee53748708e9e98f307f769dc3055eecf7b0c46f7a2d5600391be14d36a` |

Image size unchanged from int63..int65 (the fix is in the built-in vendor
core/shim).  Load order: helper, then `exynos-drm7580.ko`.

## Gates (all PASS)

Everything from int65 plus: `pm_runtime_set_suspended(dev)` and
`decon_kms_clocks_on()` present, `decon_kms_clocks_off()` in remove, the shim
teardown disables the five clocks, the clock-enable loop exists once, and the
new logs are in `vmlinux` strings.  Ordering: drvdata line 4798,
`pm_runtime_set_suspended` 4810, `pm_runtime_enable` 4811, explicit clocks
4820, first VIDCON0 read 4929.

## Expected log on `insmod`

```
decon: pm_runtime suspended=0
decon clocks: pclk=… aclk=… eclk=… vclk=… aclk_disp=…
clocks enabled (pclk=…, aclk=…, eclk=…, vclk=…, aclk_disp=…)
decon: regs powered (pd attached, clocks on), reading VIDCON0
```

If a clock is missing you get `DECON clock <name> missing (…)` and an errno;
if prepare/enable fails, `clk_prepare_enable(<name>) failed: N`.

Log sha256s: build `1440614ac99234ba547c87a9c8ef2a406b8869e3ce4150bf581a5fd3b61f5b27`, gates `7462da57eef5d0f50ddbf5927ef788cfc620b8d2a3e856db1ed3c3674019737a`, evidence `ca5a9696d7db4aaf717094a8ccc508402a6d6ab8aa0f7c154ab83e068cec26ec`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
