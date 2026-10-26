# Camera userspace: j7elte-cam

Status: **implemented (still capture + optional bounded AE)**.  Kernel drive
is at parity; the remaining scope is deliberate: no camera HAL, no JPEG, and
no closed-loop AWB (the ABI exposes no validated WB gain control).  The 3A
findings are in `camera-ae.md`.

## Kernel interface

The Nura `fimc-is2` driver exposes Samsung's camera-HAL video nodes rather than
a standard media-controller graph.  `j7elte-cam` therefore enumerates
`/dev/video*` through `/sys/class/video4linux/videoN/name` and selects the
FIMC-IS nodes by role:

- rear S5K3L2: `exynos-fimc-is-ss0`, `30s/30c`, `31s/31c`, `i0s`, `scp`
- front S5K5E3: `ss1/31s/31c/i0s/scp` (`--front`)

The driver registers no `media_device`, and several ioctl handlers are stubs
(`QUERYCAP`/`ENUM_FMT`/`G_FMT` on some nodes return 0 without filling the
structure), so the client never relies on them for identification or format
negotiation.  Frames are dequeued from the SCP node and converted to PPM.

There is no usable V4L2 3A control: the only exposure control,
`V4L2_CID_SENSOR_SET_AE_TARGET` on the sensor node, dereferences a NULL
`module->ops` on the rear S5K3L2 (`sensor/module/fimc-is-device-3l2.c:342`,
`CALL_MOPS` at `fimc-is-device-sensor.h:277`), writes a single 8-bit register
on the front S5K5E3, and `V4L2_CID_SENSOR_SET_GAIN` is unhandled.  The real
control surface is the shot metadata: the driver passes `frame->dvaddr_shot`
(the `camera2_shot` at offset 376 of the spare plane) as `HIC_SHOT` param1
(`fimc-is-device-ischain.c:2443`, `fimc-is-video.c:575`), so `ctl.aa` and
`ctl.sensor` are read by the firmware directly.  See `camera-ae.md` for the
source walk-through and for the dead `HIC_SET_CAM_CONTROL` path.

## Tool

`parity/userspace/camera/j7elte-cam/j7elte-cam.c` (single C file, no non-POSIX
dependencies) supports:

```
-l, --list                 list video/media nodes
-o, --output FILE          PPM (P6) output
-r, --raw FILE             raw Y/Cb/Cr planes
-s, --sensor-size WxH[@FPS] sensor mode (default 2072x1552@30)
-O, --out-size WxH         SCP output (default 1280x720, width % 16 == 0)
-n, --buffers N            3..8 (default 4)
-t, --timeout SEC          default 15
-f, --front                front S5K5E3
-m, --memory MODE          otf (default) or dma
    --range MODE           full (default) or limited
    --hal-version VER      1.0 or 3.2 (default 3.2)
-A, --ae                   bounded closed-loop AE through the shot metadata
-F, --ae-frames N          1..60 iterations (default 10)
-T, --ae-target N          target mean luma 1..250 (default 110)
-D, --ae-tol N             convergence tolerance in luma (default 8)
    --ae-iso MIN:MAX       ISO bounds (default 100:800)
    --ae-exposure-us MIN:MAX  exposure bounds in us (default 100 : 0.9/fps)
-W, --awb MODE             forward off|auto|incandescent|fluorescent|
                           warm-fluorescent|daylight|cloudy|twilight|shade
```

Without `--ae`/`--awb` the shot metadata carries no 3A control (the block
stays zeroed) and exactly one frame is captured.  With `--ae` the SCP frame is
measured (mean Y, every 4th pixel), `ctl.aa.aeMode` is set to `AA_AEMODE_OFF`
and `ctl.sensor.exposureTime`/`sensitivity` are driven by a damped
proportional controller until the luma is within tolerance, the exposure/ISO
limits are hit, or `--ae-frames` frames have passed.  The PPM is rewritten on
every iteration, so the file holds the last frame; the per-frame firmware
readback (`aeState`, DM exposure/ISO, Bv/Tv) is printed so a device run shows
whether the firmware honoured the manual values.

## Build and package

- Cross-compiled cleanly with the session's GCC 10.3 aarch64 toolchain:
  `aarch64-none-linux-gnu-gcc -O2 -pipe -Wall -Wextra -std=gnu11 ...`
- pmaports package `j7elte-cam` (APKBUILD + sources) under
  `parity/pmaports/device/archived/j7elte-cam/`.
- Exported patch:
  `parity/patches/userspace/camera/0001-pmaports-add-j7elte-cam-package.patch`
  (`git apply --check` against pmaports `ba682bd` passes).
- Runtime dependencies: `firmware-samsung-j7elte` (FIMC-IS firmware/setfiles,
  declared in `depends`) and read access to `/dev/video*` (group `video`).
- Package metadata: `makedepends="linux-headers"` because build-base/musl-dev
  do not ship the kernel uapi headers; `license="MIT AND Apache-2.0"` because
  the vendored `j7elte-cam-meta.h` is Samsung's dual GPL-2.0-only /
  Apache-2.0 `fimc-is-metadata.h`.

## Verification

- Source rebuilt from the packaged copy (extracted from the patch); no
  warnings with `-Wall -Wextra -Wshadow -Wformat=2 -Wpointer-arith
  -Wstrict-prototypes -Wmissing-prototypes -Wvla`.
- Patch applies to a clean pmaports `ba682bd` tree, the applied tree is
  byte-identical to the package directory, and the APKBUILD `sha512sums`
  verify.
- The AE controller was exercised on the host against a simulated sensor
  response (converges in 1-6 frames; stops with `at_limit` when exposure and
  ISO are both exhausted).  See `camera-ae.md`.
- No hardware was available, so no frame was captured and no 3A round-trip was
  observed.  This is a static, cross-build and simulation verification only.

## Hardware test plan

1. Install `linux-samsung-j7elte`, `firmware-samsung-j7elte`, `j7elte-cam`.
2. `j7elte-cam --list`; confirm the rear/front node names.
3. `j7elte-cam -o rear.ppm` and `j7elte-cam --front -o front.ppm`.
4. `j7elte-cam --ae -v -o rear-ae.ppm`; check that the `AE frame` lines move
   toward the target and that the `firmware fcount` lines report non-zero
   DM exposure/Bv (i.e. the firmware ran 3A and the readback works).
5. Check the PPM images and `dmesg` for FIMC-IS errors; test 640x480 and
   1280x720, `--memory dma`, and power-cycle/suspend-resume.

## Limitations

- No camera HAL and no JPEG encoder.  AE is a minimal bounded loop driven from
  userspace; autofocus is not implemented (the lens block is not touched) and
  AWB is only a mode pass-through (`--awb`), not a closed loop.
- Whether the firmware honours manual `ctl.sensor` exposure/gain cannot be
  checked without hardware; the tool prints the firmware's own readback so the
  answer is visible on a device.  That is the residual blocker for real 3A.
- The FIMC-IS ioctl surface is Samsung-specific and partly stubbed; the client
  is intentionally diagnostic rather than a camera HAL.
- Firmware and sensor calibration (`/efs`) must be present for the pipeline to
  start.
