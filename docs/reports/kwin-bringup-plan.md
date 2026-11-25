# kwin_wayland on DECON `card0` — requirement delta and first run

Status: **analysis complete; not yet executed** (no compositor run on hardware). Scope: what
KWin 6.8.3 needs from the DECON KMS driver, what our driver already provides, and the exact
first invocation to try. No kernel change is made or proposed here; the missing kernel pieces
are listed as gaps for the KMS-core owner.

Sources of truth: `docs/reports/decon-kms-STATUS.md` (hardware state, int82),
`docs/reports/decon-kms-req.md` (KWin requirement matrix), the backport inventory in
`docs/reports/decon-kms-m0.md`, the driver sources under `code/gpu31/kms/`, and the KWin
6.8.3 tarball (`kwin-6.8.3.tar.xz`) for every claim about KWin behaviour. Probe date
2026-11-25 on `nura-j7elte`, kernel `3.10.108 #37`; that boot is the **fbdev image**
(vendor `decon` fb0, UI up), so every `card0`-side fact below is either from the int82 run
recorded in `decon-kms-STATUS.md` or from source/backport inspection and is marked as such.

## 0. Verdict

`card0` is enumerated, a mode can be set, GEM maps and the DECON FRAME IRQ fires — but KWin
6.8 still cannot light the panel with the int82 driver, for three reasons, in this order:

1. **Fatal, kernel-side, and absent from the current open-items list: there is no `MODE_ID`
   property and no `DRM_IOCTL_MODE_CREATEPROPBLOB`.** The atomic core was backported from
   v4.0, and v4.0 has no `MODE_ID` (it arrived in v4.2 together with the blob ioctls,
   `drm_crtc.c:1460` in v4.2; absent from v4.0's
   `drm_mode_create_standard_properties`). `DrmCrtc::init()` treats a missing `MODE_ID` as
   fatal (`kwin src/backends/drm/drm_crtc.cpp:93-97`), so KWin **drops the only CRTC**, ends
   with zero CRTCs, and fails the modeset with
   `Modeset failed: More outputs are enabled than there are CRTCs`.
2. **Fatal for the GPU path today: gbm/kmsro pairing.** Mesa is asked for `gbm_create_device()`
   on `card0` (`core/drmdevice.cpp:194`). The system Mesa 26.3 has `exynos_dri.so` (kmsro)
   and `panfrost_dri.so`, but kmsro looks for a render device named `panfrost` while our
   render node reports `panfrost7580`; the documented result is `llvmpipe`
   (`decon-kms-STATUS.md` open item 2). KWin does not treat software GL as fatal, so this
   costs performance first and scanout quality second, not startup.
3. **Packaging: the installed `kwin-6.8.3-r0` cannot start at all on Qt 6.12** (two
   `QWindowSystemInterface` tablet symbols, `decon-kms-session.md` §2.1). `kwin=6.8.3-r1`
   (same version, Qt 6.12 rebuild) is available in `edge/community` and fixes it. This is
   userspace-only and must be done before any KMS run is meaningful.

The flip-buffer bug that blocks `kmscube` (`crtc->fb=0 primary->state->fb=0`) is **not on
KWin's path**: KWin never calls `drmModePageFlip`; it builds `DRM_IOCTL_MODE_ATOMIC` itself
with the plane's `FB_ID` (`drm_commit.cpp:201-220`, `prepareAtomicPlane`). Our broken
`exynos7580_crtc_page_flip` adapter only serves the legacy ioctl. It must still be fixed for
`kmscube`/`modetest -v`, but it is not what stops KWin.

## 1. Gap list

Classes as in `decon-kms-req.md`: **F** fatal, **f** functionally required for the intended
GPU path, **o** optional/degradable. "State" is `satisfied` / `missing` / `unknown`.

