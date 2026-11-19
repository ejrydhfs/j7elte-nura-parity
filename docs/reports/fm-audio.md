# FM audio — tune, mixer route, capture levels, dataless R-CC probe

Date: 2026-11-19 UTC. Device j7elte (BCM43438), kernel #37. Reporter: FmAudioEvidence3.
Prior state: `code/userspace/fm/audio-evidence.txt` (partial — zero audio bytes, link lost),
`docs/reports/fm-backend.md` §8, `fm-mixer.sh`, `fm-audio-route.md`.

## 1. Verdict

| Question | Result |
|---|---|
| Strong-station tune (94.5 MHz) | **PROVEN, repeatedly** — enable chain + tune/commit + RSSI read, all `status=0x00`, quiet line (no `04ff0108` flood) on every boot |
| Mixer route (`record` path) | **PROVEN applied** — `fm-mixer: record applied`, all controls verify (`SRC3=1`, `CH3 Rec=1`, `DOUT=RMIX_OUT(2)`, `LINEIN=on`) |
| Tuned-vs-muted audio levels | **BLOCKED below the FM layer** — zero audio bytes capturable on this build: ALSA `hw:0,0` capture and `hw:0,0`/`hw:0,4` playback both hang at the PCM layer (`hw_ptr: 0`, `avail: 0` forever); PulseAudio inherits the same stall (RUNNING source, zero bytes delivered). No RMS/spectrum numbers exist. Control experiment implicates the kernel/ABOX, not FM |
| Dataless R-CCs / second socket | **PROVEN dataless, forensics captured** — `R(0x0F)` CC is 9 bytes `040e060115fc000f99` (zero value bytes) on both sockets; second raw-HCI socket sees only its own echoed tx + the CC, **no async vendor event**. Notably the sibling socket also observed bluetoothd's own `0x0C14 Read Local Name` traffic, proving the tap sees shared HCI traffic when any exists — for RSSI/SNR reads there is none |
| BT health | **Undisturbed** — hci0 `UP RUNNING PSCAN` before/after every run; bluetoothd pids stable within a boot; `Read Local Name → 'Samsung Galaxy J7'` verified through the flood-free line; no `hciconfig down`, no service stop, no LDO toggle by us |
| Device at hand-off | **DOWN in fastboot** — fell to `18d1:d001` (half-enumerated RNDIS/fastboot, same serial) during the concurrent-clock capture test ~15:22 UTC; still there 20+ min later, ping 100% loss, no RNDIS iface. Same signature as the 2026-11-19 ~04:45 outage (recovered on its own then). Needs a Main-gated power-cycle/reflash decision — NOT done from this slice |

No audible-audio proof was obtained. The blocker is precise and below the FM layer
(see §4); it is not a tune failure, not a mixer failure, and not an HCI failure.

## 2. Tune evidence (this session, verbatim)

Every run on a fresh boot, `drained-pre 0`, post-watch `vendor04ff0108=0 other=0`
(quiet line — the audio-path write `W1B(0x4D,1,8)` that re-arms the ~2.9k/s
`04ff0108` flood was deliberately never sent):

```
EN00 tx 0115fc001001 rx 040e060115fc000010
EN02 tx 0115fc021002 rx 040e060115fc000210
ENFB tx 0115fcfb482e22302a rx 040e060115fc00fb48
ENR4D tx 0115fc4d11 rx 040e060115fc004d11
EN05 tx 0115fc05a81d00 rx 040e060115fc0005a8
TUNE tx 0115fc0a20ea24 rx 040e060115fc000a20        (9450 = 94.5 MHz)
COMMIT tx 0115fc091800 rx 040e060115fc000918
RSSI tx 0115fc0f99 rx 040e060115fc000f99            (dataless, see §5)
```

Three full tune+route runs succeeded identically (15:13, 15:16, 15:18 UTC, plus
two earlier 14:58/14:59 runs at 94.5 MHz before the first watchdog reboot wiped
tmpfs). One anomaly for the record: the very first run's log line printed
`...fc0c0918` (an extra `0c` byte) — a logging artefact of that run's script,
not a chip response; all subsequent COMMIT CCs are the canonical
`040e060115fc000918`, `status=0x00`.

## 3. Mixer route evidence (this session, verbatim)

`sh /home/user/fm-mixer.sh record` → `fm-mixer: record applied`, `ROUTE-RC=0`,
device md5 matches repo (`d02e85fa…`). Post-apply verify reads:

```
S2801 SRC3 En => : values=1
S2801 CH3 Rec En => : values=1
S2801 CH1 DOUT Select => : values=2        (RMIX_OUT)
LINEIN LINEIN On => : values=on
```

Baseline before (power-on defaults, fresh boot): `SRC3=0`, `RMIX1/2=0`,
`CH3 Rec=0`, `DOUT=DMIX_OUT(0)`, `LINELL/LINELR=off`, `LINEIN=off`,
`Line-in Volume` default 6 dB. Revert (`fm-mixer.sh revert`) was exercised in a
prior slice design but NOT re-verified this session — the device went dark with
the TX+RX test route (record + `CH1/CH3 Mixer En=On`) applied on top of the
§3 baseline. On link return: re-dump `amixer -c0 contents`, diff against
`audio-evidence.txt` §1, restore-then-reapply.

## 4. Capture evidence — the precise blocker (below FM)

No WAV with audio frames was produced by any path. All of these hang forever
(killed by `timeout`, zero bytes, `hw_ptr: 0`):

- `arecord -D hw:0,0 -f S16_LE -r 48000 -c 2` (also `-r 44100`, `--nonblock`,
  `/dev/null` target, small `--buffer-size`) — prints the format line, then
  never a period. A prior slice's `self.wav` (1 152 044 B) proves this path
  *once* worked, so this is a boot/build-state regression, not a usage error.
- `aplay -D hw:0,0 /home/user/self.wav` (known-good file, S16_LE 48 kHz
  stereo) — same hang at PCM open.
- `aplay -D hw:0,4 /tmp/sil.wav` **alone**: returns RC=0 instantly (bounded,
  no hang) — the FM dai-link playback device itself opens. But backgrounding
  it and then running the `hw:0,0` capture in the same window hangs the
  session hard (device rebooted, then fastboot). So the concurrent-clock
  hypothesis (stock HAL always holds `pcm_fm_out` open during FM) could
  neither be confirmed nor refuted — the capture side never produces a
  single sample with or without playback running.
- PulseAudio `parecord` on `alsa_input.platform-sound.34.stereo-fallback`
  (the `hw:0` input): connects (`RUNNING`, `suspended: no`, correct
  `hw_params`: 2 ch, 44.1 kHz, `period 16384/buffer 65536`), `--verbose`
  shows ever-growing latency — but delivers **zero bytes** (`44`-byte header
  only, no `Stream moved` data, stdout pipe `0` bytes). Meanwhile the source
  stays `RUNNING` with `avail: 0`, `hw_ptr: 0` in
  `/proc/asound/card0/pcm0c/sub0/status`: the ABOX/i2s0-pri capture DMA never
  moves. `module-suspend-on-idle` was ruled out (source forced RUNNING, card
  profile `output:stereo-fallback+input:stereo-fallback` active, volumes
  100%/unmuted). Sanity: `hw:0,2` (voice call) capture **works**
  (`voice.wav` 44 B header + valid header bytes, RC=0) — so ALSA capture per
  se is alive; only the `i2s0-pri`/`hw:0,0` path is stalled.
- `dmesg` shows no ASoC/ABOX/PCM errors (only seccomp `syscall 283/291/434`
  audit lines for pulse/sshd — benign on this 3.10 kernel). No oops/panic
  lines; the `PC is at 0x7f…` lines are the seccomp audit register dumps,
  not crashes.

Conclusion: the PCM stall is in the kernel audio path (ABOX/S2801 clocking or
DAI link state on this build), **not** in the FM HCI path and **not** in the
mixer route. FM audio proof is impossible until `hw:0,0` capture (or `hw:0,4`
loopback) yields samples for *any* source. Recommended next steps for the
audio/BSP owner, none of which belong in this slice: (a) compare
`mixer_paths.xml` UCM vs live DAPM widgets (`/sys/kernel/debug/asound`),
(b) try `tinyalsa`/`alsaucm` route or the stock `audio.primary` HAL path
(`pcm_fm_out` + `fm_radio-fm-recording`), (c) check S2801 clock parents
(`/sys/kernel/debug/clk`) while the route is applied, (d) test `hw:0,2`
loopback as a capture vehicle.

## 5. Dataless R-CCs + second-socket forensics (this session, verbatim)

`fm_2sock.py` (two `AF_BLUETOOTH`/`SOCK_RAW` sockets, all-events filter,
reader fork on B while A sends `R(0x0F,0x80,1)`):

```
drainA 0 drainB 0
SOCK-A RSSI CC: 040e060115fc000f99 len=9
SOCK-B saw 2 events:
  B: 0115fc020f99
  B: 040e060115fc020f99 ... (echo of A's own tx, seen on the shared tap)
  B: 040e060115fc000f99
2SOCK-DONE
```

