# BCM4752 ESW / RAM-patch upload — verified wire format on j7elte

Target: SM-J700M `j7elte`, kernel `3.10.108 #33` (stock-parity GPS driver),
BCM4752 at `/dev/ttySAC0` 921600 8N1, no flow control.
Patch source: the four `4752_B2` blocks extracted from `/system/bin/gpsd`
(`docs/reports/gps-esw.md`), regenerated with

```
make -C code/gps/esw
./code/gps/esw/esw_dump /tmp/stock_gpsd --dump /tmp/gps_esw/b2
```

Tools: `code/userspace/gps/src/gpsesw.c` (standalone probe, `-V` selects the
frame encoding) and the same encoder inside `j7elte-gpsd` (`--esw DIR`).
Both were run on the device; the daemon run is reproduced in §6.

**Verdict: the upload works.** All four blocks were written, and the chip
answered every CRC request with `2f15` — five acknowledgements for the ten
frames + two odd-tail frames sent. The encoding is the public-spec one
(`-V 1`): no inference was needed beyond what `docs/reports/gps-webproto.md`
§5 already stated, and the `0x7fec/4` acknowledgement is **byte-identical**
to the published capture. What does **not** work yet is the hand-off after
the patch: the GLME switch frame makes the chip stop answering BRI, but the
`e0 … e0 01` banner never arrives (§5).

---

## 1. Frame encoder that the hardware accepts

```
power cycle   GPS_PWR_EN 0 -> (>=1 s) -> 1 -> (300 ms)
prologue      16 x 80
              fe 00 fd 00 1b 07 00 00 00 fc      -> (no reply)  NVRAM word 7 := 7
              ff 00 fd 00 1b fc                  -> fe 00 fd 00 1b 07 00 00 00 fc
              ff 00 fd 40 1a fc                  -> fe 00 fd 40 1a 00 00 00 00 fc
              ff 00 fd 00 00 fc                  -> fe 00 fd 00 00 05 00 02 30 fc
              ff 00 fd 1f 95 fc                  -> fe 00 fd 1f 95 00 00 00 00 fc

for each record (bank, addr, len):
  if bank != current:  fe 00 fd 61 16 <bank> 00 00 00 fc   -> (no reply)
                       ff 00 fd 1f 94 fc                -> fe 00 fd 1f 94 <bank> 00 00 00 fc
  for piece in ceil(len / 4096):                     # CRC boundary
    for frame_chunk in ceil(piece / 1020):
      aligned = chunk & ~3 ; tail = chunk & 3
      if aligned:
        fe <aligned/4> fd 61 17 de c0 <addr_lo> <addr_hi> <aligned bytes> fc
      if tail:                                        # 4 bytes ending at addr+chunk
        fe 01          fd 61 17 de c0 <a_lo> <a_hi>    <last 4 bytes>    fc
        a = (addr + chunk - 4) & 0xffff
    fe 00 fd 21 18 <addr_lo> <addr_hi> <len_lo> <len_hi> fc   -> fe 00 fd 2f 15 <4 B> fc
```

Details that matter and were confirmed on the wire:

| item | value | evidence |
|---|---|---|
| `<nwords>` | `ceil(bytes/4)` of **that frame's** payload | block 0 frame 1 is `fe ff …` = 255 words = 1020 B; the 1000-byte frame is `fe fa …`; a 4-byte write is `fe 01 …` |
| frame payload limit | 1020 B (`0x3fc`) | 4060-byte record split 1020/1020/1020/1000 |
| UART write pacing | 160 B (`0xa0`) pieces, 1 ms gap | the vendor transport splits at `0x2a72a4`; used here, no overrun |
| CRC boundary | 4096 B (`0x1000`) pieces, not whole records | block 2 (6777 B) is two pieces: `0x8000/4096` and `0x9000/2681` |
| `de c0` | constant | present in every accepted frame |
| data frames | never acknowledged | silence in a 20 ms window after each frame |
| `2f15` | only after `21 18`, ~30 ms later | see captures |
| bank register | item `0x16` write, item `0x94` readback | `61 16 00` → `1f 94` reads 0; `61 16 01` → reads 1 |
| fresh chip default | item `0x94` = 1 | before any `0x16` write |

There is no begin/end-of-image frame and no length field on the data frames.

## 2. Hardware captures (raw, from `gpsesw -V 1 -x -Q`, `/tmp/run-final-upload.log`)

Power cycle, prologue (all replies as listed in §1), then:

```
00:03:28.003 item 0x94 = 0x00000001          ; virgin default
00:03:28.605 == block 0 bank 0 addr 0x7000 len 4060 (host crc32 0x884e382c)
             tx fe 00 fd 61 16 00 00 00 00 fc        (bank 0, no reply)
             tx fe 00 fd 1f 94 fc
             rx fe 00 fd 1f 94 00 00 00 00 fc        -> 0
             tx fe ff fd 61 17 de c0 00 70 …1020 B… fc
             tx fe ff fd 61 17 de c0 fc 73 …1020 B… fc
             tx fe ff fd 61 17 de c0 f8 77 …1020 B… fc
             tx fe fa fd 61 17 de c0 f4 7b …1000 B… fc
             tx fe 00 fd 21 18 00 70 dc 0f fc
             rx fe 00 fd 2f 15 0c 73 07 f1 fc        -> ack 0xf107730c
             (host CRC-32 0x884e382c — not the chip's value)
00:03:29.017 == block 1 bank 0 addr 0x7fec len 4
             tx fe 01 fd 61 17 de c0 ec 7f 02 73 00 00 fc
             tx fe 00 fd 21 18 ec 7f 04 00 fc
             rx fe 00 fd 2f 15 75 00 00 ec fc        -> ack 0xec000075
00:03:29.940 item 0x94 = 0x00000001          ; after 61 16 01
00:03:29.941 == block 2 bank 1 addr 0x8000 len 6777
             tx fe 00 fd 61 16 01 00 00 00 fc        (bank 1, no reply)
             … 0x8000/0x83fc/0x87f8/0x8bf4 (1020 B each), 0x8ff0 (16 B) …
             tx fe 00 fd 21 18 00 80 00 10 fc
             rx fe 00 fd 2f 15 d2 ae 09 81 fc        -> ack 0x8109aed2
             … 0x9000/0x93fc (1020 B each), 0x97f8 (640 B) …
             tx fe 01 fd 61 17 de c0 75 9a <last 4 bytes> fc   ; odd tail, addr 0x9a75
             tx fe 00 fd 21 18 00 90 79 0a fc
             rx fe 00 fd 2f 15 95 ed 05 0b fc        -> ack 0x0b05ed95
00:03:31.053 == block 3 bank 0 addr 0x7fd2 len 4 (zerofill block)
             tx fe 00 fd 61 16 00 00 00 00 fc
             rx fe 00 fd 1f 94 00 00 00 00 fc        -> 0
             tx fe 01 fd 61 17 de c0 d2 7f 00 00 00 00 fc
             tx fe 00 fd 21 18 d2 7f 04 00 fc
             rx fe 00 fd 2f 15 00 00 00 00 fc        -> ack 0x00000000
00:03:36.196 item 0x94 = 0x00000000 ; item 0x95 = 0
             reg 0 = fe 00 fd 00 00 05 00 02 30 fc  (0x30020005, unchanged)
             0x001b = fe 00 fd 00 1b 07 00 00 00 fc  (7, unchanged)
```

Two independent runs (the second after the device had been rebooted) produced
byte-for-byte identical acknowledgements:

| piece | size | `2f15` reply | ack value |
|---|---|---|---|
| `0x7000` | 4060 | `fe00fd2f150c7307f1fc` | `0xf107730c` |
| `0x7fec` | 4 | `fe00fd2f15750000ecfc` | `0xec000075` |
| `0x8000` | 4096 | `fe00fd2f15d2ae0981fc` | `0x8109aed2` |
| `0x9000` | 2681 | `fe00fd2f1595ed050bfc` | `0x0b05ed95` |
| `0x7fd2` | 4 | `fe00fd2f1500000000fc` | `0x00000000` |

The `0x7fec/4` acknowledgement is exactly the one in the public capture
(`SPEC.md` step 7 → `fe00fd2f15750000ecfc`), which is the strongest cross-check
available: the framing, the chunking and the CRC granularity all agree.

## 3. Variants

`-V 1` (public spec, described above) worked on the first hardware run, so no
other encoding was exercised:

* `-V 2` — one frame per 160-byte piece (`nwords = ceil(piece/4)`, address
  advances per piece). Untested.
* `-V 3` — one frame per chunk with `nwords = ceil(chunk/4)` and all chunk
  bytes, no odd-tail frame. Untested.

Both remain in `gpsesw.c` behind the flag. Rejection was never observed: the
chip accepted `-V 1` data frames silently and answered the CRC request, so no
power-cycled retry ladder was needed.

## 4. Chip state after the load

* Item `0x94` still reads the selected bank (0 at the end).
* Item `0x95` stays 0 (same value as "asic ready" before the load).
* Register 0 stays `0x30020005`; item `0x1b` stays 7.
* **Item `0x98` changed 1 → 3** across the load. On a virgin chip it reads 1
  (measured right after the device reboot, second capture); it reads 3 after
  an upload and stays 3 across a 3 s `GPS_PWR_EN` off/on cycle. Meaning
  unknown; it is the only observable difference the patch makes.
* `GPS_PWR_EN` off for up to 3 s is **not** a full chip reset: one prologue
  run 4 minutes later failed 4 of 5 exchanges (the chip, previously left in
  GLME mode, did not answer). A device reboot restores a clean BRI state.
  Power-cycle experiments after a GLME switch must therefore be treated as
  unreliable unless the device is rebooted.

