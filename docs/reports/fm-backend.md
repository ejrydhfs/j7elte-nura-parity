# FM backend — BCM43438 HCI VS 0xFC15: recovered table, `bcmfm` daemon, live-device evidence, precise blocker

Date: 2026-11-18/19 UTC. Chip: Broadcom BCM43438 (FM RX 65–108 MHz, RDS/RBDS) inside the
Wi-Fi/BT combo; no second tuner chip on j7elte. Stock path is Android-only
(`HybridRadio2016_M` → Samsung `FMRadioService` → Broadcom `IFmReceiverService` in the
stock Bluetooth app → bluedroid `bta_fm_*` → **HCI vendor opcode `0xFC15`**). Prior static
analysis: `docs/reports/fm-radio.md`. This report covers the userspace backend built on top
of it: `code/userspace/fm/`.

## 1. Verdict

| Question | Result |
|---|---|
| 0xFC15 subcommand table | **Recovered** — every byte grounded in disassembly (`fc15-table.md` + `re-notes.txt`) |
| `bcmfm` daemon | **Implemented, host-builds clean** (`bcmfm.h/.c`, `bcmfm-cli.c`, `Makefile`, `hci_raw.h/.c`, `install.sh`, `packaging-notes.txt`) |
| Live enable + tune | **Proven**: enable chain + tune/commit at 11 band points, all Command Complete `status=0x00` (`live-fc15-log.txt`) |
| Live RSSI/SNR/freq readback | **Blocked**: every R-type CC returns a dataless echo (6 bytes, no value bytes) |
| Live audible audio | **Not proven**: zero audio bytes captured (mixer route applied OK; capture failed on full rootfs → reboot-wiped tmpfs → USB link loss) (`audio-evidence.txt`) |
| Live RDS | **Not attempted on wire**: single W1B(0x14)+R(0x80) pair held, never sent (link lost first) |
| BT health | **Undisturbed**: hci0 UP RUNNING throughout; bluetoothd never stopped; final sanity `Read Local Name → status 00` |
| Device link at hand-off | **DOWN**: USB half-enumerated RNDIS, no driver bound, no net iface; parked per Main, resume on ping |

BT was never broken: no `hciconfig down`, no service stop, no LDO toggle, no reboot by us.

## 2. Recovered 0xFC15 table (evidence)

Source blobs (world-readable stock image):
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7stock/system.raw`,
`lib/hw/bluetooth.default.so` md5 `681475c3e9e45d2c2a6b63bae79ad2e3`,
`lib/libbluetooth_jni.so` md5 `31b7ab03d37f005d74f5f1b1c752547e`, extracted via
`debugfs -R 'cat …'`. Method: `readelf` dynsym + capstone thumbv2 disassembly of all 132
`*fm*` symbols; 54 call sites into the send helpers tabulated with register triples
(`code/userspace/fm/re-notes.txt`, full per-site context kept in `/tmp/fmx/` scratch).

Transport (observed, `bta_fm_snd_vendor_cmd @ 0xc46d0`):
`movw r0, #0xfc15` → `BTM_VendorSpecificCommand(opcode, datalen+2, [cmd, arg, data…])`.
Helper encoding (observed — the whole "arg byte" scheme): `write_1b` ORs `0x10`,
`write_2b` ORs `0x20`, `write_search_boundary` ORs `0x40` (fixed `cmd=0xFB`, two LE u16),
`read` builds `0x11/0x19` (`+0x80` if bit7). All FM traffic funnels through the `bta_fm_vcmd_*`
queue; `bta_fm_proc_vsc` matches completions to the queue head; `bta_fm_check_reg_param`
dispatches on the CC return-parameter subcommand echo (`0x0A/0x12/0x4D/0x90/0xDF/0xF8/0xFC/0xFF/0x0F/0x80`).

Command table (all rows observed unless marked; full quotes in `fc15-table.md`):

