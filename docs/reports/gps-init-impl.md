# BCM4752 init implementation in `j7elte-gpsd` (RE + build evidence)

**Verdict: PARTIAL, but the first two handshake exchanges are now byte-exact.**
The autobaud burst and three command frames (`Bri_GetOutOfLegacy`,
`Bri_RequestHwVersion`, `Bri_RequestDebugSfr`) were recovered from
`/system/bin/gpsd` down to the individual bytes and are sent by
`j7elte-gpsd --init`, with the vendor's own timeouts and retry schedule.
The sequence then stops deliberately at the first step whose bytes cannot be
derived statically (`Bri_ProgramSwFlowControl`), because that step *searches*
for a working escape-byte pair instead of emitting a fixed frame.

Everything below is cross-compilation, disassembly and an off-target test with
a PTY emulating the chip under `qemu-aarch64`. No hardware was available, so no
claim is made about what the real BCM4752 answers.

## 1. How the code was located (method)

* Binary: `/system/bin/gpsd` (4,059,464 B, ELF32 ARM EABI5, PIE linked at 0,
  stripped, `.text` @0x18990, `.rodata` @0x2f0570). File offsets equal
  virtual addresses for `.text`/`.rodata`; `.data.rel.ro`/`.got`/`.data` are
  shifted by 0x1000.
* Ghidra 12.1.2 headless (`-processor ARM:LE:32:v7`) gives the function map,
  but **plain pointer scans find no references to any of the log/format
  strings**. The build (Android NDK, `-mword-relocations` off) addresses
  `.rodata` through the Thumb-2 PIC idiom

  ```
  ldr  r3, [pc, #imm]      ; literal = target - (pc_of_add + 4)
  add  r3, pc              ; r3 = absolute target
  ```

  so the literal pool holds a *PC-relative delta*, not an absolute address.
  A scanner for `ldr rX,[pc,#imm]` immediately followed by `add rX, pc`
  recovers 31,384 rodata references and that is what maps every string to its
  function (e.g. `0x34c764 "GlMeSrdAsicinit::Bri_SendAutobaud()"` ← site
  `0x2a5b3a` in `FUN_002a5b58`). Without this step the image looks like it
  contains no GPS strings at all.
* Function boundaries come from `.ARM.exidx` (9,562 entries), which is more
  reliable than Ghidra here: several functions overlap in Ghidra's view, so
  disassembly was done with capstone/objdump (`-M force-thumb -m arm`) over
  the exidx ranges.
* The PLT is 12-byte stubs (`add ip,pc,#0x300000; add ip,ip,#0xNM000;
  ldr pc,[ip,#imm]!`), so GOT slot = `stub + 8 + 0x3c2000 + imm`. This is how
  `0x17af0` was proven to be `memset` (GOT 0x3d9b20) and not `__errno`, which
  matters for reading the autobaud code.

## 2. Function map (all addresses are `/system/bin/gpsd`)

