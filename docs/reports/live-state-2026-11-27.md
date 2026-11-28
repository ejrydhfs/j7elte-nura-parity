# Live state — 2026-11-27 22:35 UTC

Written after context compaction risk; this file is the authoritative
"what is on the phone right now" note.

## Deployed

| Item | Value |
| --- | --- |
| Kernel | `#39 SMP PREEMPT Fri Nov 27 17:14:54 EST 2026`, built from `scratch-k94` |
| Boot partition p10 | flashed and md5-verified (`b0aebc0bf297dfa7ed37a32bebbdb2e7`, readback identical); image padded to 33,554,432 B |
| Kernel deltas | panfrost MMU/GPU IRQ unmask + erratum 10797 + `TIMEOUT-DIAG`; panfrost `dumb_create`/`dumb_map_offset` |
| Display module | `int124` (`/home/user/exynos-drm7580-int124.ko`): blank-frame decision **before** `decon_kms_commit`, near-black ratio (90 %) blank test, driver shadow frame + replacement, `last_fb[]` pinning |
| Boot backup | `/data/backup-boot-p10.img` (TWRP path: p11) |

## Verified display results

| Experiment | Result |
| --- | --- |
| int122 pre-commit guard | flashing stopped; 30/30 and 40/40 consecutive photos identical |
| int123/124 shadow substitution | held frame survives client buffer recycling; `blank frame replaced from shadow` in dmesg |
| GPU status | fragment job still hangs: `js ACTIVE(0x08)`, `head==tail`, no DONE/ERR; `TIMEOUT-DIAG` = `AS0 FAULTSTATUS=0x090002c3 FAULTADDRESS=0x0b2aa000` (constant), `MMU_RAWSTAT=0`, `GPU_INT_RAWSTAT=0x600`, followed by `tiler did not idle on power-off, aborting suspend` |
| Software compositing (llvmpipe) | KWin dies: SIGSEGV in `libgallium-26.3.0.so+0x6cb5d0` (NULL destination write, `addr=0`); same with `KWIN_COMPOSE=O2` |
| Software compositing (softpipe) | **KWin survives** (`GALLIUM_DRIVER=softpipe`, `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`, dumb buffers on renderD128) |
| Shell | `plasmashell` with `QT_QUICK_BACKEND=software` loads the mobile shell; needs `/usr/lib/libexec/kactivitymanagerd` (NOT `/usr/libexec`) |
| Panel | AMOLED, no backlight; do not use brightness reasoning |

## Device scripts (root-owned unless noted)

| Path | Purpose |
| --- | --- |
| `/home/user/cs21..cs28.sh` | full Plasma session; `cs28` = int124 + panfrost GL |
| `/home/user/cs29.sh`, `cs30.sh` | session with `MESA_LOADER_DRIVER_OVERRIDE=swrast` / `kms_swrast` |
| `/home/user/kwinrun.sh <ENV=VAL>` | KWin-only, preloads `segv.so` + `addfb_shim.so`, prints exit and backtrace |
| `/home/user/pshell4.sh` | starts kactivitymanagerd + kded6 + plasmashell under the running softpipe KWin |
| `/home/user/segv.so` | musl-built crash reporter; dumps `/data/kwin-maps-crash.txt` and `/data/kwin-stack.txt` (frame-pointer walk) |
| `/home/user/dumbtest.py` | CREATE_DUMB/MAP_DUMB/mmap/write check (passes: pitch 3072, size 3932160, offset 0x10000000) |

## Constraints / hazards

- Root filesystem `/` (p20) is ~97 % full. Do NOT scp a 33 MB boot image to
  `/home/user` without deleting the previous one; upload to `/data` via
  `rootshell` or remove after flashing.
- `/dev/block/mmcblk0p10` does not exist on Nura; the node is
  `/dev/mmcblk0p10` (or `/dev/block/259:2`).
- `rmmod exynos_drm7580` fails while the DRM device is open; reboot to load a
  new display module.
