# gpu31-fsonly — fragment-only path control for the FS (probe built; silicon pending)

Date: 2026-11-22. Owner: G31_FsOnly.
Device: **OFF USB at report time** — no ping and no sshd since ~04:47
(2026-11-22), awaiting a physical power cycle. Nothing flashed from this
ticket. int38 (`42d3a160…`) is built on the host but not on the phone.
Mesa 20.3.5 diagnostic build `/data/mesabuild/mesa20-b` (source
`/data/mesabuild/mesa20`) is present but its `libgallium_dri.so` is
**invalid after the watchdog reset** (SIGSEGV on load, see §4.2); the
`ninja -j1` repair is 125/279 steps in and is blocked on the phone.
No `PAN_MESA_DEBUG=sync/trace` was ever used.

Question: does the fragment shader write pixels through our fragment job
when invoked, with the vertex→tiler geometry leg out of the picture? That
separates the FS/RT half of the render path from **Defect A** (the
VS→tiler handoff: the tiler consumes the pre-VS content of
`Draw.Position`, so real draws bin nothing) and from **Defect B** (the
shader-ISA page clobbered by a stale dirty zero line, whose kernel fix is
in int38).

Code: `code/gpu31/fsonly/fsonly.c` (probe app), `run.sh` (device matrix),
`Makefile`. This report states the method, the expected outcomes, and the
**acceptance result: NOT MEASURED — device off USB, awaiting physical
power cycle.** §3/§4 are written so the measured lines can be appended
verbatim when the phone returns.

## 0. TL;DR

**Result not measured.** The phone is off USB (no ping, no sshd;
`18d1:d001` enumerated with no network), so no silicon run of this
ticket exists. Do not read any expected number below as a measurement.

What is established without silicon:

1. A pure "fragment job only" invocation is **not reachable from GL** on
   this GPU: the Midgard fragment job is a polygon-list consumer and the
   only producer of that list is the tiler (§1). `glClear` is a fast
   clear (tiler disabled, dummy `0xa0000000` list) and
   `RASTERIZER_DISCARD` removes the polygons, so neither invokes the FS
   over pixels.
2. The probe is built and validated to the extent possible: the app
   compiles on the phone (musl gcc, `-lEGL -lGLESv2`; it was built there
   at 04:03 and validates on llvmpipe with 65536/65536 magenta pixels —
   the classifier works, but that is software, not the Panfrost path).
3. The decisive device experiment is defined and ready: give the
   fragment job real coverage **without the app's geometry path** by
   writing a CPU triangle into the tiler's `Draw.Position` input
   (`PC2_PREFILL`/`PC2_PREFILL_BIG`, the discriminator proven in
   `gpu31-chainexec.md` §5), with the app's constant-magenta FS over a
   blue clear; then blue can only be the clear and magenta only the FS.

## 1. Why a pure FS invocation is not reachable from GL

The fragment job is a *polygon-list consumer*, not a shader launcher. In
Mesa 20.3.5 (`/data/mesabuild/mesa20/src/gallium/drivers/panfrost/`) the
whole fragment job payload is bounds plus a framebuffer descriptor
(`pan_fragment.c`, `panfrost_fragment_job()`: `bound_min/max_x/y`,
`payload.framebuffer`). The shader and its inputs live in that FBD; the
pixels to shade come exclusively from the tiler's polygon lists:

- `pan_sfbd.c`, `panfrost_sfbd_fragment(batch, has_draws)`: for a batch
  with **no draws** (`has_draws == false`, the clear-only batch) the tiler
  section is emitted with `vertex_count == 0`, and
  `panfrost_emit_midg_tiler()` (`pan_context.c`) takes its else branch:
  tiler disabled (`MALI_MIDGARD_TILER_USER` on T720, no hierarchy), a
  dummy polygon list whose only content is the terminator
  `0xa0000000`, and `heap_start == heap_end`. The clear is then the SFBD's
  `clear_color_*` fast-clear fields (`panfrost_sfbd_clear()`), not the FS.
- For a batch with draws the same function emits the app's Renderer State
  and the tiler's `polygon_list`/`polygon_list_body`; the fragment job
  rasterises only what the tiler wrote there.

Consequences for the possible GL tricks:

- `glClear` is exactly the clear-only fragment job above: it writes the
  clear colour with the tiler disabled and no polygons, so it proves the
  FBD/writeback path but says nothing about the FS.
- `RASTERIZER_DISCARD` (or a degenerate/scissored draw) removes the only
  producer of polygons; the fragment job then runs with an empty list and
  shades no pixels.
- A blit/quad is still a tiler draw; it does not bypass Defect A.

So the minimum fragment-only exercising path is: the app's fragment job +
a real polygon list, with the *vertex→tiler handoff* removed from the
equation. `gpu31-chainexec.md` §5 already proved the right lever: the
tiler consumes the pre-VS content of its `Draw.Position` buffer, so a CPU
triangle written there before submit is binned verbatim (record
`98197841 3c00bf7f a0000000` in tile 0). The probe uses that, which keeps
the compiler/job/RT path entirely Mesa's own — no invented polygon-record
ABI is involved. A hand-built fragment job would have to reproduce the
tiler's private polygon-list record format (not documented in this tree)
for no extra evidence about the FS itself.

