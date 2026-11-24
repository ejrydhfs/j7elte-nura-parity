# int73: trace the VIDINTCON0 write; make teardown non-blocking

## 1. VIDINTCON0 stays 0 — instrumentation at the write and at the arm

int72 showed `VIDCON0=0x7`, `VIDINTCON0=0x0` before the forced arm, after
`decon_reg_set_int(..., 1)` and after `DSIM_IOC_VSYNC`, while DSIM's IRQ
counts.  int73 names every step (no behavioural change in this patch):

`decon_reg_set_int()` now logs, before and after the write:

```
decon_reg_set_int: id=%u dsi_mode=%d psr_mode=%d trig_mode=%d en=%u
decon_reg_set_int: writing VIDINTCON0 val=%08x (addr=%px)
decon_reg_set_int: VIDINTCON0 readback=%08x VIDINTCON1=%08x
```

(the address is `decon_int_drvdata->regs + VIDINTCON0`; the disable branch logs
its readback too).  This separates: wrong `id`/base, wrong branch
(`psr_mode`/`trig_mode`), a lost `writel`, or a value that is simply wrong.

`decon_kms_arm_frame()` additionally logs, in order:

* `state`, `pdata dsi_mode/psr_mode/trig_mode`, `drvdata_ok`
  (`decon_int_drvdata == decon`) and `output_sd`;
* a **scratch write/read of VIDOUTCON0** through the same
  `decon_write_mask()` path (`scratch VIDOUTCON0 <old> -> <new>`) — proves
  writel lands while VIDCON0 reads 0x7;
* then `decon_reg_set_int()`, the `DSIM_IOC_VSYNC` return, and the final
  `VIDINTCON0`/`VIDINTCON1` readbacks.

With these, if `after set_int` is still 0 the log says whether `set_int` chose
the I80/command branch (wrong `psr_mode`), whether the scratch write also fails
(CMU/SFR gating), or whether the write happened and something clears it.

## 2. Teardown no longer blocks in `kthread_stop`

The int72 `insmod` stack (`kthread_stop+0x70 <- decon_kms_put+0x16c <-
exynos7580_probe+0x154`) came from a worker stuck in an uninterruptible wait.
`decon_kms_teardown()` now:

1. sets `decon->kms_teardown` (new flag, `decon.h`);
2. turns the producer off: `decon_reg_set_int(..., 0)`;
3. wakes both queues: `wake_up_interruptible_all(&vsync_info.wait)` and
   `&wait_frmdone`;
4. sleeps 20 ms, disables the pipe as before;
5. calls `kthread_stop()` only if the worker is **not** in
   `TASK_UNINTERRUPTIBLE`; otherwise it logs
   `teardown: update worker in D state (0x…), skipping kthread_stop` and
   leaks the thread (a leaked thread beats a wedged kernel).

To let the worker finish, the blocking paths bail out while the flag is set:
`decon_update_regs_handler()` (drops queued work),
`decon_update_regs()` (returns), `decon_wait_for_vsync()` (returns `-ENODEV`
instead of sleeping).  The vsync thread already checks `kthread_should_stop()`.

## Artifacts (image rebuild)

| file | size | sha256 |
|---|---|---|
| `Image-int73` | 18075368 | `efcb8980b4043850ae691edd778b2f931ba59297f0047cfdb7b5a9b2ffb2e2c5` |
| `kernel-boot-int73.img` | 27213824 | `7400aecd0188cc7d7aa060494bff253f39c3ed3880aa193e616be08077ea1a79` |
| `exynos-drm7580-int73.ko` | 1434352 | `2ab0d476691b3acfe6aa52dc29afce4c16b0d0b1c1fbc956e6b826faa3732489` |
| `drm_kms_helper-int73.ko` | 1443520 | `5cde7ebd7c68f42e0a8a309f11bf1e6ad1c2950af53d6151024846a505c436c4` |

Load helper, then driver, after flashing.  Lockup-panic config and the bounded
commit waits from int71 are unchanged.

## Gates (PASS)

All int72 gates plus: the three `decon_reg_set_int` traces, the scratch
`VIDOUTCON0` write/read, `drvdata_ok=` in the arm context, the
`kms_teardown` flag and at least three core bail-outs, both queue wakes, the
`skipping kthread_stop` path with the `TASK_UNINTERRUPTIBLE` check, and all
five trace strings present in `vmlinux`.

## What to look for

```
decon_reg_set_int: id=0 dsi_mode=0 psr_mode=0 trig_mode=1 en=1
decon_reg_set_int: writing VIDINTCON0 val=00008803 (addr=...)
decon_reg_set_int: VIDINTCON0 readback=00008803 VIDINTCON1=...
```

A non-zero readback plus `/proc/interrupts` counting means the arm works.  A
zero readback with a successful scratch write points at the mask/branch; a
failed scratch write points at SFR gating (CMU/clock) — both are named by these
lines next round.

Log sha256s: build `172df43b826461b1b1628ef5498279403fce9e63433ba81ac925a713120dd02e`, gates `d33471595176706e9a32002305d86efdbb35a2d264bc47bcec596cb0e504d7b5`, evidence `6b4e213d99f0ef64e64948af5f376a67defcbcbeb37bc119218cfa2e6dc70b4d`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
