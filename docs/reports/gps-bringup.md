# GPS bring-up — BCM4752 on j7elte (verified on hardware, 2026-11-18/19)

**Verdict: the chip is alive, powered, and the UART works — the earlier "chip is
silent" conclusion was an ordering artefact in the daemon's `--init`. The
recovered BRI prologue now runs end-to-end on the phone and the chip answers
every request, including the `DebugSfr` ack (`sub == 0x95`) that Broadcom's
`bri_wait4asic_ready` stage waits for. The chip still does not, and by design
cannot, emit NMEA: NMEA is synthesised by Broadcom's `glgps` from ESW /
measurement reports, and the chip speaks only the binary BRI protocol.**

Everything below was executed on the phone (SM-J700M, Nura, kernel `3.10.108
#31`) over SSH; every frame shown is a real capture, not a derivation.

## 1. Preconditions checked (all green unless stated)

| Precondition | Evidence |
|---|---|
| GPS power-enable GPIO | `gpio-137 (GPS_PWR_EN)` in `/sys/kernel/debug/gpio`; writing `1` to `/sys/class/sec/gps/GPS_PWR_EN/value` flips it `out lo`→`out hi` and reads back `1` |
| `vdd_ldo10` rail | `/sys/class/regulator/regulator.13` = `vdd_ldo10 enabled 1050000 uV` (driver re-asserts the PMIC `L10CTRL` enable bits that `s2mpu03.c` clears when `g3d_en=1`; the *stock* DTB really does carry `g3d_en = 1`, so this clear happens in the vendor kernel too) |
| UART pin mux | debugfs `pinmux-pins`: pins 87–90 (`gpb0-4..7`) owned by `13800000.uart`, function `/pinctrl@139C0000/uart0-bus` |
| tty device | `/dev/ttySAC0` = 204:64, 8N1 raw at 921600, TX counters advance (`/proc/tty/driver/s3c2410_serial`) |
| RX path | chip replies arrive in 0–1 ms; `rx:` counter in `/proc/tty/driver/s3c2410_serial` advances on each reply |
| Flow control | **not** required: `CRTSCTS` on, manual RTS assert, both together, and both with a power cycle in the same open fd produced zero bytes in 10 s ×3 |
| Baud | nobody free-runs: 4800/9600/19200/38400/57600/115200/230400/460800/921600/1M/1.5M/2M/3M/4M, each after a power pulse, 250 ms + 3 s of listening → zero rx bytes |
| No other UART | `ttySAC2` (0x13820000) and `ttySAC3` (0x11060000) probed the same way: zero rx |

## 2. The verified bring-up sequence (exact bytes, observed replies)

Power cycle first (matches `GpioDelayMs="250"` in the stock `gps.xml`):

```
echo 0 > /sys/class/sec/gps/GPS_PWR_EN/value; sleep 1
echo 1 > /sys/class/sec/gps/GPS_PWR_EN/value; sleep 0.25
```

Then on `/dev/ttySAC0`, 921600 8N1, **no** hardware flow control:

| # | host → chip | chip → host | latency |
|---|---|---|---|
| 1 | `80` × 16 (autobaud burst) | — (no announce; this is normal) | — |
| 2 | `fe 00 fd 00 1b 07 00 00 00 fc` (NVRAM word-7 write) | — | — |
| 3 | `ff 00 fd 00 1b fc` | `fe 00 fd 00 1b 07 00 00 00 fc` | 1 ms |
| 4 | `ff 00 fd 40 1a fc` | `fe 00 fd 40 1a 00 00 00 00 fc` | 1 ms |
| 5 | `ff 00 fd 00 00 fc` *(Bri_RequestHwVersion)* | `fe 00 fd 00 00 05 00 02 30 fc` → version `0x30020005` | 1 ms |
| 6 | `ff 00 fd 1f 95 fc` *(Bri_RequestDebugSfr)* | `fe 00 fd 1f 95 00 00 00 00 fc` → `sub == 0x95` = **asic-ready ACK** | 1 ms |

Raw capture (one run, complete):

```
tx 16 B -> 16
  rx @400ms: (no bytes in 400 ms)
tx 10 B -> 10
  rx @300ms: (no bytes in 300 ms)
tx 6 B -> 6
  rx @1ms 10 B: fe 00 fd 00 1b 07 00 00 00 fc   |..........|
tx 6 B -> 6
  rx @0ms 10 B: fe 00 fd 40 1a 00 00 00 00 fc   |...@......|
tx 6 B -> 6
  rx @1ms 10 B: fe 00 fd 00 00 05 00 02 30 fc   |........0.|
tx 6 B -> 6
  rx @1ms 10 B: fe 00 fd 1f 95 00 00 00 00 fc   |..........|
```