| # | Requirement (KWin 6.8.3 reference) | Cls | State | Evidence |
|---|---|---|---|---|
| 1 | Primary KMS node with `DRIVER_MODESET`, `drmIsKMS()` true | **F** | **satisfied** | `card0`+`controlD64` exist after `insmod` (`decon-kms-STATUS.md`); `DRIVER_MODESET\|ATOMIC\|GEM\|PRIME` in `exynos_drm7580_main.c:390` |
| 2 | `gbm_create_device(card0)` non-NULL else the GPU is skipped (`core/drmdevice.cpp:194-200`) | **F** | **unknown** | Mesa 26.3 ships `exynos_dri.so`; no run has yet shown `gbm_create_device` on `card0` succeeding or failing (kmscube reported only `llvmpipe`) |
| 3 | Session/DRM master via logind/seatd; `openRestricted`/`TakeDevice` | **F** | **unknown / workaround** | SSH sessions on the phone have no seat (`loginctl`: `SEAT -`), so logind `TakeDevice` is expected to fail; run script forces the Noop session (§3.1) |
| 4 | Render device compatible with the KMS device (`core/gpumanager.cpp:100-215`) | f | **satisfied in principle** | both nodes are `DRM_BUS_PLATFORM` → mutually compatible; but the render device must open (row 9) |
| 5 | `SET_CLIENT_CAP` + `DRM_CLIENT_CAP_ATOMIC` | **F** | **satisfied** | backported in `patches/drm-atomic/0004`; sets `file_priv->atomic` and `universal_planes` (M0 §4.2) |
| 6 | `DRM_IOCTL_MODE_ATOMIC` with `TEST_ONLY\|NONBLOCK`, `NONBLOCK\|PAGE_FLIP_EVENT`, `ALLOW_MODESET`; reject `TEST_ONLY\|PAGE_FLIP_EVENT` | **F** | **satisfied** | v4.0 ioctl body (M0 §4.2); validation confirmed in `decon-kms-crtc.md` §2; kmscube reaches `exynos7580_atomic_commit: async=1` |
| 7 | `MODE_ID` property **and** `CREATE/DESTROYPROPBLOB` (MODE_ID is a blob) | **F** | **MISSING** | v4.0 has neither (v4.2: `drm_crtc.c:1460`); no `CREATEPROPBLOB`/`MODE_ID` in `patches/drm-atomic/*` or `patches/drm-kms/*`; `decon-kms-req.md` row 7/10 predicted this |
| 8 | `MODE_OBJ_GETPROPERTIES` on CRTC/connector/plane | **F** | **satisfied** | 3.10 core (`drm_crtc.c`, ioctl `0xB9`), used by modetest `-p` |
| 9 | Render node opens: `RenderDevice::open(renderD128)` → `gbm_create_device` → EGL | f | **unknown, likely failing** | driver name is `panfrost7580`; Mesa loader has no such DRI module (only `panfrost`), so this is the documented name trap (`tools/env.sh`) |
| 10 | ≥1 plane with `type=Primary`, `possible_crtcs` covering the CRTC | **F** | **satisfied** | `exynos_drm7580_plane.c:507` `drm_universal_plane_init(..., BIT(0), ..., DRM_PLANE_TYPE_PRIMARY)`; modetest lists plane 20 |
| 11 | Plane props `SRC_*` (16.16), `CRTC_*`, `FB_ID`, `CRTC_ID`, `type` | **F** | **satisfied** | created/attached by v4.0 `drm_universal_plane_init` + `drm_mode_create_standard_properties` |
| 12 | CRTC `ACTIVE` (bool) | **F** | **satisfied** | `prop_active` from v4.0; `modetest -p` shows `ACTIVE` on CRTC 18 |
| 13 | CRTC `MODE_ID` (blob) | **F** | **missing** | same as row 7 |
| 14 | Connector `CRTC_ID` | **F** | **satisfied** | attached by v4.0 `drm_connector_init` when `DRIVER_ATOMIC` (`v4.0 drm_crtc.c:913`) |
| 15 | Connected connector with ≥1 mode (≥640 px or `PREFERRED`) | **F** | **satisfied** | `DSI-1 connected 720x1280@59.92`, preferred+driver (`decon-kms-STATUS.md`); `drm_mode_connector_attach_encoder` present (`exynos_drm7580_dsi.c:378`) |
| 16 | `drmPrimeFDToHandle()` on the KMS fd + `drmCloseBufferHandle` | **F** | **missing proof** | `exynos7580_gem_prime_import()` exists (`exynos_drm7580_dmabuf.c:501`), `.gem_prime_import` wired; no run has imported a panfrost buffer (no `fd_to_handle` line) |
| 17 | `ADDFB2` of the imported GEM, plane 0, XRGB8888/ARGB8888, no modifier; `ADDFB` fallback | **F** | **missing proof** | `exynos7580_fb_create()` wired; plane formats include ARGB/XRGB8888 (`exynos_drm7580_plane.c:72`); `DRM_CAP_ADDFB2_MODIFIERS` is absent so KWin uses the plain `ADDFB2` path (`drm_gpu.cpp:1011`) |
| 18 | FB teardown: `drmModeCloseFB` with `drmModeRmFB` fallback | o | **satisfied by fallback** | `CLOSEFB` absent in 3.10, KWin falls back (`drm_buffer.cpp:43-45`) |
| 19 | Dumb buffers `CREATE/MAP/DESTROY_DUMB`, `DRM_CAP_DUMB_BUFFER` | **F** | **satisfied** | `.dumb_create = drm_gem_cma_dumb_create`; 3.10 `drm_getcap` returns 1 when `dumb_create` exists |
| 20 | Page-flip completion event with `user_data` + timestamp; `drmEventContext` v3 | **F** | **satisfied (machinery), unproven with a real buffer** | `exynos7580_arm_vblank_event()` + `exynos7580_vblank_irq()` (`fence.c`); `drm_send_vblank_event` delivers `tv_sec/usec`; IRQ 234 climbs |
| 21 | Vblank counter/enable/disable; IRQ calls `drm_handle_vblank()` | **F** | **satisfied** | `.get_vblank_counter/.enable_vblank/.disable_vblank` (`exynos_drm7580_main.c:394-396`) |
| 22 | Non-zero vblank timestamps (else `Got invalid timestamp`) | f | **satisfied, coarse** | no `get_vblank_timestamp` hook, so 3.10 `drm_get_last_vbltimestamp()` falls back to `ktime_get()` at IRQ time (`v3.10 drm_irq.c:759-781`) — non-zero, ±one IRQ |
| 23 | `DRM_CAP_TIMESTAMP_MONOTONIC` | o | **satisfied** | 3.10 returns `drm_timestamp_monotonic` (default 1) → KWin uses `CLOCK_MONOTONIC`; readable at `/sys/module/drm/parameters/timestamp_monotonic` |
| 24 | `IN_FORMATS` + `DRM_CAP_ADDFB2_MODIFIERS` | o | **missing (by design)** | neither exists (v4.14+); KWin logs `drmModeAddFB2WithModifiers is not supported` and uses `DRM_FORMAT_MOD_INVALID` — exactly the intended v1 |
| 25 | Cursor plane; `DRM_CAP_CURSOR_WIDTH/HEIGHT` | o | **missing (by design)** | no cursor plane; caps absent → KWin uses 64×64 and composites the cursor into the primary (`drm_gpu.cpp:71-82,910-950`) |
| 26 | Legacy-only: `MODE_CURSOR2`, legacy CRTC vtable, `SETGAMMA` | **F** *only if legacy* | **n/a** | not reachable: `DRM_CLIENT_CAP_ATOMIC` succeeds, so KWin stays atomic; do **not** run with `KWIN_DRM_NO_AMS=1` (it would fail on `CURSOR2`, absent in 3.10) |
| 27 | `ASYNC_PAGE_FLIP`/`ATOMIC_ASYNC_PAGE_FLIP`, `VRR_ENABLED` | o | **missing** | caps 0x7/0x15 and the property are absent → no tearing/VRR; KWin degrades |
| 28 | Syncobj/`SYNCOBJ_TIMELINE`, `IN_FENCE_FD`, leases, gamma/CTM, HDR | o | **missing** | v4.9+/v5.5 features; KWin warns/falls back (`drm_gpu.cpp:667-700`, `drm_commit.cpp:130-136`) |
| 29 | `NONBLOCK` must return before completion | f | **satisfied in practice** | ours is synchronous but KWin's commit thread holds its mutex across the ioctl (`drm_commit_thread.cpp:153-166`), so an event that arrives during the ioctl is processed after `m_committed` is set; cost is ≤1 frame of latency |
| 30 | `kwin_wayland` itself runs | **F** | **missing (packaging)** | `kwin_wayland --version` fails to relocate on Qt 6.12 (probe 2026-11-25); `kwin=6.8.3-r1` available in `edge/community` |

