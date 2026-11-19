# Stock glgps UART transport: open / termios / write path

Slice: GpsReTransport. Target: `/tmp/stock_gpsd` (Broadcom glgps, 32-bit ARM,
ET_DYN, Android 23, stripped). Repo context: userspace daemon
`j7elte-gpsd` from `patches/userspace/gps/0002-userspace-add-j7elte-gpsd-source.patch`.

Method: capstone 5.0.7, Thumb-2, linear disassembly of `.text`
(vaddr `0x18990`, size `0x2c510e`). In this ELF the single `R E` LOAD segment
has `p_offset == p_vaddr == 0`, so **function address == file offset** for
everything below. Function starts were validated against `.ARM.exidx`
(`0x2ddaa0`); every address quoted as a function entry is an exidx entry.
String references were resolved through the binary's `ldr rX,[pc,#d] ;
add rX, pc` PC-relative-delta idiom (literals are deltas, not absolute
addresses - that is why a naive pointer scan finds nothing).

Analysis scripts: `/tmp/j7repo/code/gps/analysis/{xrefs.py,disasm.py}`.

---

## 1. Where the port name comes from

`/dev/ttySAC0` is **not** a string in the binary (no `/dev/tty`, `ttySAC` hit
anywhere in `.rodata`/`.data`). The path is runtime configuration:

* `HalPlatf` parameter table at **file 0x3d9160 / vaddr 0x3da160** (`.data`, 20-byte records:
  `{param_name, member_name, type_char, 0, size}`):
  * `PortName` -> `acPortName` (type `s`, size 0x40)  @vaddr 0x3da160
  * `BaudRate` -> `lBaudRate` (type `l`, size 4)      @vaddr 0x3da174
  * `FlowControl` -> `eFlowControl`                  (table continues)
  * `SwFlowControlXon/Off` -> `ucSwFlowControlXon/Off`
* Second, save/restore-oriented name/member table at **file 0x3ddf50 / vaddr 0x3def50** repeats the
  same pairs (`PortName`/`acPortName`, `BaudRate`/`lBaudRate`, ...).
* Default config path literal `/vendor/etc/gps.xml` @0x2f0570; pointers to it
  live in `.data` at file 0x3deed4/0x3deed8 (vaddr 0x3dfed4/0x3dfed8).

The actually shipped config is in the stock system image
(`.../j7stock/system.raw`, ext4 inode 725, path `/etc/gps.xml` i.e.
`/system/etc/gps.xml` at runtime):

```xml
<hal PortName="/dev/ttySAC0"
     BaudRate="921600"
     GpioNStdbyPath="/sys/class/sec/gps/GPS_PWR_EN/value"
     GpioDelayMs="250"
     NvStorageDir="/data/system/gps/" ... />
