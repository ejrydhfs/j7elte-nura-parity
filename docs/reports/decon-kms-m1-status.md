# DECON KMS — M1 hand-off (2026-11-24)

## What works (hardware-verified)

- Our atomic DRM driver **registers `card0` + `controlD64` + a KMS `fb0`** on the J7:
  `/dev/dri/card0`, `/sys/class/graphics/fb0 -> ../../devices/14830000.decon_fb/graphics/fb0`.
- The module pair loads (`drm_kms_helper.ko`, then `exynos-drm7580.ko`); 130 imports resolve.
- Vendor DECON refuses to bind under the KMS choice (verified by disassembly: `mov w0,#-19; ret`),
  so KMS owns the controller and stands it up through the shim.
- Power/clocks/PD: `pd_disp` attached, five clocks enabled at measured rates
  (pclk 133 MHz, aclk 267, eclk 160, vclk 64.5, aclk_disp 267), `VIDCON0=0x7` (pipe running).
- The atomic modeset runs to the point of arming the frame interrupt and committing planes.

## Bugs fixed on the way (each was a hard boot/load failure)

| Bug | Fix |
|---|---|
| `late_initcall_sync` move of the vendor driver → boot loop | keep stock `late_initcall`; vendor probe refuses under the KMS choice |
| devm double free in vendor `remove`/probe fail paths | drop the `kfree(decon…)` calls |
| `platform_set_drvdata(pdev, ex)` before `decon_kms_get` → false `-EBUSY` | publish drvdata after the handle is obtained |
| `R_AARCH64_PREL32` in `.eh_frame` → `insmod: overflow in relocation type 261` | `-fno-unwind-tables` on loadable objects |
| `RPM_ACTIVE` device → `pm_runtime_get_sync()` no-op → DECON clocks off → external abort | `pm_runtime_set_suspended()` + explicit clock enable/verify |
| NULL encoder `.mode_set` (`crtc_set_mode`) | no-op encoder `mode_set` |
| unguarded `funcs->commit/.dpms/.atomic_update` in the atomic-helper backport | null-guard every optional op |
| NULL `load_lut`, encoder/connector `destroy` | no-op `load_lut`/`gamma_set`, `drm_encoder_cleanup`, `drm_connector_cleanup` |
| ESD IRQ double-enable (WARN) | `esd_irq_on` idempotence |

## Open blockers (in order)

1. **FRAME interrupt never fires.** With the pipe running (`VIDCON0=0x7`), `VIDINTCON0` reads
   `0x0` before and after the forced arm, and `/proc/interrupts` shows 0 on the DECON lines
   (DSIM counts normally). int72 bypasses the vsync refcount (`decon_reg_set_int` +
   `DSIM_IOC_VSYNC` unconditionally, logged) — the readback after `set_int` is the next datum
   and will tell whether the write is lost (shadow/register-protect, wrong `DSI_MODE_*`/
   frame-selection mask, or the wrong DECON id) or whether the DSIM refuses the vsync request.
2. **Teardown deadlock on the failure path.** `decon_kms_put() → kthread_stop()` blocks forever
   (verified via `/proc/<insmod pid>/stack`), so a failed load wedges `insmod`. The worker
   thread must observe `kthread_should_stop()` around its vsync/regs wait (or the wait must be
   interruptible), otherwise every failed probe needs a reboot.
3. Commit waits are bounded (100 ms) and return `-ETIMEDOUT` with a register dump, which is what
   makes (1) and (2) observable at all.

## Test procedure (no reflash for module-only changes)

1. Flash the current image, boot headless (vendor refuses → no fb0; SSH works).
2. `systemctl stop lightdm; pkill Xorg nura-shell fbwin`
3. `insmod drm_kms_helper-<rev>.ko; insmod exynos-drm7580-<rev>.ko`
4. Read `dmesg | grep -E "decon_kms|VIDINTCON|decon irq"` and `/proc/interrupts`.
5. On hang: reboot (lockup-panic is enabled in int71+); on wedged kernel: battery pull + TWRP,
   then `dd` the int49 image back to `mmcblk0p10`.

## Working image for daily use

`kernel-boot-int49.img` (sha256 `62377e6573ed5045…`) — nura-shell UI, GPU at 155 fps,
19 consecutive clean runs. All KMS work is additive and lives in the int5x–int7x series.
