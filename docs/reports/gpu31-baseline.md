# gpu31-baseline: versioned llvmpipe baselines + hardware acceptance thresholds

Source files (new files, green): `code/gpu31/baseline/llvmpipe-v1.json`
(canonical numbers; `schema: gpu31-baseline/1`), `code/gpu31/baseline/README.md`
(slice contract + device rules + usage).

## What this slice owns

The three evidence kinds named in the assignment — full glmark2 run,
"egltest" fps, 2D surface — plus the threshold table the runbook gates on:

- **`code/gpu31/baseline/llvmpipe-v1.json`** — the one versioned file.
  Every number carries `provenance` back to a device-measured report;
  nothing here was invented.
- **`code/gpu31/baseline/check-baseline.py`** — stdlib-only checker that
  reads the JSON and grades a live run (renderer class, `glmark2 Score: N`,
  xwd-parse stats, before/after GPU state). Exit 0 = no FAIL,
  1 = FAIL, 2 = bad invocation/missing baseline.
- **`code/gpu31/baseline/README.md`** — usage, device rules, versioning.

## The three baselines

| Kind | Baseline (llvmpipe, Mesa 26.3.0 / LLVM 23.1.5, kernel 3.10.108) | Provenance |
|---|---|---|
| Full glmark2-es2 sweep | **`glmark2 Score: 103`** @320x240, `-b :duration=2.0`; per-scene floors stay canonical in `code/gpu31/tests/scenes.tbl` (31 rows); the fresh full per-scene table lives in the G31_MesaTests3 report. Heavy-scene lows (fresh sweep): ideas=1, terrain=2, refract=4; non-heavy fresh range ~19–367 FPS | G31_MesaTests3 fresh full sweep 2026-11-19 (prior gpu-verify run was timeout-cut: 29/30 scenes, no Score line — see `gpu-verify.md` §3.3) |
| egltest fps | **pending-baseline** — do NOT substitute the glxgears triple below. egltest is a real, separate EGL+GLES2 binary owned by G31_EglTest (`code/gpu31/egltest/`, prints its own `N frames in S s = FPS` line, exit 0/1/3); its llvmpipe number was still being verified when v1 froze. Canonical home: `docs/reports/gpu31-egltest.md` | G31_EglTest (correction received this turn) |
| 2D surface | XWD 720×1280 depth-24, `bytes_per_line 2880` (4 B/px), sampled 172800 → **distinct 2065**; `xdpyinfo` 720×1280, root depth 24; `/dev/fb0` bpp 32 (raw `read()` returns 0 bytes on this DECON fb, so XWD is the evidence) | `gpu-verify.md` §3.1 |
| Reference (not egltest) | glxgears 320x240: 173.678 / 220.863 / 188.528 FPS (range 173–221). X11/GLX health reference only | `gpu-verify.md` §3.3 |
| GPU idle signature | `utilization=0`, `tis_total` flat across GL load (software render leaves kbase idle) | `gpu31-tests.md` G4 |

## Hardware acceptance thresholds (thresholds-v1, quoted in `llvmpipe-v1.json`)

