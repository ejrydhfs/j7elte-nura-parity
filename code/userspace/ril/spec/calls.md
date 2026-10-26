# j7elte call control over `/dev/umts_ipc0` — message catalog (CALL_CMD + SND audio path)

Status: **static evidence only, no hardware.** Every byte below is traced to a
stock builder/parser instruction in `libsec-ril.so` (build id
`8d7c7e152d01b1d46c79629c4d9b9424`) or to kernel source. Items that are inferred
or unresolved are labelled **[INFERENCE]** / **[UNKNOWN]**; nothing is presented as
a runtime observation.

Scope: originating/answering/releasing calls, incoming-call and call-state
indications, DTMF, hold, emergency call type, and the voice-call audio-path
message. SMS/data are out of scope.

---

## 0. Provenance

Extracted from `system.raw` (ext4, image root = `/system`) with
`debugfs -R 'dump …'`; the extraction was repeated for this pass and the hashes
match the copies in `parity/userspace/ril/stock/`.

| path in image | size | SHA-256 |
|---|---|---|
| `/bin/rild` | 22 072 | `64650e0b4ab11816539264b351da2d3dd965fd3753dd4f250c9191b0e18c57f4` |
| `/lib/libsec-ril.so` | 2 468 316 | `09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73` |
| `/lib/libsec-ril-dsds.so` | 2 468 316 | `640f0ea0a7c6d650d3ede7e5410af72717db1fda35c6f78a6d07bfb78a67cfa0` |

All three are ELF32 ARM EABI5, Thumb-2, PIE, bionic, stripped *but* with a full
`.dynsym`: **the exported symbol names are intact**, so every function below is
named, not guessed. Address convention: `vaddr = file offset + 0x8000` in the
first LOAD segment (sections `.plt`/`.text`/`.rodata`), `+ 0x9000` in
`.data.rel.ro`. Disassembly: `aarch64-linux-gnu-objdump -d
--disassembler-options=force-thumb`. All symbol values are odd (Thumb bit).

Which protocol object is live: `IpcModem::CreateProtocol` (0x198b2c) selects
`IpcProtocol40` only when `ModemConfig[+0x1c]` (`ModemProtoVer`) is 0,
`IpcProtocol41Iil` for 8, and `IpcProtocol41` otherwise. On this ROM the
`IpcProtocol41` copies of every call builder exist and are the ones a normal
`ModemProtoVer` selects; `IpcProtocol40` copies exist for older protocol
versions. **The 40 and 41 payloads are not always identical** (§3.3, §3.16), so
an implementation must pin the protocol version it emulates. Tables below give
the 41 layout first and call out 40 differences.

---

## 1. Transport and preconditions (what the kernel expects)

The wire framing is the kernel's, not userspace's:

* userspace writes **the IPC payload only**; `misc_write()`
  (`drivers/misc/modem_v1/modem_io_device.c:845-975`) prepends one 4-byte SIPC5
  link header per `write(2)`: `cfg=0xF8|padding`, `ch=235`
  (`SIPC5_CH_ID_FMT_0`, `modem_v1.h:172`), `len=4+count`. `misc_read()` strips
  it again and returns **0, not EOF**, when its queue is empty.
* payload header is `struct __packed sipc_fmt_hdr`
  (`modem_prj.h:205-212`), 7 bytes little-endian:

```
0  u16 len      total payload length, header included
2  u8  msg_seq  stock builders leave 0 (buffer is memset before use)
3  u8  ack_seq  stock builders leave 0
4  u8  main_cmd
5  u8  sub_cmd
6  u8  cmd_type
```

* `cmd_type`: `1 EXEC`, `2 GET`, `3 SET` on TX (names from
  `IpcProtocol41::GetCmdTypeName` 0x1bb0fc, strings 0x228727…); on RX the same
  byte is `1 ?`, `2 RESP`, `3 NOTI`. Every call *indication* below checks for
  **3**; a RESP (2) is not parsed by the call parsers, so **do not block a call
  operation waiting for a positive ack** — state arrives as NOTI.
* header length field is trusted (`IpcProtocol::GetIpcMessage` 0x1a2ef4
  reassembles on it; cap 0x2000).
* Writes are refused with `-EPERM` unless the modem is `STATE_ONLINE`:
  `modem_io_device.c:873-876`, `cp_online()` `modem_prj.h:660-664`,
  `STATE_ONLINE` `modem_prj.h:150`.
* The AP half of the CP handshake requires **both** `/dev/umts_ipc0` and
  `/dev/umts_rfs0` open: `rild_ready()` `link_device_shmem.c:261-288`,
  `INIT_END` sent at `:346-348`/`:359-362`, `shmem_init_comm()` `:1490-1500`.
* A RIL started after `j7elte-cbd` must re-issue `IOCTL_MODEM_BOOT_OFF`
  (`_IO('o',0x23)` = 0x6f23, `modem_prj.h:78`) on its own fd to make the kernel
  publish `STATE_ONLINE` to its channels (see `parity/reports/ril-re.md` §2.3).
  After that, `ioctl(fd, 0x6f27)` (`IOCTL_MODEM_STATUS`, `modem_prj.h:84`)
  returns 4.

No CRC, no per-frame ack, no sequence matching in this generation.

