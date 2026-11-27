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
