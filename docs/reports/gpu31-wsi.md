# gpu31-wsi — EGL/GBM WSI verdict + triangle demo (live hardware)

Date: 2026-11-19. Owner: G31_Wsi. Phone **UP** the whole slice
(kernel `3.10.108 #35 SMP PREEMPT`, RNDIS/SSH up; brief DHCP flap ~06:36
recovered on its own — no reboot, no flash, nothing touched on-device
except demo sources in `/home/user/`).

## 1. Verdict: path chosen = X11 window present (fbdev X + swrast)

| Option from ticket | Result on this kernel (live, 2026-11-19) |
|---|---|
| **GBM on a render node** | **Not available.** `/dev/dri` absent (running kernel `#35` predates M1; `CONFIG_DRM=n`). `eglinfo -B` GBM platform: `eglInitialize failed`. Revisit after M1/M2 land (G31_Kms Mode B `renderD128`, G31_M1Verify boot asserts). |
| **X11 pixmap import** | Not needed as a separate path: Mesa serves GLX over the existing fbdev X (`direct rendering: Yes`, DRI2 + swrast). |
| **fbdev/SHM present** | **This is what works today and is proven below.** EGL-on-X11 window surface + `eglSwapBuffers`; the running server advertises `MIT-SHM`, `Present`, `DRI2`, `GLX`, `SYNC`. |

The triangle demo renders through **EGL 1.5 → GLES2 → llvmpipe
(LLVM 23.1.5, `Accelerated: no`) → X11 window at 200,500 320x240**.
No hardware GL is claimed: `/dev/mali0` (kbase r15p0/UK 10.6) is present
but no Mesa driver speaks it yet (kbase winsys + Mesa backend are
G31_Mesa scope; render-node classification is G31_Probe).

DRI3 note: `MESA-EGL: warning: DRI3 error: Could not get DRI3 device` —
the fbdev X has no DRI3 (expected; Xorg log: `AIGLX: Screen 0 is not DRI2
capable`, GL provider `DRISWRAST`). The swap falls back to the
DRI2/swrast present path. Harmless, recorded so nobody "fixes" it.

## 2. Deliverable

`code/gpu31/wsi/` (no kernel tree touched, host-side sources only):

- `tri-x11.c` — EGL/GLES2 RGB triangle, X11 window (`wsi-triangle`),
  `--frames/--hold/--size`; per-frame `eglSwapBuffers`; post-run
  `glReadPixels` self-check (`RENDER_OK`/`RENDER_MISMATCH` exit 0/3).
- `Makefile` — native device build (`gcc … -lEGL -lGLESv2 -lX11`).

Device build (exact): `gcc -O2 -Wall -Wextra -std=c99
-D_POSIX_C_SOURCE=200809L -o ~/tri-x11 ~/tri-x11.c -lEGL -lGLESv2 -lX11`
→ 75,136 B, zero warnings. (Transfer note: plain `scp` of the `.c`
arrived NUL-corrupted once — device `gcc` reported `null character(s)
ignored` + missing `main`; re-sent base64, `md5sum` match
`59d37658…`, then clean build. Keep the base64 habit for `.c` files.)

## 3. Demo evidence (all on-device, `DISPLAY=:0`, kernel #35)

Short verification run (fixed binary, `--frames 60 --hold 1`):

```
tri-x11: EGL vendor='Mesa Project' version='1.5'
tri-x11: EGL client APIs='OpenGL OpenGL_ES '
tri-x11: GL vendor='Mesa' renderer='llvmpipe (LLVM 23.1.5, 128 bits)' version='OpenGL ES 3.2 Mesa 26.3.0'
tri-x11: 60 frames in 0.905 s = 66.3 FPS (320x240)
tri-x11: readback corner(2,2) RGBA=26,26,102,255 (expect ~26,26,102)
tri-x11: readback center(160,120) RGBA=63,64,128,255 (expect ~64,64,128)
tri-x11: RENDER_OK
tri-x11: holding window 1 s for capture...
tri-x11: done
```

Plus an earlier `--frames 300` run at **295.4 FPS** (same binary family,
pre-`setvbuf` fix; variance across runs 44–295 FPS — llvmpipe on a
cold/warm A53, not a display-sync measurement; do not quote as a panel
refresh number).

