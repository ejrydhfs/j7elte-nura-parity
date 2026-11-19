# GPS UART RX matrix — SM-J700M (j7elte), BCM4752 on /dev/ttySAC0

Scope: does the GPS chip put *any* byte on the RX line of UART0 under any
plausible serial configuration? Full 9 baud x 2 flow-control x 3 power-sequence
x 2 parity matrix, plus the repo's own daemon handshake and `gpsboot`.

## 1. Verdict

**The RX line produces no traffic under any tested combination.** 108/108
matrix runs returned 0 bytes, and every independent instrument (shell
`stty`+`dd`, `gpsboot`, `j7elte-gpsd --init --probe-baud`) also received
0 bytes over windows totalling ~15 minutes of listening.

The chip also does **not** answer the recovered Broadcom BRI autobaud burst:
16 x `0x80` written at 921600 baud produced 300 ms of complete silence, so the
BRI state machine never advanced past `Bri_SendAutobaud`.

Therefore, at the electrical/protocol level the symptom is not a baud-rate or
parity mismatch: nothing arrives at all, at any rate. The chip is either not
running (clock/reset/XTAL or its own power sequencing inside the module), or
its UART TX driver is held off — a defect ahead of the UART block, not in it.

## 2. Setup

| item | value |
|---|---|
| device | `nura-j7elte`, Linux 3.10.108 #31 aarch64, Alpine/musl userspace |
| tty | `/dev/ttySAC0` (204:64), UART0 pins muxed to `13800000.uart` |
| power control | `/sys/class/sec/gps/GPS_PWR_EN/value` (`0`/`1`, root) |
| lock | every device command wrapped in host `flock /tmp/gpsdev.lock`; longest hold 70 s (< 120 s) |
| matrix instrument | `stty` termios + `timeout 2.2 dd bs=1 count=2048 | od -An -v -tx1` |
| cross-check instrument 1 | `gpsboot` (repo `code/gps/gpsboot.c`, static aarch64) |
| cross-check instrument 2 | `j7elte-gpsd` (repo `code/pmaports/device/archived/j7elte-gpsd`, static aarch64) |

Per matrix cell, in this order:

1. `stty -F /dev/ttySAC0 <baud> cs8 [-parenb|parenb -parodd] -cstopb raw
   -echo -echoe -echok -echoctl clocal cread -ixon -ixoff [-crtscts|crtscts]`
   (opening the device flushes stale input);
2. optional power sequence on `GPS_PWR_EN`:
   * `none` — GPIO left at 1 (previous state, chip continuously powered);
   * `t250` — write 0, sleep 250 ms, write 1, sleep 250 ms;
   * `t2000` — write 0, sleep 2000 ms, write 1, sleep 250 ms;
3. read window: 2.2 s or 2048 bytes, whichever first; print hex.

Power was verified `1` at the start and end of every segment.

### 2.1 Note on the `parenb` annotation in the raw logs

The per-cell readback field printed `applied=8NN`/`8NC`; the parity character
of that field is bogus (the matcher required a leading space, but busybox
prints `parenb` at the start of the cflag line). Re-checked with token grep:

```
$ stty -F /dev/ttySAC0 9600 parenb -parodd cs8 -cstopb raw -echo clocal -crtscts
$ stty -F /dev/ttySAC0 -a | tr ' ' '\n' | grep -E 'pare|cs[5-8]'
parenb
-parodd
cs8
```

So the 8E1 rows really were 8E1; the two parity states did differ. RTS/CTS was
applied as requested (`8NC` rows), and `cs8` was applied everywhere.

## 3. Full matrix (108 runs, 2026-11-19 01:26–01:37 local)

All cells: `bytes=0`, `hex=none` — the capture buffer was empty, so there is
no raw hex to show. Segments were 18 cells each (66 s / 57 s / 57 s / 50 s /
60 s / 57 s wall, all under one lock hold).

