# int72: force-arm the FRAME interrupt past the vsync refcount

int71's register readback named the bug exactly:

```
decon_kms_enable: decon_enable ret=0 state=1        (hmm: state 1 = INIT, see note)
decon_kms_enable: VIDCON0=00000007 VIDOUTCON0=01000000 VIDINTCON0=00000000 TRIGCON=00000040
decon_kms_arm_frame(...): VIDINTCON0 before=00000000
decon_kms_arm_frame(...): VIDINTCON0 after=00000000
decon_kms_commit: bounded vsync wait failed
```

`decon_activate_vsync()` (decon_core.c:1177-1201) only programs
`decon_reg_set_int()` + `DSIM_IOC_VSYNC` when
`vsync_info.irq_refcount` goes 0→1.  `decon_kms_register_vblank_cb()` took that
reference in `->load()`, before the first `decon_reg_init()`/`decon_reg_start()`
cleared VIDINTCON0, so every later refcounted arm saw `prev_refcount != 0` and
did nothing — VIDINTCON0 stayed 0 and the FRAME IRQ never fired.

## Fix (`0115-kms-force-arm-frame-irq.patch`, built-in shim)

* `decon_kms_register_vblank_cb()` only stores callback/data; it takes **no**
  reference (arming is per `decon_reg_start()`).
* `decon_kms_arm_frame(decon, where)` is now unconditional:
  * log `VIDINTCON0` + the refcount;
  * `decon_to_psr_info()` → `decon_reg_set_int(DECON_INT, &psr,
    DSI_MODE_SINGLE, 1)`;
  * log `VIDINTCON0` right after `set_int` (tells a lost write from a wrong
    mask);
  * `v4l2_subdev_call(decon->output_sd, core, ioctl, DSIM_IOC_VSYNC, 1)` with
    its return logged;
  * log `VIDINTCON0`/`VIDINTCON1` finally; if `VIDINTCON0` still reads 0,
    `decon_err("VIDINTCON0 still zero after arm")`.
* Call sites (3): after `decon_enable()` (which does `decon_reg_start()`),
  after `decon_kms_commit()`'s program (another `reg_start` on a full modeset),
  and once more after the bounded wait succeeds (the wait's own deactivate
  clears VIDINTCON0 again, so the IRQ stays armed between commits).
* `decon_kms_disable()` no longer deactivate-s vsync (no reference is held).
  `decon_wait_for_vsync()`'s own 0→1/1→0 bracket still works because the
  refcount is now 0 at rest; Main's "stop arming before/after the wait"
  concern is handled by that bracket being the only owner during a wait, with
  the explicit force-arm after it.

Note on the logged `state=1`: `decon_enable()` returns with `state` still
`DECON_STATE_INIT` when `psr_mode == DECON_VIDEO_MODE`?  No — the log line is
emitted after `decon_enable()` returned, and the value shown by Main was
`state=1`; the follow-up dmesg from this image prints the same fields, so the
state transition and the arm readbacks can be compared directly.

## Artifacts (image rebuild)

| file | size | sha256 |
|---|---|---|
| `Image-int72` | 18058984 | `a9cd9f80df28583c85c92b1de40ac5ac71d70384b77ac16d2ae2e6b4bc1c98db` |
| `kernel-boot-int72.img` | 27197440 | `75dd1df6a69654394b625ce7ed8fabbbb77f142e7e0543b0bf349e2a9ef93c61` |
| `exynos-drm7580-int72.ko` | 1434352 | `f521748e5b4947e8bf904fcf883510bd1b3cda1746c1549bfd9bbde990a135bb` |
| `drm_kms_helper-int72.ko` | 1443520 | `5f41142efad36adf90a7eefe98171cec303df45128da2e82f43b92eccee68c79` |

Load helper, then driver, after flashing.

## Gates (PASS)

All int71 gates plus: `decon_reg_set_int(..., 1)` and `DSIM_IOC_VSYNC` in the
shim, `decon_helper.h` included, no `decon_activate_vsync()` left in the shim,
exactly **3** force-arm call sites, the zero-readback error log, and
`VIDINTCON0 after set_int=` / `DSIM_IOC_VSYNC ret=` /
`VIDINTCON0 still zero after arm` in vmlinux.

## Expected dmesg

```
decon_kms_enable: decon_enable ret=0 state=...
decon_kms_enable: VIDCON0=... VIDOUTCON0=... VIDINTCON0=... VIDINTCON1=... TRIGCON=...
decon_kms_arm_frame(after decon_enable/reg_start): VIDINTCON0 before=00000000 refcount=0
decon_kms_arm_frame(...): VIDINTCON0 after set_int=0000....        <- non-zero, FRAME/INT_ENABLE
decon_kms_arm_frame(...): DSIM_IOC_VSYNC ret=0
decon_kms_arm_frame(...): VIDINTCON0 final=0000.... VIDINTCON1=...
decon_kms_enable: FRAME irq armed, VIDINTCON0=0000....
decon irq: VIDINTCON1=... frame=1                                   <- /proc/interrupts moves
```

If `after set_int` still reads 0, the log now distinguishes a lost write
(shadow/protect) from a mask problem, and the DSIM call's return is right next
to it.

Log sha256s: build `df40565ac5cfb0e3bb8c3eac9f9a373fcd254e5031988934f7df83a8e51f16ce`, gates `c8f7dd99cec1eac7d9513e079927b29f58cc7a799bd85971ace46499dfa01672`, evidence `8861ce0db0cc84d35af6c424f52e1a0e8f457b05aea9bde3a06381a9144fc42a`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
