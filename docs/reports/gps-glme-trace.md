# GPS GLME full-trace capture: complete frames, timings, replay flag, offline check

Target: BCM4752 (`0x30020005`) on SM-J700M. The assignment asked for a device-side
run: point the chroot `gps.xml` PortName at a PTY, answer the BRI prologue, ESW
CRCs and the `0x55aa` switch from a device-side emulator, then log every host
write with exact length and timestamp. That device path was **not available in
this session**: the phone enumerates as `18d1:d001` fastboot only
(`lsusb` + `ip link` show no RNDIS interface), `ping 172.16.42.1` is 100% loss
and SSH times out. No chip access happened (no `/dev/ttySAC0` opens anywhere in
this slice), no reboot was issued. Everything below that is marked **[HW]** was
observed on hardware in earlier sessions; everything else is offline-verifiable
in this repo.

Evidence tags: **[OBS]** = observed on the wire; **[HW]** = observed on this
phone's chip in an earlier session; **[INFERENCE]** = reading, not confirmed.

---

## 1. What is actually captured (complete bytes) vs what is prefix-only

### 1.1 Fully captured: banner + BRI prologue + ESW CRCs + txn1 (id 0x01) [OBS/HW]

These bytes are complete and committed under `code/userspace/gps/glme/`:

| file | bytes | content |
|---|---|---|
| `txn1_seq00_225.bin` | 225 | retry-0 225 B frame, seq `00` |
| `txn1_seq00_46.bin` | 46 | retry-0 46 B companion, seq `01` |
| `cfg1_retry{1..8}.bin` | 225/46 ×4 | all 4 retries of the 271 B transaction |
| `glme_emu.py` | — | the PTY emulator (BRI answers + silent GLME log) |

Banner (chip → host, 17 B, **[HW]** live golden run 2026-11-19):

```
e0 00 00 00 07 10 02 01 72 06 30 02 00 05 5f e0 01
```

(`30 02 00 05` LE = ASIC `0x30020005`. Note: `docs/reports/gps-glme.md` §6
quotes the emulator's earlier placeholder `...7e 06 ... 50...`; the real chip
bytes are `...72 06 ... 5f...` as fixed in `gps-glme-decode.md` §1.1.)

BRI prologue + ESW exchange (stock daemon on PTY, emulator log **[OBS]**,
device run `GpsGlmeDecode.GlmeEmuCapture`; times are emulator-monotonic):

```
1.357 RX  fe00fd21180070dc0ffc                    ; CRC 0x7000/4060
1.357 RX  fefa... (data frame block 0)
1.388 TX  fe00fd2f150c7307f1fc                    ; ack 0xf107730c [HW value]
1.389 RX  fe00fd2118ec7f0400fc                    ; CRC 0x7fec/4
1.389 RX  fe01fd6117dec0ec7f02730000fc            ; block 1
1.419 TX  fe00fd2f15750000ecfc                    ; ack 0xec000075 [HW value]
1.420 RX  fe00fd611601000000fc                    ; bank select -> 1
...
1.456 RX  fe00fd000055aa00fffc                    ; *** GLME SWITCH ***
1.862 TX  e0000000071002017206300200055fe001      ; banner (17 B)
```

First post-banner host transaction (txn1, id 0x01, **[OBS]** — 8 frames = 4
retries of a 225 B + 46 B pair; host gets no reply from the silent emulator and
retries with seq `00/02/04/06` + `01/03/05/07`, ~1.75 s apart):

```
1.863 RX-RAW(271)  e000000100c3...                ; retry 0, both frames in one read
1.864 RX-GLME(225) e000000100c3...ac e001         ; seq 00, tail ac
1.864 RX-GLME(46)  e00001010103...f4 e001         ; seq 01, tail f4
3.617 RX-GLME(225) e000020100c3...63 e001         ; seq 02, tail 63
3.617 RX-GLME(46)  e00003010103...2e e001         ; seq 03, tail 2e
5.370 RX-GLME(225) e000040100c3...f7 e001         ; seq 04, tail f7
5.371 RX-GLME(46)  e00005010103...85 e001         ; seq 05, tail 85
7.124 RX-GLME(225) e000060100c3...38 e001         ; seq 06, tail 38
7.124 RX-GLME(46)  e00007010103...5f e001         ; seq 07, tail 5f
```

Full 225 B frame (retry 0, raw wire bytes):

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

Full 46 B companion (retry 0, raw wire bytes):

