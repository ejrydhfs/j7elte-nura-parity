# GPU31-MesaTests — piglit subset + llvmpipe baseline (shader, texture, depth)

Slice: `mesatests` (G31_MesaTests3). Device UP at report time (kernel `#37`,
Mali-T72x MP2 r1p0 `0x0720` in `gpuinfo`, `/dev/mali0` present,
`/dev/dri` absent — no driver bound, X on fbdev 720x1280).
Live device runs under `flock` (rootshell for `/data` writes).

Related: `docs/reports/gpu31-tests.md` (G1–G5 harness this wires into),
`docs/reports/gpu31-baseline.md` (versioned llvmpipe numbers),
`code/gpu31/tests/run-harness.sh` (the harness; piglit subsection §4),
`code/gpu31/mesatests/` (the code: subset, runner, baselines).

## 1. What was built

`code/gpu31/mesatests/` (pre-existing collector scripts + new deliverables):

| File | Role |
|---|---|
| `run-mesatests.sh` | raw sweep collector (renderer probes, gears, glmark2, piglit stub) |
| `chunk.sh` | chunked reboot-resilient batch driver (one flock hold < 120 s per chunk) |
| `batch.sh` | older single-shot background batch (superseded by `chunk.sh`) |
| `make-subset.py` | subset builder: quick_gl → 60-test shader/texture/depth slice |
| `list-names.py` | list `quick_shader`/`quick_gl` profile test names |
| `one.py` / `fails.py` / `inspect.py` | device-side result-debug helpers |
| `piglit-subset.txt` (**new**) | the 60-test canonical list (this report's baseline input) |
| `run-piglit-subset.sh` (**new**) | run/compare wrapper with the fixed env; hardware re-run path |
| `results-llvmpipe-20261119.txt` (**new**) | per-test baseline verdicts (51 pass / 5 skip / 4 fail) |
| `glmark2-es2-llvmpipe-20261119.txt` (**new**) | glmark2-es2 sweep, Score 104, 33 FPS lines |

Harness wiring: `code/gpu31/tests/run-harness.sh` §G-piglit (new, optional,
`--skip-piglit` default-off only when piglit present) replays the
10-test smoke slice and diffs against the baseline; full 60-test runs use
`run-piglit-subset.sh compare`.

## 2. Baseline results (llvmpipe, 2026-11-19, kernel #37)

Renderer: `llvmpipe (LLVM 23.1.5, 128 bits)`, Mesa 26.3.0,
EGL 1.5 / GLES 3.2, GL 4.6 compat+core. X :0 fbdev.
glxgears 320x240: ~207–222 FPS. glmark2-es2: Score **104**, 33/33 scenes
with FPS lines (ideas=25 this run — variable; earlier sweep 1 FPS).

Piglit 60-test subset (`quick_gl`, `-1 -p glx`, fixed env §3):
**51 pass / 5 skip / 4 fail** (full table: `results-llvmpipe-20261119.txt`).

| Group | Tests | Result |
|---|---|---|
| shaders (glsl-vs/fs, uniform-limits, loop, sampler) | 13 | 13 pass |
| object namespace pollution (buffer/framebuffer/program/renderbuffer/texture) | 12 | 12 pass |
| spec !opengl 1.0/2.0/3.0 (scissor, samplers, cubemap, npot, generatemipmap) | 9 | 9 pass |
| spec fbo/generatemipmap + DSA + PBO + cubemap + array (incl. s3tc) | 12 | 12 pass |
| object namespace pollution vertex-array ×4 | 4 | 4 skip — `create_vertex_array requires GL_APPLE_vertex_array_object` |
| hiz depth-read fbo-d24-s8 | 1 | 1 skip — FBO incomplete 0x8cdd (no D24+S8 config) |
| hiz depth-read fbo-d24-s0 + window-stencil0/1, multithread-texture | 4 | 4 pass |
| glx-visuals-depth(+pixmap), glx-visuals-stencil(+pixmap) | 4 | 4 fail — `X Error BadDrawable ... MIT-SHM X_ShmPutImage` (§5) |
| ext_texture_array fbo-generatemipmap-array s3tc_dxt1 | 1 | pass alone (batch `incomplete` = 95 s-timeout kill artifact) |

## 3. Env fix that made the baseline real (read before re-running)

The first attempt logged **60/60 fail, rc=127**:
`Error loading shared library libwaffle-1.so.0: Exec format error`
+ `libpiglitutil*.so: No such file`. Two stacked causes:

1. `LD_LIBRARY_PATH` omitted `/data/pigroot/usr/lib/piglit/lib`
   (libpiglitutil*.so live there; the old `batch.sh` only had
   `/data/pigroot/usr/lib`). Direct-binary probe confirmed:
   binary resolves only with **both** entries.
2. The pigroot `libwaffle-1.so.0.8.3` had been overwritten by a broken copy
   (repaired 06:57, after the 06:50 summary); identical md5 to system lib now.

Canonical env (in `run-piglit-subset.sh`, `chunk.sh` REVISION 2):

```sh
export PYTHONPATH=/data/pigroot/usr/lib/python3.14/site-packages:/data/pigroot/usr/lib/piglit
export LD_LIBRARY_PATH=/data/pigroot/usr/lib/piglit/lib:/data/pigroot/usr/lib
export PIGLIT_SOURCE_DIR=/data/pigroot/usr/lib/piglit
export DISPLAY=:0 LIBGL_ALWAYS_SOFTWARE=1
```

Writes to `/data/mesatests` need **root** (dir is root-owned):
run via rootshell (`/tmp/dev110.sh` pattern), never the `user@` account.

## 4. Harness wiring (later hardware runs)

`code/gpu31/tests/run-harness.sh` gains an optional piglit gate:

- `--skip-piglit` (default: run it when piglit + subset list are present,
  skip silently otherwise — llvmpipe-only hosts without pigroot stay green).
- Replays a 10-test smoke slice (shader compile/link + texture + depth +
  scissor), each binary directly with `-auto -fbo`, no piglit framework.
- `verdict.json` gains `"piglit": {"run": bool, "pass": n, "fail": n,
  "skip": n}`; gate is WARN-only (never flips RESULT) until hardware data
  exists — llvmpipe is the reference, not a failure.
- Full 60-test hardware comparison:
  `sh run-piglit-subset.sh compare` → `regressions=N fixed=N` line;
  any `REGRESSION` (baseline pass → hardware non-pass) needs triage.

## 5. Known-failure notes (for the hardware diff)

- `glx-visuals-{depth,stencil}[ -pixmap]`: fail identically standalone
  (`BadDrawable ... MIT-SHM X_ShmPutImage`) — X/fbdev presentation issue
  on the llvmpipe+Xorg path, not GL rendering. If they pass on hardware
  (DRI), that is a FIXED, not a surprise.
- `ext_texture_array ... s3tc_dxt1` batch-`incomplete`: timeout kill, not a
  test property. Count it as pass (verified alone, rc=0); chunked runners
  should re-run remainder lists instead of `piglit resume` (resume
  re-parses the profile and KeyErrors on test-list runs).
- `piglit summary console/html` needs `mako` (absent on device) — use the
  `tests/*.json` shards or `results.json.bz2` directly (see `fails.py`).

## 6. Evidence

Device dir (root fs 94% full → all big outputs under `/data`):
`/data/mesatests/baseline-llvmpipe-20261119/` =
`piglit-subset.txt` + `piglit-remaining.txt` + `one-test.txt` +
`results-summary.json` + `results-table.txt` +
`glmark2-es2.log` (Score 104) + `glxinfo-B.log` + `es2info.log` +
`eglinfo-B.log` + `glxgears.log` (~207–222 FPS).
Raw piglit shards: `/data/mesatests/piglit-results/tests/` (49) +
`/data/mesatests/piglit-rest/results.json.bz2` (11) +
`/data/mesatests/piglit-one/results.json.bz2` (1, the `incomplete` re-run).
Repo copies: `code/gpu31/mesatests/{piglit-subset.txt,results-llvmpipe-20261119.txt,glmark2-es2-llvmpipe-20261119.txt,run-piglit-subset.sh}`.
All device commands under host `flock /tmp/gpsdev.lock` (< 120 s each).
No flash, no reboot issued by this slice (device rebooted itself once
mid-task; results survived, baseline subdir re-saved).