Rows 7 + 13 are the same blocker and are the only known *driver-core* fatal item left.
Rows 9/16/17 are the Mesa/PRIME workstream (unproven, not proven broken); row 30 is a
two-package upgrade.

## 2. What each of those means for the first run

### 2.1 No `MODE_ID` → no CRTC → no output

`DrmCrtc::updateProperties()` returns false unless `modeId` and `active` are both valid when
atomic is on, and `DrmGpu::initDrmResources()` then `continue`s without adding the CRTC
(`drm_gpu.cpp:239-262`). With one connector connected and one enabled pipeline, the CRTC
assignment search hits `crtcs.isEmpty()` and returns `NotEnoughCrtcs`
(`drm_gpu.cpp:394-399`); `maybeModeset()` logs it as
`Modeset failed: More outputs are enabled than there are CRTCs` (`drm_gpu.cpp:846-848`) and
re-probes in a loop. That is the predicted first-run outcome until the KMS core gains:

* `DRM_IOCTL_MODE_CREATEPROPBLOB`/`DESTROYPROPBLOB` (0xBD/0xBE, v4.2) and
* the CRTC `MODE_ID` blob property (v4.2) plus its `drm_atomic_set_mode_prop_for_crtc()` path.

Neither needs DECON code: both are core-only, donor v4.2, ~180 lines. It is the cheapest
unblocker in the project, and it is not in `decon-kms-STATUS.md`'s open list (which tracks the
flip buffer, Mesa and the lock leak).

