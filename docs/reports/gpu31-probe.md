# gpu31-probe — render-node classifier (kbase-vs-DRM) + JSON harness output

Slice: `code/gpu31/probe/` — extends the v1 `mesa-render/render-node-probe`
(H1/H2/neither, exit 0/2/1) with deterministic exit codes and JSON output
for the harness. Tested against the host model; live device run recorded
in §4.

Live-device note (2026-11-19, kernel #35 `3.10.108` SMP PREEMPT): phone is UP
over RNDIS/SSH. `/dev/mali0` present, `/dev/dri` absent, `/dev/fb0` present,
Xorg running, renderer `llvmpipe (LLVM 23.1.5, 128 bits)`. Earlier "link down"
reports are stale for this slice; no reboot was performed (cold boot is a
scarce shared resource with the GPS/FM workstreams).

## 1. What changed vs v1

`code/userspace/gpu/mesa-render/render-node-probe.c` classifies but always
exits 0 on any successful open (H1, H2, and NEITHER are all exit 0), so the
harness cannot gate on it, and it has no machine-readable output.

v2 (`code/gpu31/probe/render-node-probe.c`, self-contained C99, only
`<drm/drm.h>` + `<drm/panfrost_drm.h>` + libc, no libdrm):

- Deterministic exit codes: `0`=H1 (incl. H1+H2 hybrid, H1 path wins),
  `3`=H2-only, `4`=NEITHER, `2`=NO_DEVICE, `1`=ERROR (usage only; ioctl
  failures are classification signals, never exit 1).
- `--json` output: node availability (`exists`/`chardev`/`open`/`open_errno`
  as stable `E*` tokens), DRM identity (`driver`/`driver_desc`), `h1`/`h2`
  booleans, `t720` product gate (`GPU_PROD_ID == 0x0720`, null when the
  driver is not panfrost), the 9-value `params` map (null per failed
  GET_PARAM), the `kbase` handshake record (`answered`/`major`/`minor`/
  `too_old`/`errno`/`ret`), plus `verdict` and `exit` (the `exit` field
  always equals the process exit code — asserted by self-test T5).
- Same probe order and safety as v1: DRM identity first, GET_PARAM only on
  a panfrost driver, kbase UK 10.6 version-query last (no SET_FLAGS, no
  state change; ENOTTY on real DRM fds is the expected H2-negative).
- Fixes carried over from review: `check-aarch64` runs the shell suite
  under qemu via `PROBE_RUNNER` (v1 tried to exec the script under qemu);
  `kbase.errno` is null (not `"OK"`) when the node never opened.

## 2. Files

- `code/gpu31/probe/render-node-probe.c` — v2 probe.
- `code/gpu31/probe/Makefile` — native + `CROSS_COMPILE=aarch64-linux-gnu-`
  (static) builds, `make check` (native self-test), `make check-aarch64`
  (cross binary under qemu), `make clean`.
- `code/gpu31/probe/run-probe-tests.sh` — 21-assertion self-test suite
  (host model, no hardware; POSIX sh).
- `code/gpu31/probe/run-device-probe.sh` — device-side runner (root shell,
  no reboot/flash/build; logs under `/data/probe`).

## 3. Host-model test results (acceptance: probe passes its own tests)

Native (`make check`) and cross-under-qemu (`make check-aarch64`) both:
`PROBE SELFTEST: PASS (21 tests)`. Coverage: missing node → 2/NO_DEVICE,
regular file and `/dev/null` → 4/NEITHER with correct availability flags,
text/JSON exit agreement on all nodes, JSON `exit` == process exit, JSON
parses under python3 with the harness schema, bad flag / two positionals
→ 1, host `/dev/dri/renderD128` (nvidia-drm, non-panfrost) deterministic
exit 4 across runs with text/JSON agreement. Native JSON for the host node:

```json
{"node":"/dev/dri/renderD128","exists":true,"chardev":true,"open":true,
 "open_errno":null,"drm":true,"driver":"nvidia-drm",
 "driver_desc":"NVIDIA DRM driver","h1":false,"h2":false,"t720":null,
 "params":{},"kbase":{"answered":false,"major":null,"minor":null,
 "too_old":false,"errno":"ENOTTY","ret":0},"verdict":"NEITHER","exit":4}
```

Cross artifact: `aarch64-linux-gnu-gcc -static` → 880 KiB static aarch64
binary (`file`: ARM aarch64, statically linked); qemu smoke of the same
binary classifies `/nonexistent-xyz` → NO_DEVICE/exit 2 and `/dev/null` →
NEITHER/exit 4 with `ENOTTY` kbase-negative.

## 4. Live device state (kernel #35, read-only + probe open, no flash)

Recorded 2026-11-19 over short `flock /tmp/gpsdev.lock` SSH holds:

- `uname -r`: `3.10.108`; `/proc/version`: Integrator build #35
  (`#35 SMP PREEMPT Wed Nov 18 23:36:05 EST 2026`, gcc 16.2.0).
- `/dev/mali0` chardev present; `/dev/fb0` chardev present; `/dev/dri`
  absent (expected: `CONFIG_DRM=n` on #35, M1 gate pending).
- kbase bound: `/sys/devices/11400000.mali` and `/sys/module/mali_kbase`
  present. (Note: path is `/sys/devices/11400000.mali`, not
  `/sys/devices/platform/11400000.mali` as older scripts assume.)
- Xorg running; renderer `llvmpipe (LLVM 23.1.5, 128 bits)` — software GL
  only, as expected pre-M3.
- Root fs 94% full (`/dev/mmcblk0p20` 114 MiB free); `/data` 53% used —
  probe staging and all logs go under `/data/probe`.

Device probe run (2026-11-19T06:39Z): `run-device-probe.sh`
`DEVICE PROBE: PASS (skips: 1)`. Observed, verbatim from
`/data/probe/device-probe-20261119T063917Z.log` (also preserved as
`/data/probe/probe_dev_mali0-*.{txt,json}`, `nodes/dri/dmesg/eglinfo` logs):
`/dev/mali0` → H2/exit 3 (JSON `kbase UK 10.6 answered`, `drm:true` with
empty driver name); `/dev/dri/renderD128` and `card0` absent (skipped).
One dmesg line of note: `mali 11400000.mali: state restore latency
exceeded, new value 1067904 ns` — informational, not a failure.

Expected post-M1/M3 transitions (harness gates): M1 creates `/dev/dri`
(`run-device-probe.sh` SKIP becomes PASS); a panfrost render node flips
`/dev/dri/renderD128` to H1 (exit 0, `t720:true` on 0x0720 hardware); an H2
shim instead flips it to H2 (exit 3); `/dev/mali0` stays H2 throughout.

## 5. Harness contract

`run-device-probe.sh [probe] [outdir]` — device root shell, POSIX sh.
Asserts: kernel `3.10.108`, `/dev/mali0` + `/dev/fb0` chardevs (FAIL on
regression), `/dev/dri` presence (SKIP while `CONFIG_DRM=n`), kbase sysfs
bound, `/dev/mali0` → H2/exit 3. Logs to `$OUT/device-probe-$STAMP.log`
(default `/data/probe`). Integrator copies the static binary + script to
`/data/probe/` and runs one flock-held command; no reboot, no flash.
