# Camera 3A: what the kernel exposes, and the minimal AE in j7elte-cam

Status: **AE implemented through the shot metadata; no usable V4L2 3A
control exists**.  Closed-loop AWB is **not** implemented (justified below).
Nothing was run on hardware: this is a static + cross-build + simulated
controller verification.

Kernel: `android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
(`drivers/media/platform/exynos/fimc-is2`, Nura 3.10).

## 1. The V4L2 control surface has no usable 3A control

Every `VIDIOC_S_CTRL` case handled by any FIMC-IS2 video node
(`fimc-is-video*.c`) is a Samsung pipeline control.  Grepping for
`case V4L2_CID_` in those files yields only `V4L2_CID_IS_*`,
`V4L2_CID_SCENEMODE` and two `V4L2_CID_SENSOR_SET_*`.  Nothing equivalent to
`V4L2_CID_EXPOSURE`, `V4L2_CID_GAIN` or `V4L2_CID_AUTO_WHITE_BALANCE`
exists on any node.

The only exposure-related control is on the **sensor** node
(`exynos-fimc-is-ss0` / `ss1`), `fimc-is-video-sensor.c:535`:

```c
case V4L2_CID_SENSOR_SET_AE_TARGET:
        if (fimc_is_sensor_s_exposure_time(device, ctrl->value)) {
```

`V4L2_CID_SENSOR_SET_AE_TARGET` is `V4L2_CID_SENSOR_BASE + 1`
(`include/uapi/linux/videodev2_exynos_camera.h:189`).  It ends in
`fimc_is_sensor_s_exposure_time()` -> `CALL_MOPS(module, s_exposure, ...)`
(`fimc-is-device-sensor.c:1726-1760`), where

```c
#define CALL_MOPS(s, op, args...) (((s)->ops->op) ? ((s)->ops->op(args)) : 0)
```
(`fimc-is-device-sensor.h:277`).

Per sensor:

| sensor | `module->ops` | behaviour of the AE-target CID |
|---|---|---|
| S5K3L2 (rear) | **NULL** (`sensor/module/fimc-is-device-3l2.c:342`) | `CALL_MOPS` dereferences `module->ops` unconditionally -> **NULL pointer dereference in kernel context**. The node's `core_ops` only has `.init` (`:96-102`), so no subdev fallback can save it. |
| S5K5E3 (front) | `&module_5e3_ops` (`:785`) | `sensor_5e3_s_exposure()` writes `exposure & 0xFF` to register `0x600A` (`SENSOR_REG_VIS_AE_TARGET`, `fimc-is-device-5e3.c:514-543`). One 8-bit register, not a shutter line count. `s_again`/`s_dgain` are empty stubs (`:556-600`). |

`V4L2_CID_SENSOR_SET_GAIN` (`:201`) and `V4L2_CID_SENSOR_SET_AUTO_EXPOSURE`
(`:203`) are **not** handled by any switch: they fall into the `default:`
branch of `fimc_is_ssx_video_s_ctrl()`, which forwards to
`fimc_is_sensor_s_ctrl()` -> `v4l2_subdev_call(subdev_module, core, s_ctrl,
ctrl)`; neither sensor subdev defines `s_ctrl`, so the call fails with
`-ENOIOCTLCMD`.  `g_ext_ctrls` likewise just forwards to the sensor subdev,
which has no ext-controls op.

Consequence, and the reason this tool never issues those ioctls: **the only
in-kernel AE/gain control is either a no-op, a panic, or a single uncalibrated
register write.**

## 2. The real 3A control surface: the shot metadata

The driver hands the firmware a pointer to the userspace buffer's metadata
per frame group:

- `fimc-is-video.c:565-582` — for shot queues
  (`FRAMEMGR_ID_SHOT`), `ext_size = sizeof(camera2_shot_ext) -
  sizeof(camera2_shot)` (376), `frame->dvaddr_shot = buf_dva + ext_size`,
  `frame->shot_ext = buf_kva`, `frame->shot = buf_kva + ext_size`.
- `fimc_is_itf_grp_shot()` — `fimc-is-device-ischain.c:2443-2447` —
  sends `HIC_SHOT` with `param1 = frame->dvaddr_shot` (after only checking
  `frame->shot->magicNumber == 0x23456789` at `:2418`).

So the 30000-byte `struct camera2_shot_ext` that userspace writes into the
last (spare, 32 KiB) plane of an OUTPUT_MPLANE queue is exactly the memory the
firmware's 3A engine reads.  The kernel itself does not consume
`ctl.aa`/`ctl.sensor`: the only `shot->ctl` accesses in the driver are the
magic-number check and `ctl.vendor_entry`/`ctl.entry` parameter-index
bookkeeping (`fimc-is-device-ischain.c:1399-1407`, `:7488-7496`).

Fields used by `j7elte-cam --ae` (all documented in
`include/fimc-is-metadata.h`):

| field | meaning | citation |
|---|---|---|
| `ctl.aa.aeMode` | `AA_AEMODE_OFF = 1` disables firmware AE | `enum aa_aemode`, `:736-751`; member at `:894` |
| `ctl.sensor.exposureTime` | shutter, nanoseconds | `:230` |
| `ctl.sensor.frameDuration` | 1/fps, nanoseconds | `:231` |
| `ctl.sensor.sensitivity` | gain, ISO arithmetic units | `:232` |
| `ctl.aa.awbMode` | AWB mode (off/auto/incandescent/...) | `enum aa_awbmode`, `:788-798`; member at `:901` |
| `ctl.aa.aeTargetFpsRange[2]` | fps range reported to AE | `struct camera2_aa_ctl`, `:896` |

Readback for diagnosis (written by the firmware into the same buffer):
`dm.sensor.exposureTime/sensitivity`, `dm.aa.aeState` and
`udm.internal.vendorSpecific2[100..103]`, documented as
exposure / ISO(gain) / Bv / Tv (`:1293-1296`).

### What is dead in this build

`fimc_is_ischain_camctl()` (`fimc-is-device-ischain.c:7901-7940`) is the
function that would translate a `camera2_uctl` peripheral control into
`frame->shot->dm.sensor`, and `fimc_is_hw_s_camctrl_nblk()`
(`fimc-is-interface.c:3651`) is the `HIC_SET_CAM_CONTROL` sender it pairs
with.  Both are unreachable:

- `ENABLE_SENSOR_DRIVER` is **not defined anywhere in the tree** (only
  `#ifdef` sites: `fimc-is-device-ischain.c:7901,7946`; the older
  `fimc-is/fimc-is-device-ischain.c:7626,7671` and
  `fimc-is/fimc-interface.c:1624,1658`).  The block would not even compile:
  it uses `sensorUd.ctl.*` although `struct camera2_sensor_uctl` in
  `fimc-is-metadata.h` has flat fields.
