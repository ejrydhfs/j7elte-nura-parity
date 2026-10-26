# SMS message catalog — j7elte SS310AP (`/dev/umts_ipc0`, main cmd 4)

**Status: spec complete, static evidence only. No hardware run.**
Every message ID, byte offset and constant below is traced to a vaddr in
`libsec-ril.so` build id `8d7c7e152d01b1d46c79629c4d9b9424` (SHA-256
`09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73`).
Fields whose *meaning* is not provable from the binary are marked
**[UNKNOWN]**; nothing in this file is a guess about runtime behaviour.

Companion documents: `spec/framing.md` (SIPC5 link layer, one payload per
`write()`), `spec/messages.md` (main/sub name tables). This file covers only
`main_cmd = 0x04` (`SMS_CMD`).

---

## 1. Extracted binaries

Re-extracted from `system.raw` (ext4, `debugfs`) on 2026-10-24 and compared
byte-for-byte with the copies already in `parity/userspace/ril/stock/`
(all 9 identical):

```
debugfs -R "dump /lib/libsec-ril.so /tmp/rilx/libsec-ril.so" system.raw
```

| Image path | SHA-256 |
|---|---|
| `/lib/libsec-ril.so` | `09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73` |
| `/lib/libsec-ril-dsds.so` | `640f0ea0a7c6d650d3ede7e5410af72717db1fda35c6f78a6d07bfb78a67cfa0` |
| `/lib/libril.so` | `ad8072ac410f914fa87942aabd58ef71de73fbec1d664429a66b8ff2672b5618` |
| `/lib/libsecril-client.so` | `0506f623307ea4dc039164b987b4042de3b829e60fbeffa0a3927b7eea8d8020` |
| `/lib/librilutils.so` | `066981be45065af9633015cabdcecf54bb726562d7957808cbcf8402a97dc894` |
| `/lib/libaudio-ril.so` | `5e9103221c2f940c63ef5fe1a3e351794ed62cda709ed20b4fde88ba48c1e0fa` |
| `/lib/libreference-ril.so` | `fc32b63443445fc563fbc5cb10858614ba2f1216d61406f5f355e6f1812c1807` |
| `/bin/rild` | `64650e0b4ab11816539264b351da2d3dd965fd3753dd4f250c9191b0e18c57f4` |
| `/bin/at_distributor` | `9d01e6167da24cf808fe976842552b9e3f1640a0faa72982c8b4c7d3f9a8435e` |

All are ELF32 ARM, EABI5, Android 23. `libsec-ril.so` is stripped of
`.symtab` but keeps 9 574 `.dynsym` entries (C++ names); the addresses in
this document are file vaddrs.

## 2. Which protocol class is active — `IpcProtocol41`

`rild.libpath=/system/lib/libsec-ril.so`; inside it:

* `IpcModem::Init` (`0x1a1bd8`) reads `ModemProtoVer` from `ModemConfig+0x1c`
  (`ldr r1,[r6,#28]` @`0x1a1c0c`) and calls
  `IpcModem::CreateProtocol(ModemProtoVer)` (`0x198b2c`).
* `CreateProtocol` (`0x198b2c`): `0 → IpcProtocol40` (`0x198b3c`),
  `2 → IpcProtocol41(ver)` (`0x198b4e`), `8 → IpcProtocol41Iil`
  (`0x198b62`), **any other value → IpcProtocol41(ver)** (`0x198b74`).
* `ModemConfig::Init` (`0xf70bc`) stores its third stack argument at
  `this+0x1c` (`str.w r3,[r8,#0x1c]` @`0xf711c`).
* `ModemFactory::LoadModemConfiguration` (`0xf754c`) is the only producer of
  those configs in this library. The two ways it calls `ModemConfig::Init`:
  * table path @`0xf7790`: the proto-ver stack slot is entry field
    `[r5+0x10]` (`str r3,[sp,#8]` @`0xf776e`); both table entries
    (`0x263198`, 52-byte stride) hold **1** at that offset;
  * built-in path @`0xf782c`: `movs r5,#2` @`0xf7830`, then
    `str r5,[sp,#8]` @`0xf7838` — **2**. Every branch out of the model-name
    comparison (`0xf780c`, `0xf7882`, `0xf7898`) only changes `ModemType`
    (`r1`), never `[sp+8]`.

No path supplies `0`, so `IpcProtocol40` is unreachable in this binary and
**all SMS frames below are the `IpcProtocol41` variants**. Where the two
classes differ materially (write length, stub handlers) that is noted.

## 3. Frame header and transport

7-byte userspace payload header (`struct sipc_fmt_hdr`, `modem_prj.h:205`):