## 2. Method

### 2.1 Probe app (`code/gpu31/fsonly/fsonly.c`)

256×256 EGL pbuffer, ES2. Clear to opaque blue `(0,0,1,1)`; draw a
full-screen quad (two triangles) with a trivial VS (pass-through
`gl_Position`) and a **constant-magenta FS**
(`gl_FragColor = vec4(1,0,1,1)`) — no varyings, no texture, no uniforms.
After `glFinish()` the whole target is read back and classified:
magenta (FS colour), clear (blue), other; it prints counts, the
magenta/other bounding boxes, three sample points, and
`FS_WROTE=0|1` (= magenta pixels present). Blue can only come from the
clear, magenta only from the FS, so `FS_WROTE=1` is an FS write that no
geometry or clear path can fake.

### 2.2 Coverage without the app's geometry (`PC2_PREFILL`)

`LD_PRELOAD=/data/polylist/polycensus.so` (`code/gpu31/polylist/polycensus.c`)
walks the chain submit, finds the tiler's `Draw.Position` GPU address
(job word 20/21), maps the BO and writes a CPU triangle over the app's
vertex data immediately before the submit:

- `PC2_PREFILL` → `(0,0),(8,0),(0,8)` px (tile 0 only),
- `PC2_PREFILL_BIG` → `(20,140),(236,140),(128,20)` px (covers the centre
  and most of the frame).

The tiler bins that triangle (proven in `gpu31-chainexec.md` §5), so the
fragment job receives a real polygon list and runs the app's FS — without
depending on the VS output that Defect A loses.

### 2.3 Controls

- **ctrl** — identical app/driver, no prefill: valid FS and an empty
  polygon list. Any FS pixels here would contradict the §1 model.
- **nofrag** — prefill, but `PC2_REFUSE_FRAG=1` makes the hook return
  `-EIO` for the fragment submit. The tiler still bins the CPU triangle,
  so a magenta pixel can only come from the fragment job (attribution).
- **small** — tiny prefill: the magenta bbox must be tile-0 local
  (`x,y ≲ 8`), proving the pixels follow the CPU-supplied polygon and not
  a stale RT or a full-screen clear artefact.
- **defectb** — prefill with `PAN_FBD_DEBUG=1` so compile-time and live
  ISA words are logged. On int38 (Defect-B dcache flush in the kernel) the
  live ISA must match the compile-time one and the case must write pixels;
  on a pre-fix kernel it is the zero-page case (0x52, no pixels).
- **presleep / recheck** — the pre-int38 Mesa-only workarounds for the
  clobbered ISA page (`PAN_FBD_PRESLEEP_US=2000000`,
  `PAN_FBD_RECHECK=1`); kept so the same matrix can separate Defect B from
  the FS on an older kernel without reflashing.

Every app run is wrapped in `/data/wdarm 18` (the SoC caps ~19 s), dmesg
is cleared before it and grepped for `p7580_job: (ERR|TIMEOUT)` and MMU
faults afterwards; each case is one `flock /tmp/gpsdev.lock` hold.

## 3. Results

**NOT MEASURED — device off USB, awaiting physical power cycle.** The
table states the expected outcome of each case so the run can be judged
in one glance; none of the "expected" cells is a measurement.

| Case | Prefill | Fragment submit | Expected pixels | Expected `FS_WROTE` | Expected dmesg |
|---|---|---|---|---|---|
| ctrl | none | yes | 65536 blue, 0 magenta (empty polygon list) | 0 | no ERR |
| prefill | big CPU triangle | yes | magenta bbox ≈ x 20..236, y 20..140; blue elsewhere | 1 | no ERR |
| nofrag | big CPU triangle | refused (`-EIO`) | 65536 blue, 0 magenta | 0 | no ERR |
| small | tiny CPU triangle | yes | magenta bbox ≈ x 0..8, y 0..8; rest blue | 1 | no ERR |
| defectb | big CPU triangle | yes | as prefill on int38; 0 magenta + 1× `0x52` on a pre-fix kernel | 1 on int38 | `0x52` only pre-fix |
| presleep | big CPU triangle | yes | as prefill (ISA page kept valid by the Mesa knob) | 1 | no ERR |
| recheck | big CPU triangle | yes | as prefill (ISA rewritten at emit) | 1 | no ERR |

Hook evidence expected in the same logs (`PC2_POLLSCAN`): for the big
prefill, `tiles_nonempty` > 1 with the centre tile non-empty; for the tiny
prefill, `tiles_nonempty=1` and `sample tile0: 98197841 3c00bf7f
a0000000`; for ctrl, `tiles_nonempty=0`, 256 terminators.

## 4. Evidence

### 4.1 Verified without the phone

- `fsonly.c` passes host `-fsyntax-only` and host link against
  EGL/GLESv2; `run.sh` passes `sh -n`; the app was cross-built on the
  phone at 04:03 before the outage (`gcc … -lEGL -lGLESv2`, 75032 B).