| Function | Address | Notes |
|---|---|---|
| `GlMeSrdAsicinit` stage "initial" | `0x2a1728` | sets next stage = cfg/autobaud (`ldr` of GOT 0x3d99d8 @0x2a173a) |
| cfg + autobaud stage | `0x2a5b58` | escape setup, `Bri_SendAutobaud`, then → `wait4flowcontrol` or `wait4version` |
| `Bri_SendAutobaud` | `0x2a5ae4` | `memset(buf,0x80,16)` → transmit |
| BRI transmit (escaping + write) | `0x2a599c` | escapes `0xE0`→`E0 02`, `[0x10c]`→`E0 04`, `[0x10d]`→`E0 03`; call `this->[0x44].vtable[2]` |
| BRI send wrapper (tx counter) | `0x2a5cd0` | `0x2a599c` + `this->[0x128]++` |
| `Bri_GetOutOfLegacy` | `0x2a618c` | 10-byte template |
| `Bri_RequestHwVersion` | `0x2a6784` | 6-byte template; asserts `transport->[4] == 1` |
| `Bri_RequestDebugSfr` | `0x2a6104` | 6-byte template |
| `Bri_RequestNvRamWord7` | `0x2a70b8` | three templates (10+6+6 B), not sent by `--init` |
| `Bri_CheckForReceivedPacket_Sardine` | `0x2a789c` | receive parser, byte at a time |
| `Bri_EswLoadSetAsicData` (escape decode) | `0x2a599c`/`0x2a5b58` | escape table lives at `this+0x10c/0x10d`, flag at `+0x10e` |
| stage "bri_wait4bri" | `0x2a174c` | 300 ms timer (`0x12c`) |
| stage "bri_wait4asic_ready" | `0x2a6234` | sends DebugSfr, 500 ms timer (`0x1f4`), needs `sub == 0x95` |
| stage "esw_wait4version" (INIT_STAGE A) | `0x2a6508` | resends GetOutOfLegacy, 50+20n ms backoff, ≤4 trials |
| stage "bri_wait4flowcontrol" | `0x2a6dd8` | → `Bri_ProgramSw/HwFlowControl` |
| `Bri_ProgramSwFlowControl` | `0x2a6aa0` | **searches** escape byte pairs — stop point |
| `Bri_ProgramHwFlowControl` | `0x2a6cfc` | idem |
| generic state-step driver | `0x2a1ef0` | calls `this->[0x18]`, stores return into `this->[0x1c]` |
| ASIC state reset | `0x2a1f38` | zeroes 0xe0/0xe1/0x10c..0x10e/0x11e..0x122 |
| transport mode / escape chars | `0x1c9554` | `[0x94]=mode`; `mode != 2` ⇒ escapes `0x11`, `0x13` |
| transport escape query | `0x1c958c` | `esc1=[0x98]`, `esc2=[0x99]`, returns `[0x94]==2` |
| UART open (`OpenTTY`) | `0x43e80` | int baud → `speed_t`, `tcsetattr`; **only call site `0x443d4`**, and no termios call exists anywhere else |

The stage pointers are stored as Thumb function pointers (`|1`) in the GOT at
`0x3d99d8 … 0x3d9a24`; the driver advances through them, which is why a plain
call-graph search finds no edges between the stages.

## 3. Wire protocol (verified)

```
host → chip, 6 B        FF 00 FD <cmd> <arg> FC
host → chip, 10 B       FE 00 FD <cmd> <arg> <d0> <d1> <d2> <d3> FC
chip → host, 10 B       FE 00 FD <cmd> <sub> <d0> <d1> <d2> <d3> FC
```

* `0xFC` is a constant frame terminator. There is **no CRC and no sequence
  number** on this link: the `%sTXcrc/%sRXcrc/%sTXseq/%sRXseq` counters belong
  to the unrelated glgps↔Android-HAL socket protocol implemented around
  `0x169090`, not to the ASIC UART.
* Receiver validation, exactly as in `0x2a789c`: `buf[0]==0xFE`,
  `ldrh(buf[1..2])==0xFD00`, `buf[9]==0xFC`, `len==10`; then `cmd=buf[3]`,
  `sub=buf[4]`. Recognised `cmd/sub` pairs: `0x2F/0x15`, `0x00/*`,
  `0x1F/*`, `0x21/0x00`; anything else logs
  `"unknown command(%u, %u)!"`.
* For `cmd==0` the parser stores `buf[4]` at `this+0x13c` and the little-endian
  32-bit value of `buf[5..8]` at `this+0x140`; that word is what
  `"Got BRI HW version(0x%lx)"` prints.
* Escaping is active only when `transport->[0x94] == 2`
  (`[0x10e] = ([0x94]==2)`); in mode 1 `0x10e == 0` and the transmit path
  copies bytes verbatim. None of the frames below contain `0xE0`, and their
  bytes never collide with the mode-1 escape pair (`0x11`, `0x13`), so the
  wire image is identical in both modes.

### 3.1 Messages recovered

