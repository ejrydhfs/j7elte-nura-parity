# int70: re-arm the FRAME interrupt after `decon_enable()`, idempotent ESD IRQs, IRQ/register diagnostics

int69 registers fb0 but the CRTC never scans out.  `/proc/interrupts` after
the failed load:

```
234: 0 ... GIC 14830000.decon_fb     <- IRQ0 (fifo)
235: 0 ... GIC 14830000.decon_fb     <- IRQ2 (frame)
239: 64 ... GIC 14800000.dsim        <- DSIM works
558: 0 ... err-irq                   <- ESD (unbalanced enable)
```

## Why the FRAME IRQ never fires

In `DECON_VIDEO_MODE` the FRAME interrupt + VIDINTCON0 INT_ENABLE are armed by
`decon_activate_vsync()` (it also tells DSIM to enable its vsync source).  The
KMS `->load()` registers the vblank callback **before** the first modeset, so
`decon_kms_register_vblank_cb()` calls `decon_activate_vsync()` there — and
then the modeset's `decon_enable()` runs `decon_reg_init()`/`decon_reg_start()`,
which rewrite VIDINTCON0 and clear that arming.  `decon_enable()`'s own
interrupt setup is `if (psr_mode != DECON_VIDEO_MODE)`, so nothing arms it
again on the KMS path; the vendor fbdev path re-arms it in pan_display/set_par.

Evidence for the abort was a mapped register read with the domain attached
(int65/66), then no FRAME interrupt at all (int69), so the commit waited for a
vsync that never came, `fbdev init failed: -5`, and the fbdev pan/cleanup path
then hit the `drm_crtc_index` BUG on the invalid first-commit state.

## Exact sequence forced (0107/0108/0112 in `decon_kms_enable`)

```
decon_kms_enable: enter state=<INIT> psr_mode=0 trig=1     (logged)
  INIT -> OFF                                              (logged)
  decon_enable():
      pm_runtime_get_sync (forced via set_suspended first)
      DSIM s_stream(1) -> iovmm_activate -> decon_reg_init -> decon_reg_start
      state = ON
  decon_kms_enable: decon_enable ret=0 state=2             (logged)
  readback VIDCON0/VIDOUTCON0/VIDINTCON0/VIDINTCON1/TRIGCON (logged)
  decon_activate_vsync()  -> re-arms VIDINTCON0 INT_ENABLE|INT_FRAME|VSYNC
  readback VIDINTCON0 ("FRAME irq re-armed")
```

`decon_kms_disable()` now calls `decon_deactivate_vsync()`, so the enable-time
arm is balanced per enable/disable cycle (registration arms once separately).

## ESD IRQs

`decon->esd_irq_on` (new bool, `decon.h`) makes
`decon_esd_enable_interrupt()`/`decon_esd_disable_interrupt()` idempotent:
`decon_init_done` enables them, `decon_enable()`'s second call is skipped, and
`decon_disable()` clears the flag.  The `Unbalanced enable for IRQ 558` WARN is
gone; 558 should now show a single enable in the log, not two.

## Diagnostics added

* `decon_kms_enable()`/`decon_kms_disable()`: state, psr_mode, trig_mode,
  `decon_enable()` return, state after, and the five register readbacks.
* new `decon_kms_log_state(d, tag)` (GPL export): state/psr_mode/trig + the
  five registers, for the fbdev slice.
* `decon_int_register_irq()`: logs each requested IRQ number.
* `decon_int_irq_handler()`: for the first five interrupts with a KMS vblank
  callback, logs VIDINTCON1/VIDCON0/VIDINTCON0 and whether the FRAME bit is
  set, so "never fires" vs "fires without FRAME bit" is distinguishable.
* fbdev scanout failure (module, 0113): logs CRTC id, state pointer, enable
  flag, primary->fb and the wanted fb, then calls `decon_kms_log_state()`.

## Artifacts (image rebuild)

| file | size | sha256 |
|---|---|---|
| `Image-int70` | 18058984 | `4f935e9391f2dd74d2cc561d3a4d59903364a1a3fab8d78853948060032dc773` |
| `kernel-boot-int70.img` | 27197440 | `517250734711f3099d4eca0473fd7b9ec1baec26614038c6eb7138e1a31ce93b` |
| `exynos-drm7580-int70.ko` | 1434352 | `1aa8dc193958a6512fb7ab031baa3638f284343d80a647bec750ec759b8f897f` |
| `drm_kms_helper-int70.ko` | 1443520 | `9bdc8d37151798b59291be4ef169cee3c12261c2a16bc76b18ec6a28753606ff` |

Load helper, then driver.  Gates PASS: the int69 set plus FRAME re-arm /
deactivate balance, `esd_irq_on` guards, IRQ logging, log_state declaration and
export, the five built-in log strings in `vmlinux`, the fbdev state log in the
`.ko`, and the 0113 module-only path check.

## What dmesg should show on `insmod`

```
decon_kms_enable: enter state=2 psr_mode=0 trig=1
decon_kms_enable: decon_enable ret=0 state=2
decon_kms_enable: VIDCON0=... VIDOUTCON0=... VIDINTCON0=... VIDINTCON1=... TRIGCON=...
decon_kms_enable: FRAME irq re-armed, VIDINTCON0=0000....
decon irq: VIDINTCON1=... frame=1        (first five frames)
exynos7580: load: initial modeset
exynos7580: fb0: drmfb ... scanout IOVA ...
```

If the frame bit stays 0 in the `decon irq:` lines, the next suspects are the
DSIM vsync source (`DSIM_IOC_VSYNC`) and VIDINTCON0's FRAMESEL; the logged
values separate them.

Log sha256s: build `c10ab61ace26905727ba95707e71695e794fc4e6871874e8d2f5a5c6a73ee60a`, gates `4ba6cd314ac85ccc16060a8ec86cebd7b61f3d0827b342b7303446f81bad3a50`, evidence `a742088aedcd32d068caff89207f82ec6615e6c73e3084fe299dbdc15d91a88f`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
