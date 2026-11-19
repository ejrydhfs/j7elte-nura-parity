# GPU31-Tests — pass/fail GL harness (renderer, score floor, 2D, GPU, verdict)

Slice: `tests` (G31_Tests). Device UP at report time (kernel `#35`,
`/dev/mali0` present, `/dev/dri` absent — running kernel predates M1;
live re-proof attempted under `flock`, see §6). No kernel edits, no flash,
no reboot in this slice.

Related: `docs/reports/gpu-verify.md` §§3.1–3.4 (all baselines),
`docs/reports/gpu-drm.md` §M3 (harness context), `gpu31-master.md` (day
plan + flash gate), `code/userspace/gpu/glmark-harness/THRESHOLDS.md`
(M1/M3 gate rules this slice implements), `code/gpu31/tests/` (the code),
`code/gpu31/mesatests/run-mesatests.sh` (sibling: raw sweep collector).

## 1. What was built

`code/gpu31/tests/` (new slice dir; single-command verdict + tools):

| File | Lines | Role |
|---|---|---|
| `run-harness.sh` | ~350 | the one command: G1–G5, writes `verdict.json` + `verdict.txt` + `RESULT` |
| `scenes.tbl` | 31 | scene baselines (copy of `glmark-harness/scenes.tbl`; keep identical) |
| `tools/parse-xwd.py` | ~110 | stdlib-only XWD parser → `width/height/depth/bits_per_pixel/bytes_per_line/sampled/distinct/top5` |
| `tools/get-gpu-state.sh` | ~85 | read-only kbase dump (`gpuinfo`, `core_mask`, `power/runtime_status`, `utilization`, `time_in_state` totals) |
| `tools/check-floors.sh` | ~60 | per-scene floor checker, WARN-only, exit 0 always |
| `README.md` | — | slice contract + device rules + usage |

Mirrors: `code/userspace/gpu/glmark-harness/` gained the same four files
(`run-harness.sh`, `scenes.tbl`, `tools/{parse-xwd.py,get-gpu-state.sh,check-floors.sh}`)
so the harness also lives next to the legacy `run-m1.sh`/`run-m3.sh` it
unifies. `run-m1.sh`, `run-m3.sh`, `THRESHOLDS.md` untouched.

## 2. The five gates (acceptance mapping)