| Message | Bytes | Evidence |
|---|---|---|
| autobaud burst | `80 80 80 80 80 80 80 80 80 80 80 80 80 80 80 80` | `Bri_SendAutobaud` @0x2a5ae4: `memset(buf,0x80,16)` then one transmit |
| `Bri_GetOutOfLegacy` | `fe 00 fd 00 00 55 aa 00 ff fc` | @0x2a618c, template `.rodata` 0x398b2c (`ldr` 0x398b2c/0x398b30 + `ldrh` 0x398b34) |
| `Bri_RequestHwVersion` | `ff 00 fd 00 00 fc` | @0x2a6784, template 0x398b38 (6 B) |
| `Bri_RequestDebugSfr` | `ff 00 fd 1f 95 fc` | @0x2a6104, template 0x398b04 (`0x1ffd00ff`, `0xfc95`) |
| `Bri_RequestNvRamWord7` (recovered, not sent) | `fe 00 fd 00 1b 07 00 00 00 fc`, then `ff 00 fd 00 1b fc`, then `ff 00 fd 40 1a fc` | @0x2a70b8, templates 0x398b40/0x398b4c/0x398b54 |

### 3.2 Ordering and timing

```
initial (0x2a1728)
  └─> cfg/autobaud (0x2a5b58):  16 x 0x80
        ├─ transport mode 1 -> wait4flowcontrol (0x2a6dd8)
        └─ otherwise        -> wait4version     (0x2a680c) -> RequestHwVersion
wait4version -> ... -> wait4asic_ready (0x2a6234) -> RequestDebugSfr, t=500 ms
wait4asic_ready -> esw_wait4version / INIT_STAGE A (0x2a6508)
        -> GetOutOfLegacy, retry <=4, backoff 0x32+20n ms (50/70/90/110 ms),
           fallback timer 1000 ms
```

Timers are taken from the immediates in the stage functions
(`0x12c`=300 ms in `0x2a1786`, `0x1f4`=500 ms in `0x2a62ec`,
`0x3e8`=1000 ms in `0x2a661c`, `0x32+20n` at `0x2a65fe`).

The UART is opened exactly once, at `gps.xml`'s `BaudRate="921600"`, and never
re-programmed (`cfsetispeed`/`cfsetospeed`/`tcsetattr` have a single call site
each, all inside `OpenTTY`). The autobaud burst is therefore sent at 921600,
and the chip's boot code is what adapts.

## 4. What `--init` now does

`parity/userspace/gps/src/j7elte-gpsd.c`, new section
"BCM4752 BRI handshake" plus `asic_init()`:

1. `tcflush(TCIFLUSH)`, then write the 16-byte autobaud burst in one write.
2. Wait ≤300 ms for a `FE 00 FD … FC` frame; log it (or log the silence and
   abort the handshake, keeping the daemon in receive-only mode).
3. Send `Bri_GetOutOfLegacy`, retrying up to 4 times with 50/70/90/110 ms
   waits; log any reply.
4. Send `Bri_RequestHwVersion`; on a `cmd==0` reply decode the 32-bit version
   and the ASIC-type byte exactly like `0x2a7b60` does.
5. Send `Bri_RequestDebugSfr`; accept a reply with `sub==0x95` (the check at
   `0x2a6330`) and log the acknowledgement.
6. Log `init: stopping here.` with the reason, then fall through to the normal
   NMEA receive loop.

No byte is ever written that is not in the table of §3.1. The parser is a
byte-at-a-time state machine that mirrors `0x2a789c` (including resync on a
malformed header), and non-frame bytes are counted, never interpreted.

New option:

```
  -i, --init            send the recovered BCM4752 BRI handshake first
```

