# j7elte experience layer

Bare-metal Nura bring-up for the Galaxy J7 (2015), vendor 3.10.108 kernel,
fbdev-only display (no DRM, no working GTK).

| File | Purpose |
| --- | --- |
| `fbwin.c` | ION buffer + `S3CFB_WIN_CONFIG` + `FBIOPAN_DISPLAY` tool. Modes: `(commit+pan)`, `nopan`, `panonly`, `fbfill`, `single`, `dump`, `pand` |
| `launcher.c` | Xlib + Cairo touch launcher (6 tiles) |
| `start-ui.sh` | commit once, start pan daemon, run X with the launcher |
| `xsession.initd` | openrc service wiring for the above |

Build on the device:

```sh
gcc -O2 -o fbwin fbwin.c                      # needs linux/fb.h, linux/ion.h
gcc -O2 -o launcher launcher.c $(pkg-config --cflags --libs cairo x11)
```

Install:

```sh
install -m755 fbwin launcher /usr/local/bin/
# launcher -> /usr/local/bin/nura-launcher
install -m755 xsession.initd /etc/init.d/xsession
install -m755 start-ui.sh /usr/local/bin/start-ui.sh
rc-update add xsession default
```

See `docs/HARDWARE-BRINGUP-2026-11-16.md` for the full findings.