---

## 2. TX message catalog (AP → CP)

Payload = 7-byte header + body. "len" is the constant written at offset 0 and is
the exact number of bytes handed to one `write()`; bodies are zero-padded to
that length with `memset` before the fields are stored, i.e. every unused byte
is 0.

| # | message (stock name) | main/sub/type | len | builder (IpcProtocol41 unless noted) | safe standalone? |
|---|---|---|---|---|---|
| 2.1 | `CALL_OUTGOING` (dial, 2 string args) | 2 / 1 / 1 EXEC | 99 | `IpcTxCallOriginate(char*,ClirType,CallType,int,const char*,const char*)` 0x1b5335 | yes (needs `call_id`, see body) |
| 2.2 | `CALL_OUTGOING` (dial, bool+int) | 2 / 1 / 1 EXEC | 101 | `IpcTxCallOriginate(char*,ClirType,CallType,int,bool,int)` 0x1b543d | yes |
| 2.3 | `CALL_ANSWER` | 2 / 4 / 1 EXEC | 7 | `IpcTxCallAnswer(CallType)` 0x1b58dd | yes; `CallType` argument unused |
| 2.4 | `CALL_RELEASE` | 2 / 3 / 1 EXEC | 7 | `IpcTxCallRelease()` 0x1b5b39 | yes |
| 2.5 | `CALL_LIST` query | 2 / 6 / 2 GET | 7 | `IpcTxCallGetCallList()` 0x1b5569 | yes |
| 2.6 | `CALL_LIST` upload | 2 / 6 / 3 SET | 8+Σ | `IpcTxCallSetCallList(CallList*)` 0x1b55d5 | no — needs live call-list state |
| 2.7 | `CALL_HOLD` | 2 / 19 / 1 EXEC | 7 | `IpcTxHoldCall()` 0x1b6421 | yes |
| 2.8 | `CALL_BURST_DTMF` | 2 / 7 / 1 EXEC | 91 | `IpcTxCallSendBurstDtmf(CallToneType,int,char*)` 0x1b5cc1 | yes, during a call |
| 2.9 | `CALL_CONT_DTMF` start | 2 / 8 / 3 SET | 9 | `IpcTxCallStartContDtmf(char)` 0x1b5db5 | yes, during a call |
| 2.10 | `CALL_CONT_DTMF` stop | 2 / 8 / 3 SET | 9 | `IpcTxCallStopContDtmf()` 0x1b5e31 | yes, during a call |
| 2.11 | `CALL_VOICE_PRIVACY` set | 2 / 12 / 3 SET | 9 | `IpcTxCallSetVoicePrivacy(VoicePrivacyType)` 0x1b5ea5 | yes |
| 2.12 | `CALL_VOICE_PRIVACY` get | 2 / 12 / 2 GET | 8 | `IpcTxCallGetVoicePrivacy()` 0x1b5f29 | yes |
| 2.13 | `CALL_FLASH_INFO` | 2 / 17 / 3 SET | 90 | `IpcTxCallFlash(char*)` 0x1b5f91 | yes |
| 2.14 | `CALL_E911_CB_MODE` | 2 / 16 / 3 SET | 8 | `IpcTxCallSetEmergencyCbMode(CallEcbModeType)` 0x1b6075 | yes |
| 2.15 | `CALL_CALL_TIME_COUNT` / call time set | 2 / 13 / 3 SET | 37 | `IpcTxCallSetCallTime(unsigned long)` 0x1b60ed | yes |
| 2.16 | call time get | 2 / 13 / 2 GET | 9 | `IpcTxCallGetCallTime()` 0x1b6171 | yes |
| 2.17 | total call time set | 2 / 13 / 3 SET | 37 | `IpcTxCallSetTotalCallTime(CallTimeCount*)` 0x1b61fd | no — needs counters |
| 2.18 | total call time get | 2 / 13 / 2 GET | 9 | `IpcTxCallGetTotalCallTime()` 0x1b62a5 | yes |
| 2.19 | E911 call status | 2 / 23 / 3 SET | 9 | `IpcTxCallSetEmergencyCallStatus(E911CallStatus,RadioTechnology)` 0x1b6319 | yes |
| 2.20 | `CALL_SRVCC` | 2 / 20 / 3 SET | 8 | `IpcTxCallSetSrvccState(CallSrvccState)` 0x1b63a5 | yes |
| 2.21 | `CALL_BLOCK_STATUS` | 2 / 24 / 3 SET | 8 | `IpcTxCallSetCallBlockStatus(unsigned char)` 0x1b6609 | yes |
| 2.22 | `CALL_MODIFY` | 2 / 48 / 1 EXEC | var | `IpcTxCallModify(int,CallDetails*)` 0x1b5949 | no — needs CallDetails |
| 2.23 | `CALL_MODIFY_CONFIRM` | 2 / 50 / 1 EXEC | var | `IpcTxCallModifyConfirm(int,CallType,CallDetails*)` 0x1b5a45 | no |
| 2.24 | test call | 2 / 1 / 1 EXEC | 99 | `IpcTxCallSetTestCall(char*,int,int)` 0x1b648d | factory/diagnostic only |
| 2.25 | end test call | 2 / 3 / 1 EXEC | 7 | `IpcTxCallEndTestCall()` 0x1b659d | identical to 2.4 |
| 2.26 | `SND_AUDIO_PATH` (voice call) | 9 / 3 / 3 SET | 9 | `IpcProtocol41::IpcTxSndSetAudioPath(int,int)` 0x1ce085 | yes |
| 2.27 | explicit call transfer | 12 / 129 / 3 SET | 358 | `IpcTxCallTransfer(char*,char*,char*,char*,int)` 0x1b6679 | no — needs dialled numbers |
| 2.28 | cancel transfer | 12 / 130 / 3 SET | 8 | `IpcTxCallCancelTransfer(int)` 0x1b6865 | yes |
| 2.29 | `CALL_MANAGE_CALL` (hold/retrieve/swap…) | 12 / 7 / 3 SET | 92 | `IpcTxCallSetManageCall(CallManageType,int,char*)` 0x1b5ba5 | partly — needs call index |
| 2.30 | audio path (proto 40 only) | 9 / 11 / 3 SET | 8 | `IpcProtocol40::IpcTxSndSetAudioPath(int,int)` 0x1b02c5 | protocol-40 only |

