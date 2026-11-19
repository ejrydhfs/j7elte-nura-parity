# Running the stock Android gpsd in a chroot on the phone (BCM4752 / j7elte)

Slice: `gps-chroot`. Host: Arch x86_64. Target: SM-J700M (j7elte), Nura (Alpine musl
aarch64 userspace, our 3.10.108 arm64 kernel), SSH as `user@172.16.42.1` with
`/usr/local/bin/rootshell` for root.

## Result in one paragraph

The stock 32-bit bionic `/system/bin/gpsd` **does execute and initialize its GPS
engine inside a chroot on the device**, but it never gets that far on its own:
before touching the UART it registers an OEM RIL client and blocks forever
retrying an abstract UNIX socket `@Multiclient` (1 s sleep per retry,
`ECONNREFUSED`, no rild on this ROM). With a 12-line fake `@Multiclient`
server accepting and ACKing nothing, gpsd proceeds: enables
`/sys/class/sec/gps/GPS_PWR_EN/value` (writes `"1\n"`), waits 250 ms, opens
`/dev/ttySAC0`, sets termios, reads (chip silent), writes exactly one BRI
frame `fe 00 fd 00 00 55 aa 00 ff fc` (`GetOutOfLegacy`), reads again, creates
`/cache/gpspipe` + `/cache/glgpsctrl` FIFOs, prints
`[cat /cache/gpspipe] to stream out NMEA`, and exits 0 after ~2–3 s because the
chip never answers.

This independently confirms the `GetOutOfLegacy` frame implemented by the repo
daemon (`patches/userspace/gps/0002-…`) byte-for-byte, and shows the stock init
order differs from the repo daemon's (stock sent only the 10-byte
`GetOutOfLegacy` after its legacy-read phase; the 16×`0x80` autobaud burst was
*not* observed on this fixed-baud path).

## 1. Extraction (host, no mount, image is ext4)

Image: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7stock/system.raw`
(UUID 57f8f4bc-…, 2206203904 bytes). Used `debugfs -R "dump /path /dest"` per
file, then iterated `readelf -d` NEEDED transitively from `gpsd` and from the
HAL/`libwrappergps` closure.

```
IMG=…/j7stock/system.raw
debugfs -R "dump /bin/gpsd  /tmp/stockfs/system/bin/gpsd"  $IMG
debugfs -R "dump /bin/linker /tmp/stockfs/system/bin/linker" $IMG
debugfs -R "dump /bin/gps.cer /tmp/stockfs/system/bin/gps.cer" $IMG
debugfs -R "dump /etc/gps.xml /tmp/stockfs/system/etc/gps.xml" $IMG
debugfs -R "dump /etc/gps.conf /tmp/stockfs/system/etc/gps.conf" $IMG
debugfs -R "dump /lib/hw/gps.default.so /tmp/stockfs/system/lib/hw/gps.default.so" $IMG
# then, loop: for every NEEDED lib not yet present:
#   debugfs -f dump.cmds $IMG   (one "dump /lib/<name> /tmp/stockfs/system/lib/<name>" per lib)
```

Closure: 93 files in `system/lib/` + `lib/hw/gps.default.so`, plus 4 binaries/configs.

- `system/bin/gpsd` 4 059 464 B (ELF32 ARM, interpreter `/system/bin/linker`, Android API 23)
- `system/bin/linker` 190 292 B
- `system/bin/gps.cer` 32 531 B
- `system/etc/gps.xml` 1 500 B, `system/etc/gps.conf` 205 B
- `system/lib/hw/gps.default.so` 141 500 B
- `system/lib/` 39 MiB total, 93 libs; largest: libLLVM 10.9M, libpdfium 4.3M,
  libskia 4.1M, libsec-ril 2.47M, libicui18n 1.56M, libicuuc 1.19M,
  libandroid_runtime 946K, libxml2 893K, libmedia 772K, libcrypto 738K,
  libc 678K, libgui/libbinder/libhardware_legacy/libwrappergps …

Names (93): libAstcEnc … libwrappergps, libxml2, libz (full list is reproducible
with the loop above; `gpsd` NEEDED = liblog libicuuc libutils libhardware_legacy
libcutils libssl libcrypto libgui libbinder libandroid libwrappergps libc++ libdl
libc libm; `libwrappergps` adds libril libsecril-client libsqlite libsec-ril,
which in turn pulls libbinder/libgui/libhardware_legacy/libsqlite/libcrypto …).

## 2. Deploy (device has 31.7 MB free on `/`, 7.6 GB on `/data`)

`/opt` exists on the root fs (31 MB free) so the payload lives on `/data/opt/stock`
and `/opt/stock` is a symlink (chroot works on either path).

```
tar czf /tmp/stockfs.tgz -C /tmp/stockfs .          # 22 MB, 42.3 MB unpacked
cat /tmp/stockfs.tgz | ssh user@172.16.42.1 \
  '/usr/local/bin/rootshell sh -c "mkdir -p /data/opt/stock && tar xzf - -C /data/opt/stock && \
     mkdir -p /data/opt/stock/{data/system/gps,dev,proc,sys,tmp} && chmod 1777 /data/opt/stock/tmp && \
     ln -sfn /data/opt/stock /opt/stock"'
