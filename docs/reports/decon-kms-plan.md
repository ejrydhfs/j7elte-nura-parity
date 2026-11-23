# DECON KMS + atomic plan (Plasma Mobile prerequisite)

Status: **scoped, not started**. Owner: this repo. Evidence: `DeconKmsSurvey`
(survey of the vendor tree, all citations below are file:line in
`android_kernel_samsung_universal7580/`).

## 1. What blocks Plasma Mobile today

| Fact | Evidence |
|---|---|
| Plasma Mobile 6.8.3 + kwin 6.8.3 are already installed on the phone | `apk list -I`; `startplasmamobile`, `kwin_wayland` present |
| No KMS node: only `renderD128` | `/sys/class/drm/` on device |
| Display owned by legacy fbdev `decon_fb` | `decon_core.c:55-58, 4867-4890`; `/sys/bus/platform/drivers/decon/14830000.decon_fb` |
| `exynos-drm` has **no** device bound and cannot bind | `exynos_drm_core.c:132-136` returns `-EINVAL` with `fine_cnt == 0`; no exynos subdriver matches `samsung,exynos5-decon_driver` |
| The 3.10 DRM core has **no atomic at all** | no `drm_atomic.h`, no `DRIVER_ATOMIC` (`drmP.h:144-158`), `drm_plane_init` still in `drm_crtc.c:806`, `drm-y` has no `drm_atomic.o`/`drm_plane.o` |
| The vendor CRTC/plane vtables are legacy-only | `exynos_drm_crtc.c:188-197, 307-311`; `exynos_drm_plane.c:246-251` |
| KWin 6 requires atomic + universal planes | upstream requirement; reason M3 below matters |

So the missing piece is not a config switch: it is a **DRM atomic core backport plus a
DECON KMS subdriver**. The good news is that the hardware half already exists and ships
in the fbdev driver: window programming (`decon_set_win_config`, `decon_core.c:2185-2365`),
buffer/dma-buf attach (`decon_set_win_buffer`, `:1516-1737`), commit + shadow registers
(`decon_update_regs`, `:2066-2183`), enable/disable (`:894-1127`), vsync
(`decon-int_drv.c:73-124, 542-615`), and register programming (`decon_reg_7580.c:414-668`).

## 2. Chosen approach (ranked in the survey)

**(a) Extend the vendor `exynos_drm` with an atomic core + a DECON CRTC** backed by the
existing window API. Chosen because it reuses ~all hardware code; the alternative
(b) porting mainline `exynos_drm`/`decon5` drags in newer clock/power/DSI APIs that do not
exist in 3.10, and (c) a from-scratch minimal KMS still needs the same atomic core.

Hard constraint learned from the earlier attempt (`patches/drm-kms-coexist/DESIGN.md`):
**do not steal the `samsung,exynos5-decon_driver` / `samsung,exynos5-mipi-dsi`
compatibles** — that attempt bound DRM to them, the fbdev driver lost its device, and the
screen went dark with no `fb0`. Coexistence must be explicit (see M3).

## 3. Milestones

**M0 — atomic core backport (build-only; no device risk)**
Backport from Linux 3.14/3.15 into this 3.10 DRM core: `drm_atomic.c`,
`drm_atomic_helper.c`, `drm_plane.c`, `drm_framebuffer.c` (split out of `drm_crtc.c`),
plane helper, universal planes, `DRIVER_ATOMIC`, `DRM_IOCTL_MODE_ATOMIC`,
`DRM_CLIENT_CAP_ATOMIC`, atomic state structs + properties (`include/drm/drm_atomic.h`,
`drm_atomic_helper.h`, `drm_plane_helper.h`), plus `drm_modeset_lock`/crtc-state plumbing.
Constraint: must not regress the existing legacy paths other drivers use (this tree still
builds `exynos_drm` legacy + fbdev).
Acceptance: kernel builds clean; no new warnings in touched files; host-side compile of
the DRM core; boot on the phone with the same fb0/UI (atomic core present but unused);
`DRM_CLIENT_CAP_ATOMIC` reachable.
Estimate: 1–2 focused days.

**M1 — DECON KMS subdriver (card0, no compositor yet)**
New `exynos_drm_decon.c` subdriver: one CRTC + one primary plane + one overlay, encoder +
DSI connector (`dsim_0`), atomic check/commit implemented over
`decon_set_win_config`/`decon_update_regs`, vblank from the existing vsync work,
`drm_fb_helper` fbdev emulation on KMS. Modes from the panel (`ea8061`) timings already in
the board DTS. Coexistence: bind only when the legacy fb driver is disabled/absent, or via
an explicit Kconfig choice (M3).
Acceptance: `card0` appears; `modetest -M exynos` lists CRTC/planes/connector; `modetest -s`
draws a test pattern and survives 10 flips; fb0 path untouched when KMS is off.
Estimate: 2–4 days.

**M2 — buffer/fence integration with panfrost7580**
dma-buf import into KMS (the vendor `decon_set_win_buffer` already consumes dma-buf/ion),
fences on atomic commit (`drm_atomic_helper` waits in-fences), GBM allocation from
panfrost exported to KMS, flips at vsync. Validate with `kmscube`, then `weston-simple-egl`.
Acceptance: 300 consecutive flips of a panfrost-rendered buffer with no underruns, no
`0x08` timeouts, correct colours (bit-identical to a CPU readback of the same buffer).
Estimate: 2–3 days (overlaps GPU work; the GPU path is already stable at 155 fps).

**M3 — coexistence and safety**
Make KMS-vs-fbdev a DT/boot choice, not a race: e.g. `status="disabled"` on the fb node in
an alternative DTS, or a single driver that registers both fb0 (legacy) and card0 (KMS) and
forwards fbs. Default image keeps nura-shell working; KMS enabled by a boot parameter.
Acceptance: both images boot; with KMS off nothing changes (X11 UI intact); with KMS on the
legacy fb is not registered and KWin gets master. Rollback is a boot-image swap.
Estimate: 1 day.

**M4 — Plasma Mobile session**
`lightdm` → `startplasmamobile` on `kwin_wayland`; zram for the 1.4 GB RAM; input via
evdev; brightness/rotation via the panel driver. Acceptance: home screen interactive,
app launch, no OOM in 30 min, suspend/resume smoke.
Estimate: 1–2 days after M3.

Total: ~2 weeks focused, with M0/M1 independent of the phone (build-host only) and M2+
needing boot cycles.

## 4. Risks

| Risk | Mitigation |
|---|---|
| Atomic backport API drift (3.10 `drm_crtc.c` predates the plane/fb split) | M0 keeps the legacy paths untouched; gate new code behind `DRIVER_ATOMIC`; compile-only first |
| Losing fb0 again (prior attempt) | never claim the DECON/DSIM compatibles; coexistence is an explicit milestone with rollback |
| KWin 6 also needs `DRM_CAP_*`/format modifiers not present in 3.10 | validate with `modetest`/`kmscube` at M1/M2 before touching Plasma |
| Memory pressure (Plasma 6 on 1.4 GB) | zram + `MALLOC_ARENA_MAX`, and the leak/race fixes already landed |
| Panel/DSI timing only proven by the fb driver | reuse the same timings and the vendored dsi low-level ops (`decon_display/decon_mipi_dsi_lowlevel.c`) |

## 5. Definition of done

Plasma Mobile 6.8 home screen runs at a usable frame rate on the J7, with `card0` (atomic)
provided by the DECON KMS subdriver, panfrost scanout through dma-buf, and a documented
rollback to the current X11/nura-shell image.