## 5. The remaining unknown: the post-patch hand-off

The GLME switch `fe 00 fd 00 00 55 aa 00 ff fc` is accepted in the sense that
the chip **stops answering BRI**, but no `e0 … e0 01` banner arrives and no
bytes follow in a 1 s or 3 s window. Three post-patch sequences were tried,
each on a fresh full upload:

| # | post-upload frames | result |
|---|---|---|
| A | `61 16 01` (bank 1), `1f 94`, `1f 95`, `1f 96`, `1f 97`, then GLME | 0x94→1, 0x95→0, 0x96→0xf4, 0x97→0; after GLME: **BRI reads silent** |
| B | re-autobaud `80`×16, then GLME | same, BRI silent |
| C | version block (`fe 00 1b 07…`, `ff 00 1b`, `ff 40 1a`) — both replies normal — then GLME | same, BRI silent |

So the switch frame does something (the BRI handler disappears), but the patch
does not visibly start. This matches the vendor stage names that have no
counterpart in our sequence yet: `bri_wait4eswload`, `bri_wait4patchload`,
`bri_wait4eswbankswitch`, `esw_wait4version`, `Patch will jump to ROM`
(`docs/reports/gps-esw.md` §4, `gps-flowcontrol.md` §6). The likely missing
piece is whatever the vendor does between "last CRC ack" and the switch —
probably a bank switch/jump or an item write that the ARM32 loader performs via
a virtual call (`0x2a588e` is the analogous un-resolved RF-plan write,
`gps-flowcontrol.md` §10.6).

Caveat from the hardware session: one GLME run coincided with a device reboot,
and the following run found the chip unresponsive until rebooted. Causality
was not established (the device's dmesg only contains unrelated
`openrc-user`/`elogind` seccomp dumps), but the switch is the last thing sent
before the reboot. The daemon therefore sends it only with an explicit
`--glme` flag.

## 6. What was changed

| file | change |
|---|---|
| `code/userspace/gps/src/gpsesw.c` | new: power cycle + prologue + parameterised upload driver (`-V`, `-C` CRC piece, `-c` chunk, `-p` write piece, `-w`/`-z` raw frames, `-Q` item scan) |
| `code/userspace/gps/src/j7elte-gpsd.c` | `--esw DIR` uploads `b2-4752_B2-{0..3}.bin` after the BRI prologue; `--glme` adds the switch; bank switch + `0x94` readback; per-4096 CRC; logs `2f15` values |
| `code/userspace/gps/Makefile` | `gpsesw` target |
| `code/pmaports/device/archived/j7elte-gpsd/j7elte-gpsd.c` | mirror of the daemon source |
| `patches/userspace/gps/0001`, `0002` | regenerated |

Verified end-to-end with the daemon itself on the device:

```
# /tmp/j7elte-gpsd -i -E /tmp/gps_esw -r /tmp/gpsrun -v
… esw: CRC ack 7000/4060 = 0xf107730c
… esw: CRC ack 7fec/4 = 0xec000075
… esw: CRC ack 8000/4096 = 0x8109aed2
… esw: CRC ack 9000/2681 = 0x0b05ed95
… esw: CRC ack 7fd2/4 = 0x00000000
… init: ESW upload finished (all pieces acknowledged)
… init: stopping after the patch load (no GLME switch; --glme enables it)
```

and the no-`--esw` handshake is unchanged (the `init_test.py` sequence is
untouched; the ESW branch runs only when `--esw` is given).

## 7. Next experiments

1. **Find the vendor's post-CRC step.** Run the stock `gpsd` in the chroot
   against a logged UART (`gps-chroot.md` rig) and diff its frames between the
   last `2f15` and `fe 00 fd 00 00 55 aa 00 ff fc`. This is the shortest path
   to the missing jump/start frame.
2. **Check whether the switch needs a bank.** A and C above ended with bank 0
   and bank 1 respectively; both silent. Try the switch while bank 1 is
   selected *and* with a `61 16 01` re-read after it.
3. **Identify item `0x98`.** Sweep `0x96..0x9f` before/after each piece on a
   virgin boot to see which write moves it from 1 to 3.
4. **Fit the `2f15` value.** It is not host CRC-32; the `0x7fec/4` block gives
   a known input (`02 73 00 00`) and a known output (`0xec000075`), and
   `0x7fd2/4` gives zeros → `0x00000000`, which rules out several CRC
   variants; a 16/24/32-bit CRC search over the four known (data, value) pairs
   should settle it.
5. **Reset semantics.** Determine whether `GPS_PWR_EN` low ever resets the
   ESW domain; if not, a documented reboot (or a chip-level reset line) is
   required between GLME experiments.