### 2.2 Mesa pairing is fatal only past modeset

`DrmDevice::openWithAuthentication()` returns `nullptr` if `gbm_create_device(fd)` fails, and
`addGpu()` then silently skips the GPU (one warning: `Failed to create gbm device for
/dev/dri/card0`). With `exynos_dri.so` present this should not fail on `card0`; the failure mode
seen so far is the opposite — gbm succeeds on `card0` but kmsro cannot match a render device,
so Mesa falls back to `kms_swrast` (`llvmpipe`). KWin then renders in software into `card0`
buffers. That proves the whole KMS path; it is slow (~1-3 fps at 720×1280) but *is* a useful
first-run result. Do not chase it in the same run: `MesaPair` owns it, and the fix is a Mesa
mapping for the `panfrost7580` render-node name, not a kernel change.

### 2.3 The flip-buffer bug is not KWin's path

`kmscube` uses `drmModePageFlip` → `exynos7580_crtc_page_flip` → `drm_atomic_helper_page_flip`
→ the v4.0 helper must install `plane_state->fb`; it does not in our build. KWin instead sets
`FB_ID` as a property in its own atomic request (`prepareAtomicPlane`), which the kernel's
`drm_atomic_set_property` maps straight into the plane state. So the KWin run is *not* gated on
fixing that adapter — but a successful KWin run also will not prove the adapter, and
`modetest -v`/`kmscube` will stay broken until the manual-page-flip-state diagnostic
(`patches/drm-kms/0132`, later revisions in that series) has been used.

### 2.4 The packaging gate comes first

`kwin_wayland --version` must print `kwin 6.8.3` before any KMS test. Pin the rebuild:

```sh
apk add --no-cache 'kwin=6.8.3-r1' 'kwin-lang=6.8.3-r1'   # 2 packages, ~7.4 MB
```

`/` has ~13 MB free on the current image — check `df -h /` first and free space if needed; do
not run `apk add --upgrade kwin` (76-package resolver).

## 3. First invocation

### 3.1 Environment