| sub | payload | function |
|---|---|---|
| 0x00 | `W1B(0x00, region=1/3, 0)` | enable |
| 0x02 | `W1B(0x02, 2/3, 0)` | init step |
| 0x01 + 0xFB | `W1B(0x01, class, 0/0x88)` + `W4B(0xFB, lo, hi)` | region band edges (region table) |
| 0xFD | `W2B(0xFD, step-table, 0x88)` | scan step (100/50 kHz) |
| 0x4D | `R(0x4D,0,1)` / `W1B(0x4D, val, 0/8)` | audio-path power-domain bit |
| 0x05 | `W2B(0x05, word, 0x88)` | master config word: b0 RF-mute, b1 mute, b6 de-emphasis (bit names [INFERENCE], positions observed) |
| 0x0A + 0x09 | `W2B(0x0A, freq, 0)` + `W1B(0x09, 0/1, 8)` | tune + commit |
| 0x14/0x16/0x18/0xFC | scan-engine preset/park/result | seek up/down (`W1B(0x14)`, `W2B(0x16/0x18)`, `R(0xFC,0,1)`) |
| 0x09 | `W1B(0x09, 1, 8/0x88)` | abort |
| 0xF8 | `W2B(0xF8, vol, 0)` + `R(0xF8,0x80,2)` | volume |
| 0x0F / 0xDF | `R(0x0F,0x80,1)` / `R(0xDF,0x80,1)` | RSSI / SNR |
| 0x80 + 0x14 | `W1B(0x14, cfg, 0)` + `R(0x80,0,0xF0)` | RDS mode + poll |
| 0x14 | `W1B(0x14, cfg, 0)` | RDS set (same primitive) |
| 0x08/0xDE/0x07/0xFE | gated criteria bytes | seek thresholds, AF jump, NFE (`W1B(0xFC,2,0)+W1B(0xFE,0,8)+W1B(0x07,0xB2,8)`) |
| 0x10/0x12/0x88 | `R(0x12)+W2B(0x10,mask)` | mask read-modify-write (`set_mask`) |
| 0x0B | `W1B(0x0B, cmp, 0)` | de-emphasis compare |
| 0xFF | `R(0xFF, …)` | chip-ver/RSSI-cal [INFERENCE] |

JNI boundary (`libbluetooth_jni.so` `0x23e2d..0x2401c`): `enableFmNative`,
`disableFmNative`, `tuneNative`, `muteNative`, `searchNative`, `comboSearchNative`,
`searchAbortNative`, `setRdsModeNative`, `setAudioModeNative`, `setAudioPathNative`,
`setScanStepNative`, `setRegionNative`, `configureDeemphasisNative`, `estimateNoiseFloorNative`,
`setFMVolumeNative`, `getCurrentRSSINative`, `getCurrentSNRNative`, `setRFMuteNative`,
`setFMIntennaNative`, … registered by `register_com_broadcom_fm_service`; each maps 1:1 onto
a `BTA_Fm*` post (`0x2300..0x2326`). Minimal power/tune/audio sequence:
`W1B(0x00,region)` → `W1B(0x02,2/3)` → `W4B(0xFB,lo,hi)` → `R(0x4D)` → `W2B(0x05,init,0x88)` →
`W1B(0x4D)` audio bit + `W2B(0x05,path-word,0x88)` → `W2B(0x0A,freq)` tune.
Bring-up order with app/framework constants (freq 8750–10800, `audioPath=1`,
`deemphasis=64`, `funcMask=0x50`): `fm-bringup-seq.md`.

## 3. `bcmfm` daemon

`code/userspace/fm/`: `hci_raw.h/.c` (raw `AF_BLUETOOTH`/`SOCK_RAW`/`BTPROTO_HCI` transport
bound to `HCI_CHANNEL_RAW`; H4 `01 + opcode-LE + plen + payload` send; `poll()`-timeout event
loop filtering Command Complete `0x0E` by opcode, passing vendor `0xFF` to caller),
`bcmfm.h/.c` (FM layer: builders cite table §0; every command waits for CC + checks status;
`--raw` escape hatch), `bcmfm-cli.c`
(`info|enable [region]|disable|tune <MHz>|seek up|down|abort|mute|rfmute|vol|rds|region|step|path|deemph|dump|raw`),
`Makefile` (`CROSS_COMPILE`-prefixable, `-Wall -Wextra -Werror -std=c11`, libc only),
`install.sh` (`sh -n` clean; `setcap cap_net_admin+ep` or root), `packaging-notes.txt`
(aport outline + OpenRC loopback sketch + manifest). Verified: `make clean && make`
zero-warning host build; `./bcmfm --help` lists all commands. No device touched during
implementation.

## 4. Live-device output (real chip responses)

Chip (read-only, before any FM write): `hciconfig -a hci0` → BD `43:43:A1:12:1F:AC`,
`UP RUNNING PSCAN`, HCI 4.0 rev `0xe6`, LMP 4.0 sub `0x2209`, Broadcom (15);
ALSA card 0 `Universal7580-I2S`: playback `hw:0,0–6` incl. `hw:0,4 'FM audio cod3022x-aif-4'`,
capture `hw:0,0/2/3`; 105 `amixer -c0 scontrols`. Raw-HCI proven viable from python3
(`AF_BLUETOOTH`, `HCI_FILTER=all`, H4-prefixed sends): CC for `0x1003` + `0x0C14`
(`'Samsung Galaxy J7'`). Full baseline: `probe-baseline.txt`.