- `fimc_is_hw_s_camctrl_nblk()` has **no callers**.

So the `uctl` path (`uctl.sensorUd`, `CAM_SENSOR_CMD = 1<<1`,
`fimc-is-metadata.h:1801`) is a kernel-side no-op here, and its
"backward compatibility" 32-bit `exposureTime` has no documented unit
(the HAL-1.0 predecessor embedded a `camera2_sensor_ctl`, i.e. ns, but the
HAL-3.2 struct does not say).  `j7elte-cam --ae` therefore **does not write
`uctl`**: guessing microseconds vs nanoseconds there could produce a 1000x
wrong exposure on the channel a firmware might prefer, while `ctl.sensor`
carries an explicit unit.

## 3. What was implemented

`parity/userspace/camera/j7elte-cam/j7elte-cam.c`:

- `shot_apply_3a()` — writes `ctl.aa.aeMode = AA_AEMODE_OFF`,
  `ctl.aa.aeTargetFpsRange`, `ctl.sensor.exposureTime/frameDuration/
  sensitivity` into the 3AA leader shot (`30s`/`31s`), and `ctl.aa.awbMode`
  for `--awb`.  Called for every 3AA shot (initial queueing in
  `pipeline_configure()` and every requeue in `pipeline_requeue_shot()`),
  so the state used for shot N+1 is the one measured from frame N.
- `frame_luma_mean()` — mean Y from the SCP YUV420M frame, every 4th pixel
  (1/16 of the frame).
- `ae_update()` — damped proportional controller on `exposure * sensitivity`
  (`new = old * (target + luma) / (2 * luma)`), exposure first, gain absorbs
  the remainder; 8x step out of black, /4 step out of clipping; both ends
  clamped; stops as `at_limit` when neither value can move.
- `shot_read_fw_3a()` + `ae_log()` — reads the firmware's DM/UDM result from
  the 3AA shot *before* `shot_fill()` memsets it, and prints
  `aeState`, exposure, ISO, Bv, Tv per iteration.
- `pipeline_capture()` — bounded loop: every SCP frame is measured and the
  PPM/raw output is rewritten, so the file always holds the last captured
  frame; the loop ends on convergence, at the limits, or after
  `--ae-frames` (default 10, max 60).
