# gpu31-baseline — versioned llvmpipe baselines + HW acceptance thresholds

Slice contract: `code/gpu31/baseline/llvmpipe-v1.json` is the canonical,
versioned record (`schema: gpu31-baseline/1`). Every number carries
provenance; nothing here was invented. `check-baseline.py` is the machine
reader (it reads the JSON — threshold numbers live in exactly one place).
Per-scene glmark floors stay canonical in `code/gpu31/tests/scenes.tbl`
(31 rows, shared with `code/userspace/gpu/glmark-harness/`); the fresh
full per-scene table lives in the G31_MesaTests3 report; the egltest fps
line will live in `docs/reports/gpu31-egltest.md` (G31_EglTest).

## Device rules

- Host: `flock /tmp/gpsdev.lock -c '…ssh…'` one short hold (<120 s) per
  run; big logs default to `/data` on the device (root fs 94% full).
- Never reboot, never flash, never build the kernel (Integrator/M1Verify
  own that). Read-only GPU checks only — safe while GPS/FM share the phone.

## Versioning

- `llvmpipe-v1.json` → `llvmpipe-v2.json`: copy the whole file, bump `id`,
  append a `supersedes` note. Re-baseline only deliberately (new Mesa,
  new geometry/duration, new egltest binary) — never silently shift floors.
  v2 plan (recorded in the JSON): freeze the llvmpipe egltest fps line and
  activate the H6 fps gate.
- `check-baseline.py --baseline <file>` checks against any version; it
  rejects unknown `schema` values (exit 2).

## Usage

```sh
# grade one harness verdict dir after an --expect hardware run:
python3 code/gpu31/baseline/check-baseline.py \
  --renderer-txt  h-<stamp>/renderer.txt --expect hardware \
  --glmark-log    h-<stamp>/glmark2-es2.log \
  --xwd-parse     h-<stamp>/xwd-parse.txt --geometry 720x1280 --min-distinct 100 \
  --gpu-before    h-<stamp>/gpu-state-before.txt \
  --gpu-after     h-<stamp>/gpu-state-after.txt \
  --require-expl
```

Exit codes: 0 = no FAIL, 1 = FAIL, 2 = bad invocation/missing baseline.
H2/H3/H5 WARNs never FAIL; they print `(needs explanation)`, and
`--require-expl` FAILs unless the operator passes `--expl TEXT` for each
(recorded into the verdict notes). Single-input checks work too
(`--renderer-txt` alone, `--glmark-log` alone, …). Single-source rule:
to change a number or threshold, edit the JSON; the checker and the
report (`docs/reports/gpu31-baseline.md`) quote it, never duplicate it.