- KWin's flip event is sent at commit time, so a held buffer can be rewritten
  by the next (failed) client frame; that is why the driver keeps its own
  shadow copy.
- llvmpipe (`kms_swrast` default) crashes in `libgallium`; use
  `GALLIUM_DRIVER=softpipe`.

## Next

1. Check the softpipe Plasma shell screen (photo) and whether the shell panel
   is live; if yes, freeze that configuration as the usable UI.
2. GPU root cause: dump the fragment job descriptor + SFBD (w10/11) and the
   tiler heap first page at timeout; the fault address is constant
   `0x0b2aa000`, outside every BO in `ERR-MAP`.
3. If the fragment job is fixed, retest with panfrost GL (`cs28`) and int124.

## Update 23:0x UTC — software session

- Display module now **int125**: blank predicate is `<= 2 distinct colours`
  (a dark theme with a panel is 3+ colours and must be scanned out; the 90 %
  near-black rule froze the panel on the shadow image).
- Working software configuration:
  `MESA_LOADER_DRIVER_OVERRIDE=swrast LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=0`
  plus `QT_QUICK_BACKEND=software` for plasmashell. This makes GBM allocate
  **dumb buffers** (720x1292, pitch 2880) on renderD128, so kms_swrast really
  owns the scanout buffers (the earlier `kms_swrast`-only override left GBM on
  panfrost and produced untouched/zero frames).
- Session script: `/home/user/cs31.sh` (dbus, KWin, kactivitymanagerd from
  `/usr/lib/libexec`, kded6, plasmashell).
- Observed: KWin starts, creates the primary/cursor fbs, commits **one** blank
  frame, then goes idle (all threads futex/ppoll; no DRM ioctl wait). A new
  client (`qmlscene6 /data/redfull.qml`) does not trigger another commit.
  plasmashell loads the mobile shell QML (homescreen applets) but nothing is
  painted to the panel.
- Conclusion: with int125 the guard is no longer freezing dark UI; KWin is
  simply not submitting further frames on the kms_swrast path. Next angle:
  kms_swrast swap/callback (LOG `WAYLAND_DEBUG=1` on a client, KWin's
  `kwin_scene_opengl`/swap debug, and whether a second atomic commit ever
  reaches `exynos7580_plane_commit_frame`).

## Update 23:5x UTC — shadow OOB fix, commits flowing

- **int126**: `exynos7580_fb_shadow_write()` is bounded by the allocated
  shadow size and the guard only caches frames >= 320x480. Root cause found in
  the kernel log: `Unable to handle kernel paging request at ffffff800333e000`
  in `exynos7580_fb_shadow_write+0x7c` — a 720x1280 blank frame tried to read a
  shadow that had been sized 32x32 from an early cursor commit. The oops killed
  KWin's commit thread, which is why presentation stopped after one frame.
- After int126: KWin commits continuously (36+ frames), no oops, shell and a
  `qmlscene` client both cause commits. DECON register dumps show the latch
  working: programmed `addr=0x11000000` then `ADD0=0x11000000` after vsync,
  next frame `ADD0=0x10c00000`.
- Remaining: committed frames are 99.9 % black with a few grey/white samples
  (the cursor/panel), i.e. KWin composites its own background but not the
  client scene; the panel still shows an earlier blue image. Next checks:
  (a) confirm the panel follows DECON by displaying a solid colour from a KMS
  test pattern with KWin stopped; (b) confirm client buffers reach KWin
  (shm vs dma-buf: plasmashell is `QT_QUICK_BACKEND=software`, qmlscene is GL).
- Driver switch note: `modetest` cannot open card0 while KWin holds master;
  stop the session first.

## Update 22:35 UTC — vsync reference held; heap experiment; Mesa 20.3.5 building

