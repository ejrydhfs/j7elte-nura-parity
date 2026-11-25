# int74: atomic fb0 postcondition, guarded pan path (module-only)

int73 proved the FRAME interrupt works (`VIDINTCON0=0x8803`,
`decon irq: VIDINTCON1=00000002 … frame=1`).  Two follow-ups:

## 1. `fbdev scanout failed` was a wrong predicate

`exynos7580_fbdev_start_scanout()` compared the **legacy** fields
`crtc->primary->fb` and `crtc->state->enable`.  The atomic commit path does not
maintain them, so the check failed while the pipe was demonstrably scanning
(VIDINTCON0=0x8803, FRAME IRQ firing, TRIGCON=0x40, VIDCON0=0x7).

The postcondition is now the atomic state:

```c
crtc->state && crtc->state->active && crtc->enabled &&
crtc->primary->state && crtc->primary->state->fb == fb
```

and the failure log prints every value involved:
`crtc id, enabled, state, state->active, state->crtc, primary->state,
primary->state->fb, legacy primary->fb, want fb`, then
`decon_kms_log_state()`.

## 2. `drm_crtc_index()` BUG in the pan path

`drm_fb_helper_pan_display()` → `drm_mode_set_config_internal()` →
`drm_atomic_get_crtc_state()` → `drm_crtc_index()` BUGs when the CRTC is not in
`dev->mode_config.crtc_list` — which happened after a failed first load tore
the CRTC down while fb0 stayed alive.  `drm_fb_helper_pan_display()` now checks
the CRTC is the registered object first and returns `-ENODEV` with
`pan_display: crtc %p is not registered` instead of crashing.

## 3. Teardown ordering

No change needed beyond int73's guards: `decon_kms_teardown()` turns the pipe
off before the worker/thread stops and before clocks/IOVMM/PM are dropped, the
worker's blocking paths bail out on `kms_teardown`, and the D-state worker is
skipped instead of wedging.  The repeated `Kernel BUG at 0, code 0x96000004/5`
lines came from userspace touching the fb0 of the torn-down load; with the load
completing (fix 1) that path should not be reached.

## Artifacts (module-only)

| file | size | sha256 |
|---|---|---|
| `exynos-drm7580-int74.ko` | 1434352 | `318b3beee894fdac8d7bff629ba47839b0fea3276f2b4129c4de0ec303d0e505` |
| `drm_kms_helper-int74.ko` | 1443520 | `91e63e55bd25d50f5598f704e7b3b6c22a18203884d3424b083be26432b6e003` |

`Image-int74` (`264afa84…`, 18075368 bytes) is identical in code to int73
(only the two module sources changed; gated by the vmlinux/module-only checks),
so **keep the int73 image** and load this `.ko` pair, or flash int74 for
completeness.

## Gates (PASS)

All int73 gates plus: the atomic predicate fields
(`crtc.state->active`, `primary->state->fb != fb`, the extended log), the
pan-path membership guard, `drm_fb_helper_pan_display` absent from vmlinux (so
both edits are module-only), and the new log strings present in the `.ko`s.

## Success criteria for the device run

* `kms_rc=0`, `card0` and `fb0` stay,
* no Oops in dmesg (the `drm_crtc_index` BUG is now a clean `-ENODEV` if it is
  ever hit),
* `/proc/interrupts` DECON lines count up during a flip,
* `dd` to `/dev/fb0` or `kmsflip` changes the screen.

Log sha256s: build `ce3950968dfd7a1ccdea20f40a0ece7ea3f72ab157ff701f99611d9a1d20726b`, gates `c10328d1bae29a5d407f236ffb794de28d288883a6703e062847648f69924879`, evidence `35b3aecd16d4caa98d66e16c465c7e464d3424798ab8d78bc9beadc653cd112c`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
