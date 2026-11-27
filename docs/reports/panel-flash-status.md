# Panel flash status — 2026-11-27

Device: Samsung Galaxy J7, SM-J700M (`j7elte`, Exynos 7580), Nura 3.10.108.
Display: AMOLED panel driven by the in-tree `exynos_drm7580` KMS driver.
This report lists only facts verified on hardware on 2026-11-27.

## Build and load state

- Kernel build **#37**, built 15:49 EST, flashed to `/dev/block/mmcblk0p10`
  with an md5-verified readback.
- `exynos_drm7580` module is **int121**, running, and includes the blank-frame
  guard: an atomic update is skipped when the framebuffer has three or fewer
  distinct sampled colours.
- Userspace: KWin 6.8.3 (Wayland, `--drm`) and plasmashell 6.8.3.

## Symptom

- The panel is black most of the time.
- Intermittent blue/magenta wallpaper frames appear, separated by black frames;
  sometimes the panel is solid black or solid blue.
- Title bars and windows appear in the "good" frames, so the compositor is
  presenting content.

## Evidence gathered

- Composer liveness: cursor visible, occasional title bar drawn.
- Driver scanout dumps show successive KMS commits alternate between all-zero
  buffers (skipped by the blank-frame guard) and buffers with real content.
- Panfrost job timeouts repeat every ~1.3 s with the signature
  `js_status=0x08`, `head == tail`, `jc_age=0`, followed by `recover-done` and
  bursts of `rawstat=0x02` IRQs.
- With KWin SIGSTOPped (`kill -STOP`), a 6 s photo sample showed **zero panel
  flashing**; the panel resumed flashing after SIGCONT.
- The panel is AMOLED; there is no valid backlight node.

## Open questions

- Whether the flashing follows DECON commit cadence (i.e. the panel flips
  between two buffers on every presented frame) or is retriggered by panfrost
  job timeout recovery.
- Why all-zero buffers are still reaching scanout when the blank-frame guard is
  active, and which caller produces them.
- Whether the panfrost timeouts (`js_status=0x08`, `head == tail`) are a cause
  or a side effect of the same KMS/presentation stall.

## Next experiments

1. **GPU-fault-free compositing test** — run a compositing session with software
   GL (no panfrost jobs), and sample the panel photographically. If the
   flashing stops, the panfrost timeout path is implicated; if it persists, it
   is KMS/DECON-side.
2. **Commit/fb id correlation** — record the DECON commit and framebuffer id
   for each atomic commit and correlate them with photographed frames, to prove
   which buffers are actually visible and how long each stays on screen.

## Rollback

Boot TWRP from `mmcblk0p11`, then restore `/data/backup-boot-p10.img` over
`/dev/block/mmcblk0p10` (`dd` with `conv=fsync`, then `sync`, then reboot).