```
off  size  field
0    2     len      total payload length, little-endian (authoritative)
2    1     msg_seq  request sequence; builders leave 0, IpcProtocol::SetMsgSequence
                    (0x1a42d2, `strb r0,[r4,#2]`) fills it from GenerateMsgSequence
3    1     ack_seq  always 0 in the send path
4    1     main_cmd 0x04 = SMS_CMD
5    1     sub_cmd  see tables below
6    1     cmd_type 1=EXEC 2=GET 3=SET 4=CFRM 5=EVENT (AP→CP);
                    1=INDI 2=RESP 3=NOTI (CP→AP)
```

* Userspace writes *payloads only*; `drivers/misc/modem_v1` adds/strips the
  SIPC5 link header (`spec/framing.md` §3).
* `misc_write()` returns `-EPERM` unless `phone_state == STATE_ONLINE`
  (`modem_io_device.c:869-876`); the CP reaches that state only after the
  AP opens both `/dev/umts_ipc0` and `/dev/umts_rfs0` (INIT_END).
* `read()` returns one stripped payload fragment; multi-fragment messages
  must be reassembled on `len`.
* Stock does not pipeline SMS requests: `IpcModem::SendMessage` allocates one
  transaction per message (`AllocatedTransaction` @`0x19c2ec`, id 401 for
  send) and the reply is matched on `(main, sub)` through `IpcHistory`
  (`IpcProtocol40::FindRecord` @`0x1a42de`). A new RIL must keep one
  outstanding SMS request per `(main,sub)`.

All SMS frames are written with `main_cmd=0x04`.

## 4. Sub-command IDs (evidence: `GetSubCommandName` @`0x1a89b0`)

The name table for main 4 is selected at `0x1a8e08`; the string blob is at
`0x220d6a` (`SMS_SEND_MSG`, `SMS_INCOMING_MSG`, `SMS_READ_MSG`,
`SMS_SAVE_MSG`, `SMS_DEL_MSG`, `SMS_DELIVER_REPORT`, `SMS_DEVICE_READY`,
`SMS_SEL_MEM`, `SMS_STORED_MSG_COUNT`, `SMS_SVC_CENTER_ADDR`,
`SMS_SVC_OPTION`, `SMS_MEM_STATUS`, `SMS_CBS_MSG`, `SMS_CBS_CFG`,
`SMS_STORED_MSG_STATUS`, `SMS_PARAM_COUNT`, `SMS_PARAM`, `SMS_STATUS`).

| sub | name | AP→CP use | CP→AP use |
|---:|---|---|---|
| 1 | SMS_SEND_MSG | send SMS (§5.1) | send result (§6.2) |
| 2 | SMS_INCOMING_MSG | — | incoming SMS indication (§6.1) |
| 3 | SMS_READ_MSG | read stored SMS (§5.2) | stored-message result (§6.4) |
| 4 | SMS_SAVE_MSG | write SMS to SIM (§5.3) | save/status result (§6.3) |
| 5 | SMS_DEL_MSG | delete stored SMS (§5.4) | delete result (§6.6) |
| 6 | SMS_DELIVER_REPORT | MTSMS ack (§5.5/5.6) | delivery report (§6.5) |
| 7 | SMS_DEVICE_READY | device ready (§5.7) | device-ready notification (§6.7) |
| 8 | SMS_SEL_MEM | — | — (handler returns 0) |
| 9 | SMS_STORED_MSG_COUNT | count stored SMS (§5.8) | count result (§6.8) |
| 10 | SMS_SVC_CENTER_ADDR | get/set SC address (§5.9/5.10) | SC address result (§6.9) |
| 11 | SMS_SVC_OPTION | — | — (handler returns 0) |
| 12 | SMS_MEM_STATUS | set SIM-full flag (§5.11) | memory full/available (§6.10) |
| 13 | SMS_CBS_MSG | — | incoming CB message (§6.12) |
| 14 | SMS_CBS_CFG | set CB config (§5.12) | CB config result (§6.11) |

Responses echo the request's sub ID. Note the two *directions* can share a
number: `sub=6` is the AP→CP MTSMS ack (EXEC) and also the CP→AP delivery
report (RESP/INDI); `sub=1` is the send request and the send result. The
`cmd_type` byte (offset 6) disambiguates. p40's table at `0x1aff22` is a
different, partly-stubbed table; p40 is never selected (see §2).

## 5. AP→CP messages (byte-exact)

Templates below are the *payload* (after the 7-byte header); `<seq>` is the
sequence byte, `len` is the little-endian `u16` at offset 0.

### 5.1 Send SMS — `04/01/EXEC`
Builder `IpcProtocol41::IpcTxSendSms(unsigned char,int,int,unsigned char,unsigned char,char*,uint)`
@`0x1cd280` (p40 twin @`0x1af7cc`).

```
off       value
0..1      len = 11 + N            (N = SCA+TPDU blob length, 1..256)
2         seq
3         00
4         04
5         01
6         01
7         02                  message type (GsmSmsMessage::Init sets +0x10 = 2)
8         d                   (1 on the GSM path; see below)
9         ConvertDomainToIPCE(e)
10        N                   (u8; SCA+TPDU blob length)
11..      SCA+TPDU blob (N bytes)
```

* Write size is **exactly `11+N`** (`add.w r2,r5,#11; strh r2,[r4]` @`0x1cd30a`;
  the same `r2` is the `write()` length at `0x1cd316`). The p40 twin instead
  writes the whole 267-byte buffer (`movw r2,#267` @`0x1af852`) — irrelevant
  here because p40 is not selected.
* The blob is produced in `IpcModem::SendSms` (`0x19c2b8`): first
  `SmsCenter::BuildScaPdu(sca_digits, toa, out, &scaLen)` (`0x181340`),
  then `memcpy` of the raw TPDU (`Pdu+8`, `Pdu+0x108` bytes) right after it
  (`0x19c336-0x19c348`).
