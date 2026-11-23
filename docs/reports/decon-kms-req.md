# KWin 6.8 / Plasma Mobile — DRM-KMS requirement matrix (DECON KMS prerequisite)

Status: **complete** (static + binary analysis; no KMS node exists yet, so nothing below is
hardware-validated on `card0`). Owner: `KmsReq`. Feeds `decon-kms-plan.md` §3 (M0/M1/M2) and
slices `DRMAtomicM0`, `DeconCrtc`, `DeconPlane`, `DsiEncoder`, `KmsDmabuf`, `KmsTooling`.

Citation conventions
- `KWin src/…:N` — **kwin 6.8.3** upstream source (tarball
  `download.kde.org/stable/plasma/6.8.3/kwin-6.8.3.tar.xz`), which is byte-identical in
  version to the installed package (`kwin-6.8.3-r0`); every env-var/format string quoted
  below was also found with `strings` in the installed `libkwin.so.6.8.3`.
- `K3.10 …:N` — the vendor kernel tree
  (`android_kernel_samsung_universal7580-2a746c1…`, `3.10.108`, int49).
- `vX.Y path:N` — upstream Linux tag (fetched file contents; version facts are exact).
- `dev:` — read-only observation over SSH on `nura-j7elte`; anchored on
  `/tmp/gpsdev.lock`.

## 0. Verdict

1. **KWin 6.8 does not hard-require atomic at the API level.** It has a complete legacy
   pipeline (`KWin src/backends/drm/drm_pipeline_legacy.cpp`,
   `src/backends/drm/drm_commit.cpp:340-378`) and falls back when
   `drmSetClientCap(ATOMIC)` fails:
   `"drmSetClientCap for Atomic Mode Setting failed. Using legacy mode on GPU"`
   (`src/backends/drm/drm_gpu.cpp:185-232`; string present in the installed binary).
2. **But the fallback is not a shortcut on this tree**, and atomic stays the target:
   - legacy cursor needs `DRM_IOCTL_MODE_CURSOR2` (`drm_pipeline_legacy.cpp:287`), absent in
     3.10 (`K3.10 include/uapi/drm/drm.h` has no `0xBB`) and unsupported by the vendor
     exynos CRTC (no `cursor_set`/`cursor_set2`, `K3.10 exynos_drm_crtc.c:307-312`);
   - legacy forbids scale/crop (`drm_pipeline_legacy.cpp:54,87`), overlay planes, damage,
     tearing and per-plane formats;
   - `DrmCommitThread` is not even created in legacy mode (`drm_commit_thread.cpp:31-33`).
3. The **fatal** set is exactly rows **1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 17, 18, 19, 21,
   22, 23** (17 items): node/gbm/session, the atomic UAPI + blobs + object properties, one
   connected connector with modes, a Primary plane, PRIME import + ADDFB2, and vblank-armed
   flip events. Row 27 (`CURSOR2`) is fatal *only* if the legacy fallback is chosen. Everything
   else in this document is a performance/feature knob with a defined fallback.
4. Two **pre-existing blockers unrelated to DRM** were found and must be fixed before
   M4/`PlasmaSession` can be evaluated: no KMS node at all (expected) and a
   **kwin ↔ Qt 6.12 private-ABI break** that makes `kwin_wayland` fail to start today (§5.2).

## 1. Evidence base (what the target actually is)

| Item | Value | Evidence |
|---|---|---|
| KWin | `kwin-6.8.3-r0`, `/usr/sbin/kwin_wayland` 1 590 656 B, `libkwin.so.6.8.3` | `dev: apk list -I`; `ldd` |
| KWin ↔ libdrm surface | full list in §6 (e.g. `drmSetClientCap`, `drmModeObjectGetProperties`, `drmModeAddFB2WithModifiers`, `drmPrimeFDToHandle/HandleToFD`, `drmModePageFlip`, `drmModeSetCrtc`, `drmModeSetCursor`, syncobj, lease, `DRM_IOCTL_MODE_ATOMIC` via raw `drmIoctl`) | `dev: nm -D --undefined-only libkwin.so.6.8.3` |
| libdrm | `libdrm-2.4.134-r0` | `dev: apk list -I` |
| Mesa | `mesa-26.3.0-r0` (feeds `libgbm.so.1.0.0`, `/usr/lib/dri/*_dri.so`) | `dev: apk list -I`, `ls /usr/lib/dri` |
| Wayland stack | `wayland-protocols`, `libinput-1.32.0`, `elogind`, `libseat` | `dev: apk list -I` |
| Kernel | `3.10.108 #37 PREEMPT`, int49 | `dev: uname -a` |
| DRM nodes today | **only** `/dev/dri/renderD128` → `panfrost7580` (`mali@0x11400000`) | `dev: ls -l /sys/class/drm`, `cat .../renderD128/device/uevent` |
| Display owner today | fbdev `decon`, `/dev/fb0` 720×1280 | `dev: ls /sys/bus/platform/drivers/ | grep decon`; `virtual_size` |
| Kernel atomic | absent: no `drm_atomic.h`, no `DRM_CLIENT_CAP_*`, no `DRM_IOCTL_MODE_ATOMIC`/`SET_CLIENT_CAP`, `drm-y` has no atomic objects | `K3.10 include/uapi/drm/drm.h:706-781`; `grep -rn "ATOMIC\|SET_CLIENT_CAP" include/uapi/drm/drm.h drivers/gpu/drm/drm_ioctl.c` → empty |
| Vendor exynos DRM | `DRIVER_MODESET|GEM|PRIME`, dumb create/map/destroy, `gem_prime_import/export`, legacy CRTC vtable only | `K3.10 exynos_drm_drv.c:263-282`; `exynos_drm_crtc.c:307-312` |