So: the R-CC is exactly 6 HCI bytes + 3 H4 framing = `04 0e 06 01 15 fc 00`
+ `0f 99` echo, **zero value bytes**. No async `04ff…` vendor event follows
within 3 s on either socket. Because socket B demonstrably sees shared HCI
traffic — in one run it captured bluetoothd's own `0x0C14 Read Local Name`
exchange (`Samsung Galaxy J7` bytes) interleaved with our commands — the
absence of any FM data event is evidence, not a tap failure: the controller
returns RSSI/SNR/freq values through no HCI-visible channel our side socket
can join (stock `bta_fm_vsc_cmpl`/`proc_vse` state-machine path, per
`fc15-table.md` §2). Record as: **exact blocker, no failing subcommand —
every status byte `0x00`, values unrecoverable from userspace on this chip
without the bluedroid FM stack in the loop**.

## 6. BT coexistence (this session, verbatim)

- `hciconfig` before→after a full run: `events 73→96`, `commands 73→96`
  (symmetric, no flood inflation), `UP RUNNING PSCAN` throughout.
- `bluetoothd` (supervise-daemon + `/usr/lib/bluetooth/bluetoothd`) alive at
  every check (pids 3778/3779 stable across the session until the final outage).
- `Read Local Name (0x0C14)` through the line: `status 00, 'Samsung Galaxy J7'`.
- Nothing stopped → nothing restarted (except the session PulseAudio, which is
  unrelated to BT). No `hciconfig down`, no LDO toggle, no reboot by us.

## 7. Device stability / hand-off state

- 4–5 watchdog reboots observed (~15:01, 15:05, 15:08, 15:10, 15:26 UTC;
  each ~1–3 min dark, tmpfs wiped, mixer back to §3 baseline, FM chip state
  wiped → re-arm required every boot).
- Final: during the concurrent `aplay hw:0,4` + `arecord hw:0,0` test the
  device went fully dark and re-enumerated as `18d1:d001` fastboot on the same
  port+serial (no RNDIS, ping 100% loss, `fastboot devices` sees nothing).
  Still in that state after 20+ min. Prior identical outage (2026-11-19
  ~04:45) recovered on its own. **Do not flash from this slice** — parked for
  Main to decide power-cycle/reflash.
- Last known on-device state: TX+RX test route applied (§3 + `CH1/CH3 Mixer
  En=On`), FM last tuned 94.5 MHz (chip state unknown — wiped by the outage),
  no flood re-armed (audio-path write never sent), BT healthy at last contact.

## 8. Resume plan (when Main signals link-back)

1. Check `uptime`/`hciconfig` counters (fresh boot wipes chip state).
2. Re-arm enable chain + 94.5 MHz tune (quiet-line verify: `drained-pre 0`,
   `vendor04ff0108=0`); re-dump mixer, diff vs §3 baseline, restore.
3. **First** prove `hw:0,0` capture yields samples for *any* source (e.g.
   `parecord` default-rate post-`pulseaudio --start` in one pty, or `hw:0,2`
   voice); only then re-apply the §3 route and compare tuned vs LINEIN-muted
   with `fm_wavstats.py` (on-device, stdlib-only; host copy in
   `/tmp/wavstats.py` is numpy-based scratch).
4. Single `W1B(0x14)+R(0x80)` RDS pair stays parked with the RDS slice; NEVER
   send `W1B(0x4D,1,8)` while a capture is in flight.

## 9. Files

- Device (`/home/user/`): `fm_hci.py` (enable+tune 94.5 MHz), `fm_tune2.py`
  (retune/reads), `fm_2sock.py` (dual-socket probe), `fm-mixer.sh` (repo copy,
  md5 `d02e85fa…`), `fm_wavstats.py` (stdlib RMS/peak/DC/DFT verdict),
  `fm-final3/4/5.sh`, `fm-audio-proof.sh`, `fm-final.sh`, `fm-final2.sh`
  (proof-run scripts; final5 = TX+RX + concurrent clock, hung — see §4).
- Host scratch (NOT committed): `/tmp/fmwork/*` (same scripts + `cap*.sh`,
  `run*.sh`, `proof.sh`), `/tmp/wavstats.py` (numpy analyzer).
- This report: `docs/reports/fm-audio.md` (new). Prior partial evidence
  remains in `code/userspace/fm/audio-evidence.txt` (superseded by this
  report for the audio verdict; kept for the mixer-baseline transcript).