```

32-bit compat confirmed: `chroot /data/opt/stock /system/bin/linker --help` prints
the linker banner (RC 0), so the arm64 kernel runs the ARM32 bionic binaries.

## 3. Chroot runtime setup that gpsd actually needs

```
CH=/data/opt/stock
mount --bind /dev $CH/dev      # ttySAC0, alarm, binder, null
mount --bind /proc $CH/proc
mount --bind /sys  $CH/sys
mkdir -p $CH/dev/socket && chmod 777 $CH/dev/socket   # MUST be after the /dev bind mount
mkdir -p $CH/cache $CH/data/gps/log $CH/data/system/gps $CH/vendor/etc
cp $CH/system/etc/gps-nosupl.xml $CH/vendor/etc/gps.xml
ANDROID_DATA=/data ANDROID_ROOT=/system \
  chroot $CH /system/bin/gpsd /system/etc/gps-nosupl.xml gpsd
```

(`/vendor/etc/gps.xml` is also probed by the binary; `/dev/socket` does not exist on
Nura, hence the mkdir inside the bind mount. `/cache/gpspipe` and
`/cache/glgpsctrl` are FIFOs the daemon creates inside the chroot. Missing tzdata
produces harmless `__bionic_open_tzdata…` warnings.)

Test config: copy of stock `gps.xml` with `SuplEnable="false"`, `LbsEnable="false"`,
`HttpSyncLto="false"`, `LogDirectory="/data/gps/log"`; UART settings unchanged:
`PortName="/dev/ttySAC0" BaudRate="921600" GpioNStdbyPath="/sys/class/sec/gps/GPS_PWR_EN/value" GpioDelayMs="250"`.

## 4. Failure mode A — stock gpsd alone: blocked on `@Multiclient` RIL, never opens the UART

Running the command above without any RIL service leaves gpsd alive indefinitely
with no `/dev/ttySAC0` in its fd table. Threads (from `/proc/<pid>/task/*`):

- main: `hrtimer_nanosleep`, ARM syscall 162 (`nanosleep`) 1.000 s
- tid 2: `poll_schedule_timeout`
- tid 3: `pipe_wait`

A small ptrace tracer (`/data/opt/stock/tmp/tracer.c`, aarch64 musl, follows the
32-bit tracee via `PTRACE_GETREGSET/NT_PRSTATUS`) shows an endless loop:

```
socket(AF_UNIX, SOCK_STREAM) -> fd
connect(fd, {AF_UNIX, sun_path="\0Multiclient"}, 14) = -111 (ECONNREFUSED)
close(fd)
nanosleep({1,0})
```

14 = 2 + 1 + strlen("Multiclient"). On stock this is Samsung's rild "Multiclient"
OEM RIL channel; there is no rild here. Preceding setup (one-time): open
`/dev/log/main|radio|events|system` O_WRONLY (all fail, legacy logger probe),
`/sys/power/wake_lock|wake_unlock`, `/system/etc/gps.xml`, `/data/system/gps/.gpsd.lock`,
`/dev/alarm` + ioctl `0x40086134`, `/cache/glgpsctrl` (FIFO), then the RIL loop.
**The UART is never opened in this state.** This is the exact failure mode when
running the stock daemon on Nura as-is.

## 5. Failure mode B — with a fake `@Multiclient` server: UART is opened, one BRI frame goes out, chip silence kills it

`oemfake.c` (aarch64 musl, `/data/opt/stock/tmp/oemfake`) binds the same abstract
name, `listen()`s, accepts, and hexdumps/keeps the connection open (sends nothing).
Then gpsd proceeds. Two 32-byte RIL OEM-hook writes arrive:

```
0000001c 3b 000000 01 000000 0d000000 0e37000d 00080000 00112803 01000000
0000001c 3b 000000 02 000000 0d000000 0e37000d 00080000 00112803 00000000
```

(little-endian framing, len 0x1c, cmd 0x3b, sequence 1 then 2). gpsd then:

1. `openat("/sys/class/sec/gps/GPS_PWR_EN/value", O_WRONLY…)` → fd, `write("1\n")`, close
2. `nanosleep(0.250 s)` (GpioDelayMs)
3. `openat("/dev/ttySAC0", 0x20902)` → fd 10 (O_RDWR|O_NOCTTY|O_NONBLOCK + 0x20000)
4. `fcntl64(fd, F_GETFL)` → 0x20802, `fcntl64(fd, F_SETFL, …)` (clears O_NONBLOCK)
5. termios: `ioctl(TCGETS)`, `ioctl(TCFLSH, TCIOFLUSH)`, `ioctl(TCSETS)`,
   `ioctl(TCFLSH)`, `ioctl(TCGETS)` — all return 0 (the 921600-baud termios from
   `gps.xml` is accepted; ioctl numbers 0x5401/0x5402/0x540b)
6. 27 reads on the tty (4, 60 and 268-byte buffers) returning nothing (chip silent)
7. exactly one UART write:

```
nr=4 write fd=10 n=10 hex=fe00fd000055aa00fffc
```

8. more empty tty reads; creates `/cache/gpspipe` and `/cache/glgpsctrl` FIFOs and
   prints `[cat /cache/gpspipe] to stream out NMEA`; exits with status 0 after
   ~2–3 s (no retry loop).

Live fd snapshot (polled every 100 ms until exit) contains:

```
16 /data/opt/stock/dev/ttySAC0      # i.e. /dev/ttySAC0 inside chroot
8  /data/opt/stock/dev/alarm
5  /data/opt/stock/sys/power/wake_lock
6  /data/opt/stock/sys/power/wake_unlock
7  /data/opt/stock/data/system/gps/.gpsd.lock
9  /data/opt/stock/cache/glgpsctrl
13 socket:[…]                        # @Multiclient client
```

So the stock daemon holds the tty open only for ~1–2 s before giving up.

## 6. Observing NMEA / chip answers

- Stock daemon creates `/cache/gpspipe` (inside the chroot: `/opt/stock/cache/gpspipe`)
  and prints `[cat /cache/gpspipe] to stream out NMEA`. If the chip ever answers,
  `cat /opt/stock/cache/gpspipe` (inside the chroot, or host path
  `/data/opt/stock/cache/gpspipe`) is the NMEA sink. `/cache/glgpsctrl` is its
  control FIFO.
- No liblog output was produced: fd tables never contain `/dev/socket/logdw`, and a
  working `/dev/socket/logdw` DGRAM listener (`logdw.c`) received nothing; the
  binary probes `/dev/log/main|radio|events|system` first and its messages go
  through that legacy path (they are lost without a legacy logger).
- `strace` is not installed; syscall evidence above comes from the in-repo-built
  ptrace tracer. `PTRACE_O_TRACECLONE` was not enabled, so only the main thread's
  syscalls were followed.

## 7. Frame verification against the stock binary

`.rodata` file offsets == runtime addresses (PIE linked at 0). Template bytes:

```
0x398b04  ff 00 fd 1f 95 fc                      RequestDebugSfr (6 B)
0x398b0c  fe 00 fd 00 4d 80 00 00 00 fc          (10 B, not in repo daemon)
0x398b18  ff 00 fd 00 4d fc                      (6 B, not in repo daemon)
0x398b20  fe 00 fd 00 4d 40 00 00 00 fc          (10 B, not in repo daemon)
0x398b2c  fe 00 fd 00 00 55 aa 00 ff fc          GetOutOfLegacy (10 B)
0x398b38  ff 00 fd 00 00 fc                      RequestHwVersion (6 B)
0x398b40  fe 00 fd 00 1b 07 00 00 00 fc          (10 B, not in repo daemon)
0x398b4c  ff 00 fd 00 1b fc                      (6 B, not in repo daemon)
0x398b54  ff 00 fd 40 1a fc                      (6 B, not in repo daemon)
```

Disassembly matches the cited functions:

- `Bri_SendAutobaud` @0x2a5ae4: `movs r1,#0x80; movs r2,#0x10; blx memset@0x17af0`
  → 16×0x80 buffer (`bri_autobaud` in the repo daemon).
- `Bri_GetOutOfLegacy` @0x2a618c: mode check `[r5+0x4c][4]==1`, then
  `movs r2,#0xa` and copies 4+4+halfword from literal pool offset +0x3d8
  (= 0x398b2c) → exactly `fe 00 fd 00 00 55 aa 00 ff fc`, then send @0x2a5cd0.
- `Bri_RequestHwVersion` @0x2a6784: `movs r2,#6`, 4 B from +0x3e4 (= 0x398b38)
  + halfword at +4 → `ff 00 fd 00 00 fc`.
- `Bri_RequestDebugSfr` @0x2a6104: 6-byte template at 0x398b04.

Repo daemon (`j7elte-gpsd.c`) implements exactly the four frames above
(autobaud, GetOutOfLegacy, RequestHwVersion, RequestDebugSfr) plus the
`FE..FC` response parser; the five extra 0x398b18/0x398b40-series templates are
not implemented. On the wire, the stock binary sent only
`fe 00 fd 00 00 55 aa 00 ff fc` — byte-identical to
`bri_get_out_of_legacy[]` in the repo daemon. The 16×0x80 burst was not observed
from the traced main thread on this fixed-baud (921600) path; either it is sent
by an untraced clone or skipped when the baud is preconfigured. That is a
difference worth knowing when comparing bring-up logs: a daemon that sends the
autobaud burst first may be one step ahead of what stock does here.

## 8. Reproduce

All helpers are on the phone under `/data/opt/stock/tmp/`:

```
chroottest.sh / chroottest4.sh   # no RIL: block in @Multiclient loop
chroottest7.sh                   # oemfake + gpsd, output/fds
chroottest8.sh                   # oemfake + tracer (full syscall trace)
chroottest10.sh                  # oemfake + gpsd, fd snapshot every 100 ms
tracer.c, tracer                 # aarch64 ptrace strace-lite for ARM32 tracees
oemfake.c, oemfake               # fake abstract @Multiclient server
logdw.c, logdw                   # /dev/socket/logdw DGRAM listener (unused by gpsd)
```

Device-side total: 42.3 MB in `/data/opt/stock`; `/opt/stock` symlink. Nothing
was written to `/boot`, nothing flashed, no kernel rebuild.