Notes on the two subcommand bytes that do **not** follow the visible string
table order: 20 for SRVCC and 23/24/48/50 for emergency-status/block/modify.
The numeric values are read directly from the builder immediates, so they are
the authoritative IDs; the *names* attached to non-obvious numbers are
**[INFERENCE]** from the neighbouring stock strings (`CALL_SRVCC`, … at
0x2209a6-0x220a18). If the CP rejects one of those, log the raw sub, not the name.

### 2.1 Dial, variant A (99 bytes) — 0x1b5335

Argument order (AAPCS): `r0=this, r1=char* number, r2=ClirType, r3=CallType,
stack[0]=int call_id, stack[1]=const char*, stack[2]=const char*`
(the last two are only logged).

```
off  size  value                                   instruction
0    2     0x0063  (99)                            0x1b53be strh r5,[sp,#24]
2    1     0                                       memset
3    1     0                                       memset
4    1     0x02  CALL_CMD                          0x1b53c4
5    1     0x01  CALL_OUTGOING                    0x1b53c8
6    1     0x01  EXEC                             0x1b53cc
7    2     ConvertCallTypeToIpc(CallType)         0x1b53d0..d4 (u16 LE)
9    1     ClirType (raw)                         0x1b53da
10   1     len = min(strlen(number),82)           0x1b53e2 (r7)
11   1     0x11 if number[0]=='+' else 0x21       0x1b539a..a2
12   len   number bytes (ASCII, no NUL)           0x1b53b0 memcpy_chk
12+len..97  zero padding
98   1     call_id & 0xff                         0x1b53e2 strb r8,[sp,#122]
```

`ConvertCallTypeToIpc` (0x1b5319) is a 21-entry `int16` table at
**0x2392f6**, indexed by `CallType-1`; values:

```
CallType : 1     2     3     4     5     6     7     8     9     10    11
ipc word : 0x000 0x100 0x300 0x105 0x106 0x301 0x306 0x302 0x303 0x304 0x305
CallType : 12    13    14    15    16    17    18    19    20    21
ipc word : 0x307 0x700 0x702 0x100 0x801 0x308 0x309 0x30a 0x30b 0x30c
```
`CallType <= 0` or `>21` yields `0x100`. **Emergency dialling = `CallType 13`
→ ipc word `0x700`** (`cmp r9,#0x700` at 0x1b53ee also sets a flag at
`this+0x48`+4, i.e. the stock stack marks the E911 case). Note: the
`IpcProtocol40` builder (0x1a52e1) hardcodes its own mapping
(`cmp r7,#12` at 0x1a535a) instead of using this table, so a 40-era stack maps
emergency to `CallType 12`; with `IpcProtocol41` (the normal selection) the
index-13 entry is the emergency one.

### 2.2 Dial, variant B (101 bytes) — 0x1b543d

Same as 2.1 except: len `0x65`; ClirType byte is *computed* instead of
transmitted (`0x11` when `bool && int==1`, else `0x21`, 0x1b54a0-b6), and two
extra trailing bytes:

```
98   1   int argument & 0xff
99   1   0x01                                    0x1b54c8
100  1   len-1                                   0x1b54ce
```
Also, unlike variant A, it does **not** look at `number[0]`.
Which variant the call path uses depends on the vtable slot the call manager
calls; both builders are exported and byte-exact, so either can be implemented.
A conservative reimplementation should use variant A (the `const char*`
signature matches a RIL `dial` with CLIR + call type) and set byte 98 to the
stock `call_id`.

### 2.3 Answer — 0x1b58dd

```
07 00 00 00 02 04 01
```
No body; `CallType` is not read (function only stores the constant header).

### 2.4 Release — 0x1b5b39

```
07 00 00 00 02 03 01
```

### 2.5 Call list query — 0x1b5569

```
07 00 00 00 02 06 02
```

### 2.6 Call list upload — 0x1b55d5 (SET, variable length)