```
e0 00 01 01 01 03 03 08 01 00 05 02 00 0a 00 06 04 03 00 02 00 01 00 03
00 0c 01 02 01 02 00 0a 07 01 06 00 09 07 01 00 00 00 00 f4 e0 01
```

Decoded (unescape `e0 02→e0`, `e0 03→00`, `e0 04→01`, strip `e0 00`/`e0 01`):
225 B → 216 B (`00 01 00 c3 ... 40 00 00 00 e0 ac`); 46 B → 42 B
(`01 01 01 03 ... 00 00 00 f4`).

### 1.2 Prefix-only: cfg2 (~2644 B) and cfg3 (~1410 B) — full bytes lost

The old syscall trace (`trace3`, fd=16) logged only the first 64 B of each
UART write (tracer truncation; every `n=2644`/`n=1410` entry carries
`hexlen ≤ 64`, verified by sweeping the transcript). What survives:

```
S8979 n=271  e0 00 00 01 00 c3 00 01 41 cf ec e4 d5 74 fb 12 2b 1f 4b 4b
             01 d0 19 d3 00 05 0b 00 0c 3c 42 2a 02 49 00 4b 00 01 00 03
             00 00 94 30 21 85 31 a6 48 06 48 06 48 06 48 06 48 01 ...
S9267 n=2644 e0 00 02 01 02 0b 05 01 03 39 01 6e 6e 6e 6e 0f 12 04 12 2b
             05 04 39 05 e4 72 4c 39 2e 26 11 d1 a1 71 b1 81 71 51 41 2e
             00 31 01 00 f0 60 8e 00 31 a2 32 c3 53 e4 75 05 94 e5 5c 63
             6a 72 79 ...
S9719 n=1410 e0 00 0a 01 0a 00 00 0d 9f e0 01 e0 00 0b 01 0b 27 04 05 00
             04 f0 02 02 05 0c 4a 00 4b 00 1d 00 00 07 14 00 02 05 05 37
             00 00 0a 00 00 00 32 1d 4c 00 00 00 09 00 02 00 0d 22 09 04
             06 00 00 20 ...
```

The prefixes match the live emulator captures for txn1 (`e0 00 00 01...`),
so cfg2/cfg3 ids are `0x02` (2644 B) and `0x0a` (1410 B, contains an inner
`e0 01`, i.e. two frames on the wire). Their bodies were revealed only after
the chip answered txn1 — the real-chip trace3 run shows chip→host reads
between the writes (`S8989 read fd=16 n=268` etc.), while every emulator run to
date stayed silent after the banner and only ever elicited txn1 retries. No
complete cfg2/cfg3 bytes exist anywhere (repo, /tmp, transcripts, stock
binary — the binary builds these frames dynamically; all three 64 B prefixes
return NOT-FOUND in `/tmp/stock_gpsd`). They are listed here as
known-unknowns, not evidence.

### 1.3 Corrected detail: sequence and tail byte positions

`gps-glme-decode.md` §1.2 says "seq byte at payload offset 47". The retry
diffs prove the varying bytes are **raw offset 2** (frame seq) and the
**last payload byte** (opaque per-retry tail):

- 225 B: raw[2] = `00/02/04/06`, raw[222] = `ac/63/f7/38`
- 46 B: raw[2] = `01/03/05/07`, raw[43] = `f4/2e/85/5f`

(`--glme-stream` in this tree previously patched raw offset 47 — which is the
constant `0x80` — and sent only the 225 B frame. Both are fixed by
`--glme-replay`; `--glme-stream` now sends the full 271 B transaction with the
correct bytes too. See §3.)

The tail byte is opaque: `sum`/`xor`/CRC-8 (polys `07/31/9B/1D/39/D5/A7/E0/EA`,
init `00/FF`, refin/refout both ways, over body / body-minus-seq slicings) all
fail across the 4 samples. The no-seq body sum is constant (`0x70` for the 46 B
companion), so only seq+tail vary. Replay the exact captured byte.

---

## 2. Timings

**Stock daemon vs silent emulator (device, [OBS]):** §1.1 timestamps above.
Banner at t≈1.86 s; txn1 retries at t≈1.86 / 3.62 / 5.37 / 7.12 s
(~1.75 s spacing); daemon exits after retry 4 (~8.9 s tail). Host writes are
paced in ≤160 B UART pieces (`xwrite` gap 1 ms); the emulator logs each `read`
with exact length (`RX-RAW(n)`) plus per-frame lines — no truncation.

**Replay verification (host, this session, [OBS]):** `gpsesw --glme-replay`
against the repo emulator over a PTY (fake 4060/4/6777 B patch files, `-n`
so no GPIO):