| Gate | Rule | Severity | Command / source |
|---|---|---|---|
| **H1 renderer** | PASS: class `hardware` (Mali/mali/Panfrost/ARM/Exynos/kbase/lima); FAIL: `software`, empty, `unknown`. `hardware-unknown` (non-software, unrecognised): PASS + record the id | FAIL gate | `run-harness.sh --expect hardware` (G1: first renderer-string hit across glxinfo/es2_info/eglinfo logs) |
| **H2 Score** | PASS: Score ≥ 103 (llvmpipe parity @320x240); WARN with written explanation: 52 ≤ Score < 103; FAIL: Score < 52 (50% floor — driver path broken, e.g. sync stalls/readbacks) | graded | `glmark2-es2 -s 320x240 -b :duration=2.0 --results fps` → `glmark2 Score: N` |
| **H3 scenes** | PASS: every non-heavy scene ≥ floor `max(baseline×0.5, 5 FPS)`; every heavy scene above its must-exceed value (ideas>1, jellyfish>1, terrain>2, refract>4). Broken capture (<5 FPS lines): re-run, never a FAIL | WARN-only (never FAILs on slowness) | `tools/check-floors.sh scenes.tbl glmark2-es2.log` (implements the same rule; `check-baseline.py` mirrors the capture-health part) |
| **H4 2D surface** | PASS: parseable dump, geometry 720×1280, distinct ≥ 100 (llvmpipe proof had 2065). Anything else: FAIL (blank/degenerate capture) | FAIL gate | `xwd -root -silent` + `tools/parse-xwd.py` (G3; `--geometry 720x1280 --min-distinct 100` on target) |
| **H5 GPU activity** | PASS: `utilization>0` or `tis_total` growth across the load. Flat-0 under a hardware renderer: WARN + explanation. No kbase sysfs on a foreign host: SKIP | WARN gate | `tools/get-gpu-state.sh` before/after (G4) |
| **H6 egltest** | PASS: egltest exit 0 on hardware GL. Exit 1 (self-check) / 3 (setup) is a correctness FAIL from day one. No fps threshold yet — record the fps line, do not gate on it until baseline-v2 | correctness FAIL gate; fps gate deferred | egltest onscreen + offscreen (G31_EglTest) |
| **Overall** | HW PASS needs H1+H4 PASS, H2 not-FAIL, H6 exit 0, and every H2/H3/H5 WARN paired with a written explanation in the verdict notes (`check-baseline.py --require-expl --expl TEXT…` enforces the pairing) | — | runbook §5 |
## Acceptance checks (for Main — not run by this slice)

This slice authored the files; it ran no builds, tests, linters, or device
commands (per worker hand-off rules). Main verifies once after all slices
land. Suggested checks:

- `python3 -c "import json; d=json.load(open('code/gpu31/baseline/llvmpipe-v1.json')); assert d['schema']=='gpu31-baseline/1'"`
  plus presence of Score 103 / glxgears triple / distinct 2065 / egltest
  `pending-baseline`.
- `check-baseline.py` matrix (synthetic inputs, no device needed):
  renderer llvmpipe + `--expect hardware` → FAIL (rc 1); Mali-T720 string
  + `--expect hardware` → PASS; empty renderer file → FAIL; unknown
  non-software id → PASS + WARN.
- Score: `glmark2 Score: 103` → PASS; `Score: 70` → WARN (rc 0);
  `Score: 40` → FAIL (rc 1); Score-less log → WARN + fallback note.
- xwd-parse: `width=720 height=1280 distinct=2065` → PASS with
  `--geometry 720x1280`; `distinct=3` → FAIL; geometry mismatch → FAIL.
- gpu: flat `utilization=0` / flat `tis_total` → WARN (rc 0);
  `utilization=5` after → PASS; `MISSING` → SKIP.
- `--require-expl` with 1 WARN and no `--expl` → FAIL (rc 1);
  with `--expl` → rc 0; no-input invocation → rc 2.
- Traceability: every JSON number carries a provenance string naming the
  report or slice that measured it; the two pending items (egltest fps,
  fresh full per-scene table) name their canonical homes instead of
  duplicating them.

## Runbook note (for whoever holds the lock next)

One locked hold, X up, no reboot/flash/build (GPU slice is read-only —
safe while GPS/FM share the phone):

```sh
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force timeout 115 setsid -w ssh -o StrictHostKeyChecking=no user@172.16.42.1 "DISPLAY=:0 sh /data/glmark-harness/run-harness.sh --expect auto --out /data/glmark-harness/h-\$(date +%Y%m%d-%H%M%S)"'
```

After M1+M2+M3 land, re-run with `--expect hardware` and grade the
verdict dir with `check-baseline.py`:

```sh
python3 code/gpu31/baseline/check-baseline.py \
  --renderer-txt  h-<stamp>/renderer.txt --expect hardware \
  --glmark-log    h-<stamp>/glmark2-es2.log \
  --xwd-parse     h-<stamp>/xwd-parse.txt --geometry 720x1280 --min-distinct 100 \
  --gpu-before    h-<stamp>/gpu-state-before.txt \
  --gpu-after     h-<stamp>/gpu-state-after.txt \
  --require-expl
```

## Files changed (this slice only)

- `code/gpu31/baseline/llvmpipe-v1.json` (new)
- `code/gpu31/baseline/check-baseline.py` (new, executable)
- `code/gpu31/baseline/README.md` (new)
- `docs/reports/gpu31-baseline.md` (this file)
