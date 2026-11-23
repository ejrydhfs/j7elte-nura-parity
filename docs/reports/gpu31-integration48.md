# gpu31-integration48 — perf/diagnostic overlay (prof_arm sys-time counters + always-on submit_err); 32/32 gates, inert when unarmed

Date: 2026-11-22. Owner: G31_Integrator48. Phone: on int47; host-side
build only. No flash per ticket.

Consumes **ff078f3** (gpu31-mmlock's overlay) plus the indentation repair
**0211865**. Driver 0001 is unchanged at **04db08d** (`e7474670…`): the
overlay is a **separate patch**, `code/gpu31/perf/0001-panfrost7580-prof-overlay.patch`
(now sha256 `952ff750…`), applied after 0001/0002. Repo HEAD at build:
`0211865`. The build applies **10 patches** (9 previous + the overlay).

Purpose is diagnosis, not behaviour: per-class sys-time counters behind
`p7580/prof_arm` (default 0, no-ops unarmed) and an error-only counter
for `p7580_submit_err` so a SUBMIT failure after a recovery can no
longer be printed away.

## 0. TL;DR

- **Build green**: fresh scratch tree (birth 19:58:41, tree mtime
  20:02:46), `-j4` Image, RC 0, 0 `error:` lines, 5397-line log ending
  `OBJCOPY arch/arm64/boot/Image`; **499 warnings** — same ambient count
  as int47 (the overlay's one `-Wmisleading-indentation` was fixed in
  0211865, see §6).
- **32/32 gate banners PASS, 0 FAIL**: the 31 int47 gates (with
  `p7580_drv_submit_body` / `p7580_drv_map_bo_body` /
  `p7580_gem_job_done_sync_body` retargets, §3.1) plus new **6ai**.
- **Overlay contract, pinned in tree, objects and Image**:
  - `prof_arm` + `ns[9]`/`n[9]` + `submits`/`sub_err`/`dumps` in
    `struct p7580_drv`; 9 `P7580_PROF_*` classes; `p7580_prof_stamp()`
    returns 0 and `p7580_prof_add()` is a single flag test when unarmed
    (no `ktime_get`, no accumulation, no print);
  - `p7580_gem_job_done_sync`, `p7580_drv_map_bo` and
    `p7580_drv_submit` are wrapper + `_body` pairs: the unarmed branch
    calls the body directly, and the compiled wrapper reaches the body
    **before** its only `ktime_get` (gated by line order);
  - `p7580/prof_arm` (0600, default 0; write 1 = reset+arm, 0 =
    reset+off); one window per 100 SUBMITs, printed only while armed
    (`p7580prof: window=… create=… shmem=… mapbo=… submit=… mmap=…
    wait=… free=… freejob=… jobsync=… suberr=…`), then counters reset;
  - `p7580_submit_err()` no longer reads or decrements
    `p7580_sched_trace`; it has its own `p7580_submit_err_n` counter and
    logs every error (`… stage=%s rc=%ld n=%u`, prefix unchanged), with
    no `ev_arm` coupling — the EVRING dump is one-shot, the errno must
    survive a second recovery.
- **Object proof**: `t p7580_drv_submit_body`, `t p7580_drv_map_bo_body`,
  `b p7580_submit_err_n`, `r p7580_prof_arm_fops`, `U
  p7580_prof_debugfs_init` in main.o; overlay strings in `submit.o` and
  the Image.
- **Config**: 7/7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int48` `911e7bf1…`, `kernel-boot-int48.img`
  `70345435…` (§4).

## 1. Build

Script `code/gpu31/series/build-int48.sh` (3868 lines; sha256
`c3d9b1345b43845325f6e15e37c5d980d8c7fb0a9612c4de8893c09f53946836`),
a copy of int47 with int48 names, the overlay apply step, the body-symbol
retargets and step **6ai**.

Fresh `cp -a` of the pristine tree → `…/j7k/gpu31/scratch-int48`;
`TMPDIR` and every output on the external disk. Patch inputs verified
clean immediately before scratch creation (Main's rule).

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B 0001, dmabuf 0001, driver 0001+0002, then the perf overlay. The
inventory gate now scans all 10 and prints
`patch inventory: 10 patches, all new-file hunk counts exact`; the
overlay's sha is pinned twice (`952ff750…`) — at apply and by the
inventory scan — so a changed overlay fails the build before compiling.
Driver 0001 (`e7474670…`) re-checked at 6v.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int48-build.log` (5397 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`/`SYSMAP System.map`/
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (20:02:46, gated) ≥ build start enforced.
- Gate stdout: `…/logs/gpu31-int48-gates.log` (1335 lines, 32 PASS,
  0 FAIL).
- Evidence: `…/logs/gpu31-int48-evidence.log` (1378 lines).
- Host tests: `…/logs/gpu31-int48-hosttest.log` (driver T1–T11,
  chainsplit), `…/logs/gpu31-int48-schedhost.log` (412 ×2, gates PASS,
  kcheck PASS), `…/logs/gpu31-int48-jobtest.log`.
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:454): `repo HEAD=0211865 driver-0001
sha256=e7474670…`; overlay `952ff750…`.

Freshness: `Image-int48` has the same 18209976-byte size as int47;
9,702,509 bytes differ in the common prefix.

## 2. What changed in the image

| Applied site | Change |
|---|---|
| `drv.h` | 9 `P7580_PROF_*` class ids; `prof_arm`; `prof.ns[9]`, `prof.n[9]`, `submits`, `sub_err`, `dumps`; inline `p7580_prof_stamp()` / `p7580_prof_add()` (both armed-gated); `void p7580_prof_debugfs_init();`. |
| `gem.c` | `p7580_gem_job_done_sync` wrapper + `_body`; `p7580_prof_add` at shmem alloc, create (success and err_offset), free, mmap, wait (hit and ENOENT). |
| `submit.c` | `p7580_submit_err()` error-only counter + `n=`; `p7580_drv_submit` wrapper + `_body` with the 100-SUBMIT window; `p7580_drv_map_bo` wrapper + `_body`; `free_job` class; `p7580_prof_dump`, `prof_arm` get/set + fops, `p7580_prof_debugfs_init`. |
| `main.c` | `p7580_prof_debugfs_init(drv)` after dvfs probe. |

No functional change: unarmed, the wrappers branch straight to the body,
`p7580_prof_stamp` returns 0 before any `ktime_get`, and nothing prints.
The only always-on line is the submit-error one, and only when an error
actually occurs.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates: 31/31, retargeted only for the wrapper/body split

The overlay's sole code-shape change is that three hot functions are now
`wrapper + _body`. Every submit-internal gate therefore targets the body:

- `--disassemble=p7580_drv_submit` → `$SUBMIT_BODY`
  (`p7580_drv_submit_body`) in 17 places: 6j (add_excl), 6l
  (alloc_fence/arm), 6n (bo_count stamp), 6r (ledger), 6s
  (add_dependency ×3), 6t (serial), 6w (SPLIT_DIS: gem_create /
  job_alloc / job_submit / gpu_read ×3 / shadow_put / map_bo ×2), 6z
  (add_excl ×1);
- `--disassemble=p7580_drv_map_bo` → `$MAPBO_BODY`
  (`p7580_drv_map_bo_body`) in 6k (3 invalidates, 1 rollback unmap);
- source ranges `p7580_drv_map_bo` → `_body` in 6k and 6af
  (`awk '/^static int p7580_drv_map_bo_body/…'`);
- `p7580_gem_job_done_sync` → `p7580_gem_job_done_sync_body` in 6u's
  source check; its disassembly now shows the compiler-specialised
  direct call to `p7580_gem_invalidate_for_cpu` (the body is inlined),
  so 6u accepts the helper call and relies on the helper's own
  2-call clean-then-invalidate gate; the 0xCF read and pre-signal paths
  likewise accept `job_done_sync` as the completion leg — all of them
  are gated to route through it.

Nothing else changed: 6aa–6ag, 6ah and the older layout/ABI/IRQ/fence/
MMU/cachedir gates pass as in int47.

### 3.2 New 6ai: overlay present and inert when unarmed (PASS)

Pinned in source: state, 9 classes, stamp/add gating, the three
wrapper/body pairs and their unarmed direct call, `prof_arm` setter
(reset + `val ? 1 : 0`), 0600 debugfs file count 1, probe wiring, the
`p7580prof: window=…` format, one `prof_add` site per class, and the
submit_err rewrite (no `p7580_sched_trace`, `if (err)`, `++n`, prefix
intact).

Compiled: `t p7580_drv_submit_body`, `t p7580_drv_map_bo_body`,
`b p7580_submit_err_n`, `r p7580_prof_arm_fops`, `U
p7580_prof_debugfs_init`. Wrapper line order proves the unarmed path:

```
p7580_drv_submit:  bl <p7580_drv_submit_body>  …  bl <ktime_get>   (body first)
p7580_drv_map_bo:  bl <p7580_drv_map_bo_body>  …  bl <ktime_get>
```

Strings in `submit.o` and the Image:
`p7580prof: window=%u create=…` and
`p7580_sched: submit_err stage=%s rc=%ld n=%u`.

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim.

## 4. Outputs (sha256)

- `Image-int48` (18209976 B):
  `911e7bf15ffeef7661915a0c9c4deb98a2dc9fdea39bb04f3b342820aa11540c`
- `kernel-boot-int48.img` (27348992 B):
  `7034543578b243a64a2b9668a0239ce7b305a7240aae77ca310130c9246a9d7b`
- build log:
  `dbfb51ff20defca351fa902169f6416db9c2d65fcfe57d14e11bd17aa7941db5`
- gate stdout log:
  `464325fa7051f28c74b8be766bba0bfad46fb23c98ce5632df1628e6074b4483`
- evidence log:
  `4f36271993b3938cff533add9430d3ce05c46a9fd7c14e5cf3f5022cd286a14e`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `58cfc6dde434ce302d34e6c1956414f34ef124d243b0b0880416954e34a4f3f0`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `c3d9b1345b43845325f6e15e37c5d980d8c7fb0a9612c4de8893c09f53946836`
- overlay (in-repo):
  `952ff750afbd4d2f78fbc289f9284209331b05acd7702bd3e5875bfd7321aae4`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main, after flashing int48)

**Arming (MMLock's request):** `prof_arm=1` **only** for the
boot-window/contended 300-frame loop; leave it 0 for the acceptance
matrix so the measured numbers come from the same binary in its inert
configuration. Also `echo 1 > …/ev_arm` for that loop and for the 512²
pre-finish readback loop, so a fault's EVRING/PAGEDUMP and any
post-recovery submit error sit in one dmesg.

1. **Boot-window loop (armed)**: 300 frames at 256². Expect
   `p7580prof: window=N …` every 100 SUBMITs with per-class µs and call
   counts; if a silent 0x08 happens, `TIMEOUT … js_status=0x8` →
   `recover-done … cycle=1`, and any SUBMIT error after recovery now
   prints `p7580_sched: submit_err stage=… rc=… n=…` — that line names
   the failing stage for the Mesa `!ret` assert (pan_job.c:1268).
2. **Matrix (unarmed)**: 3×300 at 256² should reproduce FpsSplit's
   clean series (155.8 / 154.9 / 155.4 fps, 0 ERR/TO, Shmem 3.1–3.3 MB
   flat) and Main's 154–158 steady state; an outlier in the 107–133 band
   immediately after boot is the known silent-0x08/recovery state, not
   the shim (FpsSplit's attribution: client sys 5.4 → 1.7 ms/frame after
   the shim fix; the slow runs show 8.5 ms/frame).
3. **Readback**: 512² pre-finish loop armed; expect clean ok counts and,
   if the storm fires, one EVRING/PAGEDUMP capture plus the submit_err
   line.
4. **Churn**: `churn.aarch64 4 250 6` ×3 — FpsSplit's fixed client is
   rc=0, errs=0, kernel faults=0; that closes the earlier SIGSEGV item
   (the client, not the kernel).
5. **Inertness check**: with `prof_arm=0` the Image must show no
   `p7580prof:` lines at all, and the run statistics must match int47
   within noise.

## 6. Script notes

- **Four gate retargets were only discovered on the built tree**, which
  is why the first int48 run failed: (1) 6u's `DONE_SRC` pattern is
  `^static void …_body` (the overlay's body is `static`, not `void`);
  (2) 6u's disassembly no longer sees two `p7580_bo_sync_pages` calls in
  `job_done_sync` because the compiler specialises the unarmed path to a
  direct `invalidate_for_cpu` call (the helper's own 2-call order is
  still gated); (3) the 0xCF and pre-signal disassembly gates must accept
  `job_done_sync` as the completion leg; (4) the wrapper-order gates must
  match the local `<p7580_…_body>` call (no `R_AARCH64_CALL26` reloc —
  static, same-section), while `ktime_get` is a reloc.
- **Overlay repair 0211865**: the mmap `p7580_prof_add(..., P7580_PROF_MMAP, t0)`
  was indented under `if (!obj) return drm_mmap(...)` (correct — the
  return skips it — but `-Wmisleading-indentation` fired and it read as
  a bug). Re-indented; the overlay sha is now `952ff750…` and the build
  pins it. Warning count is back to the ambient 499.
- Retained: all-patch hunk inventory (now 10), overlay sha pin, RC/mtime
  freshness, defectb loop proof, config closure, driver-0001 drift
  re-check.
