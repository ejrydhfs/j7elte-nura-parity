# GPS userspace — j7elte (BCM4752): `j7elte-gpsd`

**Verdict: PARTIAL.** The transport is implemented, packaged and tested, and
the first half of the chip's proprietary init exchange is now implemented
byte-for-byte: `--init` sends the autobaud burst and the
`Bri_GetOutOfLegacy` / `Bri_RequestHwVersion` / `Bri_RequestDebugSfr` frames
recovered from `/system/bin/gpsd`, with the vendor's timeouts, retries and
response decoding. The sequence stops at `Bri_ProgramSwFlowControl`, which
*searches* for a working escape-byte pair instead of emitting a constant
frame, so it cannot be derived without hardware. Full evidence, address map
and protocol table: `parity/reports/gps-init-impl.md`.

Without `--init` the daemon still works as a NMEA forwarder and as a raw UART
capture tool.

Scope: userspace only. No kernel file, no shared pmaports file and no DT was
touched. No hardware was available: everything below is cross-compilation,
binary/string analysis and an off-target functional test under `qemu-aarch64`.

Kernel side (unchanged, already at parity — see `parity/reports/gps.md`):

| Fact | Evidence |
|---|---|
| power GPIO exported as `/sys/class/sec/gps/GPS_PWR_EN/value` | `drivers/gps/sec_gps_bcm4752.c:34` (`sec_device_create(NULL,"gps")`), `:57` (`gpio_direction_output(…, 0)`), `:58-59` (`gpio_export` + `gpio_export_link(gps_dev, "GPS_PWR_EN", …)`) |
| chip is *off* after probe (output low) | `sec_gps_bcm4752.c:57` |
| `check_gps_op()` reports the pin level | `sec_gps_bcm4752.c:16-25` |
| UART is `uart0` → `ttySAC0`, console stays on `ttySAC1` | `parity/reports/gps.md` §P2 |
| no reset line, no regulator, no clock, no firmware | `parity/reports/gps.md` §P2/P4; `gps.xml` has `GpioNResetPath` unset |

## 1. What the vendor userspace does (evidence)

`/system/etc/gps.xml` (extracted from `system.raw` with
`debugfs -R 'dump /etc/gps.xml'`; note the ext4 root *is* `/system`):

```
PortName="/dev/ttySAC0"          BaudRate="921600"
GpioNStdbyPath="/sys/class/sec/gps/GPS_PWR_EN/value"   GpioDelayMs="250"
NvStorageDir="/data/system/gps/"  LtoDir="/data/system/gps/"
RfType="GL_RF_47520_BRCM_EXT_LNA"
FrqPlan="FRQ_PLAN_26MHZ_2PPM_26MHZ_300PPB"
```

Service definition (`boot_out/rd/init.samsungexynos7580.rc:454`):

```
service gpsd /system/bin/gpsd -c /system/etc/gps.xml
    class main
    user gps
    group system inet net_raw
```

and the permissions the ROM grants (`:332-345`): `/dev/ttySAC0` `root:system
0660`, `/sys/class/sec/gps/GPS_PWR_EN/value` `root:system 0664`,
`/data/system/gps` writable by `system`.

`/system/bin/gpsd` (4,059,464 B, `ELF 32-bit LSB …, ARM, EABI5 …, for Android
23`) is Broadcom's **glgps**. `strings` shows a complete measurement-engine and
ASIC-control stack, not an NMEA parser:

* ASIC bring-up state machine — `GlDineCtrl::TurnAsicOn()`,
  `GlDineCtrl::AsicToStandby()`,
  `GlMeSrdAsicinit::Bri_SendAutobaud()`,
  `GlMeSrdAsicinit::Bri_GetOutOfLegacy()`,
  `GlMeSrdAsicinit::Bri_RequestHwVersion()`,
  `GlMeSrdAsicinit::Bri_RequestDebugSfr()`,
  `GlMeSrdAsicinit::Bri_EswLoadChangeBankAndRequestCheck(bank %u)`,
  `GlMeSrdAsicinit::Starting Patch loading: %s`,
  `GlMeSrdAsicinit::Starting ESW loading: %s`,
  `GlMeSrdAsicinit::No ESW Image to Load, using ROM`,
  `GlMeSrdAsicinit::ProgramRfFrequencyPlan()`,
  `Abnormal GlEngine stop. ASIC (%lu) communication failure during stage "%s"`;