Test tools **are** installed on the phone and their real option sets were read on the
device: `modetest` (`-a` atomic API, `-v` vsynced page flipping, `-C` hw cursor, `-P`, `-s`,
`-w obj:prop:value`, `-M module`, `-d drop master`) and `kmscube` (`-A` atomic+fencing,
`-D device`, `-c count`, `-f format`, `-m modifier`; links `drmModeAtomicCommit` and prints
`"no atomic modesetting support"` when the driver lacks it). So `KmsTooling` only needs a
wrapper script — run every probe under `flock /tmp/gpsdev.lock` and `timeout`
(the phone is single-session; concurrent probes serialise).

## 2. Requirement → driver-support matrix

Legend: **F** = fatal (no usable session), **f** = functionally required for the intended
GPU path, **o** = optional, KWin degrades gracefully.

### 2.1 Node, session, render-device wiring

| # | Requirement | Evidence | Cls | Driver support today (3.10/vendor) | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 1 | A **primary** KMS node (`DRM_NODE_PRIMARY`, `DRIVER_MODESET`) enumerated into the session's seat | `drm_backend.cpp:161-170,235-260`; `drmIsKMS` at `:245` | **F** | none: only `renderD128` | `ls /dev/dri`; `udevadm info -q property -n /dev/dri/card0` | register `card0`; Kconfig + DT node (M1/M3) |
| 2 | `gbm_create_device(card0)` must return non‑NULL, else the whole GPU is skipped | `core/drmdevice.cpp:194-200` (`"Failed to create gbm device"` → `nullptr`) | **F** | exynos has dumb buffers + driver name `exynos`; Mesa ships `/usr/lib/dri/exynos_dri.so` (kmsro, `panfrost` strings) — see §7.1 | `kmscube -D /dev/dri/card0`; `eglinfo -B` | `DRM_CAP_DUMB_BUFFER=1`, `dumb_create`; **keep `DRM_DRIVER_NAME="exynos"`** |
| 3 | DRM master/authentication via logind/seatd (`drmGetMagic`+`drmAuthMagic`) | `core/drmdevice.cpp:177-190`; `drm_backend.cpp:235-260` | **F** | core only; driver needs clean master drop/restore | start KWin on a VT | no driver change; `preclose`/restore must disable CRTC + vblank |
| 4 | A `RenderDevice` **compatible** with the KMS device (else software rendering) | `core/gpumanager.cpp:109-215` — devices on `DRM_BUS_PLATFORM` are considered mutually compatible | **f** | `panfrost7580` is platform-bus → compatible with exynos `card0` | `QT_LOGGING_RULES="kwin_*.debug=true"` → `updateRenderDevice` logs | none (property of bus type); panfrost must be able to import card0 buffers (§7.3) |

### 2.2 Atomic core

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 5 | `DRM_IOCTL_SET_CLIENT_CAP` + `DRM_CLIENT_CAP_ATOMIC`; KWin treats any failure as "use legacy" | `drm_gpu.cpp:190-193` | **F** | ioctl absent in 3.10 | `kmscube`/`modetest` | backport `drm_setclientcap`; `DRIVER_ATOMIC`; atomic cap must also set `universal_planes` (`v6.12 drm_ioctl.c:338-357`) |
| 6 | `DRM_IOCTL_MODE_ATOMIC` with: `TEST_ONLY` (combined with `NONBLOCK`), `NONBLOCK|PAGE_FLIP_EVENT` for flips, `ALLOW_MODESET` (blocking) for modesets; `TEST_ONLY|PAGE_FLIP_EVENT` must be rejected | `drm_commit.cpp:153-183,201-220`; upstream validation `v6.12 drm_atomic_uapi.c:1388-1416,1514-1521` | **F** | absent (3.10 ends at `0xBA`) | `modetest` atomic set; `kmscube` | atomic UAPI backport (donor ≥ v4.2, §7.2) |
| 7 | Property blobs: create (MODE_ID is a blob), destroy, get | `drm_blob.cpp:36,24`; `drm_connector.cpp:85-90` | **F** | `GETPROPBLOB` only; create/destroy absent (added v4.2) | `modetest -p` + mode set | backport `0xBD/0xBE` |
| 8 | `DRM_IOCTL_MODE_OBJ_GETPROPERTIES` on every CRTC/connector/plane at init (also used in legacy mode) | `drm_object.cpp:28-45` | **F** | present (`K3.10 drm_crtc.c:3148`, ioctl `0xB9`) | `modetest -M exynos -p` | keep |
| 9 | Plane enumeration returns planes (KWin never sets `UNIVERSAL_PLANES`; kernels skip non-overlay planes unless `universal_planes` **or** `atomic` is set — `v6.12 drm_plane.c:793-800`, and the atomic client cap sets `universal_planes`, `v6.12 drm_ioctl.c:346-357`) and every CRTC has a **Primary** plane, else the CRTC is dropped | `drm_gpu.cpp:183-232` (`m_atomicModeSetting = !m_planes.empty()`), `:233-259` (`"Could not find a suitable primary plane"`), `:910-950` | **F** | 3.10 exposes `GETPLANE(RES)` (`drm_crtc.c:1663,1710`) but the vendor CRTC has no planes | `modetest -M exynos` (plane list, `type=Primary`) | ≥1 Primary plane with `possible_crtcs` covering the CRTC |