Notes:

* Step 2's 10-byte write gets no reply, but it changes step 3's payload:
  without it step 3 answers `… 1b 00 00 00 00 …`; with it the chip echoes the
  stored word (`07 00 00 00`). The chip's NVRAM path works.
* Step 4 answered `02 00 10 a4` on a cold boot in a parallel run and
  `00 00 00 00` later in the session; the payload is state-dependent, the
  frame is always accepted.
* **`fe 00 fd 00 00 55 aa 00 ff fc` (Bri_GetOutOfLegacy) is not answered** at
  this point (two 1.2 s waits). It is not needed to reach the asic-ready state
  on this ASIC revision.
* After step 6 the chip is silent for 6 s and still silent when polled
  (`ff 00 fd 40 1a fc` four times, identical replies): there is **no**
  spontaneous measurement stream and no NMEA.

### Why the earlier runs saw "silence"

`j7elte-gpsd --init` (and the earlier ad-hoc probes) sent
`Bri_GetOutOfLegacy` first and `Bri_RequestHwVersion` second, i.e. before the
NVRAM exchange. The chip ignores both in that state and never answers, so a
300 ms timeout expired and the handshake aborted. Ordering is the whole
difference; the vendor code itself gates `RequestHwVersion` on
`transport->[4] == 1`, which is the flag this exchange sets.

## 3. Why this cannot turn into NMEA by itself

* The chip has no NMEA generator: every NMEA sentence in the vendor stack comes
  from `GlEngineImpl::GetNextNmeaSentence` / `HasNextNmeaSentence` inside
  `/system/bin/gpsd`, fed by ESW/measurement reports.
* The chip answered every request with a 10-byte `fe 00 fd … fc` BRI frame; it
  never emitted anything unprompted.
* The remaining vendor stages cannot be replayed from the ROM:
  * `Bri_ProgramSw/HwFlowControl` **searches** for an escape-byte pair (walks
    candidates, transmits, awaits the ACK) — there is no constant frame to copy
    without a live trace of the candidate/ACK exchange.
  * ESW/patch loading: `system.raw` contains no ESW blob, so `glgps` itself
    would take its `"No ESW Image to Load, using ROM"` path — the ROM path's
    *upload protocol* is still vendor-internal.
  * RF/clock plan (`ProgramRfFrequencyPlan`, `SetRefClockParams`), measurement
    configuration and `Start` are built at run time from `gps.xml`
    (`GL_RF_47520_BRCM_EXT_LNA`, `FRQ_PLAN_26MHZ_2PPM_26MHZ_300PPB`).

So the deliverable "NMEA on /dev/ttySAC0" is not achievable through the chip:
the only producer of NMEA is Broadcom's closed-source engine, and the chip's
own output is the binary BRI/ESW stream that engine consumes.

## 4. Smallest next experiment (in priority order)

1. **Capture one cold start of the stock stack** (stock `boot.img` + a tap on
   the GPS TX/RX pads, or a stock boot with the daemon's UART log) and replay
   the first second. That single trace resolves flow control, ESW-vs-ROM, RF
   programming and the measurement start in one shot.
2. Without a tap: recover the candidate/ACK exchange of
   `Bri_ProgramSwFlowControl` (`0x2a6aa0`) by disassembling the search loop
   against `Bri_CheckForReceivedPacket_Sardine` (`0x2a789c`), then continue to
   `ProgramRfFrequencyPlan`. This is derivable but state-dependent and needs
   the chip in the loop for every candidate.

## 5. Tooling added

`code/userspace/gps/src/gpsprobe.c` — a raw op driver for `/dev/ttySAC0` used
for every capture above. Build on the phone with `gcc -O2 -o /tmp/gpsprobe
gpsprobe.c` and drive it with ops:

```
w:HEX   write raw bytes (spaces/colons ignored)
s:MS    sleep      r:MS   read+hexdump for MS
f       tcflush    P:0|1  write the GPS power-enable sysfs node
B:N     switch baud
C:0|1   clear/set CRTSCTS   R:a|d  assert/deassert RTS
```

Example (the full prologue above):

```
/tmp/gpsprobe -b 921600 f w:80808080808080808080808080808080 r:400 \
  w:fe00fd001b07000000fc r:200 w:ff00fd001bfc r:200 w:ff00fd401afc r:200 \
  w:ff00fd0000fc r:200 w:ff00fd1f95fc r:200 r:6000
```

## 6. What the daemon needs

`j7elte-gpsd --init` must send the NVRAM pair **before** `RequestHwVersion`
and must not abort when the autobaud burst is not announced (it never is).
With that order the daemon reaches the `sub == 0x95` ACK, which is the last
state the ROM-only path can verify without a vendor trace.