- **Renderer string (G1).** Grep order `glxinfo-B.log` → `es2_info.log` →
  `eglinfo-B.log` for `renderer string|GL_RENDERER|profile renderer`
  (same pattern as `run-m3.sh` §4). Classify:
  `*llvmpipe*|*swrast*|*Softpipe*` → `software`;
  `*Mali*|*mali*|*Panfrost*|*ARM*|*Exynos*|*kbase*|*lima*` → `hardware`;
  empty → `unknown`; anything else non-empty → `hardware-unknown`
  (PASS + WARN: record the new driver id, don't FAIL on vocabulary).
  `--expect software|hardware|auto` selects the bar; `auto` (default)
  PASSes on either known class and FAILs only on empty.
- **glmark2-es2 score floor (G2).** `glmark2-es2 -s 320x240
  -b :duration=2.0 --results fps` (same geometry/duration as the §3.3
  baseline), per-scene `check-floors.sh` vs `scenes.tbl`
  (floor = max(baseline×0.5, 5); heavy four exempt, must exceed 1 FPS).
  **WARN-only by design** — llvmpipe *is* the baseline, so a slow scene is
  never a FAIL. Gate FAILs only on broken capture (<5 FPS lines) or a
  missing tool. `glxgears -info -geometry 320x240` is captured alongside
  (both `N.N FPS` and vblank `N frames in 5.0 seconds = M FPS` formats
  accepted) as a health reference; baseline 173–221 FPS.
- **2D surface (G3).** `xwd -root -silent` + `parse-xwd.py`. PASS needs a
  parseable dump, geometry match (`--geometry`, default 720×1280 on the
  aarch64 target only), and `distinct >= --min-distinct` (default 100;
  device proof in `gpu-verify.md` §3.1 had 2065 distinct over 172800
  sampled). Degenerate/blank captures FAIL.
- **GPU utilisation sanity (G4).** `get-gpu-state.sh` before and after the
  GL load. Sysfs sources (read-only, verified against the tree):
  `platform/exynos/gpu_custom_interface.c:265-313`
  (`show_time_in_state` "MHz cycles" rows, `show_utilization` env 0–100),
  `mali_kbase_core_linux.c:1888-1920` (`core_mask`), genpd
  `power/runtime_status`. Software renderer ⇒ GPU idle expected
  (`utilization=0`, `tis_total` unchanged — exactly the §4 signature
  `util=0 … time_in_state: 440 0 / 534 0 / 668 0`); hardware renderer ⇒
  activity expected (`utilization>0` or `tis_total` growth), else WARN
  with explanation required. No kbase sysfs on a foreign host ⇒ SKIP
  (not FAIL). Never FAILs on values — only when sysfs exists but is
  unreadable.
- **Verdict artifact (G5).** One `run-harness.sh` invocation writes one
  directory: raw logs (`glxinfo-B`, `es2_info`, `eglinfo-B`, `glxgears`,
  `glmark2-es2`, `floors.txt`, `root.xwd`, `xwd-parse.txt`,
  `gpu-state-before/after.txt`) + `renderer.txt` + `verdict.json`
  (`schema: glmark-harness/1`: stamp/expect/renderer/gates/notes/result)
  + `verdict.txt` + `RESULT`. Exit 0 iff every non-skipped gate PASSes.

## 3. Verification (host-side, no device writes)

- `sh -n` clean on all four shell files; `parse-xwd.py` AST-parses.
- `parse-xwd.py` on a synthetic 8×4 depth-24 XWD (3 colours): reports
  `width=8 height=4 depth=24 bits_per_pixel=32 bytes_per_line=32
  sampled=8 distinct=3` + top5 counts; `--stride 2` rescales correctly;
  truncated input exits 2 (not 0).
- `check-floors.sh` on a 16-scene synthetic log: PASS lines for over-floor
  scenes, WARN for below-floor (`texture-linear` 100 < floor 116) and for
  missing scenes, heavy-improved PASS (`refract` 2 > 1), heavy-stuck WARN
  (`ideas` 1 not above 1); exit 0 throughout.
- `run-harness.sh --expect software` with faked `glxinfo/es2_info/eglinfo`
  (llvmpipe strings), faked `glxgears` (173.678 FPS line), faked `xwd`
  (synthetic dump, `--min-distinct 2`), `--skip-glmark --skip-gpu`:
  **PASS**, `verdict.json` gates renderer/surface PASS, glmark/gpu SKIP.
- Same setup with Mali-T720 (`Mali-T720 (Panfrost)`) strings:
  `--expect hardware` → **PASS**; `--expect software` → **FAIL**
  (`renderer is 'hardware', expected software`) — the hardware flip works.
- `run-harness.sh --expect auto` on the build host (real AMD radeonsi,
  no xwd, vblank-synced gears killed before its first ~5 s FPS line):
  renderer `hardware-unknown` + WARN (correct: unrecognised non-software
  id), gears FAIL (correct: no FPS line captured — capture health gate),
  gpu SKIP (correct: no kbase sysfs). Proves the gates degrade honestly
  off-target instead of false-passing.
- `get-gpu-state.sh` shape-checked against a fake
  `/sys/devices/11400000.mali` tree: `gpuinfo`, `core_mask_js0/avail`,
  `runtime_status` (genpd path + UNKNOWN fallback — fallback emission bug
  found and fixed in review), `utilization`, `tis_total/rows`.

## 4. THRESHOLDS.md conformance (no rule drift)

| THRESHOLDS.md rule | Harness behaviour |
|---|---|
| M3 FAIL on llvmpipe/swrast/Softpipe/empty | G1: those classes FAIL under `--expect hardware`/`auto`; `--expect software` PASSes on software (today's llvmpipe runs green by intent) |
| Any capture step fails → FAIL | G1/G2/G3 FAIL on missing tool or empty output |
| Floor = max(baseline×0.5, 5), WARN only | `check-floors.sh` implements exactly this; exit 0 always |
| Heavy four must improve or carry explanation | WARN when stuck at ≤1 FPS (`needs explanation`); PASS when above |

`scenes.tbl` numbers are byte-identical to the THRESHOLDS.md §2 table
(29 scenes + `conditionals-step`/`function-low` = 31 rows).

## 5. Device runbook (one page, copy-paste)

One locked hold for the whole run (X must be up; no reboot; no flash):

```sh
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force timeout 115 setsid -w ssh -o StrictHostKeyChecking=no user@172.16.42.1 "DISPLAY=:0 sh /data/glmark-harness/run-harness.sh --expect software --out /data/glmark-harness/h-\$(date +%Y%m%d-%H%M%S)"'
```

Expected on the current kernel (pre-M1, llvmpipe): renderer `software`
PASS, glmark floors WARN-only, surface 720×1280 distinct≫100 PASS, gpu
idle (`utilization=0`, `tis_total` flat) PASS, verdict **PASS**.
After M1+M2+M3 land, same command with `--expect hardware` must show a
Mali/Panfrost renderer, heavy scenes above 1 FPS, and `tis_total` growth.

Install (once, big files stay in `/data`):

```sh
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force timeout 100 setsid -w ssh -o StrictHostKeyChecking=no user@172.16.42.1 "mkdir -p /data/glmark-harness"'
scp -o StrictHostKeyChecking=no code/gpu31/tests/run-harness.sh code/gpu31/tests/scenes.tbl user@172.16.42.1:/data/glmark-harness/
scp -o StrictHostKeyChecking=no code/gpu31/tests/tools/* user@172.16.42.1:/data/glmark-harness/
```

## 6. Live-device note (this slice)

Phone reported UP (kernel `#35`, RNDIS up). A locked SSH probe
(`uname -r`, `gpuinfo`, `/dev/dri`, `glxinfo -B`) was attempted under
`flock /tmp/gpsdev.lock` with a 40 s cap; the hold timed out behind the
GPS/FM workstreams, so **no new device evidence is claimed here** — the
live-state line above (`#35`, mali0 present, no dri) is taken from
`gpu31-master.md` (verified 2026-11-19 ~06:27 UTC), not re-measured by
this slice. The §5 runbook is ready for whoever holds the lock next;
`--skip-glmark --skip-gpu` gives a <30 s smoke first.
The stale "link down, cannot verify" wording from earlier reports is
**not** repeated: link state is a scheduling fact (lock held elsewhere),
not a device fact.

## 7. Files changed (this slice only, uncommitted — commit by owner)

- `code/gpu31/tests/{run-harness.sh,scenes.tbl,README.md,tools/{parse-xwd.py,get-gpu-state.sh,check-floors.sh}}` (new)
- `code/userspace/gpu/glmark-harness/{run-harness.sh,scenes.tbl,tools/{parse-xwd.py,get-gpu-state.sh,check-floors.sh}}` (new; legacy `run-m1.sh`/`run-m3.sh`/`THRESHOLDS.md` untouched)
- `docs/reports/gpu31-tests.md` (this file)