- Kernel **#41** (built 22:31) holds one `decon_activate_vsync()` reference for
  the pipe lifetime (released in `decon_kms_disable`). Before this, every
  `decon_wait_for_vsync()` activated/deactivated with refcount 0 and its
  deactivate cleared `VIDINTCON0.INT_ENABLE`, so the FRAME line was disarmed
  and the DECON shadow update could stay unlatched. After #41 the panel
  changed from the stale blue to the currently committed (black) frame — the
  panel now follows DECON.
- Display module **int127**: `exynos7580_dump_fb` now dumps the *primary*
  plane (was the triggering/cursor plane) and logs a 6x10 pixel grid.
  Dump of the committed frame: all `ff000000` with `ff616161` at the centre —
  a black frame plus the cursor. Client content is still missing.
- Heap experiment (kernel #40): BOs created with `PANFROST_BO_HEAP` are
  allocated with 256 MiB (`heap1` count was 0, so Mesa 26.3 did not set the
  flag; allocation unaffected). The timeout fault address moved from
  `0x0b2aa000` to `0x132aa000` — exactly +0x8000000 — i.e. the access is
  relative to the heap layout, `0x12aa000` past the 4 KiB BO that follows the
  heap. The tiler access is therefore a fixed offset past the heap chain, not
  a random unmapped page.
- Recovery now also writes the *current* slot command (`JSn_COMMAND` 0x20,
  HARD_STOP) in addition to `JS_COMMAND_NEXT`.
- In flight: native musl Mesa 20.3.5 build on the device
  (`/data/mesa203-build`, install prefix `/data/mesa203-install`) — the stack
  that was previously validated at 151 fps against this driver.

## Update 04:40 UTC — Mesa 20.3.5 built; kernel MMU fault is the remaining GPU gate

- Mesa 20.3.5 built natively on the device (musl, gcc 15) and installed at
  `/data/mesa203-install` (panfrost + kmsro, no LLVM, 16.6 MB). Smoke test:
  `GL_RENDERER="Mali T720 (Panfrost)"`, `GL_VERSION="OpenGL ES 3.0 Mesa 20.3.5"`.
  Patch 0007 (vertex attribute bounds) applied; no heap/syncobj patch needed.
- Correction from the build agent: fragment jobs still hit
  `js_status=0x8 ACTIVE`, `head==tail`, and the driver recovers after ~120 ms;
  every draw also logs `AS0 FAULTSTATUS=0x800002d8 FAULTADDRESS=<job desc VA>`
  (ACCESS_FLAG, 0xD8) inside BOs that are in the submit list and whose
  page-table leaves match (PTEWALK leaf_phys == bo_phys, AF=1). Readback is
  not run-to-run stable -> kernel MMU defect, not a Mesa build problem.
- KWin 6.8.3 cannot run on Mesa 20.3.5's libgbm: `libkwin.so.6` needs
  `gbm_bo_create_with_modifiers2` and `gbm_bo_get_fd_for_plane`. Keeping the
  system libgbm 26.3 and taking only libEGL/libGLESv2/GL/dri from 20.3
  (`/data/mesa203-kwin/lib` symlink set) loads, but KWin then segfaults during
  GL init (backtrace captured via `/home/user/segv.so`, frames in libgallium).
- Consequence: the remaining GPU blocker is the MMU ACCESS_FLAG fault in the
  kernel port, independent of Mesa version. Next: compare our leaf/table ATE
  programming for the t72x with `kbase_mmu_get_mmu_flags`/`kbase_mmu_setup`
  around the exact faulting BO, and check the upper-level table entries
  (PTEWALK only validated the leaf).

## Descriptor evidence (last capture)

Fragment node at jc=0x13202a00:
`00000000 00000000 00000000 00000000 00010013 00000000 00000000 00000000
 00000000 004f002c 13202000 00000000 ...` — header type 9 (FRAGMENT), bounds
0x004f002c (44x79 tiles), FBD pointer **0x13202000**.

The timeout fault is at **0x132aa000 = FBD + 0xaa000** (and earlier
0x0b2aa000 = FBD + 0xaa000 on the 128 MiB-heap layout). So the GPU follows a
pointer inside the SFBD that lands 696,320 bytes past the framebuffer
descriptor, and the address moves with the descriptor allocation, not with the
framebuffer. Next: dump the first 320 bytes of the SFBD (at `job->fbd`) at
timeout and find the field equal to FBD+0xaa000 (candidates: tiler
`polygon_list` / `polygon_list_body` / sample-position or varying pointer).

## Update 05:55 UTC — AF bit removed: Mesa 20.3.5 renders with zero faults

- **Root cause of the ACCESS_FLAG faults: our leaf ATEs set bit 10 (AF).**
  Vendor `kbase_mmu_get_mmu_flags()` (mali_kbase_mmu.c:475-498) never sets
  `ENTRY_ACCESS_BIT` (1<<10); our `panfrost7580_mmu.c` OR'd
  `P7580_PTE_AF_BIT` into every leaf. Removing it (kernel **#42**):
  - Mesa 20.3.5 smoke (`/data/mesa203-run-smoke.sh`, gbm 720x1280):
    `fragment-job csum=0x83bb99d80da181cb RENDERED`, `eglSwapBuffers ok`,
    `RESULT PASS`, and the kernel log has **0 FAULTSTATUS lines**.
- libgbm 20.3.5 patched with `gbm_bo_create_with_modifiers2` and
  `gbm_bo_get_fd_for_plane` (Mesa 20.3.5 predates them, KWin 6.8 links them);
  rebuilt and installed into `/data/mesa203-install`.
- KWin with the full 20.3.5 stack now loads but fails at
  `DRM_IOCTL_PANFROST_CREATE_BO failed: Invalid argument` after a few
  successful CREATE_BOs. The 256 MiB heap override from the previous build was
  reverted (kernel #43, building); it did not change the hang and could be the
  failing allocation.
- Note: the earlier "descriptor is 0xaa000 past" relation tracked the
  descriptor BO layout, not the framebuffer — it moved with every allocation
  change, consistent with the AF-fault falsely aborting the job at a
  descriptor-relative address.

## Update 06:30 UTC — GPU clean, wallpaper on glass, panel windows not composited

- Kernel **#43** (AF bit removed, heap override reverted) + system Mesa 26.3:
  `timeouts=0 faults=0`; the shell's wallpaper is rendered on the panel
  (photo: 720x1280 gradient, stable). This is the first time the compositor's
  real content is on glass with no GPU faults.
- Session scripts: `rm -f $RUN/wayland-0 $RUN/wayland-0.lock` is now required
  (a killed KWin leaves the lock and the next start dies with
  "could not add wayland socket").
- The shell's panel windows exist and render (`PanelView` 720x56, 218 scene
  graph update lines; containments `org.kde.plasma.mobile.taskpanel` and
  `org.kde.plasma.mobile.panel` are in the appletsrc) but neither appears in
  the composite:
  - `KWIN_USE_OVERLAYS=0`, `KWIN_DRM_NO_DIRECT_SCANOUT=1`, and disabling the
    fullscreen effect did not change it.
  - Shell uses LayerShellQt (`org.kde.layershell`) for its windows; next check
    is the layer/anchor the panels request vs what KWin composites, and whether
    the panel surface is mapped at all (KWin supportInformation no longer lists
    windows on this build).
- Screen right now: the shell wallpaper (blue gradient) with the cursor; no
  panel, no clock.

## Contended matrix (#43) and the remaining KWin fault

Mesa 20.3.5 probe, 34 runs, own-jc attribution (agent report):
- own MMU faults: **0/34** after the AF-bit removal (the #42 shadow
  translation-fault family is gone).
- own timeouts: 3/15 DRAWS=1, 7/8 DRAWS=2, 0/5 with gap=500, 0/5 with pad,
  0/1 at 720x1280; every recover now ends `latched=0x00000000` (the
  `JSn_COMMAND` hard-stop takes effect).
- Foreign (kwin): 1-43 faults and 0-1 timeouts in **every** run window.

Direct capture of kwin on #43: `FAULTSTATUS=0x090002c3` (translation, access
type 2), `FAULTADDRESS=0x24864000`, `recover-done latched=0x8`, ~5 timeouts/s.
That address is **not inside any BO of the submit** (ERR-MAP for seq 1496/1505
covers 0x160xxxxx-0x165xxxxx, 0x1b5ba000, 0x23cf8000..0x24084000, 0x293f000,
0x3176000, 0x882c000; no range contains 0x24864000). So Mesa 26 references a
VA outside the submitted BO set, not a hole in one of them.

Pending (next build): move the `JSn_STATUS` read before `JOB_INT_CLEAR`
(panfrost7580_job.c:847 vs :879) so DONE prints 0x01 not 0x00, and add a
fault-address PTE/owner dump at TIMEOUT-DIAG.

## Kernel #44 — all-BO mapping removes KWin's fault stream

- `p7580_drv_map_file_bos()` (panfrost7580_submit.c:1756, called :1858) walks
  `file_priv->object_idr` and maps every BO the DRM file owns before dispatch.
  This closes the hypothesis that Mesa 26 references a context BO that is not
  in the batch handle array (the 0x24864000 execute VA).
- With an animated Wayland client (qmlscene6, rotating box + 16 ms timer,
  kwin 46% CPU) on #44: **0 TIMEOUT and 0 TIMEOUT-DIAG in dmesg**, versus
  1-43 faults per 2.5 s window on #43. The fault stream is gone.
- Not fixed: no client window reaches the panel. The shell background is
  scanned out (guard log: `fb 30 blank=0 colours=4 dark=27/230400
  ffd0e7dc ...`), but a GL qmlscene window, the same window forced to
  `QT_QUICK_BACKEND=software` (shm), and the shell's own PanelView are all
  absent from the composite; photos 0.5 s apart differ by max 34/765 and the
  panel never changes. KWin's `commit_tail` lines at 205-207 s with changing
  `ADD0` are not accompanied by fresh guard lines, so they are not primary
  frames from KWin.
- Next: KWin scene/client-surface capture with `kwin_scene_opengl.debug`, and
  check whether the client surfaces are committed but dropped (texture import)
  or never committed.

## Kernel #46 — shadow lifetime pin; client content composites

- Root cause of the 8/30 content losses on #44 (probe: TRANSLATION_FAULT at
  chain node2+0x40, i.e. a page of the driver-owned chain-shadow BO):
  `p7580_drv_shadow_put()` unmapped the shadow when its node refs hit zero,
  which is also when the last GEM reference drops and `gem_free_object` runs.
  Removing the explicit unmap alone was a no-op relocation (the object died at
  the same instant).