`memset` 818 bytes, then:
```
0..1   u16 total length actually used (r4, accumulates)
4      0x02, 5: 0x06, 6: 0x03 SET
7      CallList[+0xc]  = entry count
8..    entries, each:
         +0  u16  ConvertCallTypeToIpc(type) & 0xFF00   (0x1b566c bic)
         +2  u8   CallDetails[+16]
         +3  u8   2 if <details+25>!=0 else 1
         +4  u8   table[CallDetails[+8]] (5-entry byte table @0x23cf8+? see note) or 0
         +5  u8   <details+24>
         +6  u8   strlen(number at details+125), clamped
         +7  u8   0x81 (overwritten later by parser semantics)
         +8..     number bytes
       stride = 8 + strlen(number)
```
The per-entry encoder mirrors the `IpcRxCallList` decoder in §4.3 field for
field; use that table to build it. The `0x81` byte is the default TON/NPI and
becomes `0x91` when the parser sees a non-`+` number whose type nibble is 1.

### 2.7 Hold — 0x1b6421

```
07 00 00 00 02 13 01
```

### 2.8 Burst DTMF — 0x1b5cc1

```
0..1  0x005b (91)
4     0x02, 5: 0x07, 6: 0x01 EXEC
7     1 if CallToneType != 2 else 2       0x1b5d16..2a
8     min(strlen(digits),82)              0x1b5d38..42
9..   digit bytes                         0x1b5d6e
```

### 2.9 / 2.10 Continuous DTMF — 0x1b5db5 / 0x1b5e31

```
start: 09 00 00 00 02 08 03 01 <tone char>
stop : 09 00 00 00 02 08 03 02 00
```
Field at offset 7 is the action (1 start, 2 stop); only start carries the tone
at offset 8.

### 2.11 / 2.12 Voice privacy — 0x1b5ea5 / 0x1b5f29

```
set: 09 00 00 00 02 0c 03 01 <type!=1 ? 1 : 0>
get: 08 00 00 00 02 0c 02 01
```

### 2.13 Flash / USSD-style string — 0x1b5f91

```
0..1  0x005a (90)
4     0x02, 5: 0x11, 6: 0x03 SET
7     min(strlen(s),48)                    0x1b5ff8..ffc
8..   s bytes (max 48), rest zero          0x1b602e
```

### 2.14 Emergency callback mode — 0x1b6075

```
08 00 00 00 02 10 03 <mode!=1 ? 1 : 0>
```

### 2.15 Call time set — 0x1b60ed

```
0..1  0x0025 (37)
4     main: 0x02 normally, 0x22 when <this+0x14>==10   (0x1b6138..4c)
5     sub : 0x0d normally, 0x09 in that same case
6     0x03 SET
21..24 u32 call time (str.w r6,[sp,#29])
```
`<this+0x14>` is the modem type; on a normal GSM/SEC modem it is not 10, so the
frame is `25 00 00 00 02 0d 03 …`.

### 2.16 Call time get — 0x1b6171

```
09 00 00 00 02 0d 02 00        (main 0x22 / sub 0x09 when <this+0x14>==10)
```

### 2.17 / 2.18 Total call time — 0x1b61fd / 0x1b62a5

```
set: 25 00 00 00 02 0d 03 07 00 <7 × u32 from CallTimeCount[+8,+12,…,+32]>
get: 09 00 00 00 02 0d 02 07 00
```
(set: offsets 9,13,17,21,25,29,33 hold the seven u32s; see 0x1b625c-0x1b6280)

### 2.19 E911 call status — 0x1b6319

```
0..1  0x0009
4     0x02, 5: 0x17, 6: 0x03 SET
7     E911CallStatus (raw byte)                    0x1b6376
8     virtual [vtable+0x8e4](this, RadioTechnology) 0x1b6370..7c
```
The vtable call converts the RAT; its implementation was not resolved
(**slot 0x8e4 is one of the APS2-relocated entries**, see §6).

### 2.20 SRVCC — 0x1b63a5

```
08 00 00 00 02 14 03 <state: 1 if 1, 2 if 2, else 0>
```
(0x1b63dc-0x1b63f8)

### 2.21 Call block status — 0x1b6609

```
08 00 00 00 02 18 03 <status byte>
```

### 2.22 / 2.23 Modify / ModifyConfirm — 0x1b5949 / 0x1b5a45

```
modify:   len           2 / 48 (0x30) / 1 EXEC
modifyC:  len           2 / 50 (0x32) / 1 EXEC
7         call id (byte)                         0x1b59a6 / 0x1b5a94
8..9      u16 ConvertCallTypeToIpc(CallDetails::GetCallType())  0x1b59b2/0x1b5aa2
10        count = CallDetails[+16]               0x1b59bc / 0x1b5aac
11..      repeated { u16 strlen; chars } for each
          char* in CallDetails[+20], [+24], …    0x1b59be-0x1b59ee
length written to offset 0 = accumulated length
```
Modify is EXEC, not SET; `CallDetails` is the stock call object (780-byte
element of `CallList`).

### 2.24 Test call — 0x1b648d

`99` bytes: `2/1/1`, offset 7..8 = a mode-dependent u16 (`tbb` table at
0x1b6500, 0x1b64f6): mode 1→`(2,6)`, 2→`(3,6)`, 3→`(3,5)`, 4→`(2,5)`,
5→`(4,5)`, 6→`(5,6)`, 7→`(3,6)`, 8→`(3,5)`, 9→`(2,6)`, 10→`(3,6)`, otherwise
the raw u16 mode; offset 10 = number length, 11 = `0x11`/`0x21` by leading
`+`, 12.. = number. Diagnostic only; do not use for a real call.