* `BuildScaPdu` (`0x181340`) encoding, exactly:
  * strip one leading `+` (`0x18134e`);
  * `d = strlen(digits)`, `k = (d+1)/2`; if `k > 11` it logs and returns -1,
    no frame is sent;
  * `out[0] = k+1` (number of octets following), `out[1] = toa`
    (`0x91` if the number had `+`, else `SmsCenter::GetToA()` @`0x1812c5`);
  * `out[2..2+k)` = nibble-swapped BCD; an odd final digit gets `0xF` in
    the high nibble (`moveq ip,#15` @`0x1813b4`);
  * `*outLen = k+2`.
  * Empty SMSC: the code still emits `01 <toa>` (len 2), not the GSM
    "no SCA" `00`. In practice `SmsManager::DoSendSms` substitutes the SIM
    SMSC when the request string is empty (`0x120a5c`).
* Argument provenance (`SmsManager::DoSendSms` @`0x1208e0` →
  `ModemProxy::SendSms` @`0xf8df8` → `IpcModem::SendSms` @`0x19c2b8`):
  * arg1 (`[7]`) = `GsmSmsMessage+0x10`; `GsmSmsMessage::Init`
    (`0x181471`) writes 2 (`str r1,[r4,#16]` @`0x181498`).
  * arg4 (`[8]`) = 1 on the GSM path: `DoSendSms` forces it when
    `ModemType == 1` (`str sl,[sp,#32]` @`0x1209b2`); otherwise it is
    `1` when the concat-header object (`GsmSmsMessage+0x54`) has
    `[+4] < [+8]`, else `2` (`0x12098c-0x1209a0`). **[UNKNOWN]** exact
    semantics; GSM always yields 1.
  * arg5 (`[9]`) = `ConvertDomainToIPCE(DomainSelector::GetDomain())`.
    `DomainSelector` is allocated at `SmsManager+0x50` in
    `SmsManager::Initialize` (`0x125108-0x125116`), its `GetDomain` is
    vtable slot +8 (`_ZTV14DomainSelector` address point, resolved to
    `0x121f04`). `ConvertDomainToIPCE` @`0x1cd0f8` maps
    `0 -> 2, 1 -> 3, 3 -> 1, everything else -> 0`.
    `GetDomain` depends on CSC/IMS state (`ReadProperty`/`ImsManager`), so
    the runtime value is **[UNKNOWN]** on a fresh install; a
    non-IMS single-SIM GSM setup yields the `0 -> 2` branch in the common
    case (verified code path, not observed value).
  * args 2 and 3 are **not used** by the p41 builder at all (only logged).
* `[UNKNOWN]` The CP's interpretation of `[8]`/`[9]` and whether it
  validates them. A first hardware test should send `02 01 02` for
  `[7],[8],[9]` and fall back to `02 01 00` if the CP rejects the frame.

#### Worked example (single-part GSM7 SMS, no UDH)

SMSC `+15551234567`, destination `+15551234567`, text `hi`, TP-MR 0:

```
SCA blob : 07 91 51 55 21 43 65 F7                  (8 bytes)
TPDU     : 01 00 0B 91 51 55 21 43 65 F7 00 00 02 E8 34   (15 bytes)
blob     : 07 91 51 55 21 43 65 F7 01 00 0B 91 51 55 21 43 65 F7
           00 00 02 E8 34                            (N = 0x17 = 23)
frame    : 22 00 <seq> 00 04 01 01 02 01 02 17 <23 bytes>
           payload len 0x22 = 34, write() size 34
```

(`E8 34` is `h`=0x68 / `i`=0x69 packed into 7-bit septets:
`0x68 | ((0x69&1)<<7) = E8`, `0x69>>1 = 34`.)

### 5.2 Read stored SMS — `04/03/GET`
Builder `IpcProtocol40::IpcTxGetReadMsg(int,int)` @`0x1af9c6` (shared with p41).
Length 10, written 10 bytes.

```
0a 00 <seq> 00 04 03 02 <index> <u16 (storage-1)>
```
`[7]` = arg1 (index), `[8..9]` = `arg2-1` (`subs r2,r6,#1` @`0x1af9e6`;
`storage == 0` therefore becomes 0xffff). Arg2 is the storage id.

### 5.3 Write SMS to SIM — `04/04/EXEC`
Builder `IpcProtocol41::IpcTxWriteSmsToSim(int,int,int,char*,int)` @`0x1cd110`
(p40 twin @`0x1af664`). Written size **268** always; `len = 12 + N`.

```
0..1      len = 12 + N
2         seq
3         00
4         04
5         04
6         01
7         02                       (arg1; message type, GSM = 2)
8..9      u16 (arg2 == 255 ? 0xffff : arg2-1)   SIM record number, 0-based
10        (arg3 + 1) & 0xff       record status byte (SST)
11        N                        PDU length
12..      SCA+TPDU blob (N <= 256)
```
Evidence: `0x1cd138-0x1cd190` (the 255 test is `cmp r5,#255; movweq r5,#0xffff`
@`0x1cd138`, `adds r5,#12` @`0x1cd17a`, `movw r2,#268` @`0x1cd188`).
Caller `SmsManager::DoWriteSmsToSim` @`0x120f7c` passes `arg1=2` hard-coded,
`arg2=[msg+0x1c]`, `arg3=[msg+0x58]`.