Also updated: the header comment (which used to say "deliberately no init
frame"), the 10-second silence warning (now points at `--init`), and
`etc/j7elte-gpsd.conf` (documents enabling `--init`).

## 5. Build and test evidence

```
$ make CC=<session>/toolchains/gcc-arm-10.3-…/bin/aarch64-none-linux-gnu-gcc
aarch64-none-linux-gnu-gcc -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -std=c11 \
    -o j7elte-gpsd src/j7elte-gpsd.c
$ file j7elte-gpsd
ELF 64-bit LSB executable, ARM aarch64, … dynamically linked, with debug_info
```

Zero warnings. The APKBUILD's `build()`/`package()` were replayed verbatim in a
scratch `$srcdir`/`$pkgdir`; the package still installs

```
755 /usr/sbin/j7elte-gpsd
644 /etc/j7elte-gpsd.conf
755 /etc/init.d/j7elte-gpsd
644 /usr/lib/systemd/system/j7elte-gpsd.service
```

and all four `sha512sums` verify. The packaged binary prints the new option:

```
  -i, --init            send the recovered BCM4752 BRI handshake first
```

### Off-target init test

`tests/init_test.py` runs the aarch64 binary under `qemu-aarch64`, with the
master side of a PTY playing the chip. It asserts the exact bytes and the
decoding:

```
PASS: autobaud burst is 16 x 0x80
PASS: Bri_GetOutOfLegacy frame
PASS: Bri_RequestHwVersion frame
PASS: Bri_RequestDebugSfr frame
PASS: HW version decoded
PASS: DebugSfr acknowledged
PASS: residual blocker logged
PASS: no invented bytes after DebugSfr
ALL PASS
```

and `tests/smoke_test.py` (receive path) still passes unchanged. `make check`
runs both. This proves framing, ordering, timeouts and decoding — **not** the
chip.

## 6. Residual blocker (exact)

1. **`Bri_ProgramSwFlowControl` @0x2a6aa0 / `Bri_ProgramHwFlowControl`
   @0x2a6cfc do not emit a constant frame.** They walk candidate escape-byte
   pairs (stored at `this+0x10c`/`+0x10d`), reject pairs whose bytes are
   `0xE0`, `0x21`, `<5` or `>0xFB` or that collide with each other
   (`0x2a6ad2-0x2a6ba8`), transmit, and re-try until the chip ACKs
   (`"… Flow Control Was not Acked properly"`). The bytes that are actually
   accepted depend on the running chip's response, so there is no static
   string to copy. Reproducing it requires either the real chip or a captured
   trace of the stock ROM, i.e. hardware.
2. **ESW/patch images are absent.** `system.raw` contains no ESW blob
   (`/vendor/firmware` has only the Bluetooth HCD, camera and MFC firmware), so
   glgps itself would take the `"No ESW Image to Load, using ROM"` path. Whether
   the ROM path reaches a usable measurement state on this ASIC revision is
   unverified.
3. **Even a complete BRI init does not produce NMEA.** NMEA is synthesised by
   glgps (`GlEngineImplStd::GetNextNmeaSentence` from measurement/ESW reports);
   the chip speaks the binary BRI/ESW protocol. `--init` therefore gets the
   link to the point where such a decoder could be attached — it cannot make
   this daemon output a fix.

## 7. Hardware plan for `--init`

1. `j7elte-gpsd --init --verbose --raw-log /tmp/gps.rx`.
2. If the log shows `init: chip silent for 300 ms after autobaud` and
   `/tmp/gps.rx` is empty, the chip is not listening at 921600 before the
   handshake: scope the GPS RX pad with the stock ROM running and re-measure
   the first host burst (this is the one remaining baud assumption).
3. If frames appear, the log prints `init: BRI frame: …` with the decoded
   `cmd/sub`; `init: BRI HW version 0x…` is the value to compare with
   `"HW Asic Version Not Supported"`'s table (`4752_B2`/`B3`, `47521_A0`,
   `4753_A1/A2`).
4. The first `cmd/sub` that does not match §3 makes the next step obvious: that
   is where the flow-control exchange starts, and a full `--raw-log` of it is
   exactly the missing input for step 1 of §6.

## 8. Artifacts

| Path | Change |
|---|---|
| `parity/userspace/gps/src/j7elte-gpsd.c` | BRI handshake + `--init` (+168 lines) |
| `parity/userspace/gps/tests/init_test.py` | new off-target init test |
| `parity/userspace/gps/Makefile` | `check` runs both tests |
| `parity/pmaports/device/archived/j7elte-gpsd/j7elte-gpsd.c` | synced copy |
| `…/j7elte-gpsd.conf` | documents `--init` |
| `…/APKBUILD` | refreshed `sha512sums` |
| `parity/patches/userspace/gps/0001-pmaports-add-j7elte-gpsd-package.patch` | regenerated, `git apply --check` OK, applies to an empty tree and reproduces the directory exactly |
| `parity/patches/userspace/gps/0002-userspace-add-j7elte-gpsd-source.patch` | regenerated, `git apply --check` OK, reproduces the tree exactly |