* RF/clock/measurement programming — `GlDineCtrl::SetRfType(%d)`,
  `SetRefClockParams`, `SetMeasConfig`, `SetPMMSettings`, `Set1HzPhaseMs`,
  `MeasureCNTIN`, `ArmSYNCIN`;
* a serial link with CRC and sequence numbers — `Serial_Errors:`,
  `%sTXcrc %d`, `%sRXcrc %d`, `%sTXseq %d`, `%sRXseq %d`, `%sRXmal %d`,
  `GpsHalSmProtocol(…)`, `GpsHalNrProtocol(…)`, log facilities `LOG_GLLIO`,
  `LOG_RAWDATA`, `LOG_ASIC_IO`;
* NMEA is **produced by the daemon** from measurements:
  `GlEngineImplStd::GetNextNmeaSentence`, `HasNextNmeaSentence`,
  `ERROR: Couldn't generate NMEA sentence for BRM,START`, and the Broadcom
  report family `$PGLOR,<rate>,…` (`FIX`, `SAT`, `SVD`, `STA`, `NVM`, `PWR`,
  `SIO`, `PFM`, …). `$pglirm` is the daemon's *own* debug-command input.

The Android HAL confirms the direction of the NMEA flow: `/system/lib/hw/gps.default.so`
contains `broadcom::GpsiClient::ReadConfigFromGpsSocket`,
`marshal_gps_nmea_cb`, `GPS Socket Client: Error in connect(%d, %s)`, and the
paths `/dev/socket/gps` and `.gps.interface.pipe.to_gpsd`. The HAL is a socket
client of glgps; it never opens `ttySAC0`. Therefore **NMEA originates in the
daemon, not in the chip.**

### Conclusion on the chip's default behaviour

The chip does **not** free-run NMEA after `GPS_PWR_EN` goes high. The evidence
is the ASIC-init path above: the daemon has to send an autobaud pattern
(`Bri_SendAutobaud`), pull the firmware out of its ROM/legacy boot mode
(`Bri_GetOutOfLegacy`), read the hardware version, optionally load an
ESW/patch image, program the RF/clock plan, and only then does the measurement
engine produce data that glgps converts to NMEA. None of that is NMEA.

`921600` is not an NMEA default either, and the ROM's own serial layer reports
link errors per *frame* (`TXcrc`/`RXcrc`/`RXseq`), i.e. a binary framed
protocol.

## 2. The exact unknowns (why this is PARTIAL)

Resolved since the first revision of this report (see
`parity/reports/gps-init-impl.md` for the address-level evidence):

* **Autobaud / legacy-exit pattern** — *resolved*. `Bri_SendAutobaud`
  (`0x2a5ae4`) is `memset(buf, 0x80, 16)` plus one transmit; the legacy exit
  is the 10-byte frame `fe 00 fd 00 00 55 aa 00 ff fc` (template at `.rodata`
  0x398b2c). The earlier search failed because the image addresses `.rodata`
  through PC-relative literals (`ldr rX,[pc]; add rX,pc`), not absolute
  pointers, so no string or table appeared to be referenced at all.
* **Frame format of the proprietary link** — *resolved*. `00 FD` after a
  `FE`/`FF` start byte, `0xFC` terminator, no CRC, no sequence number;
  `cmd = buf[3]`, `sub = buf[4]`. The `TXcrc/RXcrc/TXseq/RXseq` counters belong
  to the separate glgps↔HAL socket protocol (`0x169090`), not to the UART.
* **Baud used for the handshake** — `OpenTTY` (`0x43e80`) has exactly one call
  site, and there is no other termios call in the image, so the handshake runs
  at `gps.xml`'s `BaudRate="921600"` and is never re-programmed.

Still open, and why the sequence stops where it does:

1. **Flow-control programming.** `Bri_ProgramSwFlowControl` (`0x2a6aa0`) and
   `Bri_ProgramHwFlowControl` (`0x2a6cfc`) do not emit a constant frame: they
   walk candidate escape-byte pairs for `this+0x10c`/`+0x10d`, reject
   candidates that are `0xE0`, `0x21`, `<5`, `>0xFB` or collide, transmit and
   retry until the chip ACKs. The accepted bytes depend on the running chip,
   so they cannot be lifted statically.
2. **Whether the chip transmits anything at all before the handshake.** Still
   hardware-only; `--init --verbose --raw-log` answers it on a device.