| Variable | Value | Why |
|---|---|---|
| `KWIN_DRM_DEVICES` | `/dev/dri/card0` | bypasses udev seat enumeration (`drm_backend.cpp:118,177-181`); without it KWin needs a udev-tagged seat, which the SSH boot does not have |
| `KWIN_RENDER_NODES` | `/dev/dri/renderD128` | explicit render-device list (`gpumanager.cpp:85,216-228`); keeps KWin from picking a different node |
| `KWIN_COMPOSE` | `O2ES` | forces the GLES path; only `O2`/`O2ES` are accepted by `attemptOpenGLCompositing()` (`compositor.cpp:267-276`), any other value makes KWin exit (`:330-333`) |
| `QT_LOGGING_RULES` | `kwin_wayland_drm.debug=true;kwin_core.debug=true;kwin_qpa_plugin.debug=true` | `KWIN_DRM` is category `kwin_wayland_drm` (`backends/drm/drm_logging.cpp:12`), default severity Warning; the useful lines (`Using Atomic Mode Setting`, plane count, new output) are debug |
| `QT_LOGGING_TO_CONSOLE` / `QT_FORCE_STDERR_LOGGING` | `1` | Qt logs to journald instead of stderr when stderr is not a tty — without this the log file stays empty |
| `XDG_RUNTIME_DIR` | fresh `0700` dir under `/tmp` | the Wayland socket lives there; root has no `/run/user/0` session |
| `XDG_SESSION_ID` | a bogus id (e.g. `kwin-try-noop`) | **forces `NoopSession`**: `LogindSession::create()` resolves `XDG_SESSION_ID` through `org.freedesktop.login1.GetSession` (`session_logind.cpp:70-90`); a nonexistent id makes it warn and return nullptr, then Noop opens `card0` directly. Needed because SSH sessions have no seat, so logind's `TakeDevice` cannot hand out a seat0 device |
| `DISPLAY`, `WAYLAND_DISPLAY`, `XAUTHORITY` | unset | otherwise KWin auto-picks X11/Wayland nested backends (`main_wayland.cpp:526-540`) — `--drm` also prevents it, but the env keeps children clean |
| `LD_LIBRARY_PATH`, `LIBGL_DRIVERS_PATH`, `MESA_LOADER_DRIVER_OVERRIDE`, `EGL_PLATFORM` | unset | use the **system Mesa 26.3**: it is the only installed stack with `exynos_dri.so` (kmsro). `/data/mesabuild/mesa20-b` has `panfrost_dri`+`swrast` only |
| `LIBGL_DEBUG`, `EGL_LOG_LEVEL`, `MESA_DEBUG` | `verbose` / `debug` / `1` | captures Mesa's own loader/kmsro decision in the same log |
| `KWIN_DRM_NO_AMS` | unset (or `0`) | legacy is a dead end here (`MODE_CURSOR2` absent in 3.10) |

### 3.2 Command

```sh
XDG_RUNTIME_DIR=/tmp/kwin-run QT_LOGGING_TO_CONSOLE=1 ... \
timeout -s TERM -k 5 20 \
  dbus-run-session -- \
  kwin_wayland --drm --no-lockscreen --no-global-shortcuts --socket kwin-test
```

* `--drm` selects `DrmBackend` (`main_wayland.cpp:501-505`); without it and with no env hints
  KWin would also choose DRM, but explicit is safer.
* `--no-lockscreen` avoids loading the screen locker (and its D-Bus/polkit expectations) in a
  throwaway run; `--no-global-shortcuts` avoids KGlobalAccel.
* No `--xwayland`: Xwayland is not installed on the image and KWin only starts it on request.
* `dbus-run-session` gives KWin a session bus (KCrash/KConfigWatcher touches it); if the
  binary is missing, run `kwin_wayland` directly — startup still works, with warnings.
* `timeout -s TERM -k 5`: TERM lets KWin's `KSignalHandler` exit cleanly; the 5 s KILL is only the
  guarantee that the script returns (a SIGKILLed compositor leaves `card0` held, so a following
  run needs a `rmmod`/reboot).

### 3.3 Expected log markers

Success ladder (in order, all with the env above):

```
KWIN environment variables: [...]                 # echoes the env above
Could not determine the active graphical session  # expected: Noop session forced
adding GPU /dev/dri/card0
drmModeAddFB2WithModifiers is not supported on GPU ...   # expected: no modifiers
Using Atomic Mode Setting on gpu ...
Number of planes on GPU ... : N
New output on GPU ...: DSI-1 (or the connector model name)
Attempting to load the OpenGL scene
Chose /dev/dri/renderD128 as the primary GPU     # or card0 if no render node opens
OpenGL 2 compositing enforced by environment variable
OpenGL compositing has been successfully initialized
```

A pass also shows a rising DECON IRQ count and no `Pageflip timed out` in the run window.
`renderer` is not logged at startup; if it matters, use
`qdbus org.kde.KWin /KWin supportInformation` (needs a live session) or Mesa's own
`MESA-LOADER` lines.

Failure markers, each mapped to the gap it proves:

| Marker | Meaning | Gap |
|---|---|---|
| `Error relocating /lib/libkwin.so.6:` / `symbol not found` | `kwin=6.8.3-r0` vs Qt 6.12 | row 30 — install r1 |
| `Failed to take control of ... session` / `Failed to open drm device /dev/dri/card0` | logind refused the node (session has no seat) | row 3 — the `XDG_SESSION_ID` trick or a VT session |
| `Failed to create gbm device for /dev/dri/card0` | gbm cannot load a DRI module for driver `exynos` | row 2 — Mesa/DRI path |
| `Skipping KMS incapable drm device node at` | `drmIsKMS` false (wrong node) | row 1 — `KWIN_DRM_DEVICES` |
| `drmSetClientCap for Atomic Mode Setting failed. Using legacy mode on GPU` | atomic cap rejected | row 5 — then expect `CURSOR2` failure |
| `Atomic Mode Setting requested off via environment variable` | `KWIN_DRM_NO_AMS` set | unset it |
| `Failed to create any plane` / `Failed to get plane resources` | plane enumeration unusable | row 10 |
| `Failed to update the basic plane properties` | `type`/`SRC_*`/`CRTC_*`/`FB_ID` missing | row 11 |
| `Failed to update the basic connector properties (CRTC_ID)` | connector `CRTC_ID` missing | row 14 |
| **`Failed to update the basic crtc properties. modeId: false active: true`** | **`MODE_ID` property missing** | **row 7/13 — the expected first-run failure** |
| **`Modeset failed: More outputs are enabled than there are CRTCs`** | no CRTC survived init, output enabled | consequence of row 7/13 |
| `Couldn't find any render device for /dev/dri/card0` | compatibility map empty | row 9 |
| `Opening render device for /dev/dri/card0 failed` → later `Chose /dev/dri/card0 as the primary GPU` | renderD128 did not open; KMS node used as software fallback — still a usable test | row 9 |
| `Atomic commit failed: ...` | kernel rejected a commit (property/flag detail in the message) | rows 6/16/17 |
| `Pageflip timed out! This is a bug in the exynos kernel driver` | commit accepted, event never delivered within 1 s | rows 20/21/29 |
| `Got invalid timestamp (sec: 0, usec: 0) on gpu ...` | vblank event with zero timestamp | row 22 |
| `Could not fulfill the requested compositing mode in KWIN_COMPOSE` | GL init failed | rows 2/9 |
| `Found no render device!` | nothing usable at all | rows 2/9 |

Driver-side markers to read in `dmesg` alongside: `exynos7580_atomic_commit: async=…`,
`DECON link enable`, `decon irq: … frame=1`, and (with `0132`) `exynos7580_page_flip: …`.
Absence of any `exynos7580_atomic_commit` line while KWin logs a modeset failure means the
failure happened in `TEST_ONLY`/property plumbing, not in hardware programming.

## 4. The manual test script

`code/gpu31/kms/session/kwin-try.sh` — runs **on the device** as root (self-elevates through
`rootshell` if started as `user`), uses no init-system command as a requirement, and never
reboots.

```sh
# from the host, under the shared device lock:
flock /tmp/gpsdev.lock scp code/gpu31/kms/session/kwin-try.sh \
     user@172.16.42.1:/data/kwin/kwin-try.sh
flock /tmp/gpsdev.lock ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no \
     user@172.16.42.1 'sh /data/kwin/kwin-try.sh'
```

What it does, in order:

1. **Preflight** — root; module pair for the chosen revision and vermagic match; kernel has the
   atomic core (`drm_atomic_state_alloc` in `/proc/kallsyms`) → otherwise it reports "not a KMS
   boot" (the current fbdev boot fails here); `kwin_wayland --version` (offers the pinned r1
   install only when `FIX_KWIN=1`).
2. **Stop the UI without OpenRC** — `rc-service xsession stop` *if that service exists*, then
   `pkill` `fbwin`, `nura-shell`, `Xorg` (TERM, then KILL) and waits until nothing holds
   `/dev/fb0` or `/dev/dri/card0`. On a KMS image this is a no-op; the vendor DECON fbdev
   refuses to bind.
3. **Arm the harness** — `wdpet.sh` (hardware watchdog, never leaves a hang needing a battery
   pull) and `logd.sh` (persisted filtered dmesg).
4. **Load the modules** — `insmod drm_kms_helper-<rev>.ko`, `insmod exynos-drm7580-<rev>.ko`,
   verify `card0`, run `modetest -c` (connector) and `modetest -p` (properties, including
   whether `MODE_ID` exists — direct evidence for row 7).
