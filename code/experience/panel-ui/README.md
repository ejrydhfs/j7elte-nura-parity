# panel-ui — device scripts from the 2026-11-27/28 panel session

These are the host and on-device scripts used by omp sessions
`01a1d845`, `01a1eaa3` and `01a21eee` to bring up KWin 6.8.3 and Plasma
Mobile on the vendor DECON KMS driver, and to diagnose the panfrost7580 GPU.
They were collected from the host `/tmp` working set after the session; they
run on the phone (`/home/user`, root) and are kept verbatim for reproduction,
not as polished tools.

Device specifics assumed by the scripts: USB network `172.16.42.2/24` (host)
→ `172.16.42.1` (phone), ssh user `user`, root via `/usr/local/bin/rootshell`.
The ssh askpass helper `/tmp/ap.sh` is intentionally not included.

## Session launchers (`cs*.sh`, `kwin*.sh`, `pshell*.sh`, `early.sh`, `minimal.sh`)

| Script | Purpose |
| --- | --- |
| `cs18.sh` … `cs48.sh` | sequential Plasma/KWin session launchers. The final one, `cs48.sh`, is the deployed autostart: dbus + KWin (int129, ARGB→BGRX) + `kactivitymanagerd` + `kded6` + supervised `plasmashell` restart loop. |
| `csdesk.sh`, `cs_noshim.sh` | variants for the desktop shell and without the `addfb` shim. |
| `kwinonly*.sh`, `kwinrun.sh`, `kwinsw.sh`, `kwinD.sh`, `kwinE.sh`, `kwindbg.sh` | KWin-only launches and debug runs (preload `segv.so`, `addfb_shim.so`, backtrace capture). |
| `pshell*.sh` | start `plasmashell` under an already-running KWin. |
| `early.sh`, `minimal.sh`, `cleanstart*.sh`, `install.sh`, `devbuild5.sh` | base session / Plasma Mobile build and install helpers. |
| `capture.sh`, `kwin-cap.sh`, `mesa203-watch.sh` | kernel-log capture and Mesa 20.3.5 contention/watch harnesses. |
| `probe.sh`, `probe2.sh`, `runpanel.sh`, `panelcfg.sh`, `usehalcyon.sh`, `qmlfix.sh`, `wp.sh`, `rebuild.sh` | panel/shell probes and config edits. |

## Crash / buffer probes (`*.c`)

| File | Purpose |
| --- | --- |
| `segv.c` | musl signal handler; dumps `/data/kwin-maps-crash.txt` and `/data/kwin-stack.txt` on SIGSEGV (used to catch the libgallium crash). |
| `addfb_shim.c` | `LD_PRELOAD` shim for `drmModeAddFB` used during the dumb-buffer experiment. |
| `touchinject.c` | uinput touch event injector. |
| `dumbprobe.c`, `dumbtest.py` | `CREATE_DUMB` / `MAP_DUMB` / mmap / write validation. |
| `fbtest.c`, `fliptest.c`, `showtest.c`, `shot.c`, `bandtest.c`, `crashtest.c` | fbdev flip, scanout and panel-band probes. |
| `mesa203-smoke.c` | minimal EGL + GLES2 probe for the Mesa 20.3.5 panfrost stack. |

## KWin scripting and QML probes (`*.js`, `*.qml`)

`fs*.js`, `wins*.js`, `probe.js`, `raise.js`, `hidedesk.js`, `panel.js`,
`activate.js` are KWin scripting probes; `*.qml` are the test scenes
(rotating box, solid colours, panel/window geometry).

## Image-analysis helpers (`*.py`)

`wpfinal.py`, `greenwp.py`, `green2.py`, `darkwp.py`, `wpimage.py`,
`redfb.py`, `redmark.py`, `redz.py`, `marker.py`, `mark.py`,
`dumpfb.py`, `parse_mjpeg.py`, `stripmarkers.py` decode the camera
snapshots (most-saturated-pixel colour probes, wallpaper checks);
`fix*.py`, `patch*.py`, `repack*.py`, `reader*.py`, `selog*.py`,
`forcer.py` are Plasma/KWin config and Mesa-source patch helpers.

## Mesa 20.3.5 libgbm shim

`patch_gbm.sh` adds `gbm_bo_create_with_modifiers2` and
`gbm_bo_get_fd_for_plane` to Mesa 20.3.5 so KWin 6.8 can load it. The
vertex-attribute fix it relies on is already in
`code/gpu31/mesa/patches/0007-mesa203-vertex-attribute-array-bounds.patch`.

Some scripts from the live phone were not present in the host working set
(`cs21.sh`–`cs30.sh` gaps are covered by the transcript). The device copy at
`/home/user` is authoritative if it is still bootable.