```
gpsesw prologue 3/5 → CRC 0xf107730c → 0xec000075 → bank 1 →
  CRC 0x8109aed2 → CRC 0x0b05ed95 → 0x55aa switch →
  banner received → 4x txn1 (225+46 B, seq 00/01..06/07) → 0 frames → exit 7
```

Emulator side logged all 8 frames byte-exact (see §4). The emulator answers
BRI ~1.9 s after the burst (host-side PTY latency); the 4 retries go out
back-to-back, then the `-L 3000` listen window elapses with the silent peer.

---

## 3. Replay implementation (`--glme-replay`, `--glme-check`)

`code/userspace/gps/src/gpsesw.c`:

- `glme_cfg1b[46]` + `glme_retry_tail[4]` / `glme_retry_tail_b[4]`: the
  companion frame and the 8 opaque per-retry tails (retry `i` = even seq `2i`,
  odd seq `2i+1`).
- `glme_send_txn1(fd, retry)`: patches raw[2] and the tail of both frames and
  sends the 271 B transaction via `xwrite` (≤160 B pieces, same pacing).
- `glme_replay_run(fd)`: sends txn1 4x (exact stock order/timing shape), then
  the standard `-L` listen/decode loop (`glme_feed` + void NMEA). Exit `0` on
  ≥1 frame, `7` on silence (same cold-boot code as the banner wait).
- `--glme-check DIR` (offline, no serial/GPIO touched): byte-compares the two
  compiled-in retry-0 frames against `DIR/txn1_seq00_{225,46}.bin`. Runs
  before any block loading.
- `--glme-stream` now sends the **full** 271 B transaction with the **correct**
  seq/tail bytes (single shot, then listen) instead of the 225 B frame with
  the offset-47 patch.

Usage:

```
gpsesw -d /dev/ttySAC0 -D <esw-dir> --glme-replay [-L MS]
gpsesw --glme-check code/userspace/gps/glme   # offline, exit 0 = match
```

`--glme-replay` implies `--golden` (blocks 0–2, bank 1, immediate `0x55aa`).

---

## 4. Offline emulator verification (this session, [OBS])

Two checks, both green, no silicon / no device / no qemu:

1. `gpsesw --glme-check code/userspace/gps/glme` → both files match
   (`225 bytes match`, `46 bytes match`), exit 0.
2. `code/userspace/gps/glme/glme_emu.py` (repo copy of the proven emulator,
   banner `...72 06 ... 5f...`) on a host PTY + `gpsesw -d <pty> -n
   -D /tmp/fake_esw --glme-replay -L 3000`: banner received, all 8 replayed
   frames logged by the emulator **byte-identical** to the stock capture
   (`FRAME 1..8`, 225/46 alternating, seq `00..07`, tails
   `ac/f4/63/2e/f7/85/38/5f`), exit 7 = silent peer as expected.

---

## 5. Remaining gap (needs one cold boot; confirm with Main before rebooting)

The chip's reply to txn1 — the measurement/fix stream — has never been
captured: the silent emulator only elicits retries, and the tracer never
dumped read buffers. After a cold boot (power gating does **not** reset the
chip; one golden/BRI run per boot; never touch `ldo38`/`ldo39`):

```
flock /tmp/gpsdev.lock /usr/local/bin/rootshell \
  /data/opt/stock/tmp/gpsesw-new -D /data/opt/stock/tmp/gps_esw \
  --glme-replay -L 15000 > /tmp/glme-replay.log 2>&1
echo $?   # 0 = banner + >=1 chip frame; 7 = silent (needs another cold boot)
```

Then paste any new chip→host id (anything but `0x00`) into §1 and extend
`glme_decode_frame()`. cfg2/cfg3 replay must wait for their complete bytes,
which only this run can produce — do **not** guess them.

## 6. Files

| file | change |
|---|---|
| `code/userspace/gps/src/gpsesw.c` | `glme_cfg1b`, retry-tail tables, `glme_send_txn1`, `glme_replay_run`, `glme_replay_selftest`, `--glme-replay` / `--glme-check` (implies `--golden`); `--glme-stream` sends full txn1 with correct seq/tail |
| `code/userspace/gps/glme/glme_emu.py` | repo copy of the PTY emulator (BRI answers + silent GLME log) |
| `code/userspace/gps/glme/txn1_seq00_{225,46}.bin` | canonical retry-0 transaction bytes |
| `code/userspace/gps/glme/cfg1_retry{1..8}.bin` | all 4 captured retries (225/46 pairs) |
| `docs/reports/gps-glme-trace.md` | this report |
