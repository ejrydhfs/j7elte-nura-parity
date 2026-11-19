# GPS GLME stream decode: frames, message table, NMEA synthesiser

Target: BCM4752 (`0x30020005`) on SM-J700M, kernel #33. Tools:
`code/userspace/gps/src/gpsesw.c` (`--glme-stream`), stock
`/system/bin/gpsd` for protocol RE. Device link is **down**
(phone USB shows only the fastboot interface, no RNDIS), so the
live `--glme-stream` run waits for the link; everything else is done
and committed.

Evidence tags: **[OBS]** = observed on the wire (PTY emulator or
hardware); **[HW]** = observed on this phone's chip;
**[INFERENCE]** = reading, not confirmed.

---

## 1. Real captured GLME frames (hex) [OBS]

### 1.1 Banner (chip -> host, live golden run, 17 B) [HW]

```
e0 00 00 00 07 10 02 01 72 06 30 02 00 05 5f e0 01
```

Received after `--golden` + `fe00fd000055aa00fffc` on a cold boot
(exit 0). `30 02 00 05` little-endian is the ASIC id `0x30020005`.

### 1.2 First host config transaction (host -> chip, 271 B) [OBS]

Captured complete from the stock daemon against a PTY emulator
(`gpsemu3.py`, silent after the banner so the daemon retries with
sequence `00/02/04/06`). Raw wire bytes, 225-byte frame:

```
e0 00 00 01 00 c3 00 01 41 cf ec e4 d5 74 fb 12 2b 1f 4b 4b 01 d0 19 d3
00 05 0b 00 0c 3c 42 2a 02 49 00 4b 00 01 00 03 00 00 94 30 21 85 03 1a
64 80 64 80 64 80 64 80 64 80 18 30 0e 00 00 00 00 30 0f 00 00 00 00 30
10 00 00 00 00 30 e0 03 00 00 00 00 30 12 00 00 00 00 30 e0 04 00 00 00
00 30 00 00 00 15 09 30 00 00 00 15 0f 38 01 00 00 00 42 38 02 00 00 00
0e 38 04 00 00 00 26 38 06 00 00 00 3d 38 07 00 00 00 81 38 0b 00 00 00
00 38 10 00 00 00 00 38 e0 03 00 00 00 30 38 12 00 00 00 e6 38 e0 04 00
00 00 8c 38 14 00 00 00 1e 38 1a 00 00 00 06 38 20 00 00 00 4c 38 2b 00
00 00 6b 38 0b 00 00 00 04 38 02 00 00 00 0b 0b 00 02 01 38 20 00 00 00
40 00 00 00 e0 02 ac e0 01
```

plus a 46-byte companion frame (same transaction; full hex in
`gpsesw.c:glme_cfg1` comment and subagent transcript
`history://GpsGlmeDecode.GlmeEmuCapture`). The emulator logged 8
frames total: the 271-byte pair retried 4x (host gets no reply and
gives up, then exits).

Truncated prefixes of the two bigger host frames seen in the older
syscall trace (trace3, fd=16): `e0 00 02 01 ...` (2644 B) and
`e0 00 0a 01 ...` (1410 B). Their full bytes were never captured
(tracer truncated at 64 B); they are listed here as known-unknowns,
not evidence.

### 1.3 Escape decoding (verified on frame 1)

GLME escapes (public bcm4752-init SPEC §9): `e0 02` = `0xe0`,
`e0 03` = `0x00`, `e0 04` = `0x01`. Terminator is raw `e0 01`.
Frame 1: 225 raw -> 216 decoded bytes:

```
off decoded (16/row)
  0: 00 01 00 c3 00 01 41 cf ec e4 d5 74 fb 12 2b 1f
 16: 4b 4b 01 d0 19 d3 00 05 0b 00 0c 3c 42 2a 02 49
 32: 00 4b 00 01 00 03 00 00 94 30 21 85 03 1a 64 80
 48: 64 80 64 80 64 80 64 80 18 30 0e 00 00 00 00 30
 64: 0f 00 00 00 00 30 10 00 00 00 00 30 00 00 00 00
 80: 00 30 12 00 00 00 00 30 01 00 00 00 00 30 00 00
 96: 00 15 09 30 00 00 00 15 0f 38 01 00 00 00 42 38
112: 02 00 00 00 0e 38 04 00 00 00 26 38 06 00 00 00
128: 3d 38 07 00 00 00 81 38 0b 00 00 00 00 38 10 00
144: 00 00 00 38 00 00 00 00 30 38 12 00 00 00 e6 38
160: 01 00 00 00 8c 38 14 00 00 00 1e 38 1a 00 00 00
176: 06 38 20 00 00 00 4c 38 2b 00 00 00 6b 38 0b 00
192: 00 00 04 38 02 00 00 00 0b 0b 00 02 01 38 20 00
208: 00 00 40 00 00 00 e0 ac
```

Note the trailing raw `e0` (`e0 02` -> literal): payloads can carry
raw `0xe0`, so framing splits only on `e0 01`.

---

## 2. Message table (with evidence)