### 2.26 Voice-call audio path — 0x1ce085 (protocol 41)

```
0..1 0x0009
4    0x09  SND_CMD
5    0x03  (SND audio-path subcommand in the 41 numbering)
6    0x03  SET
7    first int argument  (audio path)
8    second int argument (mode)
```
Protocol 40's copy (0x1b02c5) instead emits **8 bytes**:
`08 00 00 00 09 0b 03 <path>` and **ignores its second argument** — a real
divergence, evidence that the 40/41 numbering is not interchangeable.

### 2.29 Manage call — 0x1b5ba5

`92` bytes, main **12** (not CALL_CMD), sub 7, type 3:
```
7   mapped CallManageType = s8 table[type-1] @0x239352
      1→3 2→4 3→5 4→6 5→7 6→8 7→9 8→10, 9..17→-1, 18→4, 19..21→0
      (type<1 or >21 → -1)
8   index; if NeedCallIdPlusMinus() (0x1a3971, checks a build property and
    SEC product) and index < 6, index++
9   min(strlen(number),82)
10.. number bytes
```
`NeedCallIdPlusMinus()` is a Region/product check; on a non-matching product it
returns 0 and the index is passed unchanged.

### 2.27 / 2.28 Transfer family — 0x1b6679 / 0x1b6865

main 12, type 3; sub `0x81` (transfer) / `0x82` (cancel). Transfer is a fixed
358-byte frame: `[7]` = int argument, then four length-prefixed strings at
`[8]/[9..]`, `[91]/[92..]`, `[192]/[193..]`, `[275]/[276..]` (caps 82, 100, 82,
82). Cancel is `08 00 00 00 0c 82 03 <int>`.

---

## 3. RX catalog (CP → AP)

Dispatch is `IpcProtocol40::IpcRxCall` (0x1a6673, inherited by 41): it switches on
**payload byte 5 (sub_cmd)** and tail-calls the vtable slot listed. Slots live in
the `IpcProtocol40/41` vtables; those are populated by Android *packed*
relocations (`.rel.dyn` is `ANDROID_REL`/APS2, which GNU binutils does not
apply), so **slot→function binding is not statically resolved in this pass** —
the parser list is exact, the per-sub association is **[INFERENCE]** from the
parser's semantics and the sub-command string table.

| sub | vtable slot (from dispatcher) | parser symbol | parser addr | sets RegistrantType |
|---|---|---|---|---|
| 2 | +0x900 | `IpcRxCallIncoming` | 0x1a64b1 | 15 (only if cmd_type==3) |
| 5 | +0x8f8 | `IpcRxCallStatus` | 0x1b6aed | 14 (always) |
| 6 | +0x8fc | `IpcRxCallList` | 0x1b6ba1 | 14 (only if cmd_type==3) |
| 9 | +0x904 | `IpcRxCallWaiting` | 0x1b71c5 / 0x1a64c1 | 18 (if type==3) |
| 10 | +0x910 | `IpcRxCallLineId` | 0x1a6537 | — |
| 12 | +0x90c | `IpcRxCallVoicePrivacy` | 0x1b71d5 | — |
| 13 | +0x91c | `IpcRxCallCallTime`? (0x1b6269) | 0x1b6269 | — |
| 14 | +0x698 | (base-class slot) | — | — |
| 15 | +0x69c | (base-class slot) | — | — |
| 16 | +0x918 | `IpcRxCallEmergencyCbMode` | 0x1a65a5 | — |
| 18 | +0x924 | `IpcRxCallLineControl`? | 0x1b735d | 17 |
| 25 | +0x92c | `IpcRxCallOtaProgress`? | 0x1b71fd | — |
| 49 | +0x908 | `IpcRxCallEccList`? | 0x1b73e1 | — |

All other subs are dropped (dispatcher returns 0 at 0x1a6740). Parsers that
exist but are reached from other table entries: `IpcRxCallSignalInfo`
(0x1a6557), `IpcRxCallCallTime` (0x1b6269), `IpcRxCallDiagnosticOutgoing`
(0x1b72c5), `IpcRxCallSrvcc` (0x1b7559), `IpcRxBurstDtmf` (0x1b75ad),
`IpcRxCallLifeByte` (0x1b75c1), `IpcRxCallModifyNoti` (0x1a64d1, stub: logs and
returns 0), `IpcRxCallControlResult` (0x1b2139, type 3 → `StkMessage`, reg 12).

### 3.1 Incoming call (`sub 2`, NOTI)

Parser is 16 bytes: only checks `payload[6] == 3` and stores registrant type 15.
**No payload fields are parsed there**; the call data comes in the same
transaction's `CALL_STATUS`/`CALL_LIST` (or the stock stack reads the raw
message). A client must therefore treat `02 … 02 02 03` as "incoming call"
and get the number/state from the status/list messages.

### 3.2 Call status (`sub 5`) — 0x1b6aed

