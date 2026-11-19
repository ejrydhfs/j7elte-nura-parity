# GPS deploy — `j7elte-gpsd` installed as an OpenRC service (on-device verification)

Device: SM-J700M `j7elte`, Nura (Alpine aarch64/musl), kernel
`3.10.108 #33 SMP PREEMPT Wed Nov 18 21:03:07 EST 2026`. Root via `rootshell`;
no reboot, no flash.

Package: `code/pmaports/device/archived/j7elte-gpsd/` (APKBUILD,
`j7elte-gpsd.c`, `j7elte-gpsd.conf`, `j7elte-gpsd.initd`, `j7elte-gpsd.service`).
The `.c` deployed here is the packaged copy whose sha512 matches the APKBUILD.

**Verdict: PASS.** The daemon is installed at the APKBUILD's paths, enabled in the
`default` runlevel and running; it publishes `/run/j7elte-gpsd/tty` (PTY) and
`/run/j7elte-gpsd/nmea.sock`; `rc-service restart` stops and starts it cleanly with
a new PID; and the packaged `--init` handshake reproduces the four chip replies on
this kernel (`link check: 4 of 9`). The chip still emits no NMEA (host-synthesised;
ESW upload is a separate slice), so no fix output is expected or claimed.

---

## 1. Reproducible install

The phone has only `device-samsung-j7elte` + `linux-samsung-j7elte` installed and
no `abuild`/`pmbootstrap`, so the APKBUILD's `build()`/`package()` steps are
executed directly on the device. The file list, modes and destinations are exactly
`package()`'s.

```sh
# --- on the host: package sources to the phone -----------------------------
cd code/pmaports/device/archived/j7elte-gpsd
ssh user@172.16.42.1 'mkdir -p /tmp/gpspkg'
scp j7elte-gpsd.c j7elte-gpsd.conf j7elte-gpsd.initd j7elte-gpsd.service \
    user@172.16.42.1:/tmp/gpspkg/

# --- on the phone, as root: build() ---------------------------------------
#   APKBUILD: $CC $CPPFLAGS $CFLAGS -std=c11 -Wall -Wextra -Wshadow …
#   $CFLAGS (-O2) + static link, clean compile, no warnings
cd /tmp/gpspkg
gcc -static -O2 -std=c11 -Wall -Wextra -Wshadow -o j7elte-gpsd j7elte-gpsd.c
# j7elte-gpsd: ELF 64-bit LSB pie executable, ARM aarch64, static-pie linked

# --- package(): identical to the APKBUILD --------------------------------
install -Dm755 j7elte-gpsd                                  /usr/sbin/j7elte-gpsd
install -Dm644 j7elte-gpsd.conf                             /etc/j7elte-gpsd.conf
install -Dm755 j7elte-gpsd.initd                            /etc/init.d/j7elte-gpsd
install -Dm644 j7elte-gpsd.service   /usr/lib/systemd/system/j7elte-gpsd.service

# --- enable + start -------------------------------------------------------
rc-update add j7elte-gpsd default
rc-service j7elte-gpsd start >/tmp/svc-start.log 2>&1
```

`rc-update add` printed `service j7elte-gpsd added to runlevel default`; the service
is therefore started at boot (no reboot was performed to prove that line, per the
no-reboot rule — `rc-update show` is the evidence).

Installed files match the APKBUILD `sha512sums` byte-for-byte (the binary is the one
just built; the three data files):

```
b429a5ba…e73b7  /etc/j7elte-gpsd.conf
1f74e7ec…38a76  /etc/init.d/j7elte-gpsd
6a26976c…f2f48  /usr/lib/systemd/system/j7elte-gpsd.service
-rwxr-xr-x root root 535736  /usr/sbin/j7elte-gpsd
```