Wire form: `e0 00 <payload> e0 01`. Payload byte 0 is always
`0x00`; byte 1 is the message id.

| id | dir | shape | evidence |
|---|---|---|---|
| `0x00` | chip -> host | version/status, 13-decoded-byte payload `00 00 07 10 02 01 72 06 30 02 00 05 5f`; bytes 6-9 LE = ASIC `0x30020005` | **[HW]** banner + subagent emulator run |
| `0x01` | host -> chip | receiver-config download, 271 raw B (225 + 46 frames); 56-B fixed header then TLV sections; seq byte at payload offset 47 (`00/02/04/06` retries) | **[OBS]** full bytes §1.2 |
| `0x02` | host -> chip | bulk config part, 2644 B | prefix only (trace3 `n=2644`); full bytes not captured |
| `0x0a` | host -> chip | short control, 1410 B (contains an inner `e0 01`, i.e. two frames on the wire) | prefix only (trace3 `n=1410`) |

TLV record shape inside id `0x01` (decoded, verified): the `0x38xx`
family parses as 16 x `[tag u16 LE][value u32 LE]` (tags
`380f/3842/380e/3826/383d/3881/3800/3800/3830/38e6/388c/381e/3806/384c/386b/3804`,
values mostly small). The `0x30xx` family does not parse in the
same 6-byte cadence (duplicate tags) and is dumped, not interpreted.
Chip->host measurement/fix messages: **none observed** (table rows
for them are absent by design, not by omission — see §3).

Emulator bug fixed along the way: the old emulator answered the
banner with `...7e 06 30 02 00 05 50...`; the real banner is
`...72 06 30 02 00 05 5f...`. gpsd validates it and exits otherwise.

---

## 3. NMEA synthesiser (minimal, void fix)

No fix-bearing GLME message has been observed on this unit: the
passively listening chip stays silent after the banner, and the
silent emulator only elicits retries of the id-`0x01` config. So
there is **no chip fix data to convert yet**. `--glme-stream`
therefore emits valid-but-void sentences proving the consumer path:

```
$GPGGA,000000.00,0000.0000,N,00000.0000,E,0,00,99.9,,M,,M,,
$GPRMC,000000.00,V,0000.0000,N,00000.0000,E,0.0,0.0,010100,,,N
```

(checksums appended). When a fix-bearing message arrives, its
decoder plugs into `glme_decode_frame()` next to the id-`0x00`
branch.

---

## 4. Exact next missing command / reply

The missing piece is **the chip's reply to the id-`0x01` config
transaction**, not another host command. The host side is fully
recovered (exact bytes in `glme_cfg1`, paced `<=160`-byte writes,
seq `00/02/04/06`). What has never been captured is what the chip
sends back: the old syscall trace's `read()` contents were not
dumped (tracer logged only the first 64 write bytes and no read
buffers), and both emulators stayed silent by design.

Next run (needs the link + one golden-capable boot; power gating
does **not** reset the chip, only a cold boot gives one BRI session):

```
flock /tmp/gpsdev.lock /usr/local/bin/rootshell \
  /data/opt/stock/tmp/gpsesw-new -D /data/opt/stock/tmp/gps_esw \
  --glme-stream -L 15000 > /tmp/glme-stream.log 2>&1
echo $?   # 0 = banner + >=1 more frame; 7 = silent (cold boot needed)
```

`--glme-stream` implies `--golden`, sends the banner wait, replays
`glme_cfg1`, then decodes the stream for `-L` ms: every frame is
hex-dumped (`glme-frame`), decoded (`glme: id=...`), and followed by
the void NMEA pair on stdout. A new chip->host id (anything but
`0x00`) is the fix-bearing message; paste its `glme-frame` hex into
§1 and extend `glme_decode_frame()`.

---

## 5. Files

| file | change |
|---|---|
| `code/userspace/gps/src/gpsesw.c` | GLME codec (`glme_cfg1`, unescape, framer, `glme_decode_frame`), `--glme-stream` (implies `--golden`, default `-L 15000`), void NMEA synthesiser (`$GPGGA`/`$GPRMC` void) |
| `docs/reports/gps-glme-decode.md` | this report |

No firmware bytes committed. Device-side artefacts
(`gpsemu3.py`, `run_emu3.sh`, `glme-host-frames.txt`) stay on
`/data` (tmpfs does not survive the reboot the chip needs).

## 6. Recovery (keep the chip usable)

* Power gating is not a reset (patch stays resident); a cold boot
  (device reboot) is the only known recovery to clean BRI state.
* One golden/BRI run per boot; if a run ends `no GLME banner ...
  cold boot required` (exit 7), stop — do not send more BRI frames
  or another switch.
* Never toggle `ldo38`/`ldo39`; unrelated rails, unknown fan-out.
* This slice never opened `/dev/ttySAC0` except through the
  `gpsesw --glme-stream` path itself; all RE ran against PTY
  emulators. The current outage is unrelated: phone USB shows only
  the fastboot interface, no RNDIS — the device left Nura userspace
  on its own.