| # | baud | flow | power cycle | parity | result | raw hex |
|---|---|---|---|---|---|---|
| 1 | 4800 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 2 | 4800 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 3 | 4800 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 4 | 4800 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 5 | 4800 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 6 | 4800 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 7 | 4800 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 8 | 4800 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 9 | 4800 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 10 | 4800 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 11 | 4800 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 12 | 4800 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 13 | 9600 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 14 | 9600 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 15 | 9600 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 16 | 9600 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 17 | 9600 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 18 | 9600 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 19 | 9600 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 20 | 9600 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 21 | 9600 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 22 | 9600 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 23 | 9600 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 24 | 9600 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 25 | 19200 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 26 | 19200 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 27 | 19200 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 28 | 19200 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 29 | 19200 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 30 | 19200 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 31 | 19200 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 32 | 19200 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 33 | 19200 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 34 | 19200 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 35 | 19200 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 36 | 19200 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 37 | 38400 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 38 | 38400 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 39 | 38400 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 40 | 38400 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 41 | 38400 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 42 | 38400 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 43 | 38400 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 44 | 38400 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 45 | 38400 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 46 | 38400 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 47 | 38400 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 48 | 38400 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 49 | 57600 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 50 | 57600 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 51 | 57600 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 52 | 57600 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 53 | 57600 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 54 | 57600 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 55 | 57600 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 56 | 57600 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 57 | 57600 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 58 | 57600 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 59 | 57600 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 60 | 57600 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 61 | 115200 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 62 | 115200 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 63 | 115200 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 64 | 115200 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 65 | 115200 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 66 | 115200 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 67 | 115200 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 68 | 115200 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 69 | 115200 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 70 | 115200 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 71 | 115200 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 72 | 115200 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 73 | 230400 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 74 | 230400 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 75 | 230400 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 76 | 230400 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 77 | 230400 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 78 | 230400 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 79 | 230400 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 80 | 230400 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 81 | 230400 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 82 | 230400 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 83 | 230400 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 84 | 230400 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 85 | 460800 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 86 | 460800 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 87 | 460800 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 88 | 460800 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 89 | 460800 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 90 | 460800 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 91 | 460800 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 92 | 460800 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 93 | 460800 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 94 | 460800 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 95 | 460800 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 96 | 460800 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 97 | 921600 | none | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 98 | 921600 | none | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 99 | 921600 | none | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 100 | 921600 | none | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 101 | 921600 | none | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 102 | 921600 | none | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |
| 103 | 921600 | RTS/CTS | none (PWR_EN stays 1) | 8N1 | bytes=0 | hex=none |
| 104 | 921600 | RTS/CTS | none (PWR_EN stays 1) | 8E1 | bytes=0 | hex=none |
| 105 | 921600 | RTS/CTS | 0->1, 250 ms | 8N1 | bytes=0 | hex=none |
| 106 | 921600 | RTS/CTS | 0->1, 250 ms | 8E1 | bytes=0 | hex=none |
| 107 | 921600 | RTS/CTS | 0->1, 2000 ms | 8N1 | bytes=0 | hex=none |
| 108 | 921600 | RTS/CTS | 0->1, 2000 ms | 8E1 | bytes=0 | hex=none |

Summary: for every one of the 9 baud rates, both flow-control modes, both
parity modes and all three power sequences, the read window produced **zero
bytes**.

## 4. Daemon's own probe (`j7elte-gpsd`)

Built from the repo source (sha512 of `j7elte-gpsd.c` matches the APKBUILD
entry `85374e61…`), static aarch64, deployed as `/tmp/j7elte-gpsd`, run on the
phone for 70 s:

```
timeout -s TERM 70 /tmp/j7elte-gpsd -d /dev/ttySAC0 -b 921600 \
  -g /sys/class/sec/gps/GPS_PWR_EN/value -p 250 -r /tmp/gpsrun \
  -R /tmp/gps.rx -P -i -v
```

Log (verbatim):