Allocates a 0x180-byte `CallStatus` (ctor 0xd3230), fills:
```
payload[7..8]  u16 ipc state word → ConvertToIpcCmd(word, &CallStatus+8)
                (0x1b509c; switch on word & 0xff00, see §3.5)
payload[9]     u8  call id      → CallStatus[+0x5c]
                (if NeedCallIdPlusMinus() && value<=6: value-- before use,
                 0x1b6b18-0x1b6b1e/0x1b6b88)
payload[10]    u8               → CallStatus[+0x60]
payload[11]    u8               ┐ virtual [vtable+0x99c](payload[11],
payload[12]    u8               ┘ payload[12]) → CallStatus[+0x6c]
payload[12]    u8 state code    → index into a 4-byte element table
                                (0x1b6b5e..0x1b6b84; values 0x30/0xa8 special)
registrantType = 14
```
`IpcRxCallList` treats the same `payload[7..8]` word as "call type/state":
`word & 0xff00 == 0x0100 or 0x0700 or 0x0500..0x0503` sets `CallDetails+26=1`;
`word&0xff00 == 0x0300 or 0x0800` sets `CallDetails+27=1`.

### 3.3 Call list (`sub 6`) — 0x1b6ba1

```
payload[6]==3 (NOTI)   → *registrantType = 14, return NULL (no parse)
otherwise:
  CallList (16 B, ctor 0xc62d0); CallList[+0xc] = payload[7] = entry count
  CallList[+8] = array of 780-byte CallDetails
  for i in 0..count-1, entry at payload+8 (cursor advances 8+numlen):
    [0..1] u16 state word  → ConvertToIpcCmd(word, details+28)
    [2]    u8  → details[+16]
    [3]    u8  → details[+25] = (v != 1)
    [4]    u8  → if v: details[+8] = v-1
    [5]    u8  → details[+24] = (v != 1)
    [6]    u8  number length (clamped to 82)
    [7]    u8  number type; default details[+20]=0x81, becomes 0x91 when
               number[0] != '+' and ((type>>4)&7)==1
    [8..]  number bytes
  if a built property is set and the number is "plus or type nibble 1":
    details[+20] = 0x91
```
`CallList[+0xc] == 0` short-circuits via a property check (0x1b6be2-0x1b6bf0).
After the loop the parser continues with a second (dual-number) block
(0x1b6d5a onward) which was not fully decoded.

### 3.4 Smaller indications (verified field offsets)

| parser | payload use | registrant |
|---|---|---|
| `IpcRxCallLineControl` 0x1b735d | type 3; 16-byte `CallInfoRecords`; `[7]→+0x138`, `[8]→+0x139`, `[9]→+0x13a`, `[10]→+0x13b`; `[+8]=1`, `[+12][+8]=6` | 17 |
| `IpcRxCallSrvcc` 0x1b7559 | type 3; `[7]` 1→state 1, 2→state 2, else 0; `SrvccState` object; also `Modem::NotifyRegistrant(14)` when `this[+4]==2 && [7]==1` | 24 |
| `IpcRxBurstDtmf` 0x1b75ad | `(cmd_type & 0xfd) == 1` (i.e. 1 or 3) | 125 |
| `IpcRxCallLifeByte` 0x1b75c1 | `[7..10]` u32 → LifeByte+8; `[11..14]` u32 → LifeByte+12 | — |
| `IpcRxCallControlResult` 0x1b2139 | type 3; `StkMessage(payload+7, len-7, type 6)` | 12 |
| `IpcRxCallModifyNoti` 0x1a64d1 | none (log only) | — |

### 3.5 State word decoding (`ConvertToIpcCmd`, 0x1b509c)

`word & 0xff00` selects the call family; `word & 0xff` the state:
```
0x0000/0x0100 : details[8]=0; 0x0105 → state 2; 0x0106 → state 2 + extra;
                otherwise state 1 (details[+0xc]=1)
0x0300        : details[8]=3, details[+0xc]=2; 0x0301/0x0302/0x0306 set extra
                strings; 0x0303 → details[8]=1; 0x0304 → details[8] as computed;
                0x0305 → extra values (SetExtraValue)
0x0700        : details[8]=4; 0x0702 → state 2
0x0800        : separate branch at 0x1b523e
```
The names of `details[+8]`/`details[+0xc]` (CallState/CallType) were not
recovered; they are stock object fields, so a reimplementation only needs the
raw word. **Raw u16 is the safe interface.**

---

## 4. Minimum sequence to place and tear down a voice call

Preconditions (all mandatory, each with its own evidence):

1. CP booted by `j7elte-cbd` (TOC/MAIN/NV + `IOCTL_MODEM_BOOT_OFF`).
2. Open `/dev/umts_ipc0` **and** `/dev/umts_rfs0` (kernel sends `INIT_END` when
   both are open — `link_device_shmem.c:261-288, 1490-1500`). Keep both open.
3. `ioctl(fd, 0x6f23)` on the ipc0 fd so the kernel publishes `STATE_ONLINE`
   to it; otherwise every write returns `EPERM`
   (`modem_io_device.c:873-876`).
4. Radio on (stock `PWR_RADIO_POWER`, 0x1cb350):
   `09 00 00 00 01 07 01 01 00`. Not strictly a call message but the CP will
   not accept a dial while the stack is off; the state is confirmed by
   `NET`/`CALL` indications. **[INFERENCE: ordering; the CP's exact rejection
   behaviour for dial-before-radio-on is not evidenced.]**