- Real fix in #46: `shadow_create` takes one extra GEM reference and hangs the
  object on `df->shadow_pins`; `p7580_drv_file_fini()` unpins after the entity
  drain (labels: `p7580 shadow pin obj=… iova=… refs=…` /
  `p7580 shadow unpin n=…`). Shadow pages now stay mapped for the file's life.
- Result on #46 with an animated Wayland client (qmlscene6, rotating box):
  **0 TIMEOUT / 0 TIMEOUT-DIAG**, and the client's frames are now on glass —
  two stills differ by max 96/765 with ~15k changed pixels; the composited
  window (white area + rotating box) is visible in the camera photo.
- Shell after client exit: wallpaper on glass; shell restart shows the
  wallpaper again (photo), panel still not confirmed visually at this angle.

## #46 validation and the panel question

- Probe (Mesa 20.3.5, 30x 720x1280, content-verified) on #46: **30/30 PASS,
  0 FAIL, 0 VOID**; every run `pin=1 unpin=1`, no faults/timeouts, correct
  `rgba=128,64,191`, spread 0. Baseline #44 without the pin was 8/30 FAIL at
  chain node2+0x40 with black content. The lifetime pin closes that class.
- The current pin is unbounded (one 4 KiB shadow page per chain until close,
  ~1.7 GB/h at 60 fps) and must NOT ship. Production shape agreed with the
  probe agent: a per-file ring of N=2 (N=4 margin) shadow slots, slot k reused
  only after submit k+1 completes; all submits of a file are serialised by
  df->serial_prev, so one submit of slack is provable. Bounded by open files.
