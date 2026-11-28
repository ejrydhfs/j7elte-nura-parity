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
