# GPS — public host-side protocol/bring-up for Broadcom BCM475x (web slice)

**Verdict: the chip is not silent because it is broken — it is silent because it is
waiting for the host.** BCM4752 on UART implements a Broadcom boot ("SEC/BRI")
handshake: the host must send a `0x80` autobaud burst followed by a sync frame before
the receiver transmits anything. NMEA does not exist at any stage of that protocol;
NMEA is *synthesised by the vendor daemon* (`/system/bin/gpsd`, Broadcom "glgps") from
binary measurement reports, which the HAL reads over `/dev/socket/gps`.

This report is sourced from public reimplementations and vendor config, plus one
correlation against our own stock artifacts. It contains no Broadcom code and no
firmware bytes.

---

## 1. Sources

| # | Source | What it gives |
|---|---|---|
| S1 | <https://github.com/mbicz/bcm4752-init> — `SPEC.md`, `src/bcm4752_init.c`, `tools/extract_patch.py` | An **independent wire reimplementation** of BCM4752 init on the Lenovo Yoga Book (BCM4752 on `/dev/ttyS2`, vendor daemon `glgps_intelx86android`). Gives exact frames, order, timeouts, termios, patch-transfer algorithm. Verified byte-for-byte against a vendor capture and run on hardware. **Primary source for this report.** |
| S2 | <https://github.com/Yoga-Book/Yoga-Book-GNSS> — `README.md` | Confirms the same chip needs the stock daemon for "autobaud, version and patch protocol"; the receiver "does not emit standard NMEA immediately after opening its UART"; Linux `gpsd` gets NMEA only from a FIFO fed by the vendor daemon. |
| S3 | <https://redmine.replicant.us/projects/replicant/wiki/BCM4751> and <https://redmine.replicant.us/projects/replicant/wiki/BCM4751protocol>; code: <https://git.replicant.us/contrib/PaulK/bcm4751/> | Same obligation on the older BCM4751 (Galaxy S / Nexus S): autobaud → MEIF bootloader → **patch upload** → protocol switch → the daemon translates the second protocol to NMEA over `/dev/socket/gps`. Independent family-level convergence. |
| S4 | <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/devicetree/bindings/gnss/brcm,bcm4751.yaml> | Upstream DT binding for `brcm,bcm4751/4752/4753`. Confirms UART use **requires CTS/RTS**, that `enable-gpios` is the `REGPU`/`NSTANDBY` line, and that there is **no** upstream driver (verified: `drivers/gnss/sirf.c` matches only `fastrax,uc430`, `linx,r4`, `wi2wi,w2sg*`; `drivers/gnss/Kconfig` has only MTK/SiRF/u-blox/USB). |
| S5 | <https://android.googlesource.com/device/samsung/crespo/+/jb-release/gps.xml> | Public Samsung `gps.xml` for a BCM4751 phone: `acPortName`, `lBaudRate="115200"`, `gpioNStdbyPath=/sys/class/sec/gps/GPS_PWR_EN/value`, `gpioNResetPath`, `gpioDelayMs`, `lcsApiSockName=/dev/socket/gps`. Shows the HAL, not the chip, fixes the line rate. |
| S6 | Our own stock artifacts: `/system/etc/gps.xml` (`PortName=/dev/ttySAC0`, `BaudRate="921600"`, `RfType="GL_RF_47520_BRCM_EXT_LNA"`, `GpioNStdbyPath=/sys/class/sec/gps/GPS_PWR_EN/value`, `GpioDelayMs=250`) and `/system/bin/gpsd` strings (`GlMeSrdAsicinit::Bri_SendAutobaud`, `Bri_GetOutOfLegacy`, `Bri_EswLoadBlockAndRequestCrc(0x%08lX, %luB)`, `bri_wait4eswload`, `esw_wait4version`, `No ESW Image to Load, using ROM`) | Proves the same Broadcom SEC/BRI stack, port and 921600 rate apply to j7elte. [INFERENCE from names + config, not from public source] |

Negative results (checked, nothing public):

* Broadcom's Android GPS HAL sources (`BrcmGpsHalGpioSysfs.cpp`, `BrcmGpsHalDownload`,
  `glmesrd_asicinit_eswloader.cpp`, `gll_esw_4752_*`) are **not** published; no leaked
  `glgps` source found. `android_hardware_broadcom_gps` does not exist on GitHub
  (repo search: 0 results). The two `vendor_broadcom_hal_gps` repos are the generic
  AOSP NMEA-over-serial HAL (`gps.c` with `qemu_gpsd`-era NMEA parsing, `power-stub.c`),
  not BCM4752 host code.