- KWin scene enumeration via a KWin script (`org.kde.kwin.Scripting`, loaded
  over the session bus) shows exactly two windows: 720x1280 at 0,0 (layer 0)
  and **720x56 at 0,0 (layer 3, Below)**, i.e. the panel window exists at the
  top of the screen. The shell's clock applet is alive (`ClockText`, 22x15).
  KWin scripting's window properties (keepAbove/minimized/layer) are
  read-only in 6.8.3 — assignments did not stick.
- Photo zoom of the top 56/1280 rows shows only the wallpaper gradient; a
  22x15 clock would be ~1 photo pixel at this angle, so "panel absent" is not
  yet proven. Next: sample the scanout buffer at y=10/28/45 (extend
  exynos7580_dump_fb) or add a KWin-side layer rule, before patching KWin.

## Panel-row scanout sample (int128) and shell config

- int128 extends `exynos7580_dump_fb` to y=10/28/45 (the 56 px panel band).
  On #46 + the restarted shell, the sampled commit read all black at
  y=10/28/45/100/1200 and `ff616161` at y=640 — i.e. the frames arriving at the
  commit path at that moment are clear frames (the guard skips them; the panel
  shows the held wallpaper). The panel's own pixels therefore cannot be
  judged from these commits.
- The mobile-shell appletsrc has no `panelVisibility`/opacity keys (defaults).
- Open: obtain one *displayed* frame's panel rows (sample right after a
  repaid commit, or take a KWin-side capture) to decide whether the panel is
  composited-but-transparent or stacked below the desktop.