### 2.3 Object properties KWin *requires* (atomic)

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 10 | CRTC `ACTIVE` + `MODE_ID` (`MODE_ID` as blob; `ACTIVE` 0/1) | `drm_crtc.cpp:22-23,93-97`; failure ⇒ CRTC init fails | **F** | 3.10 core creates neither (`grep SRC_X|"FB_ID"|"ACTIVE"|"MODE_ID"` in `K3.10 drivers/gpu/drm/*.c` → empty) | `modetest -p` | `drm_mode_config_init` globals + `drm_crtc_init_with_planes()` from the backport |
| 11 | Connector `CRTC_ID` | `drm_connector.cpp:297-300` (`"Failed to update the basic connector properties (CRTC_ID)"`) | **F** | same | `modetest -p` | connector property from `drm_connector_init`/`drm_connector_attach_encoder` path |
| 12 | Plane `type`, `SRC_X/Y/W/H` (16.16), `CRTC_X/Y/W/H`, `FB_ID`, `CRTC_ID` | `drm_plane.cpp:31-75` (declaration), `:112-124` (`"Failed to update the basic plane properties"` → plane dropped) | **F** | same as #10 | `modetest -p` | `drm_plane_init()` from the backport creates them |
| 13 | `IN_FORMATS` immutable blob + `DRM_CAP_ADDFB2_MODIFIERS` to use explicit modifiers | `drm_gpu.cpp:89-95`; `drm_plane.cpp:126-140` | o | neither exists (`IN_FORMATS` only from v4.14 `drm_mode_config.c`); cap absent in 3.10 `drm.h` | `modetest -p` (IN_FORMATS presence) | none: fallback = plane `formats[]` with `DRM_FORMAT_MOD_INVALID` |
| 14 | Plane `zpos`, `alpha`, `pixel blend mode`, `rotation`, `FB_DAMAGE_CLIPS`, `IN_FENCE_FD`, `SIZE_HINTS`, `IN_FORMATS_ASYNC`, `COLOR_PIPELINE` | `drm_plane.cpp:31-75,91-190` | o | absent | `modetest -p` | none; overlays only used when KWin can present them, single-output only (`drm_gpu.cpp:930-941`) |
| 15 | CRTC `GAMMA_LUT(_SIZE)`, `CTM`, `DEGAMMA_LUT`, `VRR_ENABLED`, `SHARPNESS_STRENGTH` | `drm_crtc.cpp:24-30,58-91` | o | absent | — | none (SDR, no night-colour, no VRR) |
| 16 | Connector `EDID`, `DPMS`, `link-status`, `panel orientation`, `scaling mode`, HDR/bpc/colour props | `drm_connector.cpp:265-355` (`edid` parsing is best-effort, `:305-310`) | o | `EDID`+`DPMS` auto-attached by 3.10 (`drm_crtc.c:718-726`); no EDID → model name = connector name, modes still required (#17) | `modetest -c` | must expose panel modes in `drmModeConnector::modes`, EDID optional |

### 2.4 Buffers: PRIME, ADDFB2, dumb

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 17 | Connected connector (`connection==CONNECTED`) with ≥1 mode (`hdisplay≥640` or `PREFERRED`) | `drm_connector.cpp:197-199,323-350`; `drm_gpu.cpp:300-340` | **F** | n/a (no DRM connector) | `modetest -M exynos -c` → `DSI-1 connected 720x1280` | DSI connector from ea8061 timings (M1/`DsiPanel`) |
| 18 | `drmPrimeFDToHandle()` on the **KMS fd** per plane + `drmCloseBufferHandle()` | `drm_gpu.cpp:975-990` (`"drmPrimeFDToHandle failed"` → buffer import fails) | **F** for GPU | vendor exynos has PRIME import/export (`exynos_drm_drv.c:280-282`, `exynos_drm_dmabuf.c:212-260`) | `kmscube` | GEM import of panfrost dma-buf on `card0` (tc/dma-buf attach, 1 linear plane) |
| 19 | `drmModeAddFB2()` accepts the imported GEM (XRGB8888/ARGB8888, plane 0, pitch/offset); `drmModeAddFB2WithModifiers` only when cap; `drmModeAddFB` is an old-ABI fallback | `drm_gpu.cpp:995-1035` | **F** | `ADDFB2` ioctl exists (`K3.10 drm.h:732`), exynos `fb_create` (`exynos_drm_fb.c:148-270`) — must be proven with imported GEM | `modetest -s` with `-P` on an fb from panfrost | `fb_create` that wraps the imported GEM into a DECON window (`decon_set_win_buffer`) |
| 20 | FB teardown: `drmModeCloseFB` with **explicit fallback** to `drmModeRmFB` in KWin | `drm_buffer.cpp:43-44`; libdrm `drmModeCloseFB` is a bare `DRM_IOCTL_MODE_CLOSEFB` (no fallback, `libdrm-2.4.134 xf86drmMode.c:323-331`) | o | `CLOSEFB` absent in 3.10 ⇒ KWin's fallback path is what runs | leak check after 300 flips | none beyond `RMFB` |
| 21 | Dumb buffers: `CREATE_DUMB`/`MAP_DUMB`/`DESTROY_DUMB` on the KMS node; `DRM_CAP_DUMB_BUFFER` for Mesa | `core/gbmgraphicsbufferallocator.cpp:128-148,336`; `gbm_bo_create` path `:160-205` | **F** (indirectly, via #2) | vendor exynos has all three (`exynos_drm_drv.c:277-279`) | `modetest -s` + `kmscube` | keep `dumb_create` (+ `gem_prime_handle_to_fd` for the dumb→dma-buf export KWin does at `:139`) |

### 2.5 Events and timing

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 22 | Page-flip completion event with `user_data` round-trip, `DRM_EVENT_FLIP_COMPLETE`, vblank `tv_sec/tv_usec` | `drm_gpu.cpp:587-632` (`drmEventContext.version=3`, `page_flip_handler2`) | **F** — commits are serialized on the event; a missing event logs `"Pageflip timed out! This is a bug in the … kernel driver"` after **1 s** (`drm_gpu.h:77`, `drm_commit_thread.cpp:43-64`) | `drm_send_vblank_event()` exists (`drm_irq.c:861-876`); no `drm_crtc_arm_vblank_event()` (first upstream **v4.4**, see §7.2) | `kmscube` runs >10 s; check `dmesg`/kwin log for timeout | event armed at next vblank from the vblank IRQ; `drm_crtc_handle_vblank()` in IRQ |
| 23 | Vblank machinery: `get_vblank_counter`/`enable_vblank`/`disable_vblank` | `K3.10 exynos_drm_drv.c:271-274` hooks exist for the vendor CRTC; DECON CRTC must provide its own | **F** (for #22) | vendor hooks are legacy-CRTC based | `kmscube`/`dmesg`; KWin `"Got invalid timestamp"`/`"Pageflip timed out"` | DECON frame counter + IRQ; driver must call `drm_crtc_handle_vblank()` |
| 24 | `DRM_CAP_TIMESTAMP_MONOTONIC` | `drm_gpu.cpp:83-89` (else `CLOCK_REALTIME`, converted at `:600-612`) | o | cap exists in 3.10 (`drm.h:781`), value from module param `drm_timestamp_monotonic` (`drm_ioctl.c:290`, `drm_irq.c:730-740`) | `cat /sys/module/drm/parameters/timestamp_monotonic` | none; set the param or implement precise vblank timestamps |

### 2.6 Cursor

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 25 | A `type=CURSOR` plane is **optional**; without it the cursor is composited into the primary | `drm_gpu.cpp:910-950` (cursor plane attached only if one is free), `:901` (legacy always) | o | none (DECON has no HW cursor) | `modetest -M exynos` shows no cursor plane → cursor still drawn | nothing |
| 26 | `DRM_CAP_CURSOR_WIDTH/HEIGHT` | `drm_gpu.cpp:71-82` (default 64×64) | o | absent in 3.10 (cap ids `0x1-0x6` only, `drm.h:776-781`) | — | nothing; optional `drm_plane_create_*` + caps later |
| 27 | Legacy mode requires `DRM_IOCTL_MODE_CURSOR2` (hotspot) and a `cursor_set2`/`cursor_set` CRTC hook | `drm_pipeline_legacy.cpp:258-292` (raw `DRM_IOCTL_MODE_CURSOR2`); `drm_pipeline_legacy.cpp:132` (`drmModeSetCursor` off) | **F** *only if* legacy is chosen | `CURSOR2` absent in 3.10 (`v3.10 drm.h` has no `0xBB`; present from **v3.11**, `drm.h:735`); vendor CRTC has no cursor hook | `KWIN_DRM_NO_AMS=1 kwin_wayland` → expect failure | avoided by choosing atomic (§0.2) |

### 2.7 Async flip / tearing / VRR

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 28 | `DRM_CAP_ASYNC_PAGE_FLIP` (legacy) or `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` (atomic), plus `PAGE_FLIP_ASYNC` accepted when the cap is set | `drm_gpu.cpp:108-115`; `drm_commit.cpp:157,174`; upstream gate `v6.12 drm_atomic_uapi.c:1381-1392` | o | neither cap exists | — | nothing for v1; later: `mode_config.async_page_flip` + per-plane `IN_FORMATS_ASYNC` |
| 29 | `VRR_ENABLED` CRTC property | `drm_crtc.cpp:24,50`; `drm_pipeline_legacy.cpp:147-151` | o | absent | — | nothing (panel is fixed 60 Hz) |

### 2.8 Explicit sync, leases, colour

| # | Requirement | Evidence | Cls | Today | Test | Minimum driver support |
|---|---|---|---|---|---|---|
| 30 | `DRM_CAP_SYNCOBJ_TIMELINE` + syncobj ioctls → `linux-drm-syncobj-v1` advertisement (clients), screencast capability | `core/drmdevice.cpp:54`; `wayland_server.cpp:831-838`; `plugins/screencast/screencaststream.cpp:942` | o | syncobj ioctls are v4.9-era and `DRM_CAP_SYNCOBJ_TIMELINE` is v5.5 (`drm.h`, v4.9/v5.0 → absent, v5.5 → present); nothing in 3.10 | `wayland-info` (protocol absent) | nothing for v1; implicit dma-buf fences still work |
| 31 | Explicit acquire fence via plane `IN_FENCE_FD` | `drm_commit.cpp:130-136` (only if the property exists) | o | absent | — | nothing |
| 32 | DRM leases (`CREATE/LIST/REVOKE_LEASE`) for VR | `drm_gpu.cpp:667-700`; failures only warn | o | absent (v4.15+) | — | nothing |
| 33 | CRTC gamma (`drmModeCrtcSetGamma`) in legacy only | `drm_pipeline_legacy.cpp:214-260`; `drm_crtc.cpp:125-134` (`m_crtc->gamma_size`) | o | `SETGAMMA` exists in 3.10 (`drm.h:711`) | — | nothing; note: `gamma_size==0` + any legacy colour transform ⇒ commit error |

## 3. `DRM_CAP_*` — every cap KWin queries, and the fallback

KWin queries exactly seven `DRM_CAP_*` values — six in `drm_gpu.cpp`, one in
`core/drmdevice.cpp` (`grep -rn drmGetCap KWin src/`):

| Cap | Queried at | If absent / 0 | Fatal? |
|---|---|---|---|
| `DRM_CAP_TIMESTAMP_MONOTONIC` | `drm_gpu.cpp:83` | presentation clock = `CLOCK_REALTIME`, timestamps converted (`:600-612`) | no |
| `DRM_CAP_ADDFB2_MODIFIERS` | `drm_gpu.cpp:93` | implicit modifiers, `drmModeAddFB2` path (`:1011`) | no |
| `DRM_CAP_CURSOR_WIDTH` / `_HEIGHT` | `drm_gpu.cpp:71,77` | 64×64 (`:74,80`) | no |
| `DRM_CAP_ASYNC_PAGE_FLIP` | `drm_gpu.cpp:111` (legacy only) | no tearing/async presentation | no |
| `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP` (`0x15`) | `drm_gpu.cpp:113` | no tearing/async presentation | no |
| `DRM_CAP_SYNCOBJ_TIMELINE` | `core/drmdevice.cpp:54` | no `linux-drm-syncobj-v1`, no syncobj screencast path | no |
| `DRM_CAP_DUMB_BUFFER`, `DRM_CAP_PRIME` | **not queried by KWin** — used by Mesa/libdrm behind `gbm_create_device`/`drmPrimeFDToHandle` | — | indirectly (`DRM_CAP_DUMB_BUFFER` gates `gbm`/kms_swrast; `PRIME` flags gate import/export) |

Also relevant, not caps: `drmIsKMS()` (`drmGetVersion` features), `DRM_CLIENT_CAP_ATOMIC`
(#5), `CURSOR_PLANE_HOTSPOT` (set only on VMs, `drm_gpu.cpp:194-199`),
`PLANE_COLOR_PIPELINE` (opt-in via `KWIN_DRM_USE_COLOR_PIPELINE`, `:207`).

## 4. Minimum viable atomic driver — checklist

### 4.1 Kernel-side contract (M0 `DRMAtomicM0` produces these; M1 consumes)

1. `DRM_IOCTL_SET_CLIENT_CAP` (`0x0d`) accepting `ATOMIC` (3) and `UNIVERSAL_PLANES` (2);
   setting ATOMIC must imply `universal_planes` and `aspect_ratio_allowed`
   (`v6.12 drm_ioctl.c:338-357`). Upstream also refuses cap 1 for a process whose `comm`
   starts with `X`; harmless here (`kwin_wayland`, `modetest`, `kmscube` are all fine), but
   do not copy that guard into a path that could reject a Wayland compositor.
2. `DRM_IOCTL_MODE_CREATEPROPBLOB` (`0xBD`) / `DESTROYPROPBLOB` (`0xBE`) — needed for
   `MODE_ID` and for `EDID`-style immutable blobs.
3. `DRM_IOCTL_MODE_ATOMIC` (`0xBC`): accept `TEST_ONLY|NONBLOCK`, `NONBLOCK|PAGE_FLIP_EVENT`,
   `ALLOW_MODESET`; reject `TEST_ONLY|PAGE_FLIP_EVENT`; require ≥1 CRTC when
   `PAGE_FLIP_EVENT` is set (`v6.12 drm_atomic_uapi.c:1291-1293,1388-1416`).
4. Property plumbing created by the init helpers, not by the DECON driver: globals
   `MODE_ID`, `ACTIVE`; per-plane `type`, `SRC_*`, `CRTC_*`, `FB_ID`, `CRTC_ID`; per-connector
   `CRTC_ID` (3.10 core has none of these — see #10/#12).
5. Non-blocking commit semantics: return before the flip completes, run the tail in a
   workqueue, and arm the userspace event at the next vblank with the vblank timestamp
   (`drm_atomic_helper_commit_hw_done()` + `drm_crtc_arm_vblank_event()`, the latter must be
   backported — 3.10 only has `drm_send_vblank_event`). The backport must not hold
   `drm_modeset_lock_all` across the flip.
6. Do not regress existing legacy users in-tree (exynos legacy CRTC, other DRM drivers).
7. Fences: 3.10 has no `dma_resv`; KWin only uses `IN_FENCE_FD` if the property exists, and
   otherwise relies on implicit dma-buf fences. Do **not** synthesize an `IN_FENCE_FD`
   property that cannot be honoured.

### 4.2 DECON subdriver contract (M1)

1. **One CRTC**, created with `drm_crtc_init_with_planes()` (or `drm_crtc_init` +
   `drm_plane`), with `atomic_begin/flush`, `atomic_enable/disable`, `atomic_check`
   (`mode_valid`, plane format/rect validation), and `set_config` left unused.
2. **One Primary plane** with `possible_crtcs = BIT(crtc_index)`, formats
   `XRGB8888` + `ARGB8888` (RGB565 optional), `atomic_update` translating
   `SRC_*/CRTC_*/FB_ID` into `decon_set_win_config()` / `decon_update_regs()`.
3. **Overlay planes optional** (DECON windows 1..n); KWin only uses them with a single
   output and falls back cleanly when a `TEST_ONLY` commit rejects them.
4. **Vblank**: `get_vblank_counter`, `enable_vblank`, `disable_vblank`; IRQ path must call
   `drm_crtc_handle_vblank()`; flip completion uses `drm_crtc_arm_vblank_event()`.
5. **Encoder + connector**: DSI connector (`DRM_MODE_CONNECTOR_DSI`), `status=connected`,
   ≥1 mode from the ea8061 timings with 720×1280 `DRM_MODE_TYPE_PREFERRED`,
   `possible_crtcs` correct on the encoder, `connector->encoders` non-empty (libdrm's
   `drmModeConnectorGetPossibleCrtcs` walks encoders).
6. **Framebuffer**: `fb_create` accepting `ADDFB2` with imported GEM, single plane, linear
   (no modifier); destroy path releases the DECON window + GEM reference.
7. **Master handover**: on `preclose`/master-drop disable the CRTC and stop vblank so the
   legacy fbdev path can regain the panel (M3 coexistence); on re-acquire, re-modeset
   (KWin forces a modeset after resume only in legacy mode, `drm_backend.cpp:145-155`).
8. **No cursor plane** for v1 (§2.6). No modifiers/IN_FORMATS, no syncobj, no async flip,
   no leases, no colour properties (§7.4 "do not build").

### 4.3 Acceptance commands (device, no flash)

```sh
# enumeration: 1 CRTC, ≥1 Primary plane with type, DSI connector connected at 720x1280
modetest -M exynos
# properties: MODE_ID/ACTIVE on CRTC, CRTC_ID on connector, SRC_*/CRTC_*/FB_ID/CRTC_ID on plane
modetest -M exynos -p
# atomic mode set (note -a: without it modetest uses legacy SetCrtc)
modetest -M exynos -a -s <conn_id>:720x1280-60 -P <plane_id>@<crtc_id>:720x1280
# vsynced page flipping -> proves flip events + timestamps (60 s, then Ctrl-C)
modetest -M exynos -a -v
# property write (e.g. connector CRTC_ID / CRTC ACTIVE) and hw-cursor probe (expect failure,
# no cursor plane): modetest -M exynos -a -w <id>:<prop>:<val> ; modetest -M exynos -a -C
# GPU path: gbm_create_device(card0) + panfrost render + ADDFB2 scanout, 300 frames
kmscube -A -D /dev/dri/card0 -c 300
# KWin: must log "Using Atomic Mode Setting on gpu" and "Number of planes on GPU ... "
QT_LOGGING_RULES="kwin_*.debug=true" KWIN_DRM_DEVICES=/dev/dri/card0 kwin_wayland --drm
# negative control: legacy path (expected to fail on CURSOR2 today)
KWIN_DRM_NO_AMS=1 kwin_wayland --drm
# presentation clock decision
cat /sys/module/drm/parameters/timestamp_monotonic
```

Pass criteria for M1/M2: no `"Pageflip timed out"`/`"Could not find a suitable primary
plane"`/`"drmPrimeFDToHandle failed"`/`"Failed to create gbm device"` in the KWin log; 300
`kmscube` frames with no `0x08` DECON timeouts (ties into `decon-kms-plan.md` M2).

## 5. Blockers that are *not* DRM requirements (found while measuring the target)

### 5.1 No KMS node today

Expected: only `renderD128` (panfrost7580) exists; `DrmBackend::addGpu()` would reject a
render-only node anyway (`drmIsKMS` + `DRM_NODE_PRIMARY` enumeration,
`drm_backend.cpp:161-170,245`). This is exactly what M1 creates.

### 5.2 kwin 6.8.3 ↔ Qt 6.12 private-ABI break — **fatal today, independent of DRM**

`kwin_wayland` cannot even print `--version`; `ldd` and a real run both fail:

```
Error relocating /lib/libkwin.so.6:
  _ZN22QWindowSystemInterface36handleTabletEnterLeaveProximityEventEP7QWindowPK15QPointingDevicebRK7QPointFS7_6QFlagsIN2Qt11MouseButtonEEddddiS8_INS9_16KeyboardModifierEE: symbol not found
Error relocating /lib/libkwin.so.6:
  _ZN22QWindowSystemInterface17handleTabletEventEP7QWindowmPK15QPointingDeviceRK7QPointFS7_6QFlagsIN2Qt11MouseButtonEEdddddiS8_INS9_16KeyboardModifierEE: symbol not found
```

The installed Qt (`qt6-qtbase-wayland-6.12.0-r0`) exports the same functions with an extra
`double` argument (`…ddddddS8_INS9_16KeyboardModifierEE`), i.e. `libkwin.so.6.8.3` was built
against an older Qt6 `QWindowSystemInterface` private API. Consequences: `startplasmamobile`
→ `startplasma-wayland` → `kwin_wayland` dies before any DRM code runs. Fix is packaging,
not kernel: rebuild kwin 6.8.3 (or a 6.8.x rebuild refresh) against Qt 6.12, or pin
Qt 6.8-era. `PlasmaSession` (slice 8) must verify `kwin_wayland --version` before any
KMS session test is meaningful.

## 6. Appendix — the exact libdrm surface the installed libkwin uses

`dev: nm -D --undefined-only /lib/libkwin.so.6.8.3 | grep -E '^(drm|gbm)'`:

```
drmAuthMagic drmCloseBufferHandle drmDevicesEqual drmDropMaster drmFree drmFreeDevice
drmFreeDevices drmFreeVersion drmGetCap drmGetDevice2 drmGetDeviceFromDevId
drmGetDeviceNameFromFd2 drmGetDevices2 drmGetMagic drmGetVersion drmHandleEvent drmIoctl
drmIsKMS drmIsMaster drmModeAddFB drmModeAddFB2 drmModeAddFB2WithModifiers drmModeCloseFB
drmModeConnectorGetPossibleCrtcs drmModeCreateLease drmModeCreatePropertyBlob
drmModeCrtcSetGamma drmModeDestroyPropertyBlob drmModeFormatModifierBlobIterNext
drmModeFreeConnector drmModeFreeCrtc drmModeFreeObjectProperties drmModeFreePlane
drmModeFreePlaneResources drmModeFreeProperty drmModeFreePropertyBlob drmModeFreeResources
drmModeGetConnector drmModeGetConnectorTypeName drmModeGetCrtc drmModeGetPlane
drmModeGetPlaneResources drmModeGetProperty drmModeGetPropertyBlob drmModeGetResources
drmModeListLessees drmModeObjectGetProperties drmModeObjectSetProperty drmModePageFlip
drmModeRevokeLease drmModeRmFB drmModeSetCrtc drmModeSetCursor drmPrimeFDToHandle
drmPrimeHandleToFD drmSetClientCap drmSyncobjCreate drmSyncobjDestroy drmSyncobjEventfd
drmSyncobjExportSyncFile drmSyncobjFDToHandle drmSyncobjHandleToFD drmSyncobjImportSyncFile
drmSyncobjTimelineSignal drmSyncobjTimelineWait drmSyncobjTransfer
gbm_bo_create gbm_bo_create_with_modifiers2 gbm_bo_destroy gbm_bo_get_fd_for_plane
gbm_bo_get_format gbm_bo_get_height gbm_bo_get_modifier gbm_bo_get_offset
gbm_bo_get_plane_count gbm_bo_get_stride_for_plane gbm_bo_get_width gbm_create_device
gbm_device_destroy gbm_device_get_fd gbm_format_get_name
```

Notes: there is **no `drmModeAtomicCommit`** symbol — KWin 6.8 builds the atomic request by
hand and issues `DRM_IOCTL_MODE_ATOMIC` itself (`drm_commit.cpp:201-220`), which is why the
kernel-side flag/property validation in §4.1 matters more than libdrm behaviour. `drmModeAddFB`
is only the `-EOPNOTSUPP` fallback of `AddFB2`. `drmModeSetCrtc`/`drmModePageFlip`/
`drmModeSetCursor` are legacy-path only.

## 7. Corrections and risk updates to `decon-kms-plan.md`

### 7.1 Mesa kmsro makes a separate DECON card viable — keep the driver name `exynos`

`DrmDevice` opens the KMS node and calls `gbm_create_device()` on **it**
(`core/drmdevice.cpp:194`). gbm resolves the DRI module from `drmGetVersion().name`
(Mesa `src/gbm/backends/dri/gbm_dri.c:279`), and only falls back to `kms_swrast` if that
fails (`:1237-1242`, `dri_screen_create_sw` :294-305). The installed Mesa ships
`/usr/lib/dri/exynos_dri.so` (symlink to `libdril_dri.so`) containing
`__driDriverGetExtensions_exynos` plus `kmsro`/`panfrost` (dev: `ls -l`, `strings`), i.e. the
display-controller shim that renders through the Mali render node — the same architecture as
mainline Exynos+panfrost boards. **Inference to verify at M1** (not yet runnable): the
`exynos` module pairs with `panfrost`; if the new KMS driver keeps
`DRM_DRIVER_NAME == "exynos"` (the vendor driver already does, `exynos_drm_drv.c:32,286`), gbm
finds the kmsro module; a new name (e.g. `decon`) would silently drop to `kms_swrast`
(software) even though panfrost is present. Verify with
`LIBGL_DEBUG=verbose eglinfo -B` / `MESA_LOADER_DRIVER_OVERRIDE=exynos` on `card0`.

### 7.2 The atomic donor version in the plan is wrong

Plan §3 M0 says "backport from Linux 3.14/3.15". Verified against upstream tags:

| Piece | First version containing it |
|---|---|
| `drivers/gpu/drm/drm_atomic.c` (internal atomic) | **v3.19** (absent in v3.18; 657 lines at v3.19) |
| `drm_atomic_helper.c` | v3.19 |
| `DRM_CLIENT_CAP_ATOMIC` + `DRM_IOCTL_MODE_ATOMIC` (UAPI usable by KWin) | **v4.0** (`include/uapi/drm/drm.h:658-662,787`; absent in v3.20) |
| `DRM_IOCTL_MODE_CURSOR2` (needed by the legacy path) | **v3.11** (`drm.h:735`; absent in 3.10) |
| `CREATE/DESTROYPROPBLOB` | **v4.2** (`0xBD`, `0xBE`) |
| `drivers/gpu/drm/drm_plane.c` (plane code split out of `drm_crtc.c`) | **v4.9** (absent at v4.4) |
| `IN_FORMATS` property (modifier advertisement) | **v4.14** (`drm_mode_config.c`) |
| `drm_crtc_arm_vblank_event()` (flip events at vblank — required by #22) | **v4.4** (absent in v4.0–v4.3 `drm_irq.c`; 3.10 has only `drm_send_vblank_event`) |

Recommendation: make **v4.4–v4.9** the donor window — v4.4 is the first tag with **both** the
blob UAPI (4.2) and `drm_crtc_arm_vblank_event()` (4.4), i.e. the first version that can satisfy
#7 and #22; `drm_plane.c` also exists from 4.9. Explicitly skip `IN_FORMATS`/modifiers (4.14).
Attempting 3.14/3.15 as written would yield an atomic API with neither blobs nor vblank-armed
flip events — unusable for KWin.

### 7.3 M2 restated precisely (and what it depends on)

"dma-buf import into KMS" = `drmPrimeFDToHandle()` on `card0` (#18) + `ADDFB2` accepting the
resulting GEM (#19) + a DECON window bound to that buffer. The PRIME plumbing already exists
in the vendor tree (`exynos_drm_drv.c:280-282`); the real work is (a) DECON window
programming from an imported GEM (`decon_set_win_buffer` already consumes dma-buf) and
(b) proving **panfrost7580** can import buffers allocated on `card0` as render targets, or
that kmsro's renderonly allocation works (`multigpuswapchain.cpp:318-395`,
`drm_egl_layer_surface.cpp:440-470`). With kmsro the buffers are allocated by the renderonly
path (panfrost GEM exported into card0), which is the cheap path; the double-copy path
(`MultiGpuSwapchain`) is the fallback and costs one GPU blit per frame at 720×1280.

### 7.4 Do **not** build for v1 (no KWin requirement)

`IN_FORMATS`/explicit modifiers, `DRM_CAP_ADDFB2_MODIFIERS`, hardware cursor + cursor caps,
syncobj/explicit sync (`IN_FENCE_FD`, `linux-drm-syncobj-v1`), async flip/tearing caps,
VRR, DRM leases, gamma/CTM/degamma/sharpness, HDR/colour-space/bpc properties, EDID
generation for the panel. Each has a working KWin fallback; adding them increases the
`TEST_ONLY` surface that must be correct before anything is drawn.

## 8. Open items to measure on hardware (ordered)

1. `gbm_create_device(/dev/dri/card0)` succeeds and which module it loads (§7.1) — `M1`.
2. DECON vblank counter/timestamp source and flip-event latency (`#22/#23`) — `M1`.
3. `ADDFB2` of a panfrost-allocated linear buffer, and which side allocates scanout buffers
   (`#18/#19`, §7.3) — `M2`.
4. `drm_timestamp_monotonic` value on the shipping kernel (decides presentation clock) —
   `M1`; read-only check already scheduled in §4.3.
5. `modetest -a`/`-v` and `kmscube -A` against a real atomic `card0` (option sets verified
   on the device; nothing can be exercised while only `renderD128` exists — run them under
   `flock` + `timeout` in `KmsTooling`).