3. **ESW/patch image.** glgps contains "Patch loading"/"ESW loading" code, but
   `system.raw` ships no ESW/patch file (`/vendor/firmware` holds only
   `bcm43438A1_V0051.0230.hcd`, camera/MFC firmware). Whether the ROM path
   (`No ESW Image to Load, using ROM`) is sufficient on this part is unknown.
4. **NMEA is not produced by the chip at all** — glgps synthesises it from
   measurement/ESW reports. Even a complete BRI init yields binary traffic, so
   a measurement decoder would still be needed for a fix.

### Dynamic analysis attempt (blocked, documented)

To capture the real init bytes, `/system/bin/gpsd` was extracted and run under
`qemu-arm` 11.1.2 with the complete ROM `/lib` (535 files, dumped with
`debugfs rdump`), a PTY bound to `/dev/ttySAC0`, a writable
`/sys/class/sec/gps/GPS_PWR_EN/value` and the exact init.rc command line
(`-c /system/etc/gps.xml`).

The loader brings all libraries up, then every bionic binary deadlocks inside
the Android 6 linker's own `g_dl_mutex` (`futex` wait at
`linker+0x1f110`, owner id never matches the TLS tid) before any application
code runs. This reproduces identically for `/system/bin/toybox` and
`/system/bin/sh`, so it is a qemu-user ↔ bionic (Android 6) incompatibility, not a
property of glgps. Patching `should_trace()` and the mutex-owner check in the
extracted linker moved the failure, but did not yield a running daemon. No
UART traffic could therefore be observed; the attempt is recorded here so the
next person does not repeat it blindly.

## 3. What was implemented

`parity/userspace/gps/` — `j7elte-gpsd`, a dependency-free C daemon
(1,114 lines, POSIX + Linux):

1. **Power sequencing** — writes `1` to the sysfs attribute exported by
   `sec_gps_bcm4752.c:59`, sleeps `GpioDelayMs` (default 250 ms, from
   `gps.xml`), and writes `0` on every exit path (including SIGTERM/SIGINT/
   SIGHUP). `--no-power` disables it for externally-powered bring-up.
2. **UART** — opens `/dev/ttySAC0`, raw 8N1 (`cfmakeraw`, `CLOCAL|CREAD`, no
   `CRTSCTS`, no parity, 1 stop bit, `VMIN=0`/`VTIME=0`), speed default
   921600 (`gps.xml`), re-opens on error/EOF.
3. **`--init`: the recovered BRI handshake** — after the port is open and
   before the receive loop, the daemon sends, with no invented bytes:
   16 × `0x80` (autobaud), `fe 00 fd 00 00 55 aa 00 ff fc`
   (`Bri_GetOutOfLegacy`, retried 4× with 50/70/90/110 ms backoff),
   `ff 00 fd 00 00 fc` (`Bri_RequestHwVersion`) and
   `ff 00 fd 1f 95 fc` (`Bri_RequestDebugSfr`, 500 ms wait).
   Responses are parsed as `FE 00 FD cmd sub d0..d3 FC`; a `cmd==0` reply is
   decoded into the 32-bit HW version, a `sub==0x95` reply acknowledges the
   DebugSfr. The sequence then stops and says so, because the next vendor step
   (`Bri_ProgramSwFlowControl`) is a data-dependent escape-byte search. Every
   byte, timeout and retry is cited to an address in
   `parity/reports/gps-init-impl.md`.
4. **NMEA extraction** — incremental line framing; a sentence is forwarded only
   if it starts with `$` and contains only printable bytes; CRLF is
   normalised; checksums are verified and counted but never used to drop data;
   unterminated binary runs are counted as `non-NMEA bytes discarded`.
5. **Two consumer interfaces**
   * a **PTY**, published at the stable path `/run/j7elte-gpsd/tty`
     (symlink to `/dev/pts/N`). The daemon keeps the slave open itself, so the
     PTY survives a gpsd restart and the first sentences are not lost.
     gpsd wiring: `gpsd -n /run/j7elte-gpsd/tty` (or
     `GPSD_OPTIONS="-n /run/j7elte-gpsd/tty"` in `/etc/conf.d/gpsd`), started
     after `j7elte-gpsd` (the OpenRC unit declares `before gpsd`/`provide gps`).
   * a **Unix stream socket** `/run/j7elte-gpsd/nmea.sock`, one sentence per
     line per client, for consumers that prefer a socket.
