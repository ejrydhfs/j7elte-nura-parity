# Screen bring-up: everything tried, and what it did

Two sessions: 2026-11-17 → 11-27 (GPU/KMS bring-up) and 2026-11-27 (this one).
Purpose: stop re-running ideas that did not change the panel.

## Changed the panel (keep these)

| Change | Effect |
| --- | --- |
| DECON KMS driver + DSI panel bring-up | Modeset works, panel lights, scanout latches (register dumps show `ADD0` follows the programmed fb) |
| int122: blank decision **before** `decon_kms_commit` + `last_fb[]` pinning | Blue/black **flashing stopped** (30/30 and 40/40 photos identical) |
| int126: bound `fb_shadow_write` by the shadow size; cache only frames ≥320×480 | Fixed `Unable to handle kernel paging request` in `exynos7580_fb_shadow_write+0x7c` that killed KWin's commit thread after one frame; KWin then committed 36+ frames |
| `modetest -s 21:720x1280` | Proved the panel follows DECON (blue → black); the DSI/panel is not stuck |
| KWIN_USE_BUFFER_AGE=0 (+ KWIN_FORCE_SW_CURSOR=1) | Removed the blue↔black alternation in the software session (8/8 photos identical), but the frame is near-black — no UI yet |

## Did not change the panel (discarded)

| Attempt | Result |
| --- | --- |
| Wallpaper plugin `org.kde.image`, re-apply + plasmashell restart | Good frame for a while (full repaint), then alternation returns |
| powerdevil / brightness writes (`/sys/class/backlight/panel`) | Nothing; AMOLED, the node is bogus (`actual 500 > max 355`) |
| `KWIN_COMPOSE=Q` (QPainter) | KWin exits: mode unsupported in 6.8 |
| `KWIN_COMPOSE=O2` vs `O2ES` | Same fragment-job hang / same crash |
| Blank heuristics: all-zero, uniform, ≤3 colours, 90 % near-black | Either let clear frames through or froze the panel on a stale shadow |
| `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` alone | GBM still allocated panfrost buffers → untouched (zero) frames |
| `MESA_LOADER_DRIVER_OVERRIDE=swrast` without dumb-buffer support | `DRM_IOCTL_MODE_CREATE_DUMB failed: Function not implemented` |
| `LIBGL_ALWAYS_SOFTWARE` with GBM on renderD128 | No dumb buffers until panfrost `dumb_create` was added |
| `LP_NUM_THREADS=0` | Made frames synchronous but did not produce client content |
| `GALLIUM_DRIVER=softpipe` | KWin survives (llvmpipe crashes), but frames still black |
| `QT_QUICK_BACKEND=software` for plasmashell | Shell loads the mobile-shell QML, still paints nothing visible |
| `fb0` writes / `fbtest` | No effect (CRTC keeps its own fb; fbtest wedged in D state) |
| Kernel panfrost `TIMEOUT-DIAG` + MMU/GPU IRQ unmask | No behaviour change; revealed the fault: `AS0 FAULTSTATUS=0x090002c3 FAULTADDRESS=0x0b2aa000`, `tiler did not idle` |
| `dumb_create`/`dumb_map_offset` in panfrost | Made kms_swrast+GBM viable (720×1292 dumb buffers), no UI yet |
| glibc `LD_PRELOAD` crash handler | Would not load (musl); rebuilt with device gcc |

## Diagnosed but not yet fixed

| Finding | Evidence |
| --- | --- |
| Fragment job never retires | `js ACTIVE(0x08)`, `head==tail`, `stat=rawstat=0`, full-screen 44×79 tiles, constant fault `0x0b2aa000` outside every BO in ERR-MAP |
| Tiler never idles | `pf7580: tiler did not idle on power-off, aborting suspend` after every recovery |
| KWin composited frames are clear-only | every 720×1280 commit samples 99.9 % black + cursor pixels; client content missing under both panfrost and software |
| DECON FRAME IRQ never armed for the pipe lifetime | vendor arms/disarms only around `decon_wait_for_vsync`; our shim bypasses the refcount → line mostly disarmed |
| llvmpipe crash | SIGSEGV at `libgallium-26.3.0.so+0x6cb5d0`, NULL destination write (only llvmpipe, not softpipe) |
| Mesa 20.3.5 stack missing | `/data/mesabuild/...` deleted; symlinks in `/data/mesa20sel` dangle. The 151 fps validation used that build |

## Open experiments (in flight)

1. Rebuild Mesa 20.3.5 for aarch64 musl and run KWin+plasmashell on panfrost (validated stack).
2. Compare vendor kbase (`drivers/gpu/arm/midgard`) with our panfrost port for the fragment-job/AS/tiler-heap difference.
3. Hold the DECON FRAME IRQ armed for the CRTC lifetime and deliver real vblank events.