### Mobile-originated call

```
→ 09 00 00 00 01 07 01 01 00            (radio on, if not already)
→ 63 00 00 00 02 01 01 <t lo> <t hi> <clir> <nlen> <'+'?0x11:0x21> <digits> ... <call_id>
      variant A; t = 0x0100 for a normal voice call, 0x0700 for emergency
← (expect) 02 …, sub 5 NOTI, payload[7..8] = state word, [9] = call id,
      [12] = state; then sub 6 NOTI/list with the active call
← audio: nothing required to *set up* the call; if the CP needs the route,
      send 09 00 00 00 09 03 03 <path> <mode> (proto 41)
→ 07 00 00 00 02 03 01                  (release)
← sub 5 NOTI with an idle/release state word, and/or sub 6 with a shorter list
```

### Mobile-terminated call

```
← sub 2 NOTI (02 … 02 02 03)            → incoming indication (reg 15)
← sub 5 NOTI with state word (ringing/active) and call id
→ 07 00 00 00 02 04 01                  (answer)
→ 09 00 00 00 09 03 03 <path> <mode>    (audio route; value semantics unknown)
→ 07 00 00 00 02 03 01                  (release)
```

No positive acknowledgement exists for originate/answer/release in the traced
code: the call parsers never branch on `cmd_type == 2` (RESP), they act on
NOTI. An implementation must therefore drive its state from the sub-5/6
indications (plus sub 2 for MT) and use a timeout, not an ack, for failure.

### Things a minimal client should *not* send
`CALL_LIST` SET (2.6), `MODIFY`/`MODIFY_CONFIRM` (2.22/2.23), `TRANSFER` (2.27)
and total-call-time SET (2.17) all encode stock-side state (`CallList`,
`CallDetails`, counters). Sending them with guessed values will desynchronise
the CP's call model. The standalone-safe set is 2.1/2.2, 2.3, 2.4, 2.5, 2.7,
2.8-2.14, 2.16, 2.18, 2.19-2.21, 2.26, 2.28.

---

## 5. What the kernel `/dev/umts_ipc0` channel expects (summary)

| requirement | evidence |
|---|---|
| one `write()` = one IPC message; kernel adds/removes the 4-byte SIPC5 link header | `modem_io_device.c:845-975`, `sipc5_build_header` `:1290-1341`, `rx_fmt_ipc` `:257-278` |
| payload starts with the 7-byte `sipc_fmt_hdr` | `modem_prj.h:205-212`; every builder memsets 7 then writes len/main/sub/type |
| `main=2`, `sub∈{1,3,4,6,7,8,12,13,16,17,19,20,23,24,48,50}` for call control; `main=9` for the audio path | builder immediates (§2) |
| writes only while `STATE_ONLINE` | `modem_io_device.c:873-876` |
| reads return 0 on empty (not EOF), one frame/fragment at a time | `misc_read()` `modem_io_device.c:974-1026` |
| `umts_ipc0` + `umts_rfs0` both open before `INIT_END` | `link_device_shmem.c:261-288`, `:1490-1500` |
| no CRC / no sequence negotiation | `modem_prj.h:205-212`; all builders leave `msg_seq`/`ack_seq` 0 |

A payload whose `len` exceeds the actual write produces a kernel prefix of
`4+count`, i.e. the CP sees a truncated message; always write exactly `len`
bytes from offset 0.

---

## 6. Unknowns and risks (explicit)

1. **[UNKNOWN] vtable slot → parser function.** `.rel.dyn` is Android packed
   (`APS2`), not applied by binutils; the RX table's *sub numbers* and the
   *parser list* are both evidenced, but which parser serves sub 10/12/13/14/
   15/16/18/25/49 is inferred. Field layouts in §3.4 are per-parser, not
   per-sub.
2. **[UNKNOWN] CallStatus[+0x5c/+0x60/+0x6c/+0x70] semantics.** Offsets are
   exact; the stock names (`CallState`, direction, `CallType`) were not
   recovered. Emit raw fields.
3. **[UNKNOWN] audio-path values** (`<path>`, `<mode>`): builder bytes are
   exact (2.26), the enum was not traced. Sending the message with wrong values
   could route audio to the wrong device; gate it behind a runtime switch.
4. **[UNKNOWN] E911 RAT converter** (vtable slot 0x8e4, §2.19) — the byte at
   offset 8 of the E911-status frame is produced by an unresolved virtual call.
5. **[INFERENCE] name↔number mapping** for sub 20/23/24/48/50 and the 12/… SS
   family: numbers are read from immediates, names from adjacent stock strings.
6. **[UNKNOWN] whether the CP requires `CALL_LIST` (SET) or radio-on before
   accepting `CALL_OUTGOING`.** Not evidenced; assume not, but the first
   hardware run must check the CP's NOTI/RESP after a dial.
7. **Nothing here was executed against a CP.** No hardware was available. The
   only safe conclusion from a failed dial on hardware is "the CP rejected a
   frame"; capture the RX bytes before changing an opcode.