* Mainline `gpsd` (esr/ntpsec) has **no** Broadcom driver; the postmarketOS wiki page
  (<https://wiki.postmarketos.org/wiki/Broadcom_BCM4751_GPS>, read through its mirror
  <https://wiki.nura.eco/wiki/Broadcom_BCM4751_GPS>) states the BCM4751 "does not have
  any upstream kernel driver or upstream gpsd GPS driver" and points at Replicant's
  MEIF work.
* Broadcom product pages/PDFs document RF and host *interfaces* (UART/I2C) but no
  byte-level boot protocol.

---

## 2. Transport and power (S1 SPEC §1, [OBSERVED] on hardware)

| item | value |
|---|---|
| line settings used by the vendor daemon | **921600 8N1**, `c_cflag = B921600｜CS8｜CREAD｜CLOCAL｜CRTSCTS`, `c_iflag = c_oflag = c_lflag = 0`, `tcflush()` before/after |
| initial settings before reconfig | 9600 8N1 (`TCGETS`), then `TCSETS` to 921600 |
| baud switching later | none; the handshake never re-programs the rate |
| power sequencing | a real off→on cycle, not a level: the vendor daemon soft-blocks then unblocks the GPS rfkill switch (`RFKILL_TYPE_GPS`, 54 ms apart) and waits ~1 s; the chip answers **nothing** without it |
| vendor config delay | "Power enable delay: %dms" (our `gps.xml`: `GpioDelayMs="250"`) |
| upstream binding | `enable-gpios` = `REGPU`/`NSTANDBY`; `current-speed` example 38400 (ours: 921600); UART mode requires CTS/RTS |

Two independent confirmations that power-cycle, not just an enable level, matters:

* S1: writing `0` to `/sys/class/rfkill/rfkill0/state` "leaves the switch reporting
  `soft=1`, so the receiver stayed powered down and answers nothing"; after the proper
  rfkill off→on cycle "autobaud echo, all four query replies, all four CRC
  acknowledgements and the GLME handshake" arrive.
* S3: on BCM4751 the daemon "will actually order bootup ... when the gps is requested";
  changing `acPortName` prevented the chip from being booted at all.

For j7elte the equivalent is `GPS_PWR_EN` (kernel probe leaves it **low** =
off, `sec_gps_bcm4752.c:57`): write `0`, wait, write `1`, wait ≥250 ms (S1 uses
1000 ms) before touching the UART.

---

## 3. Frame layout (S1 SPEC §2–3, [OBSERVED])

Host → receiver:

```
<soff> 00 fd <opcode> [payload …] fc
```

* `soff` is `0xfe` or `0xff`; both appear in the capture and the choice is not understood.
* **No checksum/CRC and no escaping** on host frames in SEC/BRI mode (raw `0xfc`,
  `0xe0`, `0x00`, `0x01` occur inside patch payloads and are sent unchanged).
* The `%sTXcrc/%sRXcrc/%sTXseq/%sRXseq` counters visible in glgps strings belong to the
  glgps↔HAL **socket** protocol, not to the UART.

Receiver → host:

```
fe 00 fd <opcode> [payload …] fc
```

Every response starts `fe 00 fd`, ends `0xfc`, is usually 10 bytes, and is matched on
`fe 00 fd <opcode>` plus an item/echo byte. **One step at a time**: the next request is
sent only after the previous reply (or timeout).

Opcodes seen: `0x61` write 4-byte item, `0x1f` read item, `0x00` read register,
`0x21` header for autobaud/CRC frames, `0x2f` acknowledge of a received data piece.

---

## 4. The init sequence, in order (S1 SPEC §4)

| # | host bytes | receiver reply | meaning |
|---|---|---|---|
| 1 | `80` × 16, then `fe00fd210001330e10fc` | identical echo `fe00fd210001330e10fc` | **autobaud / line-rate training**; nothing is transmitted before this |
| 2 | `ff00fd0000fc` | `fe00fd000005000230fc` | query register 0; returns `0x30020005` |
| 3 | `ff00fd1f95fc` | `fe00fd1f9500000000fc` | query item `0x95` = 0 |
| 4 | `fe00fd611600000000fc` | *(none)* | write item `0x16` = 0 |
| 5 | `ff00fd1f94fc` | `fe00fd1f9400000000fc` | query item `0x94` = 0 |
| 6 | RAM patch piece `0x7000`/4060 B | `fe00fd2f15b69b07acfc` | patch load, 4-byte ack value |
| 7 | patch piece `0x7fec`/4 B | `fe00fd2f15750000ecfc` | patch load |
| 8 | `fe00fd611601000000fc` | *(none)* | write item `0x16` = 1 |
| 9 | `ff00fd1f94fc` | `fe00fd1f9401000000fc` | readback = 1 |
| 10 | patch piece `0x8000`/4096 B | `fe00fd2f1530b90936fc` | patch load |
| 11 | patch piece `0x9000`/2829 B | `fe00fd2f15444506dffc` | patch load |
| 12 | `fe00fd000055aa00fffc` | `e0000000071002017e063002000550e001` | **switch to GLME mode** |

Whole sequence ≈ 0.7 s at 921600. The 4-byte ack value is produced by the receiver;
its derivation is unknown and S1 does not gate progress on it.

### Patch transfer framing (SPEC §5)

```
remaining = L; sent = 0
while remaining > 0:
    chunk   = min(remaining, 1020)
    aligned = chunk & ~3
    rem     = chunk & 3
    if aligned > 0:
        header: fe (aligned/4) fd 61 17 de c0 <lo(A+sent)> <hi(A+sent)>   # 9 bytes
        data D[sent .. sent+aligned), written in pieces of ≤160 bytes
        fc
    if rem > 0:
        w = (rem + 3) >> 2
        a = (A + sent + aligned + rem - 4) & 0xffff                       # 16-bit wrap
        header: fe w fd 61 17 de c0 <lo(a)> <hi(a)>
        data: last 4 bytes of the piece
        fc
    sent += chunk; remaining -= chunk
verify: fe 00 fd 21 18 <lo(A)> <hi(A)> <lo(L)> <hi(L)> fc
```

Worked example (`0x7000`, 4060 B): four 1020/1020/1020/1000 chunks, headers
`fe ff fd 61 17 de c0 00 70`, `… 00 73 fc`-style address increments, then
`fe 00 fd 21 18 00 70 dc 0f fc`. Chunk 1020 B and write-piece 160 B are observed
constants.

### GLME handoff (SPEC §9)

The switch frame is answered with an `e0 00 … e0 01` frame. Subsequent traffic is
**binary GLME**, not NMEA. GLME defines an escape scheme (`e0 02`=`0xe0`, `e0 03`=`0x00`,
`e0 04`=`0x01`) but escaping was not in effect in the capture.

---

## 5. Ready-to-try sequence for j7elte (no patch needed for step 1)

This is the cheapest decisive test and it directly addresses our ZERO-bytes result:
our previous sweeps never sent the autobaud burst + sync frame at 921600.

```sh
# on the phone, as root
echo 0 > /sys/class/sec/gps/GPS_PWR_EN/value; sleep 1
echo 1 > /sys/class/sec/gps/GPS_PWR_EN/value; sleep 1
```

```python
# python3 on the phone; /dev/ttySAC0 needs root (or group system)
import os, termios, time, select
fd = os.open("/dev/ttySAC0", os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0                      # iflag, oflag, lflag
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL   # add CRTSCTS if CTS is wired
a[4] = a[5] = termios.B921600
termios.tcsetattr(fd, termios.TCSANOW, a); termios.tcflush(fd, termios.TCIOFLUSH)
os.write(fd, b"\x80" * 16); time.sleep(0.05)
os.write(fd, bytes.fromhex("fe00fd210001330e10fc"))
end = time.time() + 1.0
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        d = os.read(fd, 4096); print(d.hex())
        if d and d.hex().startswith("fe00fd21"): break
```

Interpretation:

* **Echo `fe00fd210001330e10fc`** → the chip is powered, clocked, listening at 921600
  and in boot ROM. Continue with steps 2–5 of §4; they need no patch.
* **Silence** → power/clock/UART wiring, not protocol. Check `GPS_PWR_EN` actually
  moves (kernel probe drives it), that UART0 is muxed to `13800000.uart`, and note that
  S1 needed a full off→on **cycle**, not just a high level.
* **Writes block** → `CRTSCTS` without CTS from the chip stalls TX on Linux; retry with
  `CRTSCTS` cleared (our in-repo `j7elte-gpsd` clears it; the vendor capture had it set)
  or assert RTS/DTR (`TIOCMBIS`).

Then, and only then, the patch: without an ESW/patch image the receiver answers the
boot-ROM handlers but will not reach GLME/measurement mode. The patch is Broadcom
firmware shipped **inside the vendor daemon**, so it must be extracted from our own
`/system/bin/gpsd` (`tools/extract_patch.py` in S1 does exactly this for the Yoga
Book unit: 24-byte records `{u8 flags; u32 addr; u64 dataptr; u32 len}` at per-revision
tables, pieces split at 4096 B). No public BCM4752 patch exists and none may be
redistributed; extraction is local-only.

**Do not expect NMEA from the chip.** No public source shows an "enable NMEA" host
command for BCM4752. S1: "after the mode switch the receiver streams GLME frames, and a
separate program has to speak that protocol"; S2: the receiver "does not emit standard
NMEA immediately after opening its UART" and Linux `gpsd` is fed by the vendor daemon;
S3: for BCM4751 the daemon "is in charge of translating the second protocol to standard
NMEA that is sent to the gps.s5pc110.so lib via the /dev/socket/gps Unix socket".
Our own RE slice reached the same conclusion for j7elte (`gps-userspace.md`,
`gps-init-impl.md`).

---

## 6. Cross-family check: BCM4751 (S3)

Replicant's free implementation of the BCM4751 boot protocol (MEIF) documents the same
three obligations and a different frame encoding — useful as corroboration, not as a
byte source for 4752:

* serial setup magic observed on the wire: `termios.c_cflag = 0x800018b2`;
* first protocol is MEIF; a **patch is uploaded** ("Sending the first part of the
  patch... 2054 bytes", ACK/NACK messages) and then the chip is switched to a second,
  undocumented protocol;
* the daemon still owns NMEA: "The GPSD component is in charge of translating the
  second protocol to standard NMEA".
* MEIF frames are `fe 00 fd 40 00 <seq> … <checksum> fc` (12 bytes, with checksum and
  sequence), unlike the SEC/BRI frames above which have neither — so the 4751 MEIF
  header must not be reused on 4752.

---

## 7. Reconciliation with the in-repo daemon

`patches/userspace/gps/0002-userspace-add-j7elte-gpsd-source.patch`
(`src/j7elte-gpsd.c`) already implements:

* 16 × `0x80` autobaud (`bri_autobaud`),
* `fe00fd000055aa00fffc` as **"GetOutOfLegacy"**,
* `ff00fd0000fc` (register query) and `ff00fd1f95fc` (item `0x95` query),
* `FE 00 FD … FC` receive framing with `cmd=buf[3]`, `sub=buf[4]`, no CRC.

Public capture (S1) says:

1. the autobaud **sync/echo** frame is `fe00fd210001330e10fc`, and the receiver's echo
   of it is what proves the link is alive — this frame is missing from our daemon;
2. `fe00fd000055aa00fffc` is the **last** step, the switch from boot mode to GLME
   (S1 SPEC §4 step 12), not the first handshake step.

This is a caution, not a verdict: S1 also states that the vendor daemon's exact frame
order is *not* constant and that an alternative frame layout exists for other ASIC
revisions (`4752_B3`), and our stock binary logs
`Bri_EswLoadBlockAndRequestCrc`/`esw_wait4version`, so our glgps build may order the
same frames differently. Recommended reading of the evidence:

* treat `fe00fd210001330e10fc` as the **first probe** (cheap, echoed, no state change);
* treat `fe00fd000055aa00fffc` as the **GLME switch**, to be sent only after the patch
  is loaded — sending it early on a patch-less ROM is at best a no-op, at worst it
  leaves the receiver in a mode our daemon does not parse.

---

## 8. Findings summary

1. **Power**: enable `GPS_PWR_EN` with a real 0→1 transition and ≥250 ms settle; a bare
   "already high" level gives a silent chip (S1 hardware-verified cause of silence).
2. **Baud**: 921600 8N1, raw (no echo/translation), optional CRTSCTS; never re-programmed
   during init. The rate is host config (`gps.xml`), not a chip default.
3. **Wake bytes**: none in the NMEA sense. The chip wakes on a `0x80`×16 autobaud burst
   followed by `fe 00 fd 21 00 01 33 0e 10 fc`, which it echoes verbatim.
4. **Host commands needed**: yes — a framed boot handshake, four boot-ROM queries, then
   an addressed RAM-patch upload (`fe <nwords> fd 61 17 de c0 <addr16>` … `fc`), then a
   mode switch. Nothing public documents a pure "power and read NMEA" path for 4752.
5. **NMEA**: not produced by the chip; GLME binary after patch, translated to NMEA by
   the vendor daemon (all three public sources + our RE agree). A Linux port therefore
   needs either GLME decoding or a full BRI+patch implementation plus measurement
   parsing.
6. **Missing prerequisite for a full port**: the BCM4752 ESW/patch image. Public
   projects extract it from their own vendor `gpsd` at runtime; we must do the same
   (offsets inside our 4,059,464-byte `/system/bin/gpsd` are not yet located — sibling
   slices own that).