Enable + tune (verbatim tx/rx in `live-fc15-log.txt` §§2–5, RETUNE-1/2/3 across three
watchdog reboots, all `status=0x00`):
`W1B(0x00,1,0)` → `040e060115fc000010`; `W1B(0x02,2,0)` → `…000210`;
`W4B(0xFB,8750,10800)` → `…00fb48`; `R(0x4D,0,1)` → `…004d11` (dataless);
`W2B(0x05,0x001D,0x88)` → `…0005a8`; tune `W2B(0x0A,9500)` → `…000a20`,
commit `W1B(0x09,0,8)` → `…000918`; band sweep 88–107.5 MHz (11 points) all tune/commit
`status=00`. FM left enabled @ 94.0 MHz (last committed tune; on-chip state after the final
outage unknown — re-arm on resume).

## 5. Precise blocker (no failing subcommand — every status byte is `0x00`)

a) **Dataless R-type command-completes.** `R(0x4D)/R(0x0A)/R(0x0F)/R(0xDF)` CCs are all
6 bytes (`040e060115fc00` + 2-byte subcmd/arg echo) with zero value bytes, although the
stock `check_reg_param` documents value parsing (`cb+0x98/0xa4/0xd2`). Values likely arrive
as async vendor events consumed by the bluedroid FM state machine (`bta_fm_vsc_cmpl`/
`proc_vse` path) which a side-channel raw socket cannot join — or require the audio-path
bit first. Consequence: no RSSI peak observable, no freq/SNR readback; "tune success" is
command-acceptance only, not confirmed signal.

b) **`W1B(0x4D,1,8)` (audio-path write) triggers a persistent spontaneous `04ff0108`
vendor-event flood** (~2.9k evts/s, still flowing 10+ min later, re-armed by the same write
after a reboot, proving command-triggered). The documented disable leg
(`W1B(0x4D)+W2B(0x05,0x0002,8)+W1B(0x00,0,0x88)`, all `status=00`) does **not** stop it.
The flood drowns every subsequent CC window and makes any future data-bearing event
unrecoverable on that boot. Mitigation for the daemon: `HCI_FILTER` accept only `0x0E`,
drain-before-send, short windows (recipe for `bcmfm` follow-up; `rds-coex` parked
unsent because the link died first).

## 6. Audio evidence (partial — no verdict possible)

Capture-side route from `fm-mixer.sh` (record path: `S2801 SRC3 En`, `RMIX2`, `CH3 Rec`,
`ADCL←LINELL`/`ADCR←LINELR`, `LINEIN On`, Line-in 4, ADC 127/127) applied clean
(`ROUTE-DONE n=20`, all csets exit 0, no missing controls; full mapping in
`fm-audio-route.md`). Captures all failed with **zero bytes**: (a) `arecord hw:0,0`
48 kHz S16 stereo killed by wrapper timeout (rootshell-private `/tmp` unrecoverable);
(b) home-dir write failed — rootfs `/dev/mmcblk0p20` **100% full**; (c) tmpfs target
`/run/user/10000/` wiped by watchdog reboot #2 mid-capture; (d) chunked plan started,
then the USB link died. No RMS/spectrum numbers exist. Revert **not verified** (link lost
first); last known mixer state = §1 snapshot + the 20 touched controls — re-dump, diff,
and restore on resume. Full account: `audio-evidence.txt` §1–7.

## 7. BT coexistence

`bluetoothd` (supervise-daemon) running at every check (pids 3949/3950 → 3956/3957 across
reboots); hci0 `UP RUNNING PSCAN` before/after; `Read Local Name (0x0C14)` through the
flood → `status 00, 'Samsung Galaxy J7'`. RX/event counters inflated only by the FM flood.
Nothing stopped → nothing restarted.

## 8. Resume plan (when Main pings link-back)

1. Check uptime/counters; re-arm enable chain + 94.0 MHz tune (quiet-line verify first).
2. Re-dump mixer, diff vs `audio-evidence.txt` §1, restore-then-reapply route.
3. Capture ≤5 s chunks to `/run/user/10000/` (rootfs full!), `scp` off promptly, chunked
   `arecord`; analyze RMS/spectrum vs LINEIN-muted control → non-silence verdict.
4. Single `W1B(0x14)+R(0x80)` RDS pair with `0x0E`-only filter + drain; **never** re-send
   `W1B(0x4D,1,8)` while a capture is in flight.
5. Fold RSSI/RDS/audio numbers back into this report.

## 9. Files (all under `code/userspace/fm/`)

`hci_raw.h/.c` transport · `bcmfm.h/.c` FM layer · `bcmfm-cli.c` CLI · `Makefile` ·
`install.sh` · `packaging-notes.txt` · `fm-mixer.sh` (route, not yet run to playback) ·
`fm-audio-route.md` · `fc15-table.md` · `re-notes.txt` · `fm-bringup-seq.md` ·
`probe-baseline.txt` · `live-fc15-log.txt` · `audio-evidence.txt` (partial) ·
`rds-coex.txt` (parked partial: pair held unsent, coex numbers + mitigation recipe).
No kernel/DT/firmware changes. BT undisturbed.