### 5.4 Delete stored SMS — `04/05/EXEC`
Builder `IpcProtocol40::IpcTxDeleteMsg(int,int)` @`0x1af934` (shared).
Length 10, written 10.

```
0a 00 <seq> 00 04 05 01 <index> <u16 (storage-1)>
```
`[7]` = arg1, `[8..9]` = `arg2-1` (`0x1af954`).

### 5.5 MTSMS ack (delivery report) — `04/06/EXEC`
Builder `IpcProtocol41::IpcTxDeliveryReport(int a,int b,int c,int d)`
@`0x1cd340`. Length **12**, written 12.

```
0..1  0c 00
2     seq
3     00
4     04
5     06
6     01
7     a                     (incoming message type; 2 for GSM)
8..9  u16 status
10    b                     ("tpid", must be <= 0xff)
11    00
```

Status selection (`0x1cd398-0x1cd424`):
* `a == 2`:  `c == 1` → status `0x0000` (success, buffer stays zeroed);
  otherwise `d == 0x80` → `0x8080`; `d == 0x81` → `0x8081`;
  `d == 22` → `0x8016`; else → `0x806f`.
* `a != 2`:  `c == 0` → `0x0000`; else status = low 16 bits of `d`.
* `b == 0x100` → the builder logs `tpid is invalid value 256.`
  (`0x22e3e8`) and returns -1 without writing.

Argument provenance (`SmsManager::DoSmsAcknowledge` @`0x1225d0`):
`a = lastGsmSms+0x10` (= incoming frame byte `[7]`, 2 for GSM),
`b = lastGsmSms+0x20` (= incoming frame byte `[11]`),
`c = requestData+8` (RIL success flag),
`d = requestData+12` (RIL cause; `211 (0xd3)` is remapped to `0x80` at
`0x1226ac-0x1226b8`).
Arguments reach the builder through
`ModemProxy::SendDeliveryReport` (`0xf8e80`, proxy vtable slot 0x370) and
`IpcModem::SendDeliveryReport` (`0x19c511`, protocol vtable slot 0x350).

### 5.6 MTSMS ack carrying the received PDU — `04/06/EXEC`
Builder `IpcProtocol41::IpcTxDeliveryReportWithPdu(int a,int b,int c,char* pdu,uint n)`
@`0x1cd4b0`. Written `12+n` bytes.

```
0..1  len = 12 + n
2     seq
3     00
4     04
5     06
6     01
7     a
8..9  status: 0x0000 if (a == 2 && c == 1), else 0x806f
10    b
11    n
12..  raw PDU (no SCA prefix)
```
Reached from `SmsManager::DoSmsAcknowledgeWithPdu` @`0x122744`.

### 5.7 Device ready — `04/07/SET`
Builder `IpcProtocol40::IpcTxSetDeviceReady()` @`0x1afa78` (shared).
Literal: `07 00 <seq> 00 04 07 03`. Written 7 bytes.

### 5.8 Stored message count — `04/09/GET`
Builder `IpcProtocol40::IpcTxGetStoredMsgCount(int)` @`0x1afaac` (shared).

```
08 00 <seq> 00 04 09 02 <storage>
```
Written 8 bytes. `SmsManager::DoGetMsgCount` (`0x121612`) passes `storage=2`.