## Colour: panel shows the complement of the framebuffer (leading finding)

- Guard sample of the wallpaper frame: `ff22b567` (RGB 0x22,0xb5,0x67 = green).
  Camera photo of the same moment: magenta/purple; the qml test window's
  `#c03030` red box photographed as cyan.  Complement of red is cyan,
  complement of the green sample is magenta — i.e. the panel is displaying an
  inverted (complemented) image.
- The camera itself does not invert (the phone's white Samsung logo and the
  teal wall photograph with their true polarity).
- No inversion bit in DECON `VIDOUTCON0` (only `LCD_ON` bit 24 and the IF
  select).  The DECON RGB-order mapping (`decon_rgborder()`,
  decon_core.c:518-556) only swaps channels, never complements.
- Therefore the inversion is in the panel path (ea8061 gamma / inversion).
  Vendor `ea8061_init()` (ea8061_lcd_ctrl.c:881) calls the DSI init sequence
  that includes `dsim_panel_gamma_ctrl()` (:343/:187); the KMS path enables
  the panel through `decon_kms_enable()` -> `decon_enable()`, so the next check
  is whether the gamma sequence actually runs (and with which br_index), and
  whether the panel needs the vendor's "gamma mode"/inversion parameter set.
- A controlled SMPTE test (`modetest -s 21@19:720x1280 -F smpte`) did not
  produce usable bars (dark striped buffer), so the colour test still needs a
  solid-colour fb (fbdev pan or a KMS solid pattern) to measure per-bar RGB.

### Colour probe method (runtime, no rebuild)

`exynos7580_plane.c` has a diagnostic override: module parameter
`exynos7580_fourcc32` (32-bpp fourcc handed to DECON).  The compiled-in remap
then turns ARGB→XRGB, ABGR→XBGR, RGBA→RGBX, BGRA→BGRX, so the parameter
selects one of four WINCON BPPMODEs:

| param value | DRM fourcc | remapped to | WINCON BPPMODE |
| --- | --- | --- | --- |
| 0x34325241 | ARGB8888 | XRGB8888 | BGRX (current) |
| 0x34324241 | ABGR8888 | XBGR8888 | RGBX |
| 0x34324152 | RGBA8888 | RGBX8888 | XRGB |
| 0x34324142 | BGRA8888 | BGRX8888 | ARGB |

Apply with `echo <value> > /sys/module/exynos_drm7580/parameters/exynos7580_fourcc32`;
the next compositor frame picks it up (an animated client makes the change
visible live).  Colour probe: the qml test window's `#c03030` box must
photograph RED under the correct mapping (currently it photographs cyan/
magenta because of the alpha-byte interpretation).

### Colour permutation result: the panel inverts; fourcc only permutes

Animated box (#c03030 red) photographed under each `exynos7580_fourcc32`
mapping (mean of the most saturated pixels):

| param | mapping | box colour |
| --- | --- | --- |
| 0x34325241 | ARGB→XRGB (BGRX) | (1,189,236) cyan = complement of red |
| 0x34324241 | ABGR→XBGR (RGBX) | (254,68,214) magenta |
| 0x34324152 | RGBA→RGBX (XRGB) | (255,2,174) magenta |
| 0x34324142 | BGRA→BGRX (ARGB) | (1,27,227) blue |

No permutation yields red, and the best case (BGRX) is the *complement* of the
source colour.  So the channel order is only a rotation; the panel is applying
an inverted (complemented) transfer.  The inversion is in the ea8061 gamma
path, not in DECON (no inversion bit) and not in the fourcc mapping.
Next: re-run the vendor ea8061 gamma sequence (`dsim_panel_gamma_ctrl`,
ea8061_lcd_ctrl.c:187, called from the panel init sequence at :343) on every
panel enable, and check `br_index`/gamma mode used at KMS enable.

### Colour fix landed (int129) + boot autostart

- Measured correct mapping: ARGB8888 buffers display correctly when DECON is
  handed **BGRX8888** (WINCON BPPMODE_XRGB8888, format=7).  The module param
  `exynos7580_fourcc32=0x34324142` proved it live (screen turned green to match
  the 0x22b567 framebuffer; photo sat-mean (1,220,142)).
- int129 makes it the compiled default: `DRM_FORMAT_ARGB8888 -> BGRX8888`
  (exynos_drm7580_plane.c), so no module parameter is needed.
- `/etc/local.d/plasma.start` now loads int129 and starts `/home/user/cs47.sh`
  in the background at boot, so the phone brings up KWin + plasmashell without
  an SSH session (log: /data/plasma-boot.log).

## Final state (2026-11-28): colours correct, UI composited

- int129 (`ARGB8888 -> BGRX8888`) is deployed and is now the compiled default;
  the final photo of the qml probe window shows **red box (correct), white
  window (correct), green wallpaper (correct)** — the magenta/complement tint
  is gone.
- `/etc/local.d/plasma.start` loads the display module and starts
  `/home/user/cs47.sh` in the background at boot, so KWin + plasmashell come up
  without an SSH session.
- Session transcript archived in `sessions/` with this commit.

## plasmashell crash and supervisor (2026-11-28)

- Symptom: wallpaper and title bar disappeared while kwin stayed up.
  `/data/plasma.exit` = `PLASMA_EXIT=134` (SIGABRT), `kf.crash: Could not find
  drkonqi`; kwin kept the last client frame.  Restarting plasmashell restored
  the shell (photo: green wallpaper, correct colours).
- Fix: the session script now supervises plasmashell —
  `while true; do plasmashell >> log; sleep 2; done` — so a shell crash
  self-heals.  `/etc/local.d/plasma.start` loads int129 and starts
  `/home/user/cs48.sh` (the supervised variant) at boot.
- Rebooted to exercise the autostart end to end (module int129, param 0,
  KWin+plasmashell without any SSH command).