```
2026-11-19 01:37:35.661 j7elte-gpsd: starting: uart=/dev/ttySAC0 baud=921600 power=/sys/class/sec/gps/GPS_PWR_EN/value delay=250ms
2026-11-19 01:37:35.912 j7elte-gpsd: opened /dev/ttySAC0 at 921600 8N1, no flow control
2026-11-19 01:37:35.913 j7elte-gpsd: NMEA PTY ready: /dev/pts/0 (stable link /tmp/gpsrun/tty -> /dev/pts/0)
2026-11-19 01:37:35.913 j7elte-gpsd: NMEA stream socket: /tmp/gpsrun/nmea.sock
2026-11-19 01:37:35.913 j7elte-gpsd: raw UART log: /tmp/gps.rx
2026-11-19 01:37:35.913 j7elte-gpsd: init: autobaud burst sent (16 x 0x80) at 921600 baud
2026-11-19 01:37:36.214 j7elte-gpsd: init: chip silent for 300 ms after autobaud (rx state: 0 bytes discarded)
2026-11-19 01:37:36.214 j7elte-gpsd: init: handshake aborted, falling back to the raw receive loop
2026-11-19 01:37:46.225 j7elte-gpsd: no UART data 10s after power-on: the BCM4752 does not free-run NMEA; try --init (see parity/reports/gps-init-impl.md)
2026-11-19 01:37:51.231 j7elte-gpsd: probing alternative baud rate 9600
2026-11-19 01:37:51.232 j7elte-gpsd: opened /dev/ttySAC0 at 9600 8N1, no flow control
2026-11-19 01:38:01.243 j7elte-gpsd: no UART data 10s after power-on: the BCM4752 does not free-run NMEA; try --init (see parity/reports/gps-init-impl.md)
2026-11-19 01:38:06.249 j7elte-gpsd: stats: rx=0 B nmea=0 bad_cs=0 dropped=0 clients=0
2026-11-19 01:38:06.249 j7elte-gpsd: probing alternative baud rate 115200
2026-11-19 01:38:06.249 j7elte-gpsd: opened /dev/ttySAC0 at 115200 8N1, no flow control
2026-11-19 01:38:16.260 j7elte-gpsd: no UART data 10s after power-on: the BCM4752 does not free-run NMEA; try --init (see parity/reports/gps-init-impl.md)
2026-11-19 01:38:21.266 j7elte-gpsd: probing alternative baud rate 4800
2026-11-19 01:38:21.267 j7elte-gpsd: opened /dev/ttySAC0 at 4800 8N1, no flow control
2026-11-19 01:38:31.279 j7elte-gpsd: no UART data 10s after power-on: the BCM4752 does not free-run NMEA; try --init (see parity/reports/gps-init-impl.md)
2026-11-19 01:38:36.285 j7elte-gpsd: stats: rx=0 B nmea=0 bad_cs=0 dropped=0 clients=0
2026-11-19 01:38:45.674 j7elte-gpsd: exiting: no bytes were ever received from /dev/ttySAC0
2026-11-19 01:38:45.675 j7elte-gpsd: GPS powered off
daemon_rc=0
-rw-r--r--    1 root     root             0 Nov 19 01:37 /tmp/gps.rx

/tmp/gpsrun:
total 0
RAWHEX:
```

`/tmp/gps.rx` was created and left **0 bytes**. The daemon created the PTY
(`/dev/pts/0`, stable link `/tmp/gpsrun/tty`) and the NMEA socket, i.e. the
userspace side is functional; only the UART side is silent. Note that the
daemon's own baud probe (9600 -> 115200 -> 4800 after 15 s each) also saw
nothing, and it does not re-send BRI frames on those retries.

## 5. `gpsboot` confirmation

`gpsboot` (separate process, own termios setup) after a 250 ms power cycle:

```
selftest_rc=0
=== baud 921600 8N1 noflow (gpsboot, exclusive) ===
[    0.000] OPEN /dev/ttySAC0 @ 921600 8N1 raw, 1 steps
[    2.002] R 0: (timeout 2000 ms)
[    2.003] DONE
=== baud 460800 8N1 noflow (gpsboot, exclusive) ===
[    0.000] OPEN /dev/ttySAC0 @ 460800 8N1 raw, 1 steps
[    2.002] R 0: (timeout 2000 ms)
[    2.002] DONE
=== baud 115200 8N1 noflow (gpsboot, exclusive) ===
[    0.001] OPEN /dev/ttySAC0 @ 115200 8N1 raw, 1 steps
[    2.003] R 0: (timeout 2000 ms)
[    2.003] DONE
=== baud 9600 8N1 noflow (gpsboot, exclusive) ===
[    0.001] OPEN /dev/ttySAC0 @ 9600 8N1 raw, 1 steps
[    2.003] R 0: (timeout 2000 ms)
[    2.003] DONE
=== baud 4800 8N1 noflow (gpsboot, exclusive) ===
[    0.001] OPEN /dev/ttySAC0 @ 4800 8N1 raw, 1 steps
[    2.003] R 0: (timeout 2000 ms)
[    2.003] DONE
=== holders after ===
HOLDER 17818 gpsboot
=== 921600 crtscts stty capture ===
Terminated
done
```

