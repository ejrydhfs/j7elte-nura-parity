# int75: bound the vendor vsync thread, KERN_ERR fb0 failure

int74's wedge was the vendor vsync thread, not the DRM code:

```
kthread_stop+0x70 <- decon_kms_put+0x1dc <- exynos7580_probe+0x154
stuck: [s3c-fb-vsync] decon_wait_for_vsync_thread+0x58
```

## 1. `decon_wait_for_vsync_thread()` — bounded stop

The loop slept in `wait_event_interruptible()` on a condition that cannot
become true during teardown (pipe off) and never checked
`kthread_should_stop()`, so `kthread_stop()` blocked forever; the lockup
watchdog does not fire because userspace is alive (single-task deadlock).

Now:

```c
while (!kthread_should_stop()) {
	timestamp = decon->vsync_info.timestamp;
	ret = wait_event_interruptible_timeout(decon->vsync_info.wait,
		!ktime_equal(...) && decon->vsync_info.active, HZ / 10);
	if (kthread_should_stop() || decon->kms_teardown)
		break;
	if (ret > 0)
		sysfs_notify(...);
}
```

`decon_kms_teardown()` already sets `kms_teardown`, turns the producer off
(`decon_reg_set_int(..., 0)`) and wakes `vsync_info.wait` **and**
`wait_frmdone` before stopping, so `kthread_stop()` completes within one 100 ms
poll.  The D-state skip from int73 stays as a last resort and should no longer
be reached.  The PSR "thread" is only a sysfs attribute (`create/destroy`
file), there is no loop to bound.

## 2. fb0 failure at KERN_ERR

The int74 run's dmesg was flooded by userspace faults, so the fb0 reason was
lost.  The postcondition failure now also emits an explicit `pr_err` with
crtc id/enabled/active/primary->state->fb/wanted fb, next to the full `dev_err`
line and the register dump (`decon_kms_log_state`) — all at KERN_ERR, so they
survive in the persisted 256 KB trace.

## Artifacts (image rebuild)

| file | size | sha256 |
|---|---|---|
| `Image-int75` | 18075368 | `d5508c28aa36484b9d35ba0a0977683708d6b6079b855b995c2065cb8d57a081` |
| `kernel-boot-int75.img` | 27213824 | `bb3ea58f45f9a918ae99a82eb7bbd1f5c324b9bf2bc26dbc544d024e4f2938b3` |
| `exynos-drm7580-int75.ko` | 1434352 | `bcf4b4edcfffb1934607fd8d87bc0d5b67a6dbb9a2073a299d4e0f533704d07b` |
| `drm_kms_helper-int75.ko` | 1443520 | `3316367a5d2faa7b84fb9448db295d2d08202140374a1aaa17330452398abc3f` |

Load helper, then driver, after flashing.  Lockup-panic config and all earlier
fixes stay.

## Gates (PASS)

All int74 gates plus: the vsync thread uses
`wait_event_interruptible_timeout(..., HZ/10)`, breaks on
`kthread_should_stop() || decon->kms_teardown`, the teardown wakes both queues,
and the new `fbdev scanout failed: crtc id=` `pr_err` string is in the module.

## Success criteria

`kms_rc=0`, `card0`/`fb0` persist, no Oops, DECON IRQ counts grow during a
flip, `dd` to `/dev/fb0` (or `kmsflip`) changes the panel.  If the load fails
again, `kms.trace` now holds the KERN_ERR fb0 line plus the register dump, and
the teardown cannot wedge in the vsync thread.

Log sha256s: build `620a9379e514748d97940c87870967241cfd0e315b7243f4a35bb5acee07dad2`, gates `cc724727200afbdf4ea48d5c8a5c4d4715f2faf268bee1800dccc070814ceea2`, evidence `e839a395cc07cfe35d1e6f3e221df4654cddd6b45b0ca0563c202ba2679543b9`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