6. **Diagnostics** — 30 s stats (`rx`, `nmea`, `bad_cs`, `dropped`,
   `clients`); an explicit 10 s warning when the UART stays silent, pointing
   at `--init`; `--raw-log PATH` appends every
   received byte (the key tool for the hardware test plan); `--probe-baud`
   retries 9600/115200/4800 if the vendor rate is silent.

Deliberate non-goals: no NVRAM/LTO handling (`/data/system/gps` is runtime
state and is meaningless without the binary protocol), no SUPL/LBS (needs the
Broadcom stack), no `gpsd` fork.

## 4. Build and test evidence

Cross-compiled with the session toolchain (glibc aarch64 — the musl aarch64
toolchain is only available inside abuild; the source uses nothing
glibc-specific):

```
$ make CC=toolchains/gcc-arm-10.3-…/bin/aarch64-none-linux-gnu-gcc
aarch64-none-linux-gnu-gcc -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -std=c11 \
    -o j7elte-gpsd src/j7elte-gpsd.c
$ file j7elte-gpsd
ELF 64-bit LSB executable, ARM aarch64, … dynamically linked, with debug_info
```

Zero warnings with `-Wall -Wextra -Wshadow -Wformat=2`.

The APKBUILD's own build/package commands were replayed verbatim in a scratch
`$srcdir`/`$pkgdir`; the package installs as

```
755 /usr/sbin/j7elte-gpsd
644 /etc/j7elte-gpsd.conf
755 /etc/init.d/j7elte-gpsd
644 /usr/lib/systemd/system/j7elte-gpsd.service
```

### Off-target functional test

`tests/smoke_test.py` runs the **aarch64 binary under `qemu-aarch64`** with the
cross sysroot, a host PTY standing in for the UART and a regular file standing
in for the GPIO attribute. Result:

```
PASS: power asserted on start
PASS: first NMEA sentence reached PTY
PASS: second NMEA sentence reached PTY
PASS: binary bytes not forwarded to PTY
PASS: NMEA reached Unix socket
PASS: power deasserted on exit
PASS: stats accounted for dropped binary bytes
PASS: stable PTY link reported
```

The run also exercised the real termios path on a tty (`opened /dev/pts/12 at
921600 8N1, no flow control`) and the PTY publication
(`/run/j7elte-gpsd/tty -> /dev/pts/13`). This proves the plumbing, **not** the
chip.

`tests/init_test.py` does the same for `--init`, with the master side of the
PTY playing the BCM4752 (it answers with the frame shapes recovered from
glgps). It asserts the exact bytes on the wire and the decoding:

```
PASS: autobaud burst is 16 x 0x80
PASS: Bri_GetOutOfLegacy frame
PASS: Bri_RequestHwVersion frame
PASS: Bri_RequestDebugSfr frame
PASS: HW version decoded
PASS: DebugSfr acknowledged
PASS: residual blocker logged
PASS: no invented bytes after DebugSfr
```

`make check` runs both tests. This proves framing, ordering, timeouts and
response decoding — again **not** the chip.

## 5. Hardware test plan (no hardware available here)

Prerequisites: `linux-samsung-j7elte` + this package + `gpsd`.

1. **Build/install**
   `pmbootstrap build j7elte-gpsd && pmbootstrap install`; add
   `j7elte-gpsd` to the device package's `depends` (not done here: shared
   pmaports files are out of scope).
2. **Power sequencing** — `rc-service j7elte-gpsd start` (or
   `systemctl start j7elte-gpsd`), then
   `cat /sys/class/sec/gps/GPS_PWR_EN/value` → `1`; after `stop` → `0`.
   Scope the pad on `gpd2-6` if accessible. `check_gps_op()` already reports
   the same pin to the kernel.
3. **Does the chip answer the handshake?**
   Run `j7elte-gpsd --init --verbose --raw-log /tmp/gps.rx`. The log shows each
   step: `autobaud burst sent`, then either
   `init: chip silent for 300 ms after autobaud` (chip not listening at
   921600, or the burst is not what its boot ROM expects) or
   `init: BRI frame: …` followed by the `Bri_GetOutOfLegacy` /
   `Bri_RequestHwVersion` / `Bri_RequestDebugSfr` exchanges. A successful run
   prints `init: BRI HW version 0x…` and `init: DebugSfr acknowledged`.
   `hexdump -C /tmp/gps.rx` then contains the complete binary dialogue — that
   capture is the missing input for the flow-control step (§2 item 1).