Zero bytes at 921600/460800/115200/9600/4800, plus a holder check performed
after the loop.

## 6. Exclusive re-runs (reader-contention controlled)

A reader that holds `/dev/ttySAC0` would silently steal bytes from the
matrix's `dd` windows, so the decisive cells were repeated while enumerating
every process with an open fd to the tty immediately before, during and after
the capture: `pre=[] mid=[] post=[]`.

```
pre_holders=[none] waited=4
mid_holders=[]
Terminated
post_holders=[]
post_pwr=1
```

```
=== 9600 noflow pre=[none] ===
mid=[]
Terminated
post=[]
=== 115200 noflow pre=[none] ===
mid=[]
Terminated
post=[]
=== 921600 crtscts pre=[none] ===
mid=[]
Terminated
post=[]
final_pwr=1
```

All zero. (During this session peer agents were twice observed holding the
tty — one `gpsboot`, one `j7elte-gpsd` — which is why these holder-checked
runs exist; they exclude "another reader ate the bytes" for the vendor rate
8N1, 8N1+RTS/CTS, 9600 and 115200.)

## 7. Harness limits (what is *not* proven here)

* **No positive control of the RX path is possible** on this hardware without
  a loopback or a second transmitter: tty echo applies to *received* bytes,
  not to bytes we write, so `write();read();` cannot fake a byte. What is
  established is that `open`/`termios`/`read` all succeed (no `EIO`, no
  `-ENODEV`) and that writes are accepted (`printf > /dev/ttySAC0` returns 0).
* **No UART register view.** `/dev/mem` exists (1:1) but `open()` returns
  `ENXIO` (strict devmem), `devmem` is not installed, and the
  `s3c2410_serial` debugfs node is not present in this mount namespace.
  `UERSTAT`/`UFSTAT` therefore could not be polled to prove "no framing
  errors, no overrun" independently of the termios read path.
* dmesg contains no UART/parity/GPS messages at all during the runs, but the
  driver only reports frame/parity errors when `INPCK` is set (matrix used
  `raw`), so this is weak evidence, not proof.

## 8. Reading of the result

A physical UART that is transmitting — at any of 4800…921600, with or without
RTS/CTS, after either power sequence — would have produced *something*: valid
NMEA, garbage, or at minimum FIFO/overrun behaviour. Across 108 independent
windows, a 70 s daemon run (including the BRI autobaud burst), and the extra
`gpsboot` and holder-checked runs, not one byte appeared.

Conclusion: the BCM4752 TX line is idle. The fault is upstream of UART0's
receiver: the chip is not booting (or its UART is not enabled) even though
`GPS_PWR_EN` is high and VDD_LDO10 is enabled. This matches
`parity/reports/gps-init-impl.md`: BRI never answers, so no protocol-level
bring-up is possible until the chip actually starts.

## Appendix — artefacts

| host path | content |
|---|---|
| `/tmp/matrix_1.log` … `/tmp/matrix_6.log` | raw per-cell matrix output, 18 cells each |
| `/tmp/daemon_probe.log` | `j7elte-gpsd --init --probe-baud` transcript |
| `/tmp/exclusive.log`, `/tmp/exclusive2.log`, `/tmp/exclusive3.log` | `gpsboot` and holder-checked captures |
| `/tmp/baudmatrix.sh` (and on device) | matrix runner script |
| `/tmp/uartreg.c` | register-peek attempt (unusable: `/dev/mem` ENXIO) |
| `/tmp/j7elte-gpsd.aarch64`, `/tmp/bm_gpsboot` | deployed static binaries |
