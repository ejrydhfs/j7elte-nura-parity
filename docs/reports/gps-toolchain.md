# `gpsboot` helper + `j7elte-gpsd` on hardware: toolchain and BRI bring-up

**Scope.** (a) Provide a scriptable raw-UART helper (`code/gps/gpsboot.c`) with a
working cross/native build route, (b) build the existing `j7elte-gpsd` userspace
bridge for the device, deploy it, run it on the phone, and determine whether
NMEA appears on its PTY or the chip answers its BRI init frames.

**Result.** The chip answers. `j7elte-gpsd --init` now completes the recovered
BCM4752 BRI prologue on real hardware and prints the replies
(HW version `0x30020005`, `DebugSfr` ack `sub=0x95`, NvRam word 7 = 7).
NMEA is still absent: the chip stops at the asic-ready stage that the vendor
reaches with stages we cannot reconstruct statically (escape-pair search and
the ESW image). The earlier "chip is silent" observation was a **sequence bug**,
not dead hardware.

## 1. Target architecture

The task text asked for a static **armv7** binary, but the device userspace is
**aarch64** (Exynos 7580 / Cortex-A53):

| Probe | Result |
|---|---|
| `uname -m` / `uname -r` | `aarch64` / `3.10.108` |
| `cat /etc/apk/arch` | `aarch64` (matches the APKBUILD's `arch="aarch64"`) |
| `readelf -h /bin/busybox`, `/usr/sbin/gcc` | `ELF64`, `AArch64` |
| `gcc -dumpmachine` | `aarch64-alpine-linux-musl` |

`gpsboot.c` contains no arch-specific code, so it still builds unchanged for
armv7 if such a rootfs is ever flashed; no armv7 toolchain exists on the host
and none is needed.

## 2. Toolchain inventory and chosen route

| Candidate | Availability | Verdict |
|---|---|---|
| `armv7-alpine-linux-musleabihf-gcc` (host) | absent | n/a, wrong arch |
| `zig` (host) | absent | not needed |
| `aarch64-linux-gnu-gcc` (host) | present, `-static` works (`/usr/aarch64-linux-gnu/lib/libc.a`) | fallback, verified |
| `qemu-aarch64` (host) | present | off-target smoke runs |
| `gcc` on device | `gcc (Alpine 15.2.0) 15.2.0`, `aarch64-alpine-linux-musl` | **primary route** |
| `python3`, `readelf`, `strip` on device | present | tests/evidence |
| pmaports/`build/` userspace toolchain | none shipped (`build/build.sh` only carries an aarch64 *kernel* gcc on a different mount) | — |

Native device builds are both shortest and most faithful (they link the musl
the target actually runs).

### Build commands

```sh
# gpsboot, on the device
cd /tmp && gcc -static -O2 -Wall -Wextra -o gpsboot gpsboot.c && strip -o gpsboot.strip gpsboot

# j7elte-gpsd, on the device (APKBUILD build() with $CFLAGS =
#   -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -std=c11, plus -static)
gcc -static -O2 -std=c11 -Wall -Wextra -Wshadow -o j7elte-gpsd src/j7elte-gpsd.c

# fallback, on the host (glibc static; runs on the 3.10.108 kernel)
aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -o gpsboot gpsboot.c
```

Both compile with `-Wall -Wextra` (daemon also `-Wshadow`) and no diagnostics;
`gpsboot` additionally passes with `-std=c99 -D_POSIX_C_SOURCE=200809L`.

### Artifact properties

| Binary | Size unstripped | Size stripped | ELF |
|---|---|---|---|
| `gpsboot` (musl, device) | 340 552 B | **67 848 B** | `ELF64`/`AArch64`, `Type: DYN` (static-PIE), **no `INTERP`** |
| `j7elte-gpsd` (musl, device) | 535 736 B | **133 240 B** | `ELF64`/`AArch64`, static-PIE, **no `INTERP`** |
| `gpsboot` (glibc, host cross) | 883 256 B | 597 904 B | `ELF64`/`AArch64`, `static`, GNU/Linux 3.7.0 |

Alpine's gcc emits static-PIE for `-static`; with no `PT_INTERP` there is no
loader dependency.

### Transfer

Sources are pushed over the existing SSH transport, inside the device lock,
base64 over stdin and verified by checksum:

```sh
tar czf - src/j7elte-gpsd.c | base64 -w0 | ssh user@172.16.42.1 \
  'cd /tmp && base64 -d | tar xzf -'
# device md5 == host md5: j7elte-gpsd.c 7ab0e47cd8a65d4b64a3f654fdf64aea
#                         gpsboot.c     bd62f1f3828ce0b14f2e58a71bfa3acd
```

Binaries stay in `/tmp` (tmpfs, so they vanish on reboot; re-run the build
command above to restore). Nothing was installed into `/usr/sbin` and no
package was installed — the boot image was not touched.

## 3. What the chip actually does (the decisive result)

Measured on the phone, `/dev/ttySAC0`, 921600 8N1, no flow control, one open
file descriptor per run, power cycled via `/sys/class/sec/gps/GPS_PWR_EN/value`
(that node is a symlink to `gpio137`; there is no separate nSTBY/reset node):

```
host -> chip                         chip -> host                        when
80 x16 (autobaud burst)              --                                  always ignored alone
fe 00 fd 00 1b 07 00 00 00 fc        --                                  ignored (primes the word)
ff 00 fd 00 1b fc                    fe 00 fd 00 1b 07 00 00 00 fc        ~1 ms
ff 00 fd 40 1a fc                    fe 00 fd 40 1a 00 00 00 00 fc        ~1 ms
ff 00 fd 00 00 fc  (HwVersion)       fe 00 fd 00 00 05 00 02 30 fc        ~1 ms, version 0x30020005
ff 00 fd 1f 95 fc  (DebugSfr)        fe 00 fd 1f 95 00 00 00 00 fc        ~1 ms, ACK sub==0x95
fe 00 fd 00 00 55 aa 00 ff fc (GOL)  --                                  never, in any order
```

Rules the hardware enforces (all reproduced 3+ times):

* **The autobaud burst is mandatory.** Without it every request is answered
  with silence (`gpsboot r40000` after power-on: `R 0`).
* **The chip never announces itself.** Listening for 40-60 s after power-on
  produces zero bytes; the vendor's `bri_wait4bri` 300 ms window never fires on
  this unit.
* **State gating is ordering.** `Bri_RequestHwVersion` sent *before* the
  NvRamWord7 exchange is silently dropped; after it, it answers in ~1 ms.
  `DebugSfr` only gets its `sub==0x95` ACK once HwVersion has been read.
  This matches the vendor code, which asserts `transport->[4] == 1` at
  `0x2a6784` before sending the version request.
* `GetOutOfLegacy` is answered neither before nor after the prologue, and it is
  not needed to reach asic-ready.
* `0x40/0x1a` payload differs by boot: `00 00 00 00` on the run above,
  `02 00 10 a4` on a fresh boot (word `0xa4100002`) — same frame, different data.
* No NMEA, no measurement stream, and no further traffic during a 6 s listen
  after the prologue (independent confirmation, peer `gpsprobe` capture).

### Why it looked dead

The shipped `asic_init()` sent `GetOutOfLegacy` first, then `HwVersion`, then
`DebugSfr`, and it `return -1`ed as soon as the 300 ms post-autobaud announce
window was empty. On this chip that is the one order that never gets a reply,
and the abort meant the frames the chip *does* answer were never sent.

### Cross-slice caveat (honesty note)

An earlier run of the *unpatched* daemon logged 90 bytes of well-formed BRI
frames (`2f/15`, `1f/94`, `00/1b`, `40/1a`). Concurrent peers were driving the
same port with `gpsprobe` (a holder was visible in `/proc/*/fd`), so that
capture is cross-talk from their stimulus and is **not** cited as evidence.
Every result quoted above was taken with a holder check and a dedicated power
cycle; the same prologue was independently reproduced by the peer's `gpsprobe`
(see `docs/reports/gps-bringup.md`).

## 4. On-device execution evidence

`gpsboot` (never opens the tty in `--selftest`):

```
$ ./gpsboot --selftest
selftest: ok (6 argv cases + stdin script)
selftest: tty not opened, no bring-up performed
$ ./gpsboot --help | head -2
usage: gpsboot [-d DEV] [-s BAUD] [-t MS] [-m MAX] [--stdin] OP...
       gpsboot --selftest
```

`j7elte-gpsd`, both test suites executed natively on the device (the tests
normally run the aarch64 binary under `qemu-aarch64`; a one-line shim that
strips `-L <sysroot>` and execs the binary makes them native):

```
$ PATH=/tmp/gpsrepo/bin:$PATH python3 tests/smoke_test.py
PASS: power asserted on start
PASS: first NMEA sentence reached PTY
PASS: second NMEA sentence reached PTY
PASS: binary bytes not forwarded to PTY
PASS: NMEA reached Unix socket
PASS: power deasserted on exit
PASS: stats accounted for dropped binary bytes
PASS: stable PTY link reported
$ PATH=/tmp/gpsrepo/bin:$PATH python3 tests/init_test.py
... 14 checks, ALL PASS ...
```

Hardware run of the patched daemon (`--init --verbose`, root via
`/usr/local/bin/rootshell`, power-delay 250 ms):

```
init: autobaud burst sent (16 x 0x80) at 921600 baud
init: no unsolicited BRI frame within 300 ms (rx state: 0 bytes discarded); sending requests anyway
init: Bri_RequestNvRamWord7 sent
init: no reply to Bri_RequestNvRamWord7 within 300 ms
init: NvRamWord7 read sent
init: reply: fe 00 fd 00 1b 07 00 00 00 fc  (cmd=0x00 sub=0x1b data=07 00 00 00)
init: NvRamWord7 word 7 = 0x00000007
init: status request 0x40/0x1a sent
init: reply: fe 00 fd 40 1a 00 00 00 00 fc  (cmd=0x40 sub=0x1a data=00 00 00 00)
init: status 0x40/0x1a = 0x00000000
init: Bri_RequestHwVersion sent
init: reply: fe 00 fd 00 00 05 00 02 30 fc  (cmd=0x00 sub=0x00 data=05 00 02 30)
init: BRI HW version 0x30020005 (asic type byte 0x00)
init: Bri_RequestDebugSfr sent
init: reply: fe 00 fd 1f 95 00 00 00 00 fc  (cmd=0x1f sub=0x95 data=00 00 00 00)
init: DebugSfr acknowledged
init: Bri_GetOutOfLegacy sent        (x4 trials, 50/70/90/110 ms, no reply)
init: link check: 4 of 9 exchanges answered
```

and at exit: `no NMEA bytes were ever received from /dev/ttySAC0 (40 bytes were
consumed by the BRI handshake)` — 40 bytes = the four answered frames.

`/dev/ttySAC0` is `crw-rw---- root:dialout` and the shell user is not in
`dialout`, so the daemon must run via the root helper:

```sh
/usr/local/bin/rootshell sh -c '/tmp/j7elte-gpsd --init --verbose \
  --device /dev/ttySAC0 --baud 921600 \
  --gpio /sys/class/sec/gps/GPS_PWR_EN/value --power-delay 250 \
  --rundir /run/j7elte-gpsd'
```

## 5. Code changes in this slice

`code/userspace/gps/src/j7elte-gpsd.c` (mirrored to
`code/pmaports/device/archived/j7elte-gpsd/j7elte-gpsd.c`):

* `asic_init()` reordered to the order the hardware answers:
  autobaud -> NvRamWord7 trio -> HwVersion -> DebugSfr -> GetOutOfLegacy
  retries. The vendor's step list and timers are kept; only the order and the
  abort behaviour changed.
* A missing announce frame no longer aborts the handshake.
* New frames `bri_nvram_word7` / `bri_nvram_read` / `bri_request_status`
  (glgps `Bri_RequestNvRamWord7` @0x2a70b8, templates `.rodata`
  0x398b40/4c/54), decoded and logged.
* New helpers `bri_step()` (send + wait + log) and `bri_word()` (LE payload
  decoder); `bri_rx_bytes` accounting so a silent-chip exit is distinguishable
  from "only the handshake spoke".
* `tests/init_test.py` now emulates the real chip (no announce, ignores the
  10-byte selection, answers read/status/HwVersion/DebugSfr in that order) and
  asserts the frames, the decodes, the ACK and `link check: 5 of 6`.
* `APKBUILD` sha512 for `j7elte-gpsd.c` updated (package replay verified:
  `build()` + `package()` install `755 /usr/sbin/j7elte-gpsd`,
  `644 /etc/j7elte-gpsd.conf`, `755 /etc/init.d/j7elte-gpsd`,
  `644 /usr/lib/systemd/system/j7elte-gpsd.service`).
* `patches/userspace/gps/0001-*.patch` and `0002-*.patch` regenerated from the
  updated files (both `git apply --check` clean; applied content md5 matches
  the working tree).

`code/gps/gpsboot.c` is the raw-experiment tool kept as fallback: it opens the
tty raw 8N1, runs a scripted op list from argv or `--stdin`, timestamps every
read, and prints an explicit `R 0: (timeout N ms)` when the chip says nothing
(ops: `w<delay>:<hex>`, `r<timeout>[:<max>]`, `d<ms>`, `f`; options `-d -s -t
-m --selftest --help`). Example that reproduces the prologue:

```sh
/tmp/gpsboot -d /dev/ttySAC0 -s 921600 \
  w0:80808080808080808080808080808080 r400 \
  w0:fe00fd001b07000000fc r400 \
  w0:ff00fd001bfc r400 w0:ff00fd401afc r400 \
  w0:ff00fd0000fc r400 w0:ff00fd1f95fc r400
```

## 6. Remaining blocker (unchanged, now precisely bounded)

After `DebugSfr`/asic-ready the vendor runs
`Bri_ProgramSwFlowControl`/`Bri_ProgramHwFlowControl` (0x2a6aa0/0x2a6cfc),
which *search* for a working escape-byte pair rather than sending fixed bytes,
then loads an ESW patch image and programs RF/clock. Neither the search result
nor the ESW image is available statically, so the chip never leaves BRI mode
and emits no NMEA. Getting NMEA needs either that search reproduced on the wire
(now possible: the link demonstrably works) or the ESW blob from the stock
image.

## 7. Files

| Deliverable | Path |
|---|---|
| Raw UART helper | `code/gps/gpsboot.c` |
| Bridge daemon (patched) | `code/userspace/gps/src/j7elte-gpsd.c` |
| Daemon tests (updated) | `code/userspace/gps/tests/{init,smoke}_test.py` |
| Package | `code/pmaports/device/archived/j7elte-gpsd/` |
| Patches (regenerated) | `patches/userspace/gps/000{1,2}-*.patch` |
| This report | `docs/reports/gps-toolchain.md` |