4. **Does the chip say anything at all without `--init`?**
   Run with `--raw-log /tmp/gps.rx --verbose` but no `--init`. After 10 s the
   daemon logs `no UART data …`. `ls -l /tmp/gps.rx`:
   * **non-empty** → the chip does transmit on power-up. `hexdump -C` it,
     capture the first 64 bytes and the baud that produced them
     (`--probe-baud`, or a scope on the TX line). That data is the missing
     evidence for §2 unknowns 1–3.
   * **empty** → confirms the ROM/legacy boot hypothesis: the host has to
     speak first, which is what `--init` now does. If step 3 also failed,
     capture with a logic analyser on the GPS TX/RX pads while running the
     stock Android ROM (`/system/bin/gpsd -c /system/etc/gps.xml`) and compare
     the first host burst with the autobaud/legacy-exit bytes in
     `gps-init-impl.md` §3.1.
5. **NMEA path** — if NMEA ever appears (it does not follow from the BRI
   handshake alone, see `gps-init-impl.md` §6): `gpsd -n /run/j7elte-gpsd/tty`,
   then `gpspipe -r` / `cgps`; expect a fix outdoors within 30–60 s (cold
   start, no NVRAM/LTO). Check `/run/j7elte-gpsd/tty` resolves to the PTY the
   daemon logged.
6. **Socket path** — `socat - UNIX-CONNECT:/run/j7elte-gpsd/nmea.sock` and
   compare with the PTY stream.
7. **Suspend/resume** — `check_gps_op()` is used by the Exynos power-mode code;
   verify that after resume the daemon still receives sentences, otherwise
   restart it (documented fallback: `Restart=on-failure`/`respawn`).
8. **UART pinmux** — if no bytes ever appear even on the scope, confirm the
   `uart0` pin group is muxed (`gpb0-4..7`, func 2) and that `ttySAC0` is not
   claimed by a console.

## 6. Artifacts

| Path | Role |
|---|---|
| `parity/userspace/gps/src/j7elte-gpsd.c` | daemon source |
| `parity/userspace/gps/Makefile` | build (`CC` overridable) |
| `parity/userspace/gps/tests/smoke_test.py` | off-target receive-path test (qemu-aarch64) |
| `parity/userspace/gps/tests/init_test.py` | off-target `--init` test (qemu-aarch64, PTY emulates the chip) |
| `parity/reports/gps-init-impl.md` | RE method, address map, protocol table, residual blocker |
| `parity/pmaports/device/archived/j7elte-gpsd/APKBUILD` | package |
| `…/j7elte-gpsd.c` | source copy for `abuild` (flat `$srcdir`) |
| `…/j7elte-gpsd.conf` | default options + gpsd wiring notes |
| `…/j7elte-gpsd.initd` | OpenRC service |
| `…/j7elte-gpsd.service` | systemd unit |
| `parity/patches/userspace/gps/0001-pmaports-add-j7elte-gpsd-package.patch` | package as a patch (applies to `parity/pmaports`) |
| `parity/patches/userspace/gps/0002-userspace-add-j7elte-gpsd-source.patch` | source tree as a patch (applies at the session root) |

Both patches were regenerated after the `--init` work, verified with
`git apply --check` against empty trees, and checked to reproduce the source
tree and the package directory byte-for-byte. The pmaports index was left
untouched (`git add -N` + `reset`).

## 7. Residual risks

1. The daemon forwards NMEA only if the chip produces it, and the chip never
   does: glgps synthesises NMEA from measurement reports. `--init` advances the
   link through the autobaud / legacy-exit / version / DebugSfr exchanges but
   stops at the flow-control step, which is data-dependent. GPS therefore stays
   **non-functional** until (a) that step is captured from a device and (b) a
   measurement-report decoder exists. This is a protocol/reverse-engineering
   blocker, not a packaging one.
2. The 921600 assumption for the handshake comes from `gps.xml` and from the
   single `OpenTTY` call site in glgps, not from a measurement of the chip's
   boot ROM; `--init --raw-log` on hardware settles it.
3. PTY/path assumptions: gpsd opens the symlink at start; if `j7elte-gpsd`
   restarts, gpsd must be restarted too (the PTY number changes) — documented
   in `/etc/j7elte-gpsd.conf`.
4. No suspend/resume or thermal behaviour was tested (no hardware).
5. musl-specific build was not run (no musl aarch64 cross compiler in the
   session); the code uses only POSIX.1-2008 + Linux (`accept4`, `O_CLOEXEC`,
   `posix_openpt`), all present in Alpine/musl.
