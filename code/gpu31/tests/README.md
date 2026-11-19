# gpu31-tests — unified GL pass/fail harness (G31_Tests)

Slice scope: the five acceptance gates plus the one-command verdict:

- **G1 renderer string** (`glxinfo -B` / `es2_info` / `eglinfo -B`):
  classify `software` (llvmpipe/swrast/Softpipe) vs `hardware`
  (Mali/Panfrost/ARM/Exynos/kbase/lima) vs `hardware-unknown`
  (non-software but unrecognised id — PASS with a WARN to record it).
- **G2 glmark2-es2 score floor** (`tools/check-floors.sh` + `scenes.tbl`):
  WARN-only per scene, floor = max(baseline×0.5, 5 FPS); the four ~1-FPS
  CPU-bound scenes (ideas/jellyfish/terrain/refract) are exempt from the
  floor but must improve over 1 FPS on hardware GL. Gate FAILs only when
  the sweep produces <5 FPS lines (broken capture, not a slow scene).
- **G3 2D surface** (`xwd -root` + `tools/parse-xwd.py`): capture must parse,
  geometry must match `--geometry` (default: 720×1280 on the aarch64 target,
  unchecked on other hosts), distinct sampled colours ≥ `--min-distinct`
  (default 100; device proof in `gpu-verify.md` §3.1 had 2065).
- **G4 GPU utilisation sanity** (`tools/get-gpu-state.sh` before/after load):
  kbase sysfs (`utilization`, `time_in_state` cycles, `core_mask`,
  `power/runtime_status`) from
  `drivers/gpu/arm/t7xx/r15p0/platform/exynos/`. Software render ⇒ GPU idle
  expected (util=0, tis unchanged); hardware render ⇒ activity expected
  (util>0 or tis growth). Missing sysfs on a non-target host ⇒ SKIP.
- **G5 single-command verdict**: `run-harness.sh` (one invocation) writes
  `verdict.json` (machine) + `verdict.txt` (human) + `RESULT` (PASS/FAIL),
  plus the raw logs that produced them.

## Layout (this slice owns all of it)

- `run-harness.sh` — the one command. Runs green on llvmpipe today
  (`--expect software`, the default for `--expect auto` on a software
  renderer) and is ready for hardware (`--expect hardware` flips G1 to
  require a non-software renderer). Never FAILs on slowness: G2 floors are
  WARN-only by design (llvmpipe *is* the baseline, not the failure).
- `scenes.tbl` — the single scene-baseline table (shared with
  `code/userspace/gpu/glmark-harness/`; keep the two copies identical).
- `tools/parse-xwd.py` — stdlib-only XWD parser (key=value stats + top5).
- `tools/get-gpu-state.sh` — read-only kbase state dump (device, root not
  needed for reads; no writes, no jobs submitted).
- `tools/check-floors.sh` — scene-floor checker (WARN-only, exit 0 always).

Legacy `code/userspace/gpu/glmark-harness/` (`run-m1.sh`, `run-m3.sh`,
`THRESHOLDS.md`) is untouched and remains the M1/M3 reference; this slice
does not replace it, it unifies G1–G5 into one verdict for M3 sign-off.

## Device rules

- Host: `flock /tmp/gpsdev.lock -c '…ssh…'` one short hold (<120 s) for the
  whole run; big logs default to `/data` on the device (root fs 94% full).
- Never reboot, never flash, never build the kernel (Integrator/M1Verify own
  that). Read-only GPU checks only — safe while GPS/FM share the phone.

## Usage (on device, X up)

```sh
DISPLAY=:0 sh /data/glmark-harness/run-harness.sh --expect auto
# or with skips for a quick smoke:
DISPLAY=:0 sh run-harness.sh --expect software --skip-glmark --skip-gpu
```

One-page runbook: `docs/reports/gpu31-tests.md` §5.
