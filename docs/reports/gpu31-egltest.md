# gpu31-egltest — EGL/GLES2 textured-triangle smoke test (llvmpipe today, Mali expectations)

Date: 2026-11-19. Slice: `code/gpu31/egltest/`. Phone untouched, no flash,
no device access (binaries verified on the bench host; device run is a
copy-and-run recipe below).

## 1. What it is

Sibling of `code/gpu31/wsi/tri-x11.c` (untextured RGB triangle, window-only)
but with the two gaps that demo leaves closed:

- **Textured** draw: 2x2 RGBA checker sampled with `NEAREST`, so the
  sampler path (not just varyings) is exercised.
- **Offscreen** path: 256x256 EGL pbuffer render + `glReadPixels` check +
  optional `--dump FILE.ppm`, so headless bring-up (no X, plain SSH) can
  prove GLES2 executes before any window exists.

Files (this slice owns all of `code/gpu31/egltest/`):

- `egltest.c` — single C99 file, only `-lEGL -lGLESv2 -lX11`, zero warnings
  (`-Wall -Wextra`). Prints EGL + GL vendor/renderer/version first, then
  runs offscreen and (when `DISPLAY` is set) onscreen.
- `Makefile` — native build on host *or* device
  (`gcc … -o egltest egltest.c -lEGL -lGLESv2 -lX11`), `make check`.
  Built natively on device like `wsi/` — the cross sysroot has no
  X11/EGL headers, so there is deliberately no `check-aarch64` target.

Usage:

```
./egltest                         # both paths, window held 1 s
./egltest --offscreen-only        # headless; works over plain SSH
./egltest --onscreen-only         # fail fast (exit 1) when no DISPLAY
./egltest --frames N --hold SECS --size WxH --dump FILE.ppm
env -u DISPLAY ./egltest          # onscreen skipped with a note, exit 0 if offscreen passes
```

Exit contract: `0` = every executed check passed (onscreen skipped with a
note when no `DISPLAY`); `1` = EGL/GL setup or API error (incl.
`--onscreen-only` without `DISPLAY`); `2` = usage error (bad flag/size);
`3` = pixel self-check `MISMATCH`.

## 2. Pixel math (why these expectations)

Clear color `(0.1, 0.1, 0.4)` -> corner `(2,2)`, outside the triangle,
reads `~(26,26,102)` (tolerance ±24).

Triangle vertices (NDC / UV): `A(-0.8,-0.8, 0,0)`, `B(0.8,-0.8, 1,0)`,
`C(0,0.8, 0,0.5)`. Centre pixel NDC `(0,0)` has barycentric weights
`a=b=0.25, c=0.5` -> `uv=(0.25,0.25)`, the centre of texel `(0,0)` with
`NEAREST` -> **red `~(255,0,0)`** (tolerance: R within ±32 of 255, G/B ≤ 32).
The 2x2 texture is bottom-row-first: red | green, then blue | white.

## 3. Real output (bench host, 2026-11-19)

Offscreen forced onto **llvmpipe** (`GALLIUM_DRIVER=llvmpipe
LIBGL_ALWAYS_SOFTWARE=1`, the software baseline the phone uses today):

```
egltest: EGL vendor='Mesa Project' version='1.5' (1.5)
egltest: EGL client APIs='OpenGL OpenGL_ES '
egltest: [offscreen] GL vendor='Mesa' renderer='llvmpipe (LLVM 23.1.4, 256 bits)' version='OpenGL ES 3.2 Mesa 26.2.6-arch1.1'
egltest: [offscreen] GL shading language='OpenGL ES GLSL ES 3.20'
egltest: [offscreen] 60 draws in 0.008 s = 7972.4 FPS (256x256)
egltest: [offscreen] readback corner(2,2) RGBA=26,26,102,255 (expect ~26,26,102)
egltest: [offscreen] readback center(128,128) RGBA=255,0,0,255 (expect ~255,0,0)
egltest: [offscreen] RENDER_OK
egltest: PASS
```

