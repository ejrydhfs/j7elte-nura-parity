# int71: self-recovering lockups, bounded vsync waits, per-frame FRAME arming

int70 added diagnostics; int71 makes a failure recoverable and caps every KMS
wait so `insmod` can never wedge the phone.

## 1. Lockup panic + auto-reboot

* `.config`: `CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=y`,
  `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y` (gated; `watchdog_enable` in vmlinux),
  `CONFIG_EXYNOS_SNAPSHOT_PANIC_REBOOT=y` stays.
* kernel command line (single line, gated): `console=tty0 loglevel=7
  ignore_loglevel hardlockup_panic=1 softlockup_panic=1 panic=5`.
  A deadlock now panics and reboots into the headless SSH state within ~30 s;
  pstore keeps the last console.

## 2. Bounded vsync waits (no indefinite block)

* `decon_update_regs()`: on the KMS path (`decon->kms_owned`) the vsync wait is
  capped at **100 ms** instead of `VSYNC_TIMEOUT_MSEC` (300 ms); before the
  wait it logs `VIDINTCON0`/`VIDINTCON1`, on timeout it logs
  `VIDCON0`/`VIDINTCON0`/`TRIGCON`.
* `decon_kms_commit()`: after the update it does one more bounded 100 ms wait;
  on failure it calls `decon_kms_log_state()` and returns `-ETIMEDOUT`, so
  `insmod` fails cleanly with the register state instead of proceeding on an
  unarmed pipe.

## 3. FRAME IRQ arming — the vendor sequence, mirrored

Vendor references:

* `decon_pan_display()` (`decon-int_drv.c:542-615`) arms through
  `decon_wait_for_vsync()`;
* `decon_wait_for_vsync()` (`decon_core.c:1228-1265`) brackets the sleep with
  `decon_activate_vsync()` / `decon_deactivate_vsync()` for
  `DECON_SW_TRIG`/`DECON_VIDEO_MODE`;
* `decon_reg_start()` (`decon_reg_7580.c:480-490`) only starts the pipe
  (`decon_reg_direct_on_off` + `decon_reg_update_standalone`), it does not arm
  the interrupt.

New `decon_kms_arm_frame(decon, where)` does the same activate/deactivate pair
(refcount-neutral) and logs `VIDINTCON0` before and `VIDINTCON0`/`VIDINTCON1`
after each arming.  It is called:

1. in `decon_kms_enable()` after `decon_enable()` (which performs the
   `decon_reg_start()`), with `decon_activate_vsync()` first so the
   registration-time arming is preserved (`d->vblank_cb` stays set), and
2. in `decon_kms_commit()` **after `decon_kms_program()`** — the point where a
   full modeset performs another `decon_reg_start()` — before
   `decon_export_update_regs()`.

So the arming happens after every `decon_reg_start()` on the KMS path, which is
what `decon_enable()` clearing VIDINTCON0 requires.  `decon_kms_disable()`
still calls `decon_deactivate_vsync()` to balance the registration arm.

## Artifacts (image rebuild)

| file | size | sha256 |
|---|---|---|
| `Image-int71` | 18058984 | `d10585f912d2690173402df5031190599252e8f98a0b5262c4bbc3a08b718c28` |
| `kernel-boot-int71.img` | 27197440 | `99c85d7fcd5ce41a261134a96105f0f47e78e87db0b0abe9d60ccb7c657f88d9` |
| `exynos-drm7580-int71.ko` | 1434352 | `96b457910b90ff735320aa7614e33331a8e1f9dd9437dc02ea0a0a7ce1d27be2` |
| `drm_kms_helper-int71.ko` | 1443520 | `818a44e715c4ebdcfe89b898b44a605fa0cff2d08594a99b21884415ef086a84` |

Load helper, then driver, after flashing.

## Gates (PASS)

All int70 gates plus: lockup panic config symbols and `watchdog_enable` in
vmlinux; cmdline contains `ignore_loglevel hardlockup_panic=1
softlockup_panic=1 panic=5` on one line; `vsync wait <=100ms` in
`decon_update_regs`; `decon_kms_arm_frame` called exactly twice (after enable,
after program); commit returns `-ETIMEDOUT` and logs `bounded vsync wait
failed`; the arming logs (`VIDINTCON0 before=/after=`) are in vmlinux.

## Expected dmesg on `insmod`

```
decon_kms_enable: enter state=2 psr_mode=0 trig=1
decon_update_regs: vsync wait <=100ms, VIDINTCON0=... VIDINTCON1=...   (wait inside enable path if any)
decon_kms_enable: decon_enable ret=0 state=2
decon_kms_enable: VIDCON0=... VIDOUTCON0=... VIDINTCON0=... VIDINTCON1=... TRIGCON=...
decon_kms_arm_frame(after decon_enable/reg_start): VIDINTCON0 before=...
decon_kms_arm_frame(after decon_enable/reg_start): VIDINTCON0 after=... VIDINTCON1=...
decon_kms_enable: FRAME irq armed, VIDINTCON0=...
decon_kms_arm_frame(commit after program): ... (per commit)
decon irq: VIDINTCON1=... frame=1        (first five frames, if the irq fires)
```

`/proc/interrupts` should now show the two `14830000.decon_fb` lines moving.  If
they stay at 0, `decon_kms_commit` returns `-ETIMEDOUT` with the
`commit vsync timeout` register dump instead of wedging, and the phone stays
reachable (and a lockup panics/reboots by itself).

Log sha256s: build `84a4d596ceec642cc4549cd0e6e6a037361d7a5a9cd7ca83a4cdd61fd578898e`, gates `16b3e7e3f61d465051baaaa1b4bd60144cc95e417c2357174e3e7073d6d2a4b9`, evidence `82829c52b6ccbae658887792ed8a16f7272297ec783e92c8702ea7746125cd83`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