### 5.9 Get SMSC address — `04/0a/GET`
Builder `IpcProtocol41::IpcTxGetSmscAddress()` @`0x1cd594`.
Literal: `07 00 <seq> 00 04 0a 02`. Written 7 bytes.
(p40's @`0x1af8c8` is byte-identical.)

### 5.10 Set SMSC address — `04/0a/SET`
Builder `IpcProtocol40::IpcTxSetSmscAddress(char*,uint)` @`0x1afa10` (shared).

```
0..1  len = 19 (fixed)
2     seq
3     00
4     04
5     0a
6     03
7..18 SCA bytes, arg2 bytes copied, zero padded to 12
```
`memcpy_chk` dest `buf+7`, size `arg2`, max 12 (`0x1afa50-0x1afa52`);
written 19 bytes.

### 5.11 Set memory-full flag — `04/0c/SET`
Builder `IpcProtocol40::IpcTxSetMemStatus(int)` @`0x1afae2` (shared).

```
08 00 <seq> 00 04 0c 03 <flag>
```
Written 8 bytes.

### 5.12 Set cell-broadcast config — `04/0e/SET`
Builder `IpcProtocol41::IpcTxSetCBConfig(int a,int b,int c,int total,int* mids)`
@`0x1cd5cc` (signature: `a,b,c` in r1..r3, `total` at `[sp+0]`, `mids` at
`[sp+4]`).

```
0..1  len = 10 + 2*total
2     seq
3     00
4     04
5     0e
6     03
7     a
8     b
9     total                     (= arg4; may be widened to it after padding)
10..  2*total bytes: the first `c` entries are u16 MID values taken from
      `mids[]` and byte-swapped (`rev16`, `0x1cd648`), the rest are 0xffff
      (padding loop `0x1cd680-0x1cd690`); skipped entirely when `a & 0x80`
```
Written `len` bytes (`0x1cd6a0-0x1cd6a8`).

### 5.13 Other p41 SMS builders (not needed for GSM SMS, listed for completeness)

| builder | message | notes |
|---|---|---|
| `IpcProtocol41::IpcTxSetCBActivation(int)` @`0x1cd738` | 04/0e/EXEC | len 50, `write()` 210; `[7]` = 1 or 2 from a bit test on the argument (mask `0x80000001`, `0x1cd74e-0x1cd786`), `[8]`=0xB0 when arg==3 else 0x01, `[9]`=0x0A, `[10..49]`=0xFFFF x20 |
| `IpcProtocol41::IpcTxSendCdmaSms` @`0x1cd83d` / p40 @`0x1afc39` | 04/01/EXEC, 521-byte buffer | CDMA only |
| `IpcProtocol41::IpcTxWriteSmsToRuim` @`0x1cd1c1` | 04/04/EXEC, 525 bytes | CDMA only; `[7]=0x12` |
| `IpcProtocol40::IpcTxDeliveryReport` @`0x1af888` | — | **stub**, returns -1; p41 overrides |

## 6. CP→AP messages

Dispatch: `IpcProtocol41::IpcRxSms(char*,int,int&,RegistrantType&)` @`0x1cdcce`
reads `buf[5]` (sub_cmd), rejects `sub-1 > 13`, and jumps through the `tbb`
table at `0x1cdce2`:

| sub | handler | out-param / return |
|---:|---|---|
| 1 | `IpcRxSendMsg` @`0x1cdb70` | returns `SmsResponse*`; registrant 4 on bad type |
| 2 | `IpcRxIncomingMsg` @`0x1cdd78` | returns `GsmSmsMessage*`/`CdmaSmsMessage*`, registrant = **57 (0x39)** |
| 3 | `IpcRxReadMsg` @`0x1cd966` | returns `SimSmsMessage*` |
| 4 | `IpcRxSaveMsg` @`0x1cd8e4` | returns `SimSmsMessageResult*` |
| 5 | `IpcRxDeleteMsg` @`0x1cd94c` | returns `SimSmsMessageResult*` |
| 6 | `IpcRxDeliveryReportMsg` @`0x1cd9b8` | registrant = 0 or 7 |
| 7 | `IpcRxDeviceReady` @`0x1afed0` | registrant = **62 (0x3e)** |
| 8 | — (returns 0) | unhandled |
| 9 | `IpcRxStoredMsgCount` @`0x1cdcae` | returns `SmsStorage*` |
| 10 | `IpcRxSmsc` @`0x1cda20` | returns `SmsCenter*` |
| 11 | — (returns 0) | unhandled |
| 12 | `IpcRxMemoryStatus` @`0x1cdc90` | registrant = 60/61 |
| 13 | `IpcRxIncomingCBMsg` @`0x1cdaf0` | returns `CBMessage*`, registrant = **58 (0x3a)** |
| 14 | `IpcRxCBConfig` @`0x1cda3c` | returns `BroadcastSmsConfig*` |

The table bytes at `0x1cdce6` are
`07 0c 25 20 2a 11 34 46 39 3e 46 2f 16 1b` (byte per sub-1); each
non-`0x46` entry is `ldr rX,[this]; ldr.w rY,[rX,#slot]`. Slot values in
order: `0xa88, 0xaac, 0xa94, 0xa90, 0xa98, 0xa8c, 0xab4, —, 0xaa0, 0xaa4,
—, 0xa9c, 0xab0, 0xaa8`.

### 6.1 Incoming SMS — `04/02/INDI`
Parser `IpcProtocol41::IpcRxIncomingMsg` @`0x1cdd78`.

```
[7]   message type: 2 = GSM, 1 = CDMA  (other -> log, return 0)
--- GSM path ([7] == 2), 0x1cddc4-0x1cde14 ---
[8]   -> GsmSmsMessage+0x18                       [UNKNOWN]
[11]  -> GsmSmsMessage+0x20                       [UNKNOWN]
[12]  total byte count of the SCA+PDU blob
[13]  SCA first octet = number of SCA octets following it
      SCA total = [13]+1        (SmsCenter::SmsCenter(char*,int) @0x181078
                                 stores `[data]+1` at SmsCenter+0x14)
[13+scaTotal ... 13+[12])  raw TPDU; length = [12]-scaTotal
```
Construction: `GsmSmsMessage()` → `SmsCenter(buf+13, 12)` →
`Pdu(buf+13+scaTotal, [12]-scaTotal)` → `+0x10=[7]`, `+0x18=[8]`,
`+0x20=[11]` → `GsmSmsMessage::Parse(0)`.
`Pdu(char*,int)` @`0x180b10` does a raw `memcpy` (not hex decoding), so the
CP sends binary TPDUs.
CDMA path: `CdmaSmsParser(msg, buf+8, arg3)` then `Decode()`.

### 6.2 Send result — `04/01/RESP`
Parser `IpcProtocol41::IpcRxSendMsg` @`0x1cdb70`.

```
[7]   type: 2 = GSM, 1 = CDMA, else registrant=4 and return 0
--- GSM (0x1cdba2-0x1cdc00) ---
[8..9]  u16 result code  -> GetSmsResult(code)   (second arg supplied as [15]
                                                but ignored by the callee)
[10]    value -> SmsResponse third field
[11]    PDU length n
[12..12+n)  raw PDU
```
`GetSmsResult` @`0x1cd090` maps the CP code to an `SmsResult` enum:

| CP code | SmsResult |
|---:|---:|
| 0 | 0 |
| 11 | 7 |
| 12, 13 | 2 |
| 18 | 8 |
| 19 | 13 |
| > 0x8000 | 4, with `code-0x8000` in a second field |
| other | 0xff |

**Note:** the call passes `[15]` as the second argument, but
`GetSmsResult` never reads its third register (`r3` is unused in the whole
function `0x1cd090-0x1cd0f0`); only the `u16[8..9]` code selects the
result. So the apparent overlap with the response PDU is harmless in the
stock code.

CDMA path (`0x1cdc26`): `SmsResponse(u16[10..11], [13], [12])`.

### 6.3 Save/status result — `04/04`
`IpcProtocol41::IpcRxSaveMsg` @`0x1cd8e4`:

```
code = u16[8]
if (code == 18 || code == 0x8080 || code == 0x8081) code = 22
return SimSmsMessageResult(code, u16[10] + 1)
```
(`add r3,r4,#0x7f80; uxth; cmp #2` @`0x1cd8ee-0x1cd8f8` covers
0x8080/0x8081; the equality test at `0x1cd8ea` covers 18.)

### 6.4 Stored message — `04/03`
`IpcProtocol41::IpcRxReadMsg` @`0x1cd966`:

```
SmsCenter(buf+12, 12)              SCA total = SmsCenter+0x14
Pdu(buf+12+scaTotal, [11]-scaTotal)
SimSmsMessage+0x1c = u16[8] + 1    (index)
SimSmsMessage+0x58 = [10]
SimSmsMessage+0x5c = [7]
```

### 6.5 Delivery report (outgoing SMS) — `04/06`
`IpcProtocol41::IpcRxDeliveryReportMsg` @`0x1cd9b8`:

```
code = u16[7]                      (unaligned u16, `ldrh.w r2,[r4,#7]` @0x1cd9c8)
res  = GetSmsResult(code, -1)
registrant = (res.result == 0) ? 0 : 7
```
No message object is returned; only the registrant event is emitted. The
raw status is therefore **not** forwarded to the framework by this parser —
the RIL reports `RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT` (event id 7)
regardless of the TP-Status value.

Because the CP→AP sub echoes the request sub, this handler is also the one
that consumes the CP's reply to an AP→CP MTSMS ack (§5.5); a status report
for an outgoing SMS and an ack result are both `04/06` and cannot be told
apart statically. The stock code does not try: it only emits event 0/7. How
the CP distinguishes them is **[UNKNOWN]**.

### 6.6 Delete result — `04/05`
`IpcProtocol41::IpcRxDeleteMsg` @`0x1cd94c`:

```
return SimSmsMessageResult(u16[8], u16[10] + 1)
```

### 6.7 Device ready notification — `04/07`
`IpcProtocol40::IpcRxDeviceReady` @`0x1afed0` (shared): registrant = 62.
No payload bytes read.

### 6.8 Stored-message count — `04/09`
`IpcProtocol41::IpcRxStoredMsgCount` @`0x1cdcae`:

```
SmsStorage+0x08 = u8 [7]        storage id
SmsStorage+0x14 = u16[8]        [UNKNOWN] which of used/capacity
SmsStorage+0x10 = u16[10]       [UNKNOWN] the other count
```

### 6.9 SMSC address — `04/0a`
`IpcProtocol41::IpcRxSmsc` @`0x1cda20`: `SmsCenter(buf+7, len-7)`, i.e. the
whole body is SCA bytes with the usual leading length octet. (p40's parser
@`0x1afd5c` instead treats `[7]` as a length and digits as `[8..]` — another
reason not to target p40.)

### 6.10 Memory status — `04/0c`
`IpcProtocol41::IpcRxMemoryStatus` @`0x1cdc90`:

```
if ([7] == 2 && [8] == 2) registrant = 60   (storage full)
if ([7] == 1 && [8] == 2) registrant = 61   (storage available)
```

### 6.11 Cell-broadcast config — `04/0e`
`IpcProtocol41::IpcRxCBConfig` @`0x1cda3c`: byte-swaps each u16 MID in
place (`rev16` @`0x1cda7e`) for `count = [10]` entries starting at `[11]`,
then builds `BroadcastSmsConfig([7], [8], [9], count, &buf[11])`.
The CB config request and its response both use sub 14; the incoming CB
indication uses sub 13.

### 6.12 Incoming cell-broadcast message — `04/0d`
`IpcProtocol41::IpcRxIncomingCBMsg` @`0x1cdaf0`:

```
CBMessage+0x08 = ([7] == 2) ? 2 : 1
CBMessage+0x0c = [8]          length
CBMessage+0x0f.. = [9..9+len)  text bytes
registrant = 58
```

## 7. Minimum send/receive sequence over `/dev/umts_ipc0`

Prerequisites (kernel side, `spec/framing.md`): CP booted; both
`/dev/umts_ipc0` and `/dev/umts_rfs0` open; `IOCTL_MODEM_STATUS == 4`
(`STATE_ONLINE`), otherwise every `write()` is `-EPERM`.

1. **Optional, stock order:** send `04/07/SET` device-ready
   (`07 00 seq 00 04 07 03`), then `04/09/GET` (`08 00 seq 00 04 09 02 02`).
   The stock RIL sends device-ready from `SmsManager::OnDeviceReady`
   (`0x120f50`, internal request 1010).
2. **Send:** one `write()` of `04/01/EXEC` (§5.1) with an even/odd SCA+TPDU
   blob; write size = `11+N`.
3. **Send result:** read payloads until `main==04 && sub==01`; match by
   `(main,sub)` and the outstanding transaction. The reply carries the
   status code (`u16@8`) and possibly a PDU (`[12..]`, len `[11]`).
4. **Incoming SMS:** CP sends `main=04, sub=02` (INDI). A send write
   can be preceded by unrelated notifications; the reader must dispatch on
   `(main,sub)` and reassemble by `len`.
   Parse per §6.1. Deliver to the framework.
5. **Ack:** the framework's `RIL_REQUEST_SMS_ACKNOWLEDGE` maps to
   `04/06/EXEC` (§5.5), 12 bytes, sent once per incoming SMS.
   `OnIncomingGsmSms` arms a **60 000 ms** timer when
   `GsmSmsMessage+0x24 != 0` (`movw r1,#60000` @`0x123172`). On expiry
   `SmsManager::OnMTSmsAckTimeout` (`0x122994`) calls
   `ModemProxy::SmsIncomingAckTimeout` → `IpcModem::SmsIncomingAckTimeout`
   @`0x19896c`, which is a **stub that only logs and returns -1** — the CP
   is *not* informed. Consequence: if the ack is not sent, the CP's
   retransmission behaviour is **[UNKNOWN]**; the RIL just fails locally.
6. **Status report for an outgoing SMS:** CP sends `04/06` (§6.5); RIL
   emits registrant/event 7. Match it to the outstanding send by the u16
   at `[7]` (the TP-MR echoed by the CP is **[UNKNOWN]**; stock does not
   match it to a request — it only forwards the event).

There is **no AT/text-mode path**: the RIL always transmits a binary
SCA+TPDU blob; the framework supplies the hex PDU in `RIL_REQUEST_SEND_SMS`
and `Pdu::SetRawData`/`GsmSmsMessage::Parse` convert it. A new RIL must
encode GSM 03.40 TPDUs itself (or reuse the framework's hex PDU).

## 8. Concatenated SMS

* No reassembly exists in `libsec-ril.so`: there is no symbol, string or
  code path that merges segments; the only "Concatenated" string is
  `%s: Concatenated IPC messages detected. expected %d remaining %d`
  (`0x220126`), which is SIPC5 *frame* reassembly, not SMS.
* `GsmSmsMessage` does parse a UDH header object at `+0x54` (fields at
  `+0`, `+4`, `+8`, `+0xc`, `+0x10`; printed by `ToString` @`0x1815cc`),
  and `DoSendSms` reads `[+4]`/`[+8]` to derive the send byte `[8]`
  (§5.1). Segment *content* is never combined.
* Consequence: each segment arrives as a separate `04/02` indication and
  each is acked separately; the Android framework's
  `SmsMessage`/`InboundSmsHandler` reassembles by `(ref, total, seq)`.
  A replacement RIL only has to relay each TPDU faithfully.

## 9. SIM storage

All SIM-SMS operations are IPC messages; **RFS is not used for SMS**.

| operation | request | response (CP→AP) |
|---|---|---|
| write | `04/04/EXEC` (§5.3) | `04/04` `SimSmsMessageResult(status, index+1)` |
| read | `04/03/GET` (§5.2) | `04/03` `SimSmsMessage` incl. SCA+PDU |
| delete | `04/05/EXEC` (§5.4) | `04/05` `SimSmsMessageResult(index, status+1)` |
| count | `04/09/GET` (§5.8) | `04/09` `SmsStorage{id, used, capacity}` |
| capacity flag | `04/0c/SET` (§5.11) | — |
| full/available notice | — | `04/0c` registrant 60/61 |

Record numbering is zero-based on the wire (`index-1`), and `0xffff` means
"no record" for writes (`arg2 == 255`). Status byte semantics (SST) are
**[UNKNOWN]** beyond the `arg3+1` encoding.
Class-2 handling: `SmsManager::OnIncomingGsmSms` enters its write-to-SIM
branch when `GsmSmsMessage+0x40 == 3` (`0x122ffe-0x123042`) and then sends a
`04/04` write. The stock code names this "Class2"; the field's exact
derivation in `ParseUserData` was not resolved, so treat the identity of
`+0x40` as **[UNKNOWN]**.

## 10. RFS channel

* SMS builders write only through the FMT channel object at
  `IpcModem+0x1c` (protocol `this+0x8`) — `IoChannel::Write` vtable +0x18
  (`0x1cd316`, `0x1cd190`, `0x1cd45a`).
* No SMS code path calls `Modem::SendRfsMessage` (`0xf...`), and no SMS
  parser reads from the RFS channel. RFS (`/dev/umts_rfs0`, SIPC5 ch 245)
  is EFS/NV access only.
* `/dev/umts_rfs0` must still be *opened* (and only opened) so the kernel
  answers `CMD_PHONE_START` with `INIT_END` (`rild_ready()`).

## 11. Unknowns / what hardware must settle

1. Meaning and validation of send bytes `[8]` (1 on the GSM path) and `[9]`
   (`ConvertDomainToIPCE(GetDomain())`); the runtime domain value depends
   on CSC/IMS and is not statically resolvable.
2. Meaning of incoming GSM bytes `[8]`, `[9]`, `[10]`, `[11]`; only `[8]`
   (`GsmSmsMessage+0x18`) and `[11]` (`+0x20`, echoed into the ack `[10]`)
   are used, and their semantic names are not recovered.
3. Whether the CP retransmits an unacked `04/02` indication (the RIL's
   60 s timeout is purely local — §7.5).
4. Matching a send result to its request: the stock code keys only on
   `(main,sub)` and does not compare a message reference (the `u16` at
   `[8..9]` is the result code, not an id).
5. TP-MR matching between a send and its `04/06` status report; stock does
   not match.
6. Exact `arg4/arg5` of `IpcTxSendSms` on multipart messages (only
   reachable through the non-GSM `ModemType != 1` branch).
7. `SMS_SEL_MEM` (sub 8) request layout — no builder exists in the binary;
   only the response handler is present.
8. `SMS_STATUS` (sub 18), `SMS_PARAM*` (16/17) — named in the table but no
   builder/parser found; likely unused on this device.

## 12. Evidence index

| address | object |
|---:|---|
| `0x198b2c` | `IpcModem::CreateProtocol` (p40 only for ver 0) |
| `0xf70bc` | `ModemConfig::Init` (proto ver → `+0x1c`) |
| `0xf754c` | `ModemFactory::LoadModemConfiguration` |
| `0x1cd280` | `IpcProtocol41::IpcTxSendSms` |
| `0x1af7cc` | `IpcProtocol40::IpcTxSendSms` |
| `0x19c2b8` | `IpcModem::SendSms` (SCA+PDU concat) |
| `0xf8df8` | `ModemProxy::SendSms` |
| `0x1208e0` | `SmsManager::DoSendSms` |
| `0x181340` | `SmsCenter::BuildScaPdu` |
| `0x180b10` | `Pdu::Pdu(char*,int)` (raw copy) |
| `0x181471` | `GsmSmsMessage::Init` (`+0x10 = 2`) |
| `0x1cd0f8` | `IpcProtocol41::ConvertDomainToIPCE` |
| `0x121f04` | `DomainSelector::GetDomain` |
| `0x1250f0` | `SmsManager::Initialize` (`+0x50 = DomainSelector`) |
| `0x1a42d2` | `SetMsgSequence` (byte 2) |
| `0x1cd110` | `IpcProtocol41::IpcTxWriteSmsToSim` |
| `0x1af9c6` | `IpcTxGetReadMsg` |
| `0x1af934` | `IpcTxDeleteMsg` |
| `0x1cd340` | `IpcProtocol41::IpcTxDeliveryReport` |
| `0x1cd4b0` | `IpcProtocol41::IpcTxDeliveryReportWithPdu` |
| `0x1225d0` | `SmsManager::DoSmsAcknowledge` |
| `0x122744` | `SmsManager::DoSmsAcknowledgeWithPdu` |
| `0x122f20` | `SmsManager::OnIncomingGsmSms` (60 s timer) |
| `0x122994` | `SmsManager::OnMTSmsAckTimeout` |
| `0x19896c` | `IpcModem::SmsIncomingAckTimeout` (stub) |
| `0x1afa78` | `IpcTxSetDeviceReady` |
| `0x1afaac` | `IpcTxGetStoredMsgCount` |
| `0x1afae2` | `IpcTxSetMemStatus` |
| `0x1afa10` | `IpcTxSetSmscAddress` |
| `0x1cd594` | `IpcProtocol41::IpcTxGetSmscAddress` |
| `0x1cd5cc` / `0x1cd6cc` | `IpcTxSetCBConfig` / `IpcTxGetCBConfig` |
| `0x1cdcce` | `IpcProtocol41::IpcRxSms` (dispatch) |
| `0x1cdd78` | `IpcProtocol41::IpcRxIncomingMsg` |
| `0x1cdb70` | `IpcProtocol41::IpcRxSendMsg` |
| `0x1cd090` | `IpcProtocol41::GetSmsResult` |
| `0x1cd8e4` / `0x1cd966` / `0x1cd94c` | save / read / delete parsers |
| `0x1cd9b8` | `IpcProtocol41::IpcRxDeliveryReportMsg` |
| `0x1cdcae` / `0x1cda20` / `0x1cdc90` | count / smsc / memory status |
| `0x1cda3c` / `0x1cdaf0` | CB config / incoming CB |
| `0x1aff1e` | p40 sub-dispatch table (not used) |
| `0x220d6a` | SMS sub-command names |

All addresses are for `libsec-ril.so` only (not the DSDS build). No
hardware was available; nothing here was observed at runtime.