8. `libsec-ril-dsds.so` is size-identical but a different build; addresses in
   this document apply to `libsec-ril.so` build id
   `8d7c7e152d01b1d46c79629c4d9b9424` only.

---

## 7. Evidence appendix

### 7.1 Function map (all `IpcProtocol41` unless stated)

```
0x1b5335  IpcTxCallOriginate(char*,ClirType,CallType,int,const char*,const char*)
0x1b543d  IpcTxCallOriginate(char*,ClirType,CallType,int,bool,int)
0x1b5319  ConvertCallTypeToIpc(CallType)              table @0x2392f6 (21×s16)
0x1b5569  IpcTxCallGetCallList()
0x1b55d5  IpcTxCallSetCallList(CallList*)
0x1b58dd  IpcTxCallAnswer(CallType)
0x1b5b39  IpcTxCallRelease()
0x1b5949  IpcTxCallModify(int,CallDetails*)
0x1b5a45  IpcTxCallModifyConfirm(int,CallType,CallDetails*)
0x1b5ba5  IpcTxCallSetManageCall(CallManageType,int,char*)   table @0x239352
0x1b5cc1  IpcTxCallSendBurstDtmf(CallToneType,int,char*)
0x1b5db5  IpcTxCallStartContDtmf(char)
0x1b5e31  IpcTxCallStopContDtmf()
0x1b5ea5  IpcTxCallSetVoicePrivacy(VoicePrivacyType)
0x1b5f29  IpcTxCallGetVoicePrivacy()
0x1b5f91  IpcTxCallFlash(char*)
0x1b6075  IpcTxCallSetEmergencyCbMode(CallEcbModeType)
0x1b60ed  IpcTxCallSetCallTime(unsigned long)
0x1b6171  IpcTxCallGetCallTime()
0x1b61fd  IpcTxCallSetTotalCallTime(CallTimeCount*)
0x1b62a5  IpcTxCallGetTotalCallTime()
0x1b6319  IpcTxCallSetEmergencyCallStatus(E911CallStatus,RadioTechnology)
0x1b63a5  IpcTxCallSetSrvccState(CallSrvccState)
0x1b6421  IpcTxHoldCall()
0x1b648d  IpcTxCallSetTestCall(char*,int,int)
0x1b659d  IpcTxCallEndTestCall()
0x1b6609  IpcTxCallSetCallBlockStatus(unsigned char)
0x1b6679  IpcTxCallTransfer(char*,char*,char*,char*,int)
0x1b6865  IpcTxCallCancelTransfer(int)
0x1ce085  IpcTxSndSetAudioPath(int,int)                       (41, 9-byte frame)
0x1b02c5  IpcTxSndSetAudioPath(int,int)                       (40, 8-byte frame)
0x1a6673  IpcRxCall dispatcher (switch on payload[5])
0x1b6aed  IpcRxCallStatus            0x1b6ba1  IpcRxCallList
0x1a64b1  IpcRxCallIncoming          0x1b71c5  IpcRxCallWaiting
0x1b71d5  IpcRxCallVoicePrivacy      0x1b735d  IpcRxCallLineControl
0x1b7559  IpcRxCallSrvcc             0x1b75ad  IpcRxBurstDtmf
0x1b75c1  IpcRxCallLifeByte          0x1b2139  IpcRxCallControlResult
0x1b73e1  IpcRxCallEccList           0x1b72c5  IpcRxCallDiagnosticOutgoing
0x1b71fd  IpcRxCallOtaProgress       0x1b6269  IpcRxCallCallTime
0x1a6537  IpcRxCallLineId            0x1a6557  IpcRxCallSignalInfo
0x1b509c  ConvertToIpcCmd(int,int&,int&,int&,int&)  (state-word → CallDetails)
0x1a3971  IpcProtocol::NeedCallIdPlusMinus()
```

### 7.2 Command-name string tables (0x2208a7…, 0x2209f5…)

`CALL_OUTGOING`, `CALL_INCOMING`, `CALL_RELEASE`, `CALL_ANSWER`, `CALL_STATUS`,
`CALL_LIST`, `CALL_BURST_DTMF`, `CALL_CONT_DTMF`, `CALL_WAITING`,
`CALL_LINE_ID`, `CALL_SIGNAL`, `CALL_VOICE_PRIVACY`, `CALL_CALL_TIME_COUNT`,
`CALL_OTA_PROGRESS`, `CALL_DIAG_OUTGOING`, `CALL_E911_CB_MODE`,
`CALL_FLASH_INFO`, `CALL_SRVCC`, `CALL_HOLD`, `CALL_BLOCK_STATUS`,
`CALL_DATA_CALL_BYTE_COUNTER`, `CALL_MODIFY`, `CALL_MODIFY_NOTI`,
`CALL_MODIFY_CONFIRM`; also `CALL_SUB_CMD_UNDEFINED(0x%x)` at 0x222797 and
`MAIN_CMD_UNDEFINED`/`CALL_CMD` at 0x220825/0x220747.
The string **order** is not the enum order (SRVCC is 20, not 18; MODIFY is 48),
so use the numeric IDs.

### 7.3 Registrant types observed for call messages

`12` call-control result, `14` call status / call list, `15` incoming call,
`17` line control, `18` call waiting, `24` SRVCC, `125` burst DTMF.