- The classifier was exercised with llvmpipe on the phone
  (`MESA_LOADER_DRIVER_OVERRIDE=panfrost` fell back to llvmpipe because
  the diag `.so` was already corrupt): 256×256 quad, clear blue,
  constant magenta → `clear=0 magenta=65536 other=0`, `FS_WROTE=1`.
  That validates the app/readback/classifier only; it says nothing about
  the Panfrost fragment job.
- No Panfrost measurement is usable from that window: the same runs
  produced either an all-zero RT or SIGSEGV, while `egltest` also
  SIGSEGV'd — the diag `libgallium_dri.so` was invalid (§4.2).

### 4.2 Why the diag Mesa is invalid (repaired next, not part of this ticket)

The watchdog reset that took the phone corrupted `mesa20-b/.ninja_log`
(`ninja: corrupt build log: missing field`); ninja then aborted mid-build
and the `.so` left behind SIGSEGVs at load (ELF maps, then `SIGSEGV
MAPERR NULL` before any app output; `egltest` as well as a trivial ES2
app). The build log was quarantined (`.ninja_log.corrupt.*`), a
device-only compile error in `src/gallium/drivers/panfrost/pan_assemble.c`
(`iw` undeclared in the `PAN_FBD_ISADUMP` block, line 412) was fixed, and
the rebuild is resuming with `ninja -j1` (default `-j` OOM-kills
`cc1plus` on the 1.4 GB phone). It is 125/279 steps in and cannot proceed
without the phone. A trivial load test plus mtime check is the gate before
any case runs, and G31_TilerDesc is waiting on that gate.

### 4.3 What the run must capture (append verbatim)

For each case: the app's `pixels total=/clear=/magenta=/other=`,
`FS bbox`, `FS_WROTE=` lines; the hook's `PC2_POLLSCAN` census
(`tiles_nonempty`, `sample tile0`); and the dmesg `p7580_job` lines
cleared before and grepped after the run. The acceptance sentence to
append is exactly: `<case>: clear=<n> magenta=<n> other=<n>, FS_WROTE=<0|1>,
tiles_nonempty=<n>, p7580 ERR=<n>`.

## 5. Implications

Not measured, so the following are the decision rule, not findings:

- If **prefill writes magenta** at the CPU triangle and **ctrl/nofrag
  write none**, the FS and the fragment-job/RT path are sound; the
  missing triangle is then geometry (Defect A) → the FS side of Defect B
  is a page-content issue only, and int38's flush should remove the
  `0x52` while Defect A remains the only blocker for real draws.
- If **prefill + valid ISA still writes no magenta**, the fragment path
  has a third defect downstream of coverage (shader fetch/meta/RT
  writeback) and that must be debugged before another geometry fix is
  attempted.
- In both cases, `ctrl` with a valid FS and an empty list must stay
  all-clear; anything else would mean the RT is being written by
  something other than the fragment job's FS.

## 6. Reproduce

```
# build the probe on the phone (no cross EGL/GLES2 headers)
scp code/gpu31/fsonly/fsonly.c user@172.16.42.1:/home/user/fsonly/
ssh user@172.16.42.1 'cd /home/user/fsonly && \
    gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L \
        -o fsonly fsonly.c -lEGL -lGLESv2'

# the whole matrix, one case = one lock hold
scp code/gpu31/fsonly/run.sh user@172.16.42.1:/home/user/fsonly/
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh \
    SSH_ASKPASS_REQUIRE=force ssh user@172.16.42.1 \
    "rootshell sh /home/user/fsonly/run.sh all"'

# one case (control): no prefill, valid FS, empty polygon list
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh \
    SSH_ASKPASS_REQUIRE=force ssh user@172.16.42.1 \
    "rootshell sh /home/user/fsonly/run.sh ctrl"'

# the decisive case: CPU triangle -> tiler -> app FS -> readback
flock /tmp/gpsdev.lock -c 'SSH_ASKPASS=/tmp/ap.sh \
    SSH_ASKPASS_REQUIRE=force ssh user@172.16.42.1 \
    "rootshell sh /home/user/fsonly/run.sh prefill"'
```

`run.sh` prints the app's `FS_WROTE=`, pixel counts/bboxes and the hook's
`PC2_POLLSCAN` census lines, then the dmesg `p7580_job` lines for that
case.

## 7. Files / device state

- `code/gpu31/fsonly/fsonly.c` — probe app (ES2, solid magenta FS over
  blue clear, full-target classifier).
- `code/gpu31/fsonly/run.sh` — device-side matrix + dmesg capture.
- `code/gpu31/fsonly/Makefile` — device-native build.
- Reused, not modified: `code/gpu31/polylist/polycensus.c` (`PC2_PREFILL`,
  `PC2_REFUSE_FRAG`, `PC2_POLLSCAN`), `/data/wdarm`, the Mesa 20.3.5
  diagnostic build at `/data/mesabuild/mesa20-b`.
- Device at report time: off USB, no network since ~04:47; int38 built on
  the host, not flashed; `mesa20-b` `.so` invalid with the `ninja -j1`
  repair at 125/279 steps.