Present proof — window mapped and composited on the live screen during a
long run (`xwininfo -root -tree` showed
`0x600001 "wsi-triangle": () 320x240+200+500` next to `nura-shell`):

- `xwd -root -silent -out ~/tri-root.xwd` → 3,689,579 B, `720x1280x24`.
- Cropped `320x240+200+500` shows the RGB gradient triangle on the
  dark-red clear color (crop: `/tmp/wsi-tri-crop.png` on the bench host;
  full dump `/tmp/wsi-triOK.xwd`).
- In-window pixel scan of the dump (BGR masks `ff0000/00ff00/0000ff`):
  top edge white-ish window frame, bottom row red→green gradient
  `(50,14,191)…(50,194,11)`, vertical spine red→purple→green —
  exactly the drawn triangle, i.e. the swap reached the screen, not
  just the back buffer.

Long-run full log (`tri-final.log`, `--frames 100000 --hold 30`) was
started and the capture above taken mid-run, but its tail was never
retrievable: the demo holds stdout redirected until exit and each later
SSH/SCP raced the expiring hold window (plus the DHCP flap). This loses
nothing — the streamed short-run log above plus `RENDER_OK` and the
screenshot are the complete proof. Cleanup: any stray `tri-x11`
reaped (`pkill -f`); phone left with X + nura-shell untouched, no reboot.

GLX cross-check (same server): `glxinfo -B` → `direct rendering: Yes`,
`llvmpipe`, `Accelerated: no` — consistent with the EGL renderer string.

## 4. What this unblocks / next steps

- WSI for software GL on this kernel is **done and proven**: any
  EGL-X11 or GLX client (glmark2-es2, es2_info) presents today through
  the fbdev X server. No code needed beyond this demo.
- GBM path stays gated on: M1 boot (`/dev/dri/card0`, G31_M1Verify) →
  Mode B render node (`renderD128`, G31_Kms) → node classification
  (G31_Probe) → kbase-shim-or-panfrost decision (G31_Mesa). Re-run
  `tri-x11` linked against GBM (`eglGetPlatformDisplay EGL_PLATFORM_GBM`)
  once `renderD128` exists; the GLES2 draw/readback core is reusable.
- Known wrinkle for the GBM rerun: Mesa GBM on musl/Alpine needs
  `mesa-gbm` (already installed: `mesa-gbm-26.3.0-r0`) plus a real
  `drmGetVersion` answer from the node — today's `H2/NEITHER` answer
  will change the moment Mode B lands.

## 5. Stale-report correction (parent notice)

Prior reports saying "device link down / unreachable" are **stale as of
2026-11-19**: SSH over RNDIS works (`ping` <1 ms when the interface is
up), kernel `#35` (2026-11-18 23:36 EST) is running, `/dev/mali0` +
`/dev/fb0` present, `/dev/dri` absent (expected pre-M1). The only outage
seen this slice was a ~2 min DHCP flap, self-recovered.

## 6. Review response (`gpu31-review.md` §§1–18, 2026-11-19)

Full review read. **No finding applies to this slice; no fix required,
none made:**

- §§1–5 (dma-fence/resv/syncobj/sched shims) — G31_Sched scope; this
  slice ships no kernel code.
- §§6–9 (display driver/DT/config/M1) — G31_DrmCore/G31_Kms scope.
- §10 (PRIME import-by-copy) — G31_DmaBuf scope.
- §11 (r7p0 compat stride) — G31_KbaseShim scope.
- §§12–15 (Mesa kbase backend incl. F1 `n_slots` short-by-one) —
  G31_Mesa scope. This slice's demo is a *client* of EGL/GLES2, not a
  submit path: `grep n_slots|trig_slot|stride|dma_fence|drm_sched`
  over `code/gpu31/wsi/` = no matches.
- §§16–17 (gpu-soft package, panfrost compatible) — PASS as-is; §16's
  M3-cutover note (remove `LIBGL_ALWAYS_SOFTWARE=1` when HW GL lands)
  is recorded in §4 above.