- CLI: `-A/--ae`, `-F/--ae-frames`, `-T/--ae-target` (default 110),
  `-D/--ae-tol` (default 8), `--ae-iso MIN:MAX` (default 100:800),
  `--ae-exposure-us MIN:MAX` (default 100 us : 90% of the frame period),
  `-W/--awb MODE`.
- Default behaviour (no `--ae`) is bit-for-bit the previous shot: the ctl
  block stays zeroed, one frame is captured, no readback is printed.

Safety properties: exposure/frame duration are clamped to `[100 us,
0.9/fps]` (never longer than a frame period, so frame rate cannot collapse);
ISO to `[1,6400]`; the loop is hard-bounded; nothing is written unless the
user asks for it; the tool never issues the legacy sensor-node CIDs, so the
S5K3L2 `CALL_MOPS` NULL dereference is unreachable from this tool.

## 4. AWB: why only a pass-through

`--awb` sets `ctl.aa.awbMode` (firmware AWB; `off` freezes whatever the
firmware holds).  There is no AWB *loop* because the ABI exposes no
per-frame white-balance gain that this client can drive with confidence:
`ctl.color.gains[4]` exists, but it is part of
`camera2_colorcorrection_ctl` (mode/transform/gains), i.e. the ISP colour
correction stage the setfile configures, not the AWB estimator; driving it
would change the picture only if the firmware honoured
`COLOR_CORRECTION_MODE_TRANSFORM_MATRIX` in a pipeline whose stage order is
unknown.  Implementing a grey-world loop on an unverified stage would have
been guesswork, so only the documented mode field is written.

## 5. Verification performed

Static / host (no camera hardware attached):

1. **Build**: aarch64 GCC 10.3, `-O2 -g -Wall -Wextra -Wshadow -Wformat=2
   -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wvla -std=gnu11`
   — no warnings.  Compiled from the *packaged* sources extracted from the
   patch, not from the working tree.
2. **ABI**: compile-time asserts hold — `camera2_shot_ext` = 30000,
   `camera2_shot` = 29624, `node_group` at 4, `shot` at 376 (added),
   `magicNumber` at 29616.  The 376 offset is what
   `fimc-is-video.c:575` computes as `dvaddr_shot`.
3. **Controller simulation**: a host harness (sensor model
   `luma = f(exposure*sensitivity)`, scratch file, not shipped) drove
   `ae_init()`/`ae_update()` over five scenes.  Converged in 1-6 frames for
   the reachable scenes; the out-of-range scene stopped after 2 frames with
   `at_limit` instead of looping to the bound.
4. **CLI**: valid/invalid `--ae-iso`, `--ae-exposure-us`, `--ae-frames`,
   `--awb` all take the expected path (rejected before any device access;
   a valid combination reaches node discovery).
5. **Packaging**: `git apply --check` against pmaports `ba682bd` passes, the
   applied tree is byte-identical to the package directory, the APKBUILD
   `sha512sums` verify, and the applied tree cross-compiles.

**Not verified** (no hardware): that the firmware's 3A honours
`ctl.aa.aeMode = OFF` and applies `ctl.sensor.exposureTime/sensitivity`; that
the DM/UDM readback is populated (a firmware that does not write it leaves
zeros, and the tool prints zeros rather than inventing values); the luma
target's photographic correctness; the `at_limit` thresholds on real sensor
gain granularity.

## 6. Residual blocker

The residual blocker for real 3A parity is **firmware-side, not kernel-side**:
the S5K3L2 has no in-kernel sensor driver (`module->ops = NULL`), so the only
actor that can change exposure or gain is the closed ISP firmware, which is
blob-only.  Whether it accepts manual `ctl.sensor` values instead of running
its own AE cannot be established without hardware or firmware source.  The
tool is written so that the answer is *observable* on a device: one
`j7elte-cam --ae -v` run shows the commanded exposure/ISO next to the
firmware's `aeState`, DM exposure/ISO and Bv/Tv for each frame.

Hardware test plan:

1. `j7elte-cam --ae -o ae.ppm -v` in a controlled scene.
2. Check that `AE frame N` lines move monotonically toward the target and
   that `firmware fcount` lines report non-zero `dm exposure`/`Bv`.
3. If luma moves with the published values, AE is effective; if the firmware
   readback stays 0 or ignores the command, the blocker above is confirmed
   and the fix is a firmware/setfile question, not a kernel one.
4. Never issue `V4L2_CID_SENSOR_SET_AE_TARGET` on `exynos-fimc-is-ss0`
   (S5K3L2): `CALL_MOPS` would dereference `module->ops == NULL`.