Operational note (observed): `start-stop-daemon` backgrounds this non-forking daemon
and its stdio is not part of the service log — `/tmp/svc-start.log` contains only
OpenRC's `* Starting j7elte-gpsd ... [ ok ]`. To capture the daemon's own output, run
the installed binary in the foreground as shown in §3. Also start the service with
`rc-service … start >/tmp/… 2>&1` when working over SSH: launched without the
redirect the SSH channel stays open on the daemon, and when that session is torn
down the daemon stops (its `SIGHUP` handler sets the stop flag).

## 2. Service status and consumer paths

```
$ rc-service j7elte-gpsd status
 * status: started
$ cat /run/j7elte-gpsd.pid
12045
$ ps ax -o pid,etime,args | grep j7elte-gpsd
12045  00:51  /usr/sbin/j7elte-gpsd --device /dev/ttySAC0 --baud 921600 \
       --gpio /sys/class/sec/gps/GPS_PWR_EN/value --power-delay 250 \
       --rundir /run/j7elte-gpsd
$ rc-status default | grep gpsd
 j7elte-gpsd                                                       [  started  ]
```

Command line = `/etc/j7elte-gpsd.conf`'s `J7ELTE_GPSD_OPTS` verbatim (the initd
sources the conf; note the APKBUILD installs it to `/etc/j7elte-gpsd.conf`, not
`/etc/conf.d/`). `--init` is **not** in the packaged default; enabling it is a
one-line conf edit (§5).

Runtime directory published by the daemon:

```
$ ls -l /run/j7elte-gpsd
srw-rw---- 1 root root 0  nmea.sock
lrwxrwxrwx 1 root root 10 tty -> /dev/pts/0
$ ls -l /dev/pts/0
crw------- 1 root root 136, 0 /dev/pts/0
$ ss -xl | grep nmea
u_str LISTEN 0 0 /run/j7elte-gpsd/nmea.sock 53731 * 0
```

Consumer paths:

| Consumer | Path |
|---|---|
| `gpsd` / serial NMEA tools | `/run/j7elte-gpsd/tty` → `/dev/pts/0` (stable for the daemon's lifetime; re-pointed after restart) |
| Any program | Unix stream socket `/run/j7elte-gpsd/nmea.sock`, one NMEA sentence per line per client |

Both were exercised: a Python `AF_UNIX` client connected to `nmea.sock`
(`socket client connected`), and opening the PTY link succeeded and blocked awaiting
data (terminated by `timeout`) — correct, because the chip emits no NMEA in this
state. The initd orders the service `before gpsd` and `provide gps`, so
`gpsd -n /run/j7elte-gpsd/tty` is the intended wiring once NMEA exists.

## 3. `--init` handshake — the four replies on this kernel

Captured with the **installed** binary and the packaged default options, foreground,
so the log is visible (the service discards stdio). Service stopped first; the
daemon was then stopped with `SIGTERM` and powered the chip off:

```sh
rc-service j7elte-gpsd stop
/usr/sbin/j7elte-gpsd --device /dev/ttySAC0 --baud 921600 \
  --gpio /sys/class/sec/gps/GPS_PWR_EN/value --power-delay 250 \
  --rundir /run/j7elte-gpsd --init >/tmp/gps-init.log 2>&1 &
# wait for "init: handshake sequence finished", then kill -TERM
```

Log (`/tmp/gps-init.log`, full):

```
2026-11-19 02:55:54.317 j7elte-gpsd: starting: uart=/dev/ttySAC0 baud=921600 power=/sys/class/sec/gps/GPS_PWR_EN/value delay=250ms
2026-11-19 02:55:54.568 j7elte-gpsd: opened /dev/ttySAC0 at 921600 8N1, no flow control
2026-11-19 02:55:54.568 j7elte-gpsd: NMEA PTY ready: /dev/pts/0 (stable link /run/j7elte-gpsd/tty -> /dev/pts/0)
2026-11-19 02:55:54.568 j7elte-gpsd: NMEA stream socket: /run/j7elte-gpsd/nmea.sock
2026-11-19 02:55:54.568 j7elte-gpsd: init: autobaud burst sent (16 x 0x80) at 921600 baud
2026-11-19 02:55:54.869 j7elte-gpsd: init: no unsolicited BRI frame within 300 ms (rx state: 0 bytes discarded); sending requests anyway
2026-11-19 02:55:54.869 j7elte-gpsd: init: Bri_RequestNvRamWord7 sent
2026-11-19 02:55:55.169 j7elte-gpsd: init: no reply to Bri_RequestNvRamWord7 within 300 ms
2026-11-19 02:55:55.169 j7elte-gpsd: init: NvRamWord7 read sent
2026-11-19 02:55:55.170 j7elte-gpsd: init: reply: fe 00 fd 00 1b 07 00 00 00 fc  (cmd=0x00 sub=0x1b data=07 00 00 00)
2026-11-19 02:55:55.170 j7elte-gpsd: init: NvRamWord7 word 7 = 0x00000007
2026-11-19 02:55:55.170 j7elte-gpsd: init: status request 0x40/0x1a sent
2026-11-19 02:55:55.171 j7elte-gpsd: init: reply: fe 00 fd 40 1a 00 00 00 00 fc  (cmd=0x40 sub=0x1a data=00 00 00 00)
2026-11-19 02:55:55.171 j7elte-gpsd: init: status 0x40/0x1a = 0x00000000
2026-11-19 02:55:55.171 j7elte-gpsd: init: Bri_RequestHwVersion sent
2026-11-19 02:55:55.172 j7elte-gpsd: init: reply: fe 00 fd 00 00 05 00 02 30 fc  (cmd=0x00 sub=0x00 data=05 00 02 30)
2026-11-19 02:55:55.172 j7elte-gpsd: init: BRI HW version 0x30020005 (asic type byte 0x00)
2026-11-19 02:55:55.172 j7elte-gpsd: init: Bri_RequestDebugSfr sent
2026-11-19 02:55:55.173 j7elte-gpsd: init: reply: fe 00 fd 1f 95 00 00 00 00 fc  (cmd=0x1f sub=0x95 data=00 00 00 00)
2026-11-19 02:55:55.173 j7elte-gpsd: init: DebugSfr acknowledged
2026-11-19 02:55:55.173 j7elte-gpsd: init: Bri_GetOutOfLegacy sent
2026-11-19 02:55:55.223 j7elte-gpsd: init: no reply to Bri_GetOutOfLegacy within 50 ms
2026-11-19 02:55:55.223 j7elte-gpsd: init: Bri_GetOutOfLegacy sent
2026-11-19 02:55:55.293 j7elte-gpsd: init: no reply to Bri_GetOutOfLegacy within 70 ms
2026-11-19 02:55:55.293 j7elte-gpsd: init: Bri_GetOutOfLegacy sent
2026-11-19 02:55:55.384 j7elte-gpsd: init: no reply to Bri_GetOutOfLegacy within 90 ms
2026-11-19 02:55:55.384 j7elte-gpsd: init: Bri_GetOutOfLegacy sent
2026-11-19 02:55:55.494 j7elte-gpsd: init: no reply to Bri_GetOutOfLegacy within 110 ms
2026-11-19 02:55:55.494 j7elte-gpsd: init: link check: 4 of 9 exchanges answered
2026-11-19 02:55:55.494 j7elte-gpsd: init: stopping here.  The next vendor stages (Bri_ProgramSw/HwFlowControl @0x2a6aa0/0x2a6cfc, ESW/patch loading, RF/clock programming) need either the escape-byte search result or the ESW image, neither of which is available (see docs/reports/gps-init-impl.md)
2026-11-19 02:55:55.494 j7elte-gpsd: init: handshake sequence finished
2026-11-19 02:55:57.335 j7elte-gpsd: exiting: no NMEA bytes were ever received from /dev/ttySAC0 (40 bytes were consumed by the BRI handshake)
2026-11-19 02:55:57.336 j7elte-gpsd: GPS powered off
```

The four replies:

| # | Request (host) | Reply (chip) | Decode |
|---|---|---|---|
| 1 | NvRamWord7 read | `fe00fd001b07000000fc` | cmd `0x00`/sub `0x1b`, word 7 = `0x00000007` |
| 2 | status `0x40/0x1a` | `fe00fd401a00000000fc` | cmd `0x40`/sub `0x1a`, value `0x00000000` |
| 3 | Bri_RequestHwVersion | `fe00fd000005000230fc` | HW version **`0x30020005`** (asic type byte `0x00`) |
| 4 | Bri_RequestDebugSfr | `fe00fd1f9500000000fc` | cmd `0x1f`/sub `0x95` → "asic ready"/DebugSfr ack |

`40 bytes` consumed by the handshake = 4 × 10-byte frames, matching the link check.
`Bri_RequestNvRamWord7` (the 10-byte selection frame) and all four
`Bri_GetOutOfLegacy` trials stay silent, as on the earlier runs; the daemon logs that
and does not abort. Nothing after DebugSfr was invented: the run stops at the exact
vendor stage boundary that needs flow-control programming + the ESW image (separate
slice, `gps-esw.md`).

## 4. Restart behaviour

```
$ rc-service j7elte-gpsd start     # -> [ ok ]
$ echo pid1=$(cat /run/j7elte-gpsd.pid); readlink /run/j7elte-gpsd/tty
pid1=11855
/dev/pts/0
$ rc-service j7elte-gpsd restart
 * Stopping j7elte-gpsd ... [ ok ]
 * Starting j7elte-gpsd ... [ ok ]
$ echo pid2=$(cat /run/j7elte-gpsd.pid); readlink /run/j7elte-gpsd/tty
pid2=12045
/dev/pts/0
$ rc-service j7elte-gpsd status
 * status: started
```

`restart` returned 0, the PID changed, the pidfile was rewritten, and the PTY link
plus socket were recreated (the same `/dev/pts/0` number was reused this time; the
daemon republishes whatever pts it gets). `checkpath` recreates `/run/j7elte-gpsd`
in `start_pre`. No stale pidfile or socket was left behind, and the stop path powered
the chip off before start re-powered it.

## 5. Notes / limits

* **No NMEA.** The BCM4752 does not free-run NMEA, and NMEA is synthesised by the
  stock `glgps`, not by the chip; the daemon currently forwards nothing, so the PTY
  and socket stay silent by design. This report claims transport/plumbing and the
  handshake only.
* **Enabling `--init` in the service** (optional, chip side-effect only):

  ```sh
  sed -i 's|^J7ELTE_GPSD_OPTS="\(.*\)"$|J7ELTE_GPSD_OPTS="\1 --init"|' \
      /etc/j7elte-gpsd.conf
  rc-service j7elte-gpsd restart
  ```
  The shipped default deliberately omits it; with `--init` the daemon consumes the
  four replies and then waits in the raw receive loop.
* **Logs.** OpenRC/`start-stop-daemon` gives the service no log file. For field
  debugging run the foreground command of §3 (add `--verbose --raw-log /tmp/gps.rx`).
* **No reboot** was performed; runlevel enablement was verified with
  `rc-update show` (`j7elte-gpsd | default`). The unit's `depend()`/`provide gps`
  make `gpsd` start after it, and `rc-update add` placed it in `default`.

## 6. Artifacts

| Path | Change |
|---|---|
| `docs/reports/gps-deploy.md` | this report |
| `code/pmaports/device/archived/j7elte-gpsd/*` | unchanged (deployed as-is; sha512 match APKBUILD) |
| device `/usr/sbin/j7elte-gpsd` | built on device: `gcc -static -O2 -std=c11 -Wall -Wextra -Wshadow` |
| device `/etc/j7elte-gpsd.conf`, `/etc/init.d/j7elte-gpsd`, `/usr/lib/systemd/system/j7elte-gpsd.service` | installed 644/755 per `package()` |
| device OpenRC | `j7elte-gpsd` added to `default`, started, restarted |