Full client (both paths) on llvmpipe, same host:

```
egltest: [offscreen] 60 draws in 0.009 s = 7022.9 FPS (256x256)
egltest: [offscreen] readback corner(2,2) RGBA=26,26,102,255 (expect ~26,26,102)
egltest: [offscreen] readback center(128,128) RGBA=255,0,0,255 (expect ~255,0,0)
egltest: [offscreen] RENDER_OK
egltest: [onscreen] GL vendor='Mesa' renderer='llvmpipe (LLVM 23.1.4, 256 bits)' version='OpenGL ES 3.2 Mesa 26.2.6-arch1.1'
egltest: [onscreen] 60 frames in 0.025 s = 2401.8 FPS (320x240)
egltest: [onscreen] readback corner(2,2) RGBA=26,26,102,255 (expect ~26,26,102)
egltest: [onscreen] readback center(160,120) RGBA=255,0,0,255 (expect ~255,0,0)
egltest: [onscreen] RENDER_OK
egltest: PASS
```

Also exercised: `env -u DISPLAY` (offscreen-only via default display,
`onscreen skipped`, exit 0); `--onscreen-only` with no `DISPLAY`
(exit 1); bad `--size` (exit 2). `make check` passes (offscreen +
5-frame onscreen under `:0`). `--dump` wrote a 256x256 PPM whose pixels
were decoded independently: corner `(25,25,102)`, centre `(255,0,0)`,
opposite edges red|green — the textured triangle, not the clear color.

FPS numbers are host-throughput only (tiny 256x256 / 320x240 surfaces;
run-to-run variance is large) — recorded as smoke-test evidence, not as
a benchmark. The canonical llvmpipe fps line for baseline consumers is
the `[offscreen] 60 draws … FPS (256x256)` line above.

## 4. Mali expectations (for the bring-up rerun)

Run on the phone (native build; onscreen under the repo device lock):

```
gcc -O2 -Wall -Wextra -std=c99 -o ~/egltest ~/egltest.c -lEGL -lGLESv2 -lX11
./egltest --offscreen-only            # plain SSH, no X needed
flock /tmp/gpsdev.lock ./egltest     # full run with window
```

Expected changes vs the logs in §3, in order of bring-up:

1. **Today (pre-panfrost, fbdev X + swrast):** EGL vendor stays
   `Mesa Project`, GL vendor `Mesa`, renderer `llvmpipe (LLVM …)` —
   identical shape to §3 (device LLVM rev will differ, e.g. the
   `gpu31-wsi` run showed `LLVM 23.1.5, 128 bits`, `Mesa 26.3.0`).
   `RENDER_OK` + exit 0 proves the test harness itself on-device.
2. **After M3 (panfrost binds, kbase cut over):** EGL vendor still
   `Mesa Project`; GL vendor `Mesa`, renderer should read
   `Mali-T720` (panfrost, exact string per the shipped
   `panfrost_dri.so`); GL version string becomes the panfrost ES version
   (ES 2.0 minimum — the test only needs ES2 + pbuffer + `NEAREST`
   sampling, all in the T720 feature set, so no shader changes).
   Pixel expectations are API-exact and must hold bit-for-bit modulo
   the stated tolerances; the corner/centre readbacks stay the verdict.
3. **Perf:** offscreen FPS should rise vs the llvmpipe line; any number
   is informational (same tiny-surface caveat as §3). A *drop* vs the
   recorded llvmpipe line with `RENDER_OK` still passing points at
   DVFS/clock (G31_Devfreq/G31_Power), not at this client.

Relation to `gpu31-wsi.md`: that demo proved EGL-X11 *present* through
the fbdev server; this test adds texture sampling and a headless
pbuffer path so first-HW-GL can be claimed over plain SSH before any
window is mapped. `tri-x11`'s untextured expectations (`center ~64,64,128`)
are unrelated — do not compare its pixels with this test's red centre.