```

No `FlowControl` attribute is set, so the stock transport runs with flow
control 0 (none).

---

## 2. Transport functions (address = file offset)

| addr | role | evidence |
|---|---|---|
| 0x43dc0 | `BrcmGpsHalCom` ctor: `this+4 = -1` (INVALID_HANDLE_VALUE), zeroes tx ring at `this+0x114`, config fields at `this+0x6124..0x6148` | writes `str r2,[r4,#4]` with r2=-1 |
| 0x43e30 | function-local-static singleton accessor (`__cxa_guard_acquire/release`), calls ctor 0x43dc0 | 0x43e44/0x43e58 blx 0x17a84/0x17a90 |
| **0x43e80** | **`OpenTTY`**: open + termios + baud + flush + readback; no data write | log `"%s(\"%s\",%ld)\n"` with `"OpenTTY"`, `"Open error %d %s"`, `"%s(GPS: OPEN: %d)\n"` |
| **0x44298** | **`Com::Open`**: takes path from member pointer `[this+0x612c]`, baud via config getter `0x4370c(this+0x6134)`, `strcmp(...,"null")`, `strtok_r(...,":")`, `strcmp(...,"tcp")`; TCP branch logs `"asic com thru tcp\n"`, TTY branch logs `"asic com thru TTY\n"` then calls `OpenTTY` | strings 0x2fe550/0x2fe554/0x2fe568 |
| **0x4441c** | **`Com::Write`**: drains tx ring `this+0x114` into the BSS staging buffer at vaddr `0x495ba8` (cap 0x6000), then `write(fd,...)` | `write@plt` @ **0x44446**; fmt `"write(fd=%d, %d) returned %d\n"` @0x2fe5ac |
| **0x44524** | **`Com::Read`**: `read(fd, this+8, 0x10c)` then invokes rx callback `[this+0x6124]` | `read@plt` @ **0x44544** |
| **0x43a58** | **`Com::Close`**: logs `"Close"`, dumps `TIOCGICOUNT`(0x545d), `FIONREAD`(0x541b), `TIOCOUTQ`(0x5411), `TIOCMGET`(0x5415), then `close(fd)`, `this+4 = -1` | `close@plt` @0x43c16, fmt `"%s(GPS: CLOSE: %d)\n"` |
| 0x2a599c | raw transport write: optional 0xE0 escaping when `[this+0x10e]!=0`, then vtable call `[obj]+8` with (buf,len) @0x2a5a88 | checks `ldrb r3,[r6,#0x10e]` |
| **0x2a5ae4** | **`Bri_SendAutobaud`**: `memset(sp+4,0x80,16)`, sends 16 bytes via 0x2a599c | log `"GlMeSrdAsicinit::Bri_SendAutobaud()\n"` |
| 0x2a5b58 | asic-init stage entry ("cfg + autobaud" state) | log strings `"initial"`, stage switch |
| 0x2a5cd0 | framed-send helper used by all BRI request functions | called from 0x2a6150/0x2a61ee/0x2a67d0/... |
| **0x2a618c** | `Bri_GetOutOfLegacy` | log `"GlMeSrdAsicinit::Bri_GetOutOfLegacy()\n"`, template 0x398b2c |
| **0x2a6784** | `Bri_RequestHwVersion` | log `"GlMeSrdAsicinit::Bri_RequestHwVersion()\n"`, template 0x398b38 |
| **0x2a6104** | `Bri_RequestDebugSfr` | log `"GlMeSrdAsicinit::Bri_RequestDebugSfr()\n"`, template 0x398b04 |

`Com::Open`/`OpenTTY` are also reachable via the vtable entry
`0x44299` stored in `.data.rel.ro` at **file 0x3c6a20 / vaddr 0x3c7a20**; the
BRI autobaud path takes the address of the init stage from the GOT slot at
**file 0x3d89d8 / vaddr 0x3d99d8** (value `0x2a5b59`).

The fd is stored at `this+4`; `-1` is the invalid value (assert string
`"Condition [iPortFd == INVALID_HANDLE_VALUE] failed"` @0x2fe51c).

---

## 3. Exact open/termios sequence (`OpenTTY` @0x43e80)

Arguments: `r0=this`, `r1=port path`, `r2=numeric baud`.

1. Log `OpenTTY("%s",%ld)` (level 3).
2. Map numeric baud -> termios speed constant (see table below). Unknown
   value: log `"Error identifying port speed.\n"` (0x2fe394), return 0.
3. `open(path, 0x902)` = `O_RDWR|O_NOCTTY|O_NONBLOCK` @0x43f0a.
   Failure: log `"Open error %d %s\n"`, return 0.
4. Zero 36 bytes of termios on stack (sp+8), then `tcgetattr(fd,&tio)` @0x43f36.
5. Raw-8N1 fixups (tcflag words at sp+8/0xc/0x10/0x14):
   * `c_iflag &= ~0x5eb`  (IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON)
   * `c_oflag &= ~0x1`    (OPOST)
   * `c_cflag = (c_cflag & ~0x530) | 0x8b0`
     (`0x8b0` = CLOCAL|CREAD|CS8, clears HUPCL|PARENB|CSIZE)
   * `c_lflag &= ~0x804b` (ISIG|ICANON|ECHO|ECHONL|IEXTEN)
   * `c_cc[VTIME]=1`, `c_cc[VMIN]=0` (byte at sp+0x1e := 1, sp+0x1f := 0)
6. Flow control from `getter(this+0x613c)` (returns 0/1/2):
   * 1 -> `c_cflag |= 0x80000000` (CRTSCTS)
   * 2 -> `c_cflag |= 0x1400`, `c_cc[VSTART]=[this+0x6144]`, `c_cc[VSTOP]=[this+0x6148]`
   * 0 (shipped config) -> nothing.
7. `cfsetispeed(&tio,speed)` @0x43f96, `cfsetospeed(&tio,speed)` @0x43f9e.
8. `tcflush(fd, TCIFLUSH=2)` @0x43fa6 **before** `tcsetattr`.
9. `tcsetattr(fd, TCSANOW=0, &tio)` @0x43fb0.
10. `tcflush(fd, TCIFLUSH=2)` @0x43fbe again.
11. `tcgetattr(fd,&tio)` readback @0x43fc6; failure logs
    `"Error reading back the port configuration.\n"` and aborts.
12. `this+4 = fd`; log `OpenTTY(GPS: OPEN: %d)`; return 1.

There is **no `write(2)` and no `read(2)` anywhere in 0x43e80 or in
`Com::Open` 0x44298** (verified by a complete `bl/blx` scan of `.text`: the
first `write@plt` in this subsystem is at 0x44446, the first `read@plt` at
0x44544).

### Baud table (numeric -> Bxxx, branch sites)

| baud | const | site |
|---|---|---|
| 9600 | 0xd | 0x4404c |
| 19200 | 0xe | 0x44166 |
| 38400 | 0xf | 0x44160 |
| 57600 | 0x1001 | 0x44142 |
| 115200 | 0x1002 | 0x4405e |
| 230400 | 0x1003 | 0x4415a |
| 460800 | 0x1004 | 0x44154 |
| **921600** | **0x1007 (B921600)** | **0x4414e** |
| 1000000 | 0x1008 | 0x440b6 |
| 1152000 | 0x1009 | 0x44148 |
| 1500000 | 0x100a | 0x4413c |
| 2000000 | 0x100b | 0x44058 |
| 2500000 | 0x100c | 0x4412c |
| 3000000 | 0x100d | 0x44052 |
| 3500000 | 0x100e | 0x44136 |
| 4000000 | 0x100f | 0x43efc |

(921600 is the shipped `BaudRate`; the branch is `cmp r4,#0xe1000` @0x43eb2.)

---

## 4. Does it write before the first read? YES

**The first UART I/O after the port is opened is a write, not a read.**
Nothing is written by `OpenTTY`/`Com::Open`. The first bytes put on the wire
are the BCM4752 autobaud burst:

* Function `Bri_SendAutobaud` @ **0x2a5ae4**:
  * `0x2a5b02 movs r1,#0x80`
  * `0x2a5b04 movs r2,#0x10`
  * `0x2a5b08 bl memset@plt` with `r0 = sp+4`
  * `0x2a5b0c ldr r3,[r5,#0x4c]` / `0x2a5b10 ldr r3,[r3,#4]`
  * `0x2a5b12 mov r1,r0` (buf) / `0x2a5b0e movs r2,#0x10` (len) /
    `0x2a5b14 mov r0,r5` / `0x2a5b16 bl 0x2a599c` (transport write)
* Constant buffer: **16 bytes `0x80`** (not stored in .rodata; generated by
  `memset` into a stack buffer, sent as one 16-byte write).
* The write reaches `Com::Write` (0x4441c) -> `write(2)` @ **0x44446**.
* Only after this do the BRI receive/timeout stages run
  (`bri_wait4bri`, 300 ms timer: `mov.w r2,#0x12c` @0x2a1786).

Order of the cold-start exchange: `initial` state ctor @0x2a1728 ->
autobaud stage @0x2a5b58 -> BRI wait; then
`Bri_GetOutOfLegacy` (0x2a618c, retry loop at 0x2a65e4, backoff base 0x32=50 ms
plus per-trial increment, `"Resending Bri_GetOutOfLegacy %u!\n"` @0x34ca46),
`Bri_RequestHwVersion` (0x2a6784, `"Got BRI HW version(0x%lx)\n"` @0x34cb59),
`Bri_RequestDebugSfr` (0x2a6104, 500 ms timer `mov.w r6,#0x1f4` @0x2a62ec).

BRI sends are gated on transport mode: at 0x2a61a6 `[this+0x4c]` -> `+4`
must be 1, otherwise the function logs and returns without sending.
Escaping (`0xE0` escape byte) is compiled in at 0x2a599c but disabled when
`[this+0x10e] == 0`.

### Constant byte buffers in `.rodata` (BRI template block 0x398afa..0x398b5a)

| addr | hex | len | used by |
|---|---|---|---|
| 0x398afa | `ff 00 fd 1f 94 fc` | 6 | (DebugSfr variant, sub 0x94) |
| **0x398b04** | `ff 00 fd 1f 95 fc` | 6 | `Bri_RequestDebugSfr` @0x2a6104 (offset +0x3b0) |
| 0x398b0c | `fe 00 fd 00 4d 80 00 00 00 fc` | 10 | (flow-control program stage) |
| 0x398b1c | `ff 00 fd 00 4d fc` | 6 | (flow-control program stage) |
| 0x398b20 | `fe 00 fd 00 4d 40 00 00 00 fc` | 10 | (flow-control program stage) |
| **0x398b2c** | `fe 00 fd 00 00 55 aa 00 ff fc` | 10 | `Bri_GetOutOfLegacy` @0x2a618c (offset +0x3d8) |
| **0x398b38** | `ff 00 fd 00 00 fc` | 6 | `Bri_RequestHwVersion` @0x2a6784 (offset +0x3e4) |
| 0x398b40 | `fe 00 fd 00 1b 07 00 00 00 fc` | 10 | later ESW/patch stages |
| 0x398b4c | `ff 00 fd 00 1b fc` | 6 | later ESW/patch stages |
| 0x398b54 | `ff 00 fd 40 1a fc` | 6 | later stage (cmd 0x40) |

Frame grammar: `0xFF 00 FD <cmd> <arg> FC` (6 B) and
`0xFE 00 FD <cmd> <sub> <d0..d3> FC` (10 B); `0xFC` is the terminator,
no CRC/sequence on this link.

---

## 5. Verification of the userspace `j7elte-gpsd` daemon

Source: `patches/userspace/gps/0002-userspace-add-j7elte-gpsd-source.patch`
(`src/j7elte-gpsd.c`), package in `0001-pmaports-add-j7elte-gpsd-package.patch`.

**Frames implemented by the daemon (exact byte comparison):**

| daemon array | bytes | stock match |
|---|---|---|
| `bri_autobaud` | 16 x `0x80` | matches stock `memset(buf,0x80,16)` @0x2a5b02-0x2a5b08 |
| `bri_get_out_of_legacy` | `fe 00 fd 00 00 55 aa 00 ff fc` | **exact**, .rodata 0x398b2c |
| `bri_request_hw_version` | `ff 00 fd 00 00 fc` | **exact**, .rodata 0x398b38 |
| `bri_request_debug_sfr` | `ff 00 fd 1f 95 fc` | **exact**, .rodata 0x398b04 |

All four are byte-identical to the stock templates above, including frame
shape, terminator and command bytes. The 0xFE parser in the daemon
(header `FE 00 FD`, terminate on `FC`, `cmd=buf[3]`, `sub=buf[4]`) matches the
stock parser `GlMeSrdAsicinit::Bri_CheckForReceivedPacket_Sardine` @0x2a789c.
Timing constants cited by the daemon were spot-checked in the binary:
300 ms (`0x12c` @0x2a1786), 500 ms (`0x1f4` @0x2a62ec), retry loop calling
0x2a618c @0x2a65e4 with 50 ms base backoff (`adds r6,#0x32` @0x2a6600).

**Transport config in the daemon vs stock:**

* device `/dev/ttySAC0`, baud 921600, no flow control: matches the stock
  `gps.xml` found in the image.
* `open(O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC)` vs stock `0x902`
  (`O_RDWR|O_NOCTTY|O_NONBLOCK`): `O_CLOEXEC` is an addition, harmless.
* `cfmakeraw()` + `CLOCAL|CREAD` + `CS8`, `~CRTSCTS`, `~PARENB|~CSTOPB`,
  `cfsetispeed/cfsetospeed(B921600)`, `tcsetattr(TCSANOW)`:
  equivalent to stock step 5-9 (stock's `0x5eb/0x530/0x8b0/0x804b` masks are
  the raw-8N1 set; stock does not clear `CSTOPB` explicitly but the inherited
  termios has it clear).
* Minor deltas worth aligning: stock does `tcflush(TCIFLUSH)` **before and
  after** `tcsetattr`; daemon only does `TCIOFLUSH` after. Stock sets
  `VTIME=1, VMIN=0`; daemon sets both 0 (irrelevant with `O_NONBLOCK`).
* Stock sends the autobaud burst as one 16-byte write; daemon does the same
  (`sizeof(bri_autobaud)`).

**Gaps vs stock (not needed for the first exchange, but known):**

* Stock contains six more BRI templates (0x398b0c/0x398b1c/0x398b20/
  0x398b40/0x398b4c/0x398b54) for the flow-control, ESW/patch and
  `cmd 0x40` stages. The daemon deliberately stops after DebugSfr.
* Stock's escape path (`0x2a599c`, escape byte `0xE0`) is not implemented in
  the daemon; the four frames the daemon sends contain no `0xE0`, and the
  stock path disables escaping when `[this+0x10e]==0`, so this is equivalent
  for the implemented exchange.
* Stock baud table has no 500000/576000 entries; daemon supports them
  conditionally. Irrelevant at 921600.

**Bottom line for the bring-up:** the daemon's first action on the wire is
correct and matches the stock binary exactly - open at 921600 8N1 no flow
control, then a single 16-byte `0x80` autobaud burst, then
`FE 00 FD 00 00 55 AA 00 FF FC`, `FF 00 FD 00 00 FC`,
`FF 00 FD 1F 95 FC` with 300/500 ms waits. If the chip stays silent on the
device, the transport bytes are not the problem; the remaining variables are
power sequencing state, UART pin-mux/clock state and the chip's boot mode.
