# GL mark thresholds for j7elte (M1 / M3 gates)

Source of all baseline numbers: `docs/reports/gpu-verify.md` §§3.2–3.4
(Mesa 26.3.0 llvmpipe, LLVM 23.1.5, Xorg 720×1280 fbdev, tests at 320×240).
`code/userspace/gpu/check-3d.sh` supplies the device-node / dmesg /
interpretation pattern these scripts follow.

## 1. Renderer baseline (§3.2)

| Probe | Baseline (software GL) |
|---|---|
| `EGL_PLATFORM=surfaceless eglinfo -B` | `OpenGL ES profile renderer: llvmpipe (LLVM 23.1.5, 128 bits)`, ES 3.2, EGL 1.5; GBM/Wayland/X11 platforms `eglInitialize failed` |
| `DISPLAY=:0 glxinfo -B` | `direct rendering: Yes`, `Device: llvmpipe (LLVM 23.1.5, 128 bits)`, `Accelerated: no`, GL 4.6 Core / ES 3.2, vendor Mesa |
| `DISPLAY=:0 es2_info` | `GL_VENDOR: Mesa`, `GL_RENDERER: llvmpipe (LLVM 23.1.5, 128 bits)`, `GL_VERSION: OpenGL ES 3.2 Mesa 26.3.0` |

## 2. FPS baselines (§3.3)

`glxgears -info -geometry 320x240`: 173.678 / 220.863 / 188.528 FPS
(range **173–221 FPS**).

`glmark2-es2 -b :duration=2.0 --results fps` @320×240, 29 scenes
(30th cut by timeout; values verbatim, scene names shortened):

| Scene | Baseline FPS |
|---|---|
| build use-vbo=false | 67 |
| build use-vbo=true | 87 |
| texture nearest / linear / mipmap | 224 / 232 / 244 |
| shading gouraud / blinn-phong-inf / phong / cel | 39 / 33 / 20 / 23 |
| bump high-poly / normals / height | 15 / 110 / 46 |
| effect2d kernel / 1x3 box | 43 / 48 |
| pulsar light=false | 149 |
| desktop blur / shadow | 15 / 30 |
| buffer map/0.5 / subdata / map-interleave | 21 / 32 / 33 |
| ideas / jellyfish / terrain | 1 / 1 / 1 (CPU-bound llvmpipe, §3.3) |
| shadow | 21 |
| refract | 1 (CPU-bound llvmpipe) |
| conditionals (3 steps) | 85 / 86 / 105 |
| function low | 70 |

Negative evidence (§3.4, M1/M2 context): no `/dev/dri`,
`glmark2-drm` → "Failed to find a suitable DRM device",
weston DRM backend needs seatd/logind (absent).

## 3. PASS / FAIL rules

### M1 (`run-m1.sh`): all must hold, else FAIL + nonzero exit

1. `/dev/dri` exists AND contains `card0` and/or `renderD128`.
2. `/dev/mali0` present (kbase not regressed).
3. `/dev/fb0` present (display path intact).
4. X alive at **720×1280** (`xdpyinfo` dimensions check).

### M3 (`run-m3.sh`): FAIL + nonzero exit if

1. Renderer string (glxinfo / es2_info / eglinfo) contains `llvmpipe`,
   `swrast`, `Softpipe`, or is empty → FAIL (software GL).
2. Any capture step fails (missing tool, no FPS lines) → FAIL.

Per-scene rule (WARN only, never FAIL):
floor = max(baseline × 0.5, 5 FPS); a scene below its floor emits
`WARN:`. The four ~1-FPS scenes (ideas, jellyfish, terrain, refract)
are exempt from the floor but **must improve** over 1 FPS on hardware
GL; if not, the run must carry an explanation (still llvmpipe for that
path, scene unsupported, etc.).