5. **Run KWin** — the §3.2 command with the §3.1 environment, for `KWIN_TRY_SECS` (default 20),
   all output to one log; dmesg is cleared right before and captured right after.
6. **Classify** — greps the markers above into `summary-<stamp>.txt`, prints the verdict and
   always prints the log path.
7. **Clean up** — kills leftovers, `rmmod` the driver with a timeout (skip with
   `KWIN_UNLOAD=0`; int75 bounded the teardown, but a hung `rmmod` is still possible and is
   reported, not waited on). No reboot; on a KMS image the panel stays dark until reboot, and
   the fbdev image is the way back to the UI.

Configuration (env): `KWIN_TRY_SECS`, `KWIN_REV` (module suffix, default newest), `KWIN_MODDIR`,
`KWIN_DRM_DEVICE`, `KWIN_RENDER_NODE`, `KWIN_LOG_DIR` (default `/data/kwinlogs`), `KWIN_SOCKET`,
`FIX_KWIN` (0/1), `KWIN_UNLOAD`, `KWIN_LOGGING_RULES`, `KWIN_XDG_SESSION_ID`,
`KWIN_COMPOSE`.

Exit codes: `0` ran the window, OpenGL initialized and no fatal marker matched; `1` a fatal marker
matched or the run was incomplete (no `OpenGL compositing ... initialized`);
`2` not root and no `rootshell`; `3` boot lacks the atomic core / modules unusable; `4` KWin
binary unusable (ABI) and not fixed; `5` modules loaded but `card0`/connector missing; `6`
another process holds `card0`.

Artifacts under `$KWIN_LOG_DIR`: `kwin-<stamp>.log` (KWin + Mesa), `dmesg-load-<stamp>.log`,
`dmesg-run-<stamp>.log`, `props-<stamp>.log` (modetest `-c`/`-p`, `timestamp_monotonic`),
`summary-<stamp>.txt`.

## 5. Preconditions, risk, rollback

* Boot a **KMS image** (int82: `kernel-boot-int82.img 162bb2be…`, `Image ac2f2bfe…`, or later) —
  the current boot is the fbdev image
  and the script stops with exit 3 there. Flashing is Main's call.
* `kwin=6.8.3-r1` installed (row 30); `/` free space checked.
* Keep `wdpet.sh` running: every failure mode seen so far (bad `lock_all`, teardown) can hang
  the kernel, and the 30 s watchdog turns that into a reset instead of a battery pull.
* The DECON FRAME IRQ arming is unconditional by design (int72); expect `decon irq` spam in
  dmesg during the run — it is the flip event source, not an error.
* No kernel change, no flash, no reboot is performed by this work.
* Recovery: reboot into `kernel-boot-int49.img` for the UI; module-only iteration needs no
  reflash (`insmod`/`rmmod` the pair).

## 6. Ordered next actions

1. **Backport v4.2 `MODE_ID` + `CREATEPROPBLOB`/`DESTROYPROPBLOB`** into the atomic core
   (donor v4.2 `drm_crtc.c`/`drm_ioctl.c`/`drm_atomic.c`; ~180 lines, no DECON change, keep
   3.10 blob storage). Verify with `modetest -M exynos -p` (MODE_ID listed on CRTC 18) and
   `modetest -M exynos -a -s 20@18:720x1280`. *Owner: KMS core seat.*
2. Run `kwin-try.sh` with the r1 package: the failure should move from
   `Failed to update the basic crtc properties` to either scanout or
   `Atomic commit failed` / `Pageflip timed out`. *Owner: this seat (plan) + Main (device).*
3. Fix `drm_atomic_helper_page_flip` so `plane_state->fb` is installed (0132 diagnostic first);
   this unblocks `kmscube`/`modetest -v` and makes the KWin event path indistinguishable from
   the proven one. *Owner: KMS driver seat.*
4. Make Mesa pair `exynos` (kmsro) with the `panfrost7580` render node (name remap or
   `renderonly` match), then re-run and confirm `Chose /dev/dri/renderD128` plus a non-llvmpipe
   `EGL_RENDERER`. *Owner: MesaPair.*
5. Only after 1–4: `startplasmamobile` under LightDM/elogind (`nura-plasma-session`), which
   needs seat0 `CanGraphical=yes` and the `input` group (`decon-kms-session.md` §2.3).
