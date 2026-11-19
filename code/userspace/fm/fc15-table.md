# BCM43438 FM 0xFC15 sub-command table

Recovered from stock `bluetooth.default.so`
(`681475c3e9e45d2c2a6b63bae79ad2e3`, from
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7stock/system.raw`
via `debugfs -R 'cat lib/hw/bluetooth.default.so'`, identical to prior
`/tmp/fmre/bluetooth.default.so`)
and `libbluetooth_jni.so` (`31b7ab03d37f005d74f5f1b1c752547e`).
Scope: ONLY the HCI vendor transport + FM sub-command bytes. No ALSA/audio
routing here (see `fm-audio-route.md`, owned by a sibling slice).

## 0. Transport (observed, `bta_fm_snd_vendor_cmd @ 0xc46d0`, size 0x4c)

```asm
0x0c46f6: ldr        r3, [pc, #0x20]
0x0c46f8: adds       r1, r5, #2
0x0c46fa: mov        r2, r4
0x0c46fc: movw       r0, #0xfc15
0x0c4700: uxtb       r1, r1
0x0c4702: add        r3, pc
0x0c4704: bl         #0xe7eb8        ; BTM_VendorSpecificCommand(opcode=r0, len=r1, buf=r2)
```

- HCI opcode is `0xFC15` (`movw r0,#0xfc15`), payload `[cmd, arg, data...]`.
- Entry: `r0=cmd_byte, r1=arg_byte, r2/r3=data,len` — copies data after a 2-byte
  header, so length passed to `BTM_VendorSpecificCommand` is `datalen+2`.
- All FM traffic funnels through the `bta_fm_vcmd_*` queue
  (`vcmd_enq @0xc4570`, `vcmd_qhead @0xc4610`, `vcmd_deq @0xc45c8`,
  `vcmd_pending @0xc4674`, `vcmd_empty @0xc4538`, `vcmd_no_pending @0xc4554`,
  `post_msg/bta_sys_sendmsg @0xc46a0`); `bta_fm_vsc_cmpl @0xc2a7c` dequeues the
  next queued command on every command-complete.
- `bta_fm_proc_vsc @0xc2ebc` matches a command-complete to the queue head:
  `ldrb r2,[sp,#7]` (completed cmd) vs `ldrb r0,[r5,#0xd]` (expected cmd),
  then `bl #0xc45c8` (deq). Unmatched completions are dropped (`#0xc2f9e`).
- `bta_fm_proc_vse @0xc2e98` is only a 4-instruction stub: stop timer
  `+0x244` (`bl #0xbe528` = `bta_sys_stop_timer`), then
  `READ(0x12, r1=0, r2=2)`. No vendor-event parsing lives here.

### Helper encoding (observed — this is the whole "arg byte" scheme)

| helper | addr | observed body | resulting `arg` | payload |
|---|---|---|---|---|
| `bta_fm_snd_vendor_cmd` | 0xc46d0 | raw send | caller-supplied | `[cmd, arg, data...]` |
| `bta_fm_vnd_cmd_write_1b` | 0xc4784 | `orr r2,r2,#0x10` | `flags\|0x10` | `[cmd, arg, 1 data byte]` (`r1`) |
| `bta_fm_vnd_cmd_write_2b` | 0xc479c | `orr r2,r2,#0x20`; `strb r1; lsrs; strb` | `flags\|0x20` | `[cmd, arg, LE u16]` (`r1`) |
| `bta_fm_write_search_boundary` | 0xc47b8 | `orr r2,r2,#0x40`; `movs r0,#0xfb` | `flags\|0x40` | `[0xFB, arg, lo0,hi0, lo1,hi1]` (two LE u16: `r0`=lower, `r1`=upper) |
| `bta_fm_vnd_cmd_read` | 0xc47e0 | `tst r4,#8 → r2=#0x19/#0x11`; `lsls r3,r4,#0x18; it mi; orrmi r2,#0x80` | `(r1&8 ? 0x11 : 0x19)` + `0x80` if `r1` bit7 | `[cmd, arg]` read request; special-case `sp[9]==0xf9` re-sends stored cmd via `snd_vendor_cmd` |
| `bta_fm_set_mask` | 0xc485c | `READ(0x12,0,2)` then `W2B(0x10, sb, (r1\|8))`; then `bta_sys_start_timer` if `r1!=0` | — | read-modify-write bitmask helper, `r0`=cache ptr (cb), `r1`=mask, `r2`=timer ticks, `r3`=event |

So `arg` high bits select length: `0x1x`=1 byte, `0x2x`=2 bytes,
`0x4x`=4 bytes, low bits are per-command flags; bit7 = read. Evidence:
`write_1b/2b` OR-in `0x10/0x20`, `write_search_boundary` ORs `0x40` with
`cmd=0xFB` fixed, `read` builds `0x11/0x19 (+0x80)`.

Response side (observed): `bta_fm_check_reg_param @0xc48bc` dispatches on the
*command-complete return parameter* `r3` (= sub-command echo) with cases
`0x0A` (freq → `cb+0x98`), `0x12` (RSSI word → mute/scan logic),
`0x4D` (audio path → `cb+0xa5`), `0x90` (?), `0xDF` (`cb+0xd2`),
`0xF8` (?), `0xFC` (?), `0xFF` (RDS text), `0x0F` (`cb+0xa4`),
`0x80` (RDS block loop calling `bta_fm_co_*_rds_data @0x8de44`).
Completion for `0x80` scans the payload for `7C FF FF` triplets otherwise
feeds `bta_fm_co_rds_data`. This is the ONLY response-byte parser in the FM
module. Vendor event `0xFF` handling is in the generic BTM layer, not FM.

## 1. Command table (observed unless marked)

`W1B(cmd,data,flags)` = `write_1b`, `W2B(cmd,u16,flags)` = `write_2b`,
`R(cmd,r1,r2)` = `read`.

| sub | arg / payload | wrapper (addr) | evidence (disassembly quote) | status |
|---|---|---|---|---|
| 0x00 | `W1B(0x00, r1=0x03, flags=0)` | `bta_fm_q_evt @0xc2c58` / `bta_fm_check_rds @0xc2cb8` | `movs r0,#0; movs r1,#3; mov r2,r0; bl #0xc4784` then `R(0x0A,0,2)` | observed |
| 0x01 | `W1B(0x01, r2=0, flags=0x88)` (r1 = 0/1/2/3 = region class) | static `sub_2264 @0xc2264` (called by `init_delay`, `set_region`, `set_step`) | `movs r0,#1; b.w #0xc4784` with `movw r0,#1; ite hi; movhi r1,#2; movls r1,#3` region mapping | observed |
| 0x02 | `W1B(0x02, r1=2/3, flags=0)`; `W1B(0x02, r5=chipflags, flags=0x88)` in `chg_rds_type/do_nfe` | `bta_fm_init_delay @0xc22c8`, `bta_fm_chg_rds_type @0xc2e40` | `movs r0,#2; movs r2,#0; tst r3,#0x20; ite eq; moveq r1,r0; movne r1,#3; bl #0xc4784` | observed |
| 0x02 | also `W2B(0x05, ...)` context; `chg_rds_type` path `W1B(0x02,flags=0x88)` | same | `movs r0,#2; movs r2,#0x88; b.w #0xc4784` | observed |
| 0x03 | — (BT event id only) | `bta_fm_q_evt` in-band event `0x2301` compare | `movw ip,#0x2301; cmp r5,ip` — NOT an HCI subcmd | observed (negative) |
| 0x05 | `W2B(0x05, u16, flags=0x88)` + variants `flags=0x08/0x00` | `bta_fm_init_delay`, `bta_fm_scan_freq`, `bta_fm_mute_aud @0xc2cf2`, `bta_fm_set_aud_mode @0xc2b0c`, `bta_fm_set_audio_path`, `bta_fm_set_rfmute @0xc3150`, `bta_fm_set_deemphasis @0xc2c1a`, `bta_fm_cfg_blend_mute`, `bta_fm_update_maint @0xc31a0`, `live_update sub_21fc @0xc21fc` | `movs/mov.w r0,#5` then `bl #0xc479c`; e.g. mute: `movs r0,#5; movs r2,#0x88; b.w #0xc479c` with bit1 mute / bit5 blend / bit4 deemph / bit0 RF-mute ORed into cached `cb+0xa6` word | observed |
| 0x07 | `W1B(0x07, r1=cb+0xcd, flags=0x08)` | `bta_fm_scan_freq` (seek path) | `movs r0,#7; ldrb.w r1,[r5,#0xcd]; bic r3,r2,#0xf; movs r2,#8; orrs r1,r3; bl #0xc4784` — search criteria / SNR-ish threshold merged with `cb+0xb4` nibble | observed |
| 0x08 | `W1B(0x08, r1, flags=0)` | `bta_fm_scan_freq` | `movs r0,#8; movs r2,#0; bl #0xc4784` iff `cb+0xce > 0x1f` (RSSI-threshold gate) | observed |
| 0x09 | `W1B(0x09, r1=0/1 flags=0x08/0x88)`; `W2B(0x0A, u16, flags=0x88)` pair | `bta_fm_abort @0xc2a4c`, `live_update sub_21fc`, `cur_freq @0xc3560`, `flag_tout` | abort: `mov.w r0,#9; ite ne; movne r2,#8; moveq r2,#0x88; b.w #0xc4784`; live: `movs r0,#9; movs r2,#8; b.w #0xc4784` after `READ(0x0A...)` | observed |
| 0x0A | `R(0x0A, r1=0, r2=2)` → freq/status word (also `R(0x0A, r1=0x80, r2=2)`) | `q_evt`, `check_rds`, `cur_freq`, `update_maint` | `movs r1,#0; movs r0,#0xa; movs r2,#2; bl #0xc47e0`; volume path uses `r1=#0x80` | observed |
| 0x0B | `W1B(0x0B, r1=ip&0x40, flags=0)` | `bta_fm_set_deemphasis` | `ldrb r3,[r1,#6]; and ip,r1,#0x40; cmp r3,ip; ... movs r0,#5 ...` deemph bit is `cb+0xa6` bit6, cmd `0x0B` carries the compare value (`movs r2,#0xb`) | observed |
| 0x0D | `W1B(0x0D=state, ...)` (opportunistic: shares bytes with timer id) | `bta_fm_do_nfe @0xc2da5` | `movs r0,#0xd; movs r1,#2; movs r2,#0; strb.w r0,[r5,#0xa3]; movs r0,#0xfc; bl #0xc4784` — `0x0d` here is the *state-machine* id, HCI cmd is the following `0xFC` | observed (negative: not an HCI cmd) |
| 0x0F | `R(0x0F, r1=0/0x80, r2=1/2)` → RSSI (`cb+0xa4` via check `0x0F`) | `bta_fm_get_rssi @0xc3130`, `cur_freq`, `update_aud_data @0xc2be8`, `bta_volume_ctrl` | `movs r3,#0x14; movs r1,#0x80; movs r2,#1; ... movs r0,#0xf; b.w #0xc47e0` | observed |
| 0x10 | `R(0x10, r1=sb&0xff?, r2=0)` → 2-byte RSSI/status word | `bta_fm_update_maint` | `movs r0,#0x10; ldrh.w r1,[r3,#0xae]; movs r2,#0; bl #0xc479c`… wait — this site is W2B; the R site is `bta_fm_set_mask`'s `READ(0x12..)` + `W2B(0x10, sb, r1\|8)` (see set_mask) | observed |
| 0x10 | `W2B(0x10, mask16, flags=0x88)` | `bta_fm_set_mask` + `read_rds` completion | `movs r0,#0x10; mov r1,sb; orr r2,r6,#8; bl #0xc479c` | observed |
| 0x12 | `R(0x12, r1=0, r2=2)` | `bta_fm_set_mask`, `bta_fm_proc_vse` | `movs r0,#0x12; movs r1,#0; movs r2,#2; bl #0xc47e0` — mask-cache refresh | observed |
| 0x14 | `W1B(0x14, r1=cfg, flags=0)` + `R(0x80, 0, 0xF0)` pair | `bta_fm_sch_cmpl_act @0xc36b8`, `bta_fm_set_rds_mode`, `bta_fm_get_rssi`? no — `scan_freq` dir=0 leg | `movs r0,#0x14; movs r2,#0; bl #0xc4784` (dir byte from event: 0 = stop/park) then optionally `R(0x80…)` | observed |
| 0x16 | `W2B(0x16, u16=0x3e0/0x400, flags=0)` then `W2B(0x18, r8, 0)` | `bta_fm_scan_freq` seek-up/down legs | `mov.w r8,#0x3e0; lsls r1,r0,#5` (dir=1: preset word) / `mov.w r1,#0x400` (dir=2) ; `movs r0,#0x16; movs r2,#0; bl #0xc479c` | observed |
| 0x18 | `W2B(0x18, u16=r8, flags=0)` | same | `movs r0,#0x18; mov r1,r8; movs r2,#0; bl #0xc479c` | observed |
| 0x4D | `W1B(0x4D, r1, flags=0)` (power: 1=enable-value?/0=off) ; `R(0x4D, r1=0, r2=1)` | `bta_fm_init_delay`, `bta_fm_enable`, `bta_fm_disable`, `bta_fm_set_audio_path` | enable: `mov r2,r6(=0); mov r0,r6(=0); mov r1,r7(=1/3); bl #0xc4784` — data byte IS region class (1=EU/US?,3=other), NOT power; disable: `movs r0,#0x4d; bl #0xc4784` with r1 = cached `a5&0x7f`; init: `R(0x4D,0,1)` | observed |
| 0x80 | `R(0x80, r1=0, r2=0xF0)` → RDS/group data | `bta_fm_read_rds`, `sch_cmpl_act` | `movs r0,#0x80; cbz r1...; movs r1,#0; movs r2,#0xf0; bl #0xc47e0`; completion continues into `set_mask(0x88,0,0x3a98)` in the `r1==NULL` leg | observed |
| 0x88 | `set_mask(cb, mask=0x88, ticks=0, evt=0x3a98)` → `R(0x12)+W2B(0x10,...)` | `read_rds` tail, `set_audio_path` tail, `set_rds_mode` tail | `movs r1,#0x88; movs r2,#0; movw r3,#0x3a98; bl #0xc485c` | observed |
| 0xDE | `W1B(0xDE, r1=cb+0xcf, flags=0)` | `bta_fm_scan_freq` (only if `cb+0xd0 != 0`) | `movs r0,#0xde; ldrb.w r1,[r5,#0xcf]; movs r2,#0; bl #0xc4784` — BTA_FmSetSchCriteria byte (`cf`) gated by `d0` flag | observed |
| 0xDF | `R(0xDF, r1=0/0x80, r2=1)` → SNR / audio quality | `bta_fm_get_snr @0xc3140`, `update_aud_data`, `cur_freq` | `movs r3,#0x15; movs r1,#0x80; movs r2,#1; ... movs r0,#0xdf; b.w #0xc47e0` | observed |
| 0xF8 | `W2B(0xF8, u16=r1, flags=0)` then `R(0xF8, 0x80, 2)` | `bta_volume_ctrl @0xc3078`, `bta_fm_cfg_blend_mute` | volume: `movs r0,#0xf8; movs r2,#0; bl #0xc479c` (vol≤0xff) then `movs r0,#0xf8; movs r1,#0x80; movs r2,#2; b.w #0xc47e0`; blend path same cmd with `bic r1,r2,#1` | observed |
| 0xFB | `W4B(0xFB, r0=lo-freq, r1=hi-freq, flags)` = search boundary pair | static `sub_2264` (freq-range program) | `movs r0,#0; movs r2,#8; mov r1,r0; bl #0xc47b8` (default) or register-loaded `bl #0xc47b8` with computed `(freq,freq±delta)`; callers: `init_delay`, `set_region`, `set_step`, scan-freq tail (`b.w #0xc21fc` → `sub_21fc: W2B(0x0A..)` + `W1B(0x09..)` + `W4B` chain) | observed |
| 0xFC | `W1B(0xFC, r1=sb&1, flags=0)` ; `W1B(0xFC, 3, 0)` (force-mono?) ; `R(0xFC, 0, 1)` | `bta_fm_scan_freq`, `bta_fm_do_nfe` | `movs r0,#0xfc; mov r1,sb; movs r2,#0; bl #0xc4784` (stereo blend bit), then optional `movs r1,#3` variant when seeking; `R(0xFC,0,1)` reads seek result into `cb+0xce` | observed |
| 0xFD | `W2B(0xFD, table[r3], flags=0x88)` (step table `0xfcd08`) | `bta_fm_set_step @0xc2d18` | `ldr r1,[pc,#0x34] (0xfcd08); lsrs r3,r0,#4; movs r2,#0x88; movs r0,#0xfd; ldrh.w r1,[r1,r3,lsl#1]; bl #0xc479c` | observed |
| 0xFE | `W1B(0xFE, r1=0/sl, flags=0x08)` | `bta_fm_do_nfe`, `bta_fm_scan_freq` | `movs r0,#0xfe; movs r1,#0; movs r2,#8; bl #0xc4784` (NFE leg) ; scan leg `mov r1,sl` (AF jump freq word low) | observed |
| 0xFF | `R(0xFF, r1, r2=(r0<<2)&0xfc)` → chip-ver / RSSI cal? | `bta_fm_cur_freq` (`sub_21fc` path) | `ldrb r0,[r3,#0x31]; lsls r2,r0,#2; movs r0,#0xff; and r2,r2,#0xfc; b #0xc47e0`; and `movs r0,#0xff; movs r1,#0; movs r2,#0xc8` variant | observed |

Volume (observed): `BTA_FmVolumeControl @0xc412c` posts BT event `0x2310`
with the u16 volume; `bta_volume_ctrl @0xc3078` converts to
`W2B(0xF8, vol, 0)` (+ `R(0xF8,0x80,2)` verify). Mute (observed):
`BTA_FmMute @0xc3f72` posts `0x2306`; `bta_fm_mute_aud` sets/clears bit1 of
cached `cb+0xa6` and `W2B(0x05, word, 0x88)`. RF-mute (observed):
`BTA_FmSetRFMute @0xc41cc` posts `0x2314`; `bta_fm_set_rfmute` sets/clears
bit0 of `cb+0xa6` via `W2B(0x05, word, 0x88)`.

Audio path/mode (observed): `BTA_FmConfigAudioPath @0xc3f18` posts `0x2309`;
`bta_fm_set_audio_path` does `W1B(0x4D, a5, 8)` (power-domain bit7 dance on
`cb+0xa5`) then `W2B(0x05, word|0x20/&~0x20 (I2S?) |±0x10, 0x88)`.
`BTA_FmSetAudioMode @0xc3f98` posts `0x2305`; `bta_fm_set_aud_mode` maps the
mode through table `0xefda4` (`6A 64 67 ...`) and `W1B(0x01, mapped, 0x88)`.
De-emphasis (observed): `BTA_FmConfigDeemphasis @0xc3f54` posts `0x230b`;
`bta_fm_set_deemphasis` toggles bit6 (`0x40`) of `cb+0xa6` via `W2B(0x05)` and
sends `W1B(0x0B, cmp, 0)`.

Region/step (observed): `BTA_FmSetRegion @0xc3fae` posts `0x230c`,
`BTA_FmSetScanStep @0xc3f90` posts `0x2307`; both funnel to
`static sub_2264`: region class 0..5 → `W1B(0x01, ...)` + `W4B(0xFB, lo, hi)`
band edges from the region table (`bta_fm_cfg @0x1c03d4`), step enum →
`W2B(0xFD, table)` (`bta_fm_sch_step_size = {0x64,0x32}` = 100/50 kHz);
`set_region` id `0x0c` and `set_step` id `0x07` are only `cb+0xa3` state tags.

RDS (observed): `BTA_FmSetRDSMode @0xc3eb8` posts `0x2308` (+`0x231b` RBDS);
`bta_fm_set_rds_mode @0xc31fc` (0x2d8 bytes) does `W1B(0x14, rdsbit, 0)`
then `R(0x80,0,0xF0)` then `set_mask(0x88,0,0x3a98)`; polling path
`bta_fm_read_rds` = `R(0x80,0,0xF0)`; `check_rds`/`q_evt` = `W1B(0x00,3,0)` +
`R(0x0A,0,2)` then signal setting via `set_mask(code,0,0x2bc)`.
RDS decode itself (`BTA_FmRDSDecode @0x14b434` etc.) is pure ARM, no HCI.

Seek/scan (observed): `BTA_FmSearchFreq @0xc3e20` posts `0x2303`,
`BTA_FmComboSearch @0xc3fea` posts `0x2311`, `BTA_FmSearchAbort @0xc3e7e`
posts `0x2320`, `BTA_FmTuneFreq @0xc3e60` posts `0x2304`;
`bta_fm_scan_freq @0xc24fc` (0x550 bytes) is the shared engine:
`W1B(0x14,...)` park → `W2B(0x16,...)`/`W2B(0x18,...)` preset →
`W2B(0x05,|0x02)` unmute-ish → `W1B(0xFC,sb)` stereo → `R(0xFC,0,1)` result →
gated `W1B(0x08)` + `W1B(0xDE)` criteria → `W1B(0xFE,sl,8)` AF jump →
`W1B(0x07,...)` threshold. Tune (observed): `bta_fm_tune_freq @0xc2474`
validates against the region table then `b.w #0xc21fc` =
`W2B(0x0A, lo16(freq?), 0)` + `post_msg(0x2319)` + `W1B(0x09, 0/1, 8)`.

Threshold/NFE (observed): `BTA_FmEstNoiseFloor @0xc3fcc` posts `0x230d`
(`bta_fm_start_nfe @0xc42cc`), `bta_fm_do_nfe` = `W1B(0xFC,2,0)` +
`W1B(0xFE,0,8)` + `W1B(0x07,0xB2,8)`; `BTA_FmConfigBlendSoftMuteParams`
posts `0x230f` → `bta_fm_cfg_blend_mute` → `W2B(0x05,bic#1,...)` +
`W2B(0xF8,...)` curves. `BTA_FmSetSignalNotifSetting` only sets a RAM
threshold (`[r1,#0x24]`), sends NO HCI (observed negative).

RSSI/SNR (observed): `BTA_FmGetCurRSSI @0xc414c` posts `0x2312` →
`bta_fm_get_rssi` = `R(0x0F,0x80,1)`; `BTA_FmGetCurSNR @0xc418c` posts
`0x2313` → `bta_fm_get_snr` = `R(0xDF,0x80,1)`. JNI names
(`libbluetooth_jni.so` `0x23e2d..0x2401c`: `enableFmNative`,
`disableFmNative`, `tuneNative`, `muteNative`, `searchNative`,
`comboSearchNative`, `searchAbortNative`, `setRdsModeNative`,
`setAudioModeNative`, `setAudioPathNative`, `setScanStepNative`,
`setRegionNative`, `configureDeemphasisNative`, `setFMVolumeNative`,
`getCurrentRSSINative`, `getCurrentSNRNative`, `setRFMuteNative`, …)
map 1:1 onto these BTA posts (observed string table; JNI bodies are
thunks into `btif_fm_*`, not decoded — marked inference).

Enable/disable (observed): `BTA_FmEnable @0xc3d78` posts `0x2300`
(`bta_fm_api_enable` stores `cb+0x9a/0x9c/0xa2`), `BTA_FmDisable @0xc3dac`
posts `0x2301`; `bta_fm_enable @0xc2358` = `W1B(0x00,r7=1/3(region),0)` +
`bta_sys_start_timer(0x2325)`; `bta_fm_init_delay @0xc22c8` =
`W1B(0x02,2/3,0)` + `sub_2264(region)` + `R(0x4D,0,1)` +
`W2B(0x05,init|0x1c/0x1d,0x88)` + `HCI_RESET-ish post_msg(0x2318…)` …
`bta_fm_disable @0xc23e0` = `W1B(0x4D,a5&0x7f,0)` + `W2B(0x05,2,8)` +
`W1B(0x00,0,0x88)`; `bta_fm_disable_cmpl @0xc2425` stops 4 timers and sends
`HCI 0x0c03` (`movw r0,#0x0c03`? observed `0xfffffddf`+`0xe7f30` =
`BTM_VendorSpecificCommand`? no — `0xe7f30` is `btsnd_hcic_*`; exact
opcode bytes live in callee, marked inference).

## 2. Response/event parsing (observed)

- Command-complete (`bta_fm_vsc_cmpl`): pops the queue (`vcmd_deq`) and
  immediately sends the next queued 4-tuple as raw
  `snd_vendor_cmd(r0=[sp+1],r1=[sp+2..],r2..)` — pipelining is in this
  function, not in the callers.
- State completion (`bta_fm_int_cmpl @0xc38ac`, `cb+0xa3` state id,
  `tbh [pc,r2,lsl#1]` jump over `r7-1` in `1..0x16`): per-state handlers
  read the cached words (`cb+0x98` freq, `0xa4/0xa6/0xa8/0xae` quality,
  `0xb0..0xd2` scan/RDS block) and post BT events `0x2300..0x2326`
  (`BTA_FmEnable..SearchAbort` range) then `bta_fm_next_inq @0xc3d45`.
- `check_reg_param` case `0x12` bit tests (`&0x10`, `&0x800`, `<<0x16`,
  `<<0x1e`) drive `0x231b/0x231c/0x231e/0x2322` notifications — these are
  the seek-complete / stereo / RDS-sync signals (function, not wire
  format: marked inference for the semantic labels, observed for the
  bit tests + event ids).
- No `0xFCxx` call-site bytes other than the table above exist in ANY
  `bta_fm_*`/`BTA_Fm*`/`btif_fm_*` function: full-text scan of the 54
  `→ {SND,ENQ,W1B,W2B,W4B,READ,SETMASK}` call sites in `calls_out.txt`
  (from `calls.py`, kept in `/tmp/fmx/`) shows no other `movs r0,#imm`
  flowing into `0xc4784/0xc479c/0xc47b8/0xc47e0`.

## 3. What this gives the daemon

- Minimal power/tune/audio sequence (all observed bytes):
  `W1B(0x00,region,0)` → `W1B(0x02,2/3,0)` → `W4B(0xFB,lo,hi)` →
  `R(0x4D,0,1)` → `W2B(0x05,init,0x88)` → `W1B(0x4D,..)` audio-bit →
  `W2B(0x05,path-word,0x88)` → `W2B(0x0A,freq)` tune.
- Userspace only needs: socket `AF_BLUETOOTH/HCI`, opcode `0xFC15`,
  the helpers' `arg` rule (`0x10/0x20/0x40` + `0x11/0x19/0x80` reads),
  and the BT-event/state ids `0x2300..0x2326` if it wants async parity.
