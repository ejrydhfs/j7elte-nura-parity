# j7elte — Samsung SIPC5/IPC message catalog (`rild` / `libsec-ril`)

Status: **static reverse engineering, no hardware.** Every ID, length and offset below is
traced to a concrete instruction or data address in the ROM binaries (section
[§10](#10-evidence-index)); anything not traced is marked **UNKNOWN** rather than guessed.

Scope of this document: the bytes that cross `/dev/umts_ipc0`. It is *not* an
implementation; it is the reference the `j7elte-ril` client and any future RIL are built
from.

---

## 1. Binaries and hashes

Extracted from `system.raw` (ext4, no `/system` prefix — paths are absolute in the image)
with `debugfs -R 'dump …'`; the same files are mirrored in `parity/userspace/ril/stock/`.

| Image path | Size (B) | SHA-256 | Role |
|---|---:|---|---|
| `/bin/rild` | 22 072 | `64650e0b4ab11816539264b351da2d3dd965fd3753dd4f250c9191b0e18c57f4` | Android RIL daemon: loads the vendor RIL and starts the event loop |
| `/lib/libril.so` | 82 920 | `ad8072ac410f914fa87942aabd58ef71de73fbec1d664429a66b8ff2672b5618` | AOSP RIL framework: socket protocol, request dispatch/response queue |
| `/lib/libsec-ril.so` | 2 468 316 | `09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73` | **the vendor RIL**: `RIL_Init`, `IpcProtocol40/41`, `IpcModem`, all IPC |
| `/lib/libsec-ril-dsds.so` | 2 468 316 | `640f0ea0a7c6d650d3ede7e5410af72717db1fda35c6f78a6d07bfb78a67cfa0` | dual-SIM build — **materially different code** (2 126 295 of 2 468 316 bytes differ); not covered here |
| `/bin/at_distributor` | 103 812 | `9d01e6167da24cf808fe976842552b9e3f1640a0faa72982c8b4c7d3f9a8435e` | AT-emulation bridge for the external-AT path only |
| `/lib/libsecril-client.so` | 30 208 | `0506f623307ea4dc039164b987b4042de3b829e60fbeffa0a3927b7eea8d8020` | clients of the RIL socket |
| `/lib/librilutils.so` | 17 916 | `066981be45065af9633015cabdcecf54bb726562d7957808cbcf8402a97dc894` | record-stream helpers (`rilutils`) |
| `/lib/libaudio-ril.so` | 26 160 | `5e9103221c2f940c63ef5fe1a3e351794ed62cda709ed20b4fde88ba48c1e0fa` | call-audio glue |

`boot_out/rd/default.prop:4-5`:

```
rild.libpath=/system/lib/libsec-ril.so
rild.libpath2=/system/lib/libsec-ril-dsds.so
```

so `/lib/libsec-ril.so` is the library this single-SIM device loads.

All addresses in this document are **virtual addresses in the unrelocated
`libsec-ril.so`** (`.text` VMA 0x0d7010, file offset 0x0cf010; Thumb functions have the
low bit set in `.dynsym`, the addresses quoted here are already masked).

---

## 2. What is handled where

| Layer | What it does | Evidence |
|---|---|---|
| `rild` | `dlopen()`s the path from `rild.libpath`, `dlsym()`s `RIL_Init`/`RIL_SAP_Init`, registers with `libril.so`, runs `RIL_startEventLoop`. **No modem I/O, no IPC IDs.** | strings `rilLibPath = %s`, `RIL_Init not defined or exported in %s`, `**RIL Daemon Started**`, `rild.libpath` in `rild`; imports `RIL_register`, `RIL_startEventLoop`, `RIL_register_socket`, `RIL_onRequestComplete`, `RIL_onUnsolicitedResponse`, `RIL_requestTimedCallback` |
| `libril.so` | AOSP framework half: owns `/dev/socket/rild*`, parses `RIL_REQUEST_*`/`RIL_UNSOL_*` parcels, calls the vendor `onRequest` pointer registered by `RIL_Init`, queues responses. **No modem I/O; knows no IPC ID.** | exports `RIL_register` @0xa824, `RIL_onRequestComplete` @0xa4f8, `RIL_onUnsolicitedResponse` @0x6f14, `RIL_startEventLoop` @0x6dec, `RIL_requestTimedCallback` @0x71d0, `RIL_setRilSocketName` @0x6dd4 |
| `libsec-ril.so` | Everything below the RIL request API: opens `/dev/umts_ipc0`, `/dev/umts_rfs0`, `/dev/smd0`; builds/parses the 7-byte IPC header; owns every `main_cmd`/`sub_cmd` in this catalog. | `RIL_Init` @0xdb300, `RIL_SAP_Init` @0xdb550; `IpcProtocol40/41`, `IpcModem`, `Ipc41PacketBuilder` symbols; strings `/dev/umts_ipc0`, `/dev/umts_rfs0` |
| kernel `modem_v1` | SIPC5 link framing, RX demux, channel bookkeeping; **does not interpret the 7-byte IPC header**. | `misc_write`/`misc_read`/`rx_fmt_ipc`; `struct sipc_fmt_hdr` |

Consequence: the RIL request numbers (`RIL_REQUEST_*`) are AOSP values, not Samsung
values; the Samsung IDs in this catalog are *payload* of exactly one `write()` on
`/dev/umts_ipc0`. A shim must translate both layers.

---

## 3. The channel

`arch/arm64/boot/dts/modem-isla-pdata.dtsi:53-58` (same in the ROM DTB):

```
io_device_0 {
    iod,name = "umts_ipc0";
    iod,id   = <235>;      /* SIPC5 channel byte 0xEB */
    iod,format = <0>;      /* IPC_FMT */
    iod,io_type = <0>;
    iod,links = <0x200>;
    iod,attrs = <0x2>;     /* ATTR_SIPC5 only → NO ATTR_NO_LINK_HEADER */
};
```

| Property | Behaviour | Source |
|---|---|---|
| one `write()` = one FMT message | `misc_write()` prepends a `struct sipc5_link_header` (`cfg`,`ch=235`,`len`) per write and hands the frame to `shmem_send()`; `max_tx_size` is 0 for this iodev, so no split | `drivers/misc/modem_v1/modem_io_device.c:851-960` |
| RX strips the link header | `rx_fmt_ipc()` → `rx_frame_with_link_header()` → `skb_pull(hdr_len)`; then `misc_read()` copies the FMT payload to user | `modem_io_device.c:257-278`, `:974-1001` |
| empty read | waits 100 ms, then returns 0 | `modem_io_device.c:982-991` |
| not online | `-EPERM` while `mc->phone_state != STATE_ONLINE` | `modem_io_device.c:869-876` |
| padding | `ld->aligned` is only true for `LINK_ATTR_BOOT_ALIGNED` during boot; `cmd_init_start_handler()` sets it from `LINK_ATTR_IPC_ALIGNED`, which is **not** in `mif,link_attrs = <0x7C8>` → **no 4-byte padding on IPC frames** | `link_device_shmem.c:311-314`, DT `:49` |
| peer requirement | `rild_ready()` needs *both* `umts_ipc0` and `umts_rfs0` open before the kernel answers `PHONE_START` with `INIT_END` | `link_device_shmem.c:261-285` |

Userspace side (all in `libsec-ril.so`):

| Step | Implementation | Address |
|---|---|---|
| open channel | `open(name, O_RDWR)`, 51 attempts with `usleep` | `IoChannel::Open` @0xda13c |
| readiness gate before every read | `ioctl(fd, 0x6f27, len)`; only `ret == 4` (`STATE_ONLINE`) proceeds to `read()` | `DevIoctlIoChannel::Read` @0xd9fcc |
| `0x6f27` | `IOCTL_MODEM_STATUS` = `_IO('o',0x27)`; returns `enum modem_state`, `STATE_ONLINE == 4` | `modem_prj.h:84`, `:145-156` |
| read | `read(fd, buf, len)` | `IoChannel::Read` @0xd9e2c |
| write | `write(fd, msg, len)` (the 7-byte IPv4 message) | `IoChannel::Write` @0xd9ee4 |
| length self-check before TX | `IpcTxRawIpc()` rejects the frame unless `*(u16*)msg == len` | `IpcProtocol40::IpcTxRawIpc` @0x1a4340 |

---

## 4. The FMT message header

The kernel ships the authoritative definition (`drivers/misc/modem_v1/modem_prj.h:205-212`),
and every builder/parser in `libsec-ril.so` matches it byte for byte:

```c
struct __packed sipc_fmt_hdr {
    u16 len;       /* total message length, header included */
    u8  msg_seq;   /* AP→CP sequence number */
    u8  ack_seq;   /* response acknowledgement (0 in every AP builder traced) */
    u8  main_cmd;
    u8  sub_cmd;
    u8  cmd_type;
};                 /* 7 bytes; payload follows at offset 7 */
```

Binary evidence:

| Field | Evidence |
|---|---|
| `len` (u16 @0) | every builder does `strh <total>, [buf]`; `IpcTxRawIpc` @0x1a4340 rejects `ldrh [buf] != arg_len` |
| `msg_seq` (@2) | `IpcProtocol::SetMsgSequence(char*)` @0x1a42d2 is `GenerateMsgSequence(); strb r0,[r1,#2]`; `GenerateMsgSequence` @0x1a34fe increments the byte at `this+0x4c` |
| `ack_seq` (@3) | zero-filled by `memset` and never written by any AP builder traced |
| `main_cmd` (@4) | `IpcProtocol40::IpcTxGetPinStatus` @0x1aec7c writes `5`; `GetRxData` @0x1a4100 switches on `[msg+4]` |
| `sub_cmd` (@5) | same builders write the sub ID; `IpcProtocol40::IpcTxGetImsi` @0x1ab6f4 writes `2` |
| `cmd_type` (@6) | all builders; parsers branch on it (e.g. `IpcProtocol40::IpcRxPwrUp` @0x1ae712 tests `[msg+6] == 3`) |

### 4.1 `cmd_type` values

Two enumerations share the byte, selected by direction.

**AP → CP** (`IpcProtocol40::GetCmdTypeName` @0x1a87ec, jump table @0x1a87f6, names @0x228727):

| Value | Name |
|---:|---|
| 1 | `EXEC` |
| 2 | `GET` |
| 3 | `SET` |
| 4 | `CFRM` |
| 5 | `EVENT` |

**CP → AP** (`GetResponseTypeName` @0x1a87b8, names @0x22870e):

| Value | Name |
|---:|---|
| 1 | `INDI` |
| 2 | `RESP` |
| 3 | `NOTI` |

`0` is also printed by both helpers as `UNDEFINED` (default arm).

---

## 5. `main_cmd` catalog

`IpcProtocol40::GetMainCommandName` @0x1a8838 (376 B; the function is a compare tree whose
leaves are `ldr r0,[pc,#…]; add r0,pc` addresses into the name block @0x22873f).
`IpcProtocol41::GetMainCommandName` @0x1bb148 returns the **same names for the same IDs**
and adds eight entries.

| ID | dec | Name | p40 | p41 |
|---:|---:|---|---:|---:|
| 0x01 | 1 | `PWR_CMD` | ✔ | ✔ |
| 0x02 | 2 | `CALL_CMD` | ✔ | ✔ |
| 0x03 | 3 | `CDMA_DATA_CMD` | ✔ | ✔ |
| 0x04 | 4 | `SMS_CMD` | ✔ | ✔ |
| 0x05 | 5 | `SEC_CMD` | ✔ | ✔ |
| 0x06 | 6 | `PB_CMD` | ✔ | ✔ |
| 0x07 | 7 | `DISP_CMD` | ✔ | ✔ |
| 0x08 | 8 | `NET_CMD` | ✔ | ✔ |
| 0x09 | 9 | `SND_CMD` | ✔ | ✔ |
| 0x0a | 10 | `MISC_CMD` | ✔ | ✔ |
| 0x0b | 11 | `SVC_CMD` | ✔ | ✔ |
| 0x0c | 12 | `SS_CMD` | ✔ | ✔ |
| 0x0d | 13 | `GPRS_CMD` | ✔ | ✔ |
| 0x0e | 14 | `SAT_CMD` | ✔ | ✔ |
| 0x0f | 15 | `CFG_CMD` | ✔ | ✔ |
| 0x10 | 16 | `IMEI_CMD` | ✔ | ✔ |
| 0x11 | 17 | `GPS_CMD` | ✔ | ✔ |
| 0x12 | 18 | `SAP_CMD` | ✔ | ✔ |
| 0x13 | 19 | `FACTORY_CMD` | ✔ | ✔ |
| 0x14 | 20 | `OMADM_CMD` | ✔ | ✔ |
| 0x15 | 21 | `RFS_CMD` | ✔ | ✔ |
| 0x16 | 22 | `IMS_CMD` | ✔ | ✔ |
| 0x17 | 23 | `EMBMS_CMD` | ✔ | ✔ |
| 0x18 | 24 | `MMS_PROVISION_CMD` | — | ✔ |
| 0x1f | 31 | `MODEM_TEST_CMD` | — | ✔ |
| 0x20 | 32 | `DOMESTIC_CMD` | ✔ | ✔ |
| 0x21 | 33 | `PCSC_CMD` | — | ✔ |
| 0x22 | 34 | `QMI_HIDDENMENU_CMD` | — | ✔ |
| 0x23 | 35 | `SMARTAS_CMD` | — | ✔ |
| 0x24 | 36 | `PROSE_CMD` | — | ✔ |
| 0x30 | 48 | `JPN_CMD` | ✔ | ✔ |
| 0x40 | 64 | `QMIIMS_CMD` | — | ✔ |
| 0x70 | 112 | `IIL_CMD` | — | ✔ |
| 0x80 | 128 | `GEN_CMD` | ✔ | ✔ |

`0x00`, `0x18`(p40), `0x19`–`0x1e`, `0x25`–`0x2f`, `0x31`–`0x3f`, `0x41`–`0x6f`, `0x71`–`0x7f`
return `MAIN_CMD_UNDEFINED`.

**Protocol selection is UNKNOWN.** `IpcModem::CreateProtocol(ModemProtoVer)` @0x198b2c maps
`ver==0 → IpcProtocol40`, `ver==2 → IpcProtocol41(ver)`, `ver==8 → IpcProtocol41Iil`, any
other value → `IpcProtocol41`. The value comes from `ModemFactory::LoadModemConfiguration`
@0xf754c, which is driven by `SecNativeFeature` (CSC feature) lookups; the feature name was
not traced. Both protocol variants carry every ID in this catalog, so the catalog is valid
either way; only the eight p41-only mains above are conditional.

---

## 6. `sub_cmd` catalog (`IpcProtocol40`, complete)

`IpcProtocol40::GetSubCommandName(int main, int sub)` @0x1a89b0 (7 748 B) is a two-level
switch (compare trees + `tbh` jump tables, leaves into the name block @0x22873f…0x228f00).
The table below is the result of evaluating that function for every `(main, sub)` pair
0..255 — i.e. it is what the stock binary itself would print for that pair, not a guess.

Only mains `0x01`–`0x17`, `0x20`, `0x30`, `0x80` have names here; `RFS_CMD` (0x15) has no
sub-name table (it returns a formatted fallback) and `IMS_CMD` (0x16) falls through to
`UNDEFINED`. `IMEI_CMD` (0x10) does have a table (see §A.16).

Legend: `0xMM.0xSS` = `main.sub`; the direction column in §7 tells whether a given
sub-command is a request, a response, or both.

---

## 7. Payload layouts

`len` is the value in the header (total frame length); `@7` means payload byte 0.

| ID (main.sub) | Name | Dir | Type | len | Payload (offset from frame start) | Evidence |
|---|---|---|---|---:|---|---|
| `0x01.0x01` | PWR_PHONE_PWR_UP | CP→AP | 3 NOTI | UNKNOWN (parser reads no body) | — | `IpcProtocol40::IpcRxPwrUp` @0x1ae712 (checks type==3, builds `PowerInfo(2)`) |
| `0x01.0x02` | PWR_PHONE_PWR_OFF | CP→AP (assumed) | UNKNOWN | UNKNOWN | body not decoded | `IpcProtocol40::IpcRxPower` @0x1ae7c8 is the PWR *sub-dispatcher*: it switches on `[msg+5]` (sub 1..7) and tail-calls one of three virtual handlers at `obj+0x66c/0x670/0x674`; the sub→handler split was not resolved — **inferred direction** |
| `0x01.0x03` | PWR_PHONE_RESET | CP→AP | UNKNOWN | UNKNOWN | — | `IpcProtocol40::IpcRxPwrReset` @0x1ae73c |
| `0x01.0x07` | PWR_PHONE_STATE | AP→CP | 1 EXEC | 8 | `[7]` = state code: 3 when `RadioPowerState==2`, 4 when `==1` (other values rejected, returns -1) | `IpcProtocol40::IpcTxRadioPower` @0x1ae65e |
| `0x01.0x07` | PWR_PHONE_STATE | CP→AP | 3 NOTI | UNKNOWN | `[7]` = state code; 3 or 5 → internal online(2), else offline(1) | `IpcProtocol40::IpcRxPwrState` @0x1ae78e |
| `0x02.0x01` | CALL_OUTGOING | AP→CP | 1 EXEC | 0x61 (97) | `[7..8]` u16 flags = 0x100 (default) / 0x300 (`CallType==2`) / 0x700 (`CallType==0xc`); `[9]` call id; `[10]` number length (clamped ≤ 0x52); `[11]` TOA = 0x11 if number starts `+` else 0x21; `[12..96]` number bytes (frame is fixed-size, tail zero) | `IpcProtocol40::IpcTxCallOriginate` @0x1a52e0 |
| `0x02.0x03` | CALL_RELEASE | AP→CP | 1 EXEC | 7 | — | `IpcProtocol40::IpcTxCallRelease` @0x1a5614 |
| `0x02.0x04` | CALL_ANSWER | AP→CP | 1 EXEC | 7 | — | `IpcProtocol40::IpcTxCallAnswer` @0x1a5478 |
| `0x02.0x05` | CALL_STATUS | CP→AP | UNKNOWN | UNKNOWN | `[7..8]` u16; `[9]`,`[10]`,`[0xb]`,`[0xc]` bytes | `IpcProtocol40::IpcRxCallStatus` @0x1a6184 |
| `0x02.0x06` | CALL_LIST | CP→AP | UNKNOWN | UNKNOWN | list records; not decoded | `IpcProtocol40::IpcRxCallList` @0x1a61e8 (712 B) |
| `0x02.0x08` | CALL_CONT_DTMF | AP→CP | 3 SET | 9 | `[7]` = 1; `[8]` = tone (`CallToneType`) | `IpcProtocol40::IpcTxCallStartContDtmf` @0x1a5880 |
| `0x02.0x0b` | CALL_SIGNAL | CP→AP | 3 NOTI | UNKNOWN | `[9]`,`[0xa]`,`[0xb]` = three signal bytes (copied to `SignalStrength` slots +0xc1/+0xc2/+0xc3) | `IpcRxCallSignalInfo` @0x1a6556 — the same body is exported as both `IpcProtocol40::` and `IpcProtocol41::IpcRxCallSignalInfo` |
| `0x04.0x01` | SMS_SEND_MSG | AP→CP | 1 EXEC | 0x10b (267), or 0x0b (11) when arg2 != 1 | `[7]`,`[8]`,`[9]` mode/length bytes from the SMS manager; `[0xb..]` pre-encoded PDU; `__memcpy_chk` dest limit 0x10b | `IpcProtocol40::IpcTxSendSms` @0x1af7cc |
| `0x04.0x02` | SMS_INCOMING_MSG | CP→AP | UNKNOWN | UNKNOWN | `[0xe..0xf]` u16 read first; remainder is the PDU blob | `IpcProtocol40::IpcRxIncomingMsg` @0x1affd0 |
| `0x08.0x03` | NET_SERVING_NETWORK | AP→CP | 2 GET | 7 | — | `IpcProtocol40::IpcTxNetGetOperator` @0x1acb30 |
| `0x08.0x03` | NET_IDENTITY (response of 8.3) | CP→AP | 2 RESP | UNKNOWN | no body consumed: handler returns 0 for `type==2` | `IpcProtocol40::IpcRxNetIdentity` @0x1ada54 (`[6]==2` → early return) |
| `0x08.0x05` | NET_REGIST (voice) | AP→CP | 2 GET | 9 | `[7]`=0xff, `[8]`=2 | `IpcProtocol40::IpcTxNetGetVoiceRegState` @0x1aca3c |
| `0x08.0x05` | NET_REGIST (data) | AP→CP | 2 GET | 9 | `[7]`=0xff, `[8]`=3 | `IpcProtocol40::IpcTxNetGetDataRegState` @0x1acab4 |
| `0x08.0x05` | NET_REGIST (response) | CP→AP | RESP/NOTI | UNKNOWN | `[7]` raw reg state (1..0x30); `[8]` domain; `[9]` radio tech; `[0xb..0xc]` u16 LAC; `[0xd..0x10]` u32 cell id; `[0x11]`, `[0x12]` extra bytes. `type==3` also sets an error/notification word to 0x19 | `IpcProtocol40::IpcRxNetRegState` @0x1ad38c |
| `0x08.0x0c` | NET_IDENTITY | CP→AP | 2 RESP / 3 NOTI | UNKNOWN | `[8]` flags (bit0 = first PLMN present); `[0xa]` length-ish byte; `[0xb]`; `[0x1d]`, `[0x1e]` (second PLMN / flags) | `IpcProtocol40::IpcRxNetIdentity` @0x1ada54 |
| `0x0a.0x02` | MISC_ME_IMSI | AP→CP | 2 GET | 7 | — | `IpcProtocol40::IpcTxGetImsi` @0x1ab6f4 |
| `0x0a.0x02` | MISC_ME_IMSI (response) | CP→AP | 2 RESP | UNKNOWN | `[7..]` IMSI, parsed into a `RilData` string | `IpcProtocol40::IpcRxMiscImsi` @0x1ac1c4 |
| `0x0a.0x03` | MISC_ME_SN | AP→CP | 2 GET | 8 | `[7]` = device-info type: p40 maps 1/2→1, 4→2, 8→3, 0x80→7, else unchanged | `IpcProtocol40::IpcTxGetSerialNumber` @0x1ab7a0; p41 variant `Ipc41PacketBuilder::BuildIpcMiscMobileEquipmentSerialNumber` @0x1d3bec (maps 1/2→1, 4→2, 8→3, 0x10→4, 0x20→5, 0x40→6, 0x80→7, 0→0) |
| `0x0a.0x03` | MISC_ME_SN (response) | CP→AP | 2 RESP | UNKNOWN | `[7]` = type; `[8]` = value length; `[9..]` = value (type 2/7: 4 bytes from 9; type 3: 7 bytes from 0xa) | `IpcProtocol40::IpcRxMiscSerialNumber` @0x1ac220 |
| `0x05.0x01` | SEC_PIN_STATUS | AP→CP | 2 GET | 7 | — | `IpcProtocol40::IpcTxGetPinStatus` @0x1aec7c |
| `0x05.0x01` | SEC_PIN_STATUS (response) | CP→AP | RESP | UNKNOWN | `[7]` = raw lock type (translated by helper @0x1aec16); `[8]` = lock key 1..6 | `IpcProtocol40::IpcRxSecPinStatus` @0x1af314 |
| `0x0d.0x01` | GPRS_DEFINE_PDP_CONTEXT | AP→CP | SET | UNKNOWN | PDP definition record; first bytes not decoded | `IpcProtocol40::IpcTxDefinePdpContext` @0x1a7830 (232 B) |
| `0x0d.0x03` | GPRS_PS | AP→CP | UNKNOWN | UNKNOWN | PS attach/detach parameters | `IpcProtocol40::IpcTxPsAttach` @0x1a7a82 (74 B) |
| `0x0d.0x04` | GPRS_PDP_CONTEXT | AP→CP | 3 SET | 0x70 (112) | `[7]` = arg2 (PDP type), `[8]` = arg1 (cid); APN/string block at `[13]`; last byte `[0x6f]` flag (set from arg when type==0); remaining fields not decoded | `IpcProtocol40::IpcTxSetPdpContext` @0x1a7918 |
| `0x0d.0x04` | GPRS_PDP_CONTEXT (response) | CP→AP | UNKNOWN | UNKNOWN | PDP context record | `IpcProtocol40::IpcRxPdpContext` @0x1a8700 |
| `0x10.0x01..0x08` | IMEI_START … IMEI_MASS_STORAGE_INFO | AP→CP | UNKNOWN | UNKNOWN | sub dispatch only; bodies not decoded | `IpcProtocol40::GetSubCommandName` dispatch @0x1a9c2c; builders `IpcTxImeiStartEvent` @0x1ac898, `IpcTxImeiStartConfirm` @0x1ac828, `IpcTxImeiPreconfigSet` @0x1ac980, `IpcTxImeiVerifyFactoryEvent` @0x1ac8d4, `IpcTxImeiReboot` @0x1ac910 |

### 7.1 Two fully decoded frames (byte level)

`SEC_PIN_STATUS` request (from `IpcTxGetPinStatus` @0x1aec7c):

```
07 00 <seq> 00 05 01 02
len  seq ack main  sub type
```

`NET_REGIST` request, voice (from `IpcTxNetGetVoiceRegState` @0x1aca3c):

```
09 00 <seq> 00 08 05 02 ff 02
                       ^^ ^^ payload
```

`NET_REGIST` response layout as consumed by `IpcRxNetRegState` @0x1ad38c
(`[n]` = byte n of the whole frame, i.e. payload offset n-7):

| Offset | Size | Meaning (as used by the parser) |
|---:|---:|---|
| 7 | u8 | registration state index (range-checked 1..0x30) |
| 8 | u8 | domain byte (mapped via table @0x238550) |
| 9 | u8 | radio/network type (mapped via table @0x2386e0) |
| 0xb | u16 | LAC |
| 0xd | u32 | cell id (unaligned load) |
| 0x11 | u8 | extra byte 1 |
| 0x12 | u8 | extra byte 2 |

The semantic labels (LAC/cell id) are inferred from use; the offsets and widths are exact.


---

## 8. Minimum network-registration sequence

Prerequisite: CP booted by `j7elte-cbd` and **both** `/dev/umts_ipc0` and `/dev/umts_rfs0`
open, otherwise the kernel never sends `INIT_END` and the CP keeps retrying `PHONE_START`
(`link_device_shmem.c:261-285`).

| # | Direction | Frame | Meaning | Evidence |
|---:|---|---|---|---|
| 0 | kernel↔CP | `INIT_START` / `PIF_INIT_DONE` / `PHONE_START` / `INIT_END` | SIPC5 boot handshake, not an IPC message | `link_device_shmem.c:290-370` |
| 1 | CP→AP | `PWR_CMD(1)/PWR_PHONE_PWR_UP/3` | CP is up; AP allocates a `PowerInfo` | `IpcProtocol40::IpcRxPwrUp` @0x1ae712 |
| 2 | AP→CP | `PWR_CMD(1)/PWR_PHONE_STATE/1`, `len 8`, body `03` (or `04`) | radio on | `IpcProtocol40::IpcTxRadioPower` @0x1ae65e |
| 3 | AP→CP | `NET_CMD(8)/NET_REGIST/2`, `len 9`, body `ff 02` | query voice registration | `IpcProtocol40::IpcTxNetGetVoiceRegState` @0x1aca3c |
| 4 | CP→AP | `NET_CMD(8)/NET_REGIST/{2,3}` | registration state/domain/tech/LAC/cell id | `IpcProtocol40::IpcRxNetRegState` @0x1ad38c |
| 5 | AP→CP | `NET_CMD(8)/NET_SERVING_NETWORK/2`, `len 7` | query operator | `IpcProtocol40::IpcTxNetGetOperator` @0x1acb30 |
| 6 | CP→AP | `NET_CMD(8)/NET_IDENTITY/3` | PLMN strings (the `2 RESP` arm carries no body) | `IpcProtocol40::IpcRxNetIdentity` @0x1ada54 |
| 7 | AP→CP | `SEC_CMD(5)/SEC_PIN_STATUS/2`, `len 7` | SIM lock state | `IpcProtocol40::IpcTxGetPinStatus` @0x1aec7c |
| 8 | AP→CP | `MISC_CMD(10)/MISC_ME_IMSI/2`, `len 7` | IMSI | `IpcProtocol40::IpcTxGetImsi` @0x1ab6f4 |
| 9 | CP→AP | `CALL_CMD(2)/CALL_SIGNAL/3` | signal strength; **no AP request exists** — `RIL_REQUEST_SIGNAL_STRENGTH` is answered from this cached notification | `IpcProtocol41::IpcRxCallSignalInfo` @0x1a6556; `NetworkManager::DoSignalStrength` @0x10e60c performs no IPC write |

Every AP→CP frame above carries `msg_seq` from `GenerateMsgSequence()` and `ack_seq = 0`.
The registration queries (3/5/7/8) are the *only* frames in this list that are safe to
send without first modelling call/SMS/PDP state: they are body-less or one-byte GETs and
the CP answers with `RESP`.

## 9. Safety: what must not be sent blind

| Family | Why |
|---|---|
| `PWR_CMD(1)/2,3` (`PWR_PHONE_PWR_OFF`, `PWR_PHONE_RESET`) | power-cycles/resets the CP; will kill an in-progress boot |
| `IMEI_CMD(0x10)/4,5` (`IMEI_WRITE_ITEM`, `IMEI_REBOOT`) and `IMEI_CMD/1..3` | writes device identity / reboots the CP |
| `SEC_CMD(5)/2,3,7` (`SEC_PHONE_LOCK`, `SEC_CHANGE_LOCKING_PW`, `SEC_LOCK_INFOMATION`) | consumes PIN/PUK retry counters; a wrong attempt can permanently lock the SIM |
| `CALL_CMD(2)/1,3,4,8` | changes call state; needs a live call model and response matching |
| `SMS_CMD(4)/1` | transmits a real SMS (needs a valid PDU; header bytes come from the SMS manager) |
| `GPRS_CMD(0xd)/1,4,5` | creates/activates a PDP context; the CP state machine expects the full sequence |
| `RFS_CMD(0x15)/*`, `IMS_CMD(0x16)/*` | no sub-command table exists in `IpcProtocol40`; unknown semantics |
| any `(main,sub)` not listed here | never send; the stock code refuses unknown pairs, a reimplementation must too |

## 10. Evidence index

`libsec-ril.so` (SHA-256 `09539011…`), Thumb unless noted. All are `.dynsym` functions, so
the names are the vendor's own mangled names.

| Address | Symbol | Used for |
|---|---|---|
| 0x1a8838 | `IpcProtocol40::GetMainCommandName(int)` | §5 table |
| 0x1bb148 | `IpcProtocol41::GetMainCommandName(int)` | §5 table, p41-only IDs |
| 0x1a89b0 | `IpcProtocol40::GetSubCommandName(int,int)` | §6 + appendix |
| 0x1a87ec | `IpcProtocol40::GetCmdTypeName(int)` | §4.1 request types |
| 0x1a87b8 | `IpcProtocol40::GetResponseTypeName(int)` | §4.1 response types |
| 0x1a42d2 | `IpcProtocol::SetMsgSequence(char*)` | `msg_seq` at byte 2 |
| 0x1a34fe | `IpcProtocol::GenerateMsgSequence()` | sequence counter |
| 0x1a4340 | `IpcProtocol40::IpcTxRawIpc(char*,int)` | length check before TX |
| 0x1a4100 | `IpcProtocol40::GetRxData(...)` | RX dispatch on `main_cmd` |
| 0x1aec7c | `IpcProtocol40::IpcTxGetPinStatus()` | SEC_PIN_STATUS builder |
| 0x1aec16 | `IpcProtocol40` lock-type translation helper | PIN status mapping |
| 0x1af314 | `IpcProtocol40::IpcRxSecPinStatus(...)` | PIN status parser |
| 0x1ab6f4 | `IpcProtocol40::IpcTxGetImsi()` | IMSI builder |
| 0x1ac1c4 | `IpcProtocol40::IpcRxMiscImsi(...)` | IMSI parser |
| 0x1ab7a0 | `IpcProtocol40::IpcTxGetSerialNumber(DeviceInfoType)` | ME serial (IMEI) builder |
| 0x1ac220 | `IpcProtocol40::IpcRxMiscSerialNumber(...)` | ME serial parser |
| 0x1aca3c | `IpcProtocol40::IpcTxNetGetVoiceRegState()` | NET_REGIST voice |
| 0x1acab4 | `IpcProtocol40::IpcTxNetGetDataRegState()` | NET_REGIST data |
| 0x1acb30 | `IpcProtocol40::IpcTxNetGetOperator()` | NET_SERVING_NETWORK |
| 0x1ad38c | `IpcProtocol40::IpcRxNetRegState(...)` | registration parser |
| 0x1ada54 | `IpcProtocol40::IpcRxNetIdentity(...)` | PLMN/operator parser |
| 0x1ae65e | `IpcProtocol40::IpcTxRadioPower(RadioPowerState)` | PWR_PHONE_STATE EXEC |
| 0x1ae712 | `IpcProtocol40::IpcRxPwrUp(...)` | PWR_PHONE_PWR_UP NOTI |
| 0x1ae78e | `IpcProtocol40::IpcRxPwrState(...)` | PWR_PHONE_STATE NOTI |
| 0x1a52e0 | `IpcProtocol40::IpcTxCallOriginate(...)` | CALL_OUTGOING |
| 0x1a5478 | `IpcProtocol40::IpcTxCallAnswer(CallType)` | CALL_ANSWER |
| 0x1a5614 | `IpcProtocol40::IpcTxCallRelease()` | CALL_RELEASE |
| 0x1a5880 | `IpcProtocol40::IpcTxCallStartContDtmf(...)` | CALL_CONT_DTMF |
| 0x1a6184 | `IpcProtocol40::IpcRxCallStatus(...)` | CALL_STATUS |
| 0x1a61e8 | `IpcProtocol40::IpcRxCallList(...)` | CALL_LIST |
| 0x1a6556 | `IpcProtocol41::IpcRxCallSignalInfo(...)` | CALL_SIGNAL |
| 0x1af7cc | `IpcProtocol40::IpcTxSendSms(...)` | SMS_SEND_MSG |
| 0x1affd0 | `IpcProtocol40::IpcRxIncomingMsg(...)` | SMS_INCOMING_MSG |
| 0x1a7830 | `IpcProtocol40::IpcTxDefinePdpContext(...)` | GPRS_DEFINE_PDP_CONTEXT |
| 0x1a7918 | `IpcProtocol40::IpcTxSetPdpContext(...)` | GPRS_PDP_CONTEXT |
| 0x1a7a82 | `IpcProtocol40::IpcTxPsAttach(...)` | GPRS_PS |
| 0x1a8700 | `IpcProtocol40::IpcRxPdpContext(...)` | PDP response |
| 0x198b2c | `IpcModem::CreateProtocol(ModemProtoVer)` | protocol class selection |
| 0x1989ac | `IpcModem::SendRawIpc(Message*,int,char*,int)` | TX entry (virtual channel write at vtable+0x18) |
| 0xd9c56 | `IoChannel::Init(const char*,Modem*)` | channel name plumbing |
| 0xda13c | `IoChannel::Open()` | `open(O_RDWR)`, 51 retries |
| 0xd9fcc | `DevIoctlIoChannel::Read(char*,int)` | ioctl 0x6f27 gate + read |
| 0xd9e2c / 0xd9ee4 | `IoChannel::Read/Write` | `read()`/`write()` on the fd |
| 0xdb300 | `RIL_Init` | vendor RIL entry point |
| 0xf754c | `ModemFactory::LoadModemConfiguration()` | protocol version/interface config (CSC-driven) |

Kernel sources: `drivers/misc/modem_v1/modem_prj.h:84,145-156,205-212`,
`modem_io_device.c:257-278,851-960,974-1001`, `link_device_shmem.c:261-314`,
`arch/arm64/boot/dts/modem-isla-pdata.dtsi:53-58,49`.

## 11. Unknowns (explicit)

1. **Protocol variant actually instantiated** on this unit (`IpcProtocol40` vs `IpcProtocol41`
   vs `IpcProtocol41Iil`). The selector takes a CSC-derived enum; the feature name was not
   traced. All catalog IDs exist in both variants.
2. **`ack_seq` semantics.** Always 0 in AP builders; no parser was found that compares it.
   Response matching in the stock code is done through `IpcHistory` records keyed by
   `(main,sub,cmd_type)` and `msg_seq` (see `IpcProtocol40::FindRecord` @0x1a42de).
3. **Field semantics of the registration/identity payloads** beyond the byte offsets listed
   in §7. Register-state/domain/tech are mapped through tables at 0x2386e0 / 0x238550 whose
   enum names were not recovered.
4. **SMS PDU construction** and the meaning of `SMS_CMD/1` bytes `[7]`,`[8]`,`[9]`.
5. **PDP definition record layout** (`GPRS_CMD/1`, `GPRS_CMD/3`, `GPRS_CMD/4` beyond the first
   two payload bytes).
6. **Call state machine** (ids, MT/ALS flags, `CALL_STATUS` meaning) and `CALL_LIST` records.
7. **`IpcProtocol41` sub-command name tables** — not extracted; only p40's are complete.
8. **RFS/IMS sub-commands** — p40 has no name table for them; `IpcProtocol41` has more mains
   but their sub tables were not extracted.
9. **`IpcProtocol40::ConvertToIpcCmd(int,int&,int&,int&,int&)` @0x1a4440.** Its switch maps an
   internal request id to four outputs; traced examples are
   `0x64 → (timeout 50000 ms, 1, 7, 2)` and `0xcc → (60000 ms, 0x0c, 7, 2)`, consistent with
   `(timeout, main, sub, cmd_type)`, but the input id namespace (Samsung-internal, values up
   to 0x385) is **not** the AOSP `RIL_REQUEST_*` space and was not resolved.
10. **`libsec-ril-dsds.so`** differs materially from `libsec-ril.so` (2 126 295 differing
    bytes); this catalog is valid only for the single-SIM library that `rild.libpath` names.

## 12. How this was produced / how to re-check

* Binaries dumped with `debugfs -R 'dump /lib/libsec-ril.so …' system.raw`; SHA-256 above.
* Disassembly with **Capstone 5** (ARM/Thumb-2) driven from Python: section table from
  `readelf -SW`, vaddr→file-offset mapping, `.ARM.exidx` (5441 entries) used to find function
  extents, PLT stubs decoded to symbol names via `.rel.plt` (7 771 entries).
* The command-name tables were produced by **interpreting** `GetMainCommandName` /
  `GetSubCommandName` (compare trees, `tbh`/`tbb`, `ldr r0,[pc]; add r0,pc` leaves) for all
  256×256 inputs, not by pattern-matching strings; string targets were verified to land on
  string starts in `.rodata`.
* Frame layouts were read from the builder/parser instruction streams (`strh`/`strb`/`ldr`
  offsets and immediate lengths).
* `ghidra/support/analyzeHeadless` is available in this environment and can be used as an
  independent cross-check; the addresses above are the import/link-time vaddrs Ghidra shows
  for the ARM:LE:32:v7 processor.


---

## Appendix A — complete `sub_cmd` table for `IpcProtocol40`

Every row is `(main,sub) → name` as returned by `IpcProtocol40::GetSubCommandName`
@0x1a89b0. Names are the vendor's own strings (block @0x22873f).

### A.01 `PWR_CMD` (0x01) — 7 sub-commands

| sub | name |
|---:|---|
| 0x01 | `PWR_PHONE_PWR_UP` |
| 0x02 | `PWR_PHONE_PWR_OFF` |
| 0x03 | `PWR_PHONE_RESET` |
| 0x04 | `PWR_BATT_STATUS` |
| 0x05 | `PWR_BATT_TYPE` |
| 0x06 | `PWR_BATT_COMP` |
| 0x07 | `PWR_PHONE_STATE` |

### A.02 `CALL_CMD` (0x02) — 24 sub-commands

| sub | name |
|---:|---|
| 0x01 | `CALL_OUTGOING` |
| 0x02 | `CALL_INCOMING` |
| 0x03 | `CALL_RELEASE` |
| 0x04 | `CALL_ANSWER` |
| 0x05 | `CALL_STATUS` |
| 0x06 | `CALL_LIST` |
| 0x07 | `CALL_BURST_DTMF` |
| 0x08 | `CALL_CONT_DTMF` |
| 0x09 | `CALL_WAITING` |
| 0x0a | `CALL_LINE_ID` |
| 0x0b | `CALL_SIGNAL` |
| 0x0c | `CALL_VOICE_PRIVACY` |
| 0x0d | `CALL_CALL_TIME_COUNT` |
| 0x0e | `CALL_OTA_PROGRESS` |
| 0x0f | `CALL_DIAG_OUTGOING` |
| 0x10 | `CALL_E911_CB_MODE` |
| 0x11 | `CALL_FLASH_INFO` |
| 0x12 | `CALL_SRVCC` |
| 0x13 | `CALL_HOLD` |
| 0x18 | `CALL_BLOCK_STATUS` |
| 0x19 | `CALL_DATA_CALL_BYTE_COUNTER` |
| 0x30 | `CALL_MODIFY` |
| 0x31 | `CALL_MODIFY_NOTI` |
| 0x32 | `CALL_MODIFY_CONFIRM` |

### A.03 `CDMA_DATA_CMD` (0x03) — 33 sub-commands

| sub | name |
|---:|---|
| 0x01 | `CDMA_DATA_TE2_STATUS` |
| 0x02 | `CDMA_DATA_BYTE_COUNTER` |
| 0x03 | `CDMA_DATA_INCOMING_CALL_TYPE` |
| 0x04 | `CDMA_DATA_TE2_DIALING_INFO` |
| 0x05 | `CDMA_DATA_TE2_DATA_RATE_INFO` |
| 0x06 | `CDMA_DATA_PACKET_DATA_CALL_CFG` |
| 0x07 | `CDMA_DATA_DS_BAUD_RATE` |
| 0x08 | `CDMA_DATA_MOBILE_IP_NAI` |
| 0x09 | `CDMA_DATA_CURRENT_NAI_INDEX` |
| 0x0a | `CDMA_DATA_DORMANT_CONFIG` |
| 0x0b | `CDMA_DATA_MIP_NAI_CHANGED` |
| 0x0c | `CDMA_DATA_SIGNEDIN_STATE` |
| 0x0d | `CDMA_DATA_RESTORE_NAI` |
| 0x0e | `CDMA_DATA_MIP_CONNECT_STATUS` |
| 0x0f | `CDMA_DATA_DORMANT_MODE_STATUS` |
| 0x10 | `CDMA_DATA_R_SCH_CONFIG` |
| 0x11 | `CDMA_DATA_HDR_SESSION_CLEAR` |
| 0x12 | `CDMA_DATA_SESSION_CLOSE_TIMER_EXPIRED` |
| 0x13 | `CDMA_DATA_KEEPALIVETIMER_VALUE` |
| 0x14 | `CDMA_DATA_DDTMMODE_CONFIG` |
| 0x15 | `CDMA_DATA_ROAM_GUARD` |
| 0x16 | `CDMA_DATA_MODEM_NAI` |
| 0x17 | `CDMA_DATA_KOREA_MODE` |
| 0x18 | `CDMA_DATA_DATA_SERVICE_TYPE` |
| 0x19 | `CDMA_DATA_FORCE_REV_A_MODE` |
| 0x1a | `CDMA_DATA_CUSTOM_CONFIG_MODE` |
| 0x1b | `CDMA_DATA_NAI_SETTING_MODE` |
| 0x1c | `CDMA_DATA_PIN_CTRL` |
| 0x1d | `CDMA_DATA_RAW_DATA_MODE` |
| 0x1e | `CDMA_DATA_DUN_MODE` |
| 0x1f | `CDMA_DATA_TE2_CALL_STATUS` |
| 0x20 | `CDMA_DATA_IP_CONFIGURATION` |
| 0x21 | `CDMA_DATA_CALL_STATUS` |

### A.04 `SMS_CMD` (0x04) — 18 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SMS_SEND_MSG` |
| 0x02 | `SMS_INCOMING_MSG` |
| 0x03 | `SMS_READ_MSG` |
| 0x04 | `SMS_SAVE_MSG` |
| 0x05 | `SMS_DEL_MSG` |
| 0x06 | `SMS_DELIVER_REPORT` |
| 0x07 | `SMS_DEVICE_READY` |
| 0x08 | `SMS_SEL_MEM` |
| 0x09 | `SMS_STORED_MSG_COUNT` |
| 0x0a | `SMS_SVC_CENTER_ADDR` |
| 0x0b | `SMS_SVC_OPTION` |
| 0x0c | `SMS_MEM_STATUS` |
| 0x0d | `SMS_CBS_MSG` |
| 0x0e | `SMS_CBS_CFG` |
| 0x0f | `SMS_STORED_MSG_STATUS` |
| 0x10 | `SMS_PARAM_COUNT` |
| 0x11 | `SMS_PARAM` |
| 0x12 | `SMS_STATUS` |

### A.05 `SEC_CMD` (0x05) — 11 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SEC_PIN_STATUS` |
| 0x02 | `SEC_PHONE_LOCK` |
| 0x03 | `SEC_CHANGE_LOCKING_PW` |
| 0x04 | `SEC_SIM_LANG` |
| 0x05 | `SEC_RSIM_ACCESS` |
| 0x06 | `SEC_GSIM_ACCESS` |
| 0x07 | `SEC_SIM_ICC_TYPE` |
| 0x08 | `SEC_LOCK_INFOMATION` |
| 0x09 | `SEC_IMS_AUTH` |
| 0x0a | `SEC_RUIM_CONFIG` |
| 0x0b | `IPC_SEC_SIMAPPS_INFO` |

### A.06 `PB_CMD` (0x06) — 5 sub-commands

| sub | name |
|---:|---|
| 0x01 | `PB_ACCESS` |
| 0x02 | `PB_STORAGE` |
| 0x03 | `PB_STORAGE_LIST` |
| 0x04 | `PB_ENTRY_INFO` |
| 0x05 | `PB_3GPB_CAPA` |

### A.07 `DISP_CMD` (0x07) — 5 sub-commands

| sub | name |
|---:|---|
| 0x01 | `DISP_ICON_INFO` |
| 0x02 | `DISP_HOMEZONE_INFO` |
| 0x03 | `DISP_PHONE_FATAL_INFO` |
| 0x04 | `DISP_EXT_ROAM_INFO` |
| 0x05 | `DISP_USER_INDICATION` |

### A.08 `NET_CMD` (0x08) — 19 sub-commands

| sub | name |
|---:|---|
| 0x01 | `NET_PREF_PLMN` |
| 0x02 | `NET_PLMN_SEL` |
| 0x03 | `NET_SERVING_NETWORK` |
| 0x04 | `NET_PLMN_LIST` |
| 0x05 | `NET_REGIST` |
| 0x06 | `NET_SUBSCRIBER_NUM` |
| 0x07 | `NET_BAND_SEL` |
| 0x08 | `NET_SERVICE_DOMAIN_CONFIG` |
| 0x09 | `NET_POWERON_ATTACH` |
| 0x0a | `NET_MODE_SEL` |
| 0x0b | `NET_ACQ_ORDER` |
| 0x0c | `NET_IDENTITY` |
| 0x0d | `NET_PREFERRED_NETWORK_INFO` |
| 0x0e | `NET_HYBRID_MODE` |
| 0x0f | `NET_AVOID_SYS` |
| 0x10 | `NET_HANDOVER` |
| 0x16 | `NET_DUAL_STANDBY_PREF` |
| 0x1c | `NET_VOWIFI_HO_THRESHOLD` |
| 0x24 | `NET_EPDG_HO_THRESHOLD` |

### A.09 `SND_CMD` (0x09) — 17 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SND_KEY_TONE` |
| 0x02 | `SND_NOTI_TONE` |
| 0x03 | `SND_LED_CTRL` |
| 0x04 | `SND_VIB_CTRL` |
| 0x05 | `SND_SPKR_VOLUME_CTRL` |
| 0x06 | `SND_MIC_GAIN_CTRL` |
| 0x07 | `SND_MIC_MUTE_CTRL` |
| 0x08 | `SND_SPKR_PHONE_CTRL` |
| 0x09 | `SND_HFK_AUDIO_STARTSTOP` |
| 0x0a | `SND_VOICECALL_RECORD_REPORT` |
| 0x0b | `SND_AUDIO_PATH_CTRL` |
| 0x0c | `SND_AUDIO_SOURCE_CTRL` |
| 0x0d | `SND_USER_SND_CONFIG` |
| 0x0e | `SND_GAIN_CTRL` |
| 0x0f | `SND_QUIET_MODE_CTRL` |
| 0x10 | `SND_DYVE_MODE_CTRL` |
| 0x11 | `SND_LOOPBACK_CTRL` |

### A.0a `MISC_CMD` (0x0a) — 17 sub-commands

| sub | name |
|---:|---|
| 0x01 | `MISC_ME_VERSION` |
| 0x02 | `MISC_ME_IMSI` |
| 0x03 | `MISC_ME_SN` |
| 0x04 | `MISC_KEY_EVENT_PROCESS` |
| 0x05 | `MISC_TIME_INFO` |
| 0x06 | `MISC_NAM_INFO` |
| 0x07 | `MISC_VCALL_CHANNEL_ID` |
| 0x08 | `MISC_PHONE_DEBUG` |
| 0x09 | `MISC_FUS` |
| 0x0a | `MISC_PDA_BOOT_COMPLETE` |
| 0x0b | `MISC_FACTORY_RESET_COMPLETE` |
| 0x0d | `MISC_SCREEN_STATUS` |
| 0x10 | `MISC_GRIP_SENSOR_STATUS` |
| 0x16 | `MISC_OMADM_CDMA_NAM_INFO` |
| 0x17 | `MISC_OMADM_PRL_WRITE` |
| 0x24 | `MISC_LTE_TIMER` |
| 0x27 | `MISC_DUALSTANDBY_CALL_STATUS` |

### A.0b `SVC_CMD` (0x0b) — 12 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SVC_ENTER` |
| 0x02 | `SVC_END` |
| 0x03 | `SVC_PRO_KEYCODE` |
| 0x04 | `SVC_SCREEN_CFG` |
| 0x05 | `SVC_DISPLAY_SCREEN` |
| 0x06 | `SVC_CHANGE_SVC_MODE` |
| 0x07 | `SVC_DEVICE_TEST` |
| 0x08 | `SVC_DEBUG_DUMP_MESSAGE` |
| 0x09 | `SVC_DEBUG_STRING_MESSAGE` |
| 0x0a | `SVC_CALL_DROP_LOG_INFO` |
| 0x0b | `SVC_LTE_SCAN_FILE` |
| 0x0c | `SVC_MENU_INFO` |

### A.0c `SS_CMD` (0x0c) — 10 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SS_WAITING` |
| 0x02 | `SS_CLI` |
| 0x03 | `SS_BARRING` |
| 0x04 | `SS_BARRING_PW` |
| 0x05 | `SS_FORWARDING` |
| 0x06 | `SS_INFO` |
| 0x07 | `SS_MANAGE_CALL` |
| 0x08 | `SS_USSD` |
| 0x09 | `SS_AOC` |
| 0x0a | `SS_RELEASE_COMPLETE` |

### A.0d `GPRS_CMD` (0x0d) — 22 sub-commands

| sub | name |
|---:|---|
| 0x01 | `GPRS_DEFINE_PDP_CONTEXT` |
| 0x02 | `GPRS_QOS` |
| 0x03 | `GPRS_PS` |
| 0x04 | `GPRS_PDP_CONTEXT` |
| 0x05 | `GPRS_ENTER_DATA` |
| 0x06 | `GPRS_SHOW_PDP_ADDR` |
| 0x07 | `GPRS_MS_CLASS` |
| 0x08 | `GPRS_3G_QUAL_SRVC_PROFILE` |
| 0x09 | `GPRS_IP_CONFIGURATION` |
| 0x0a | `GPRS_DEFINE_SEC_PDP_CONTEXT` |
| 0x0b | `GPRS_TFT` |
| 0x0c | `GPRS_HSDPA_STATUS` |
| 0x0d | `GPRS_CURRENT_SESSION_DATA_COUNTER` |
| 0x0e | `GPRS_DATA_DORMANT` |
| 0x0f | `GPRS_PIN_CTRL` |
| 0x10 | `GPRS_CALL_STATUS` |
| 0x11 | `GPRS_PORT_LIST` |
| 0x12 | `GPRS_LTE_QOS` |
| 0x13 | `GPRS_NWK_INIT_DISCONNECT` |
| 0x17 | `GPRS_FD_INFORMATION` |
| 0x1c | `GPRS_TRAFFIC_CHANNEL_STATUS` |
| 0x1d | `GPRS_MOBILE_DATA_STATUS` |

### A.0e `SAT_CMD` (0x0e) — 13 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SAT_PROFILE_DOWNLOAD` |
| 0x02 | `SAT_ENVELOPE_CMD` |
| 0x03 | `SAT_PROACTIVE_CMD` |
| 0x04 | `SAT_TERMINATE_USAT_SESSION` |
| 0x05 | `SAT_EVENT_DOWNLOAD` |
| 0x06 | `SAT_PROVIDE_LOCAL_INFO` |
| 0x07 | `SAT_POLLING` |
| 0x08 | `SAT_REFRESH` |
| 0x09 | `SAT_SETUP_EVENT_LIST` |
| 0x0a | `SAT_CALL_CONTROL_RESULT` |
| 0x0b | `SAT_IMAGE_CLUT` |
| 0x0c | `SAT_SETUP_CALL_PROCESSING` |
| 0x0d | `SAT_SIM_INITIATE_MESSAGE` |

### A.0f `CFG_CMD` (0x0f) — 49 sub-commands

| sub | name |
|---:|---|
| 0x01 | `CFG_DEFAULT_CONFIG` |
| 0x02 | `CFG_EXTERNAL_DEVICE` |
| 0x03 | `CFG_MAC_ADDRESS` |
| 0x04 | `CFG_CONFIGURATION_ITEM` |
| 0x05 | `CFG_TTY` |
| 0x06 | `CFG_HSDPA_TMP_SETTING` |
| 0x07 | `CFG_HSDPA_PERM_SETTING` |
| 0x08 | `CFG_SIO_MODE` |
| 0x09 | `CFG_AKEY_VERIFY` |
| 0x0a | `CFG_MSL_INFO` |
| 0x0b | `CFG_USER_LOCK_CODE` |
| 0x0c | `CFG_USB_PATH` |
| 0x0d | `CFG_CURRENT_SVC_CARRIER` |
| 0x0e | `CFG_RADIO_CONFIG` |
| 0x0f | `CFG_VOCODER_OPTION` |
| 0x10 | `CFG_TEST_SYS` |
| 0x11 | `CFG_RECONDITIONED_DATE` |
| 0x12 | `CFG_PROTOCOL_REVISION` |
| 0x13 | `CFG_SLOT_MODE` |
| 0x14 | `CFG_ACTIVATION_DATE` |
| 0x15 | `CFG_CURRENT_UATI` |
| 0x16 | `CFG_QUICK_PAGING` |
| 0x17 | `CFG_LMSC_INFO` |
| 0x18 | `CFG_TAS_INFO` |
| 0x19 | `CFG_AUTH_INFO` |
| 0x1a | `CFG_HIDDEN_MENU_ACCESS` |
| 0x1b | `CFG_UTS_SMS_SEND` |
| 0x1c | `CFG_UTS_SMS_COUNT` |
| 0x1d | `CFG_UTS_SMS_MSG` |
| 0x1e | `CFG_SCM_INFO` |
| 0x1f | `CFG_SCI_INFO` |
| 0x20 | `CFG_ACCOLC_INFO` |
| 0x21 | `CFG_MOBTERM_INFO` |
| 0x22 | `CFG_1X_EVDO_DIVERSITY_CONFIG` |
| 0x23 | `CFG_DEVICE_CONFIGURATION` |
| 0x24 | `CFG_USER_LOCK_CODE_STATUS` |
| 0x25 | `CFG_UTS_SMS_GET_UNREAD_MSG_STATUS` |
| 0x26 | `CFG_MOBILEAP_STATUS` |
| 0x29 | `CFG_ADVANCED_INFO` |
| 0x2a | `CFG_WDC` |
| 0x2c | `CFG_GET_OPERATOR` |
| 0x2d | `CFG_GET_VOICEMAIL_NUM` |
| 0x2e | `CFG_GET_WALLPAPER_STATUS` |
| 0x2f | `CFG_GET_PWRONOFF_IMG_STATUS` |
| 0x30 | `CFG_SAFE_MODE` |
| 0x31 | `CFG_SEC_VALIDATE` |
| 0x32 | `CFG_SAR_DEVICE` |
| 0x33 | `CFG_MMS_PARAM` |
| 0x34 | `CFG_HIDDEN_PROGRAM` |

### A.10 `IMEI_CMD` (0x10) — 8 sub-commands

| sub | name |
|---:|---|
| 0x01 | `IMEI_START` |
| 0x02 | `IMEI_CHECK_DEVICE_INFO` |
| 0x03 | `IMEI_PRE_CONFIG` |
| 0x04 | `IMEI_WRITE_ITEM` |
| 0x05 | `IMEI_REBOOT` |
| 0x06 | `IMEI_VERIFY_FACTORY_RESET` |
| 0x07 | `IMEI_COMPARE_ITEM` |
| 0x08 | `IMEI_MASS_STORAGE_INFO` |

### A.11 `GPS_CMD` (0x11) — 54 sub-commands

| sub | name |
|---:|---|
| 0x01 | `GPS_OPEN` |
| 0x02 | `GPS_CLOSE` |
| 0x03 | `GPS_START` |
| 0x04 | `GPS_DEVICE_STATE` |
| 0x05 | `GPS_OPTION` |
| 0x06 | `GPS_TTFF` |
| 0x07 | `GPS_LOCK_MODE` |
| 0x08 | `GPS_SECURITY_UPDATE` |
| 0x09 | `GPS_SSD` |
| 0x0a | `GPS_SECURITY_UPDATE_RATE` |
| 0x0b | `GPS_FIX_REQ` |
| 0x0c | `GPS_POSITION_RESULT` |
| 0x0d | `GPS_EXT_POSITION_RESULT` |
| 0x0e | `GPS_EXT_STATUS_INFO` |
| 0x0f | `GPS_PD_CMD_CB` |
| 0x10 | `GPS_DLOAD_STATUS` |
| 0x11 | `GPS_END_SESSION` |
| 0x12 | `GPS_FAILURE_INFO` |
| 0x13 | `GPS_HW_STATE` |
| 0x14 | `GPS_SECURITY_ENABLE` |
| 0x15 | `GPS_SECURITY_READ` |
| 0x16 | `GPS_SECURITY_WRITE` |
| 0x17 | `GPS_ENCRYPT_READ` |
| 0x18 | `GPS_REF_LOCATION` |
| 0x21 | `GPS_PGPS_TIME_INFO` |
| 0x22 | `GPS_PGPS_EXT_EPH` |
| 0x23 | `GPS_PGPS_BROADCAST_EPH` |
| 0x24 | `GPS_SENSOR_INFO` |
| 0x25 | `GPS_RXN_REF_LOCATION` |
| 0x26 | `GPS_RXN_UPDATE_CELLID` |
| 0x27 | `GPS_READY_NOTI` |
| 0x28 | `GPS_UTS_GET_POSITION` |
| 0x31 | `GPS_XTRA_SET_TIME_INFO` |
| 0x32 | `GPS_XTRA_SET_DATA` |
| 0x33 | `GPS_XTRA_CLIENT_INIT_DOWNLOAD` |
| 0x34 | `GPS_XTRA_QUERY_DATA_VALIDITY` |
| 0x35 | `GPS_XTRA_SET_AUTO_DOWNLOAD` |
| 0x36 | `GPS_XTRA_SET_XTRA_ENABLE` |
| 0x37 | `GPS_XTRA_DOWNLOAD` |
| 0x38 | `GPS_XTRA_VALIDITY_STATUS` |
| 0x39 | `GPS_XTRA_TIME_EVENT` |
| 0x3a | `GPS_XTRA_DATA_INJECTION_STATUS` |
| 0x3b | `GPS_XTRA_USE_SNTP` |
| 0x41 | `GPS_CP_POWER_ON` |
| 0x42 | `GPS_CP_FREQ_AIDING` |
| 0x43 | `GPS_CP_PRECISE_TIME_AIDING` |
| 0x44 | `GPS_CP_PSEUDORANGE_MSMT` |
| 0x45 | `GPS_CP_SESSION_CANCELLATION` |
| 0x61 | `GPS_AGPS_PDP_CONNECTION` |
| 0x62 | `GPS_AGPS_DNS_QUERY` |
| 0x63 | `GPS_AGPS_SSL` |
| 0x64 | `GPS_AGPS_MODE` |
| 0x65 | `GPS_VERIFICATION` |
| 0x66 | `GPS_DISPLAY_SUPLFLOW` |

### A.12 `SAP_CMD` (0x12) — 7 sub-commands

| sub | name |
|---:|---|
| 0x01 | `SAP_CONNECT` |
| 0x02 | `SAP_STATUS` |
| 0x03 | `SAP_TRANSFER_ATR` |
| 0x04 | `SAP_TRANSFER_APDU` |
| 0x05 | `SAP_TRANSPORT_PROTOCOL` |
| 0x06 | `SAP_SIM_POWER` |
| 0x07 | `SAP_TRANSFER_CARD_READER_STATUS` |

### A.13 `FACTORY_CMD` (0x13) — 9 sub-commands

| sub | name |
|---:|---|
| 0x01 | `FACTORY_DEVICE_TEST` |
| 0x02 | `FACTORY_OMISSION_AVOIDANCE_TEST` |
| 0x03 | `FACTORY_DFT_TEST` |
| 0x04 | `FACTORY_MISCELLANEOUS_TEST` |
| 0x05 | `FACTORY_BYPASS_TEST` |
| 0x06 | `FACTORY_SLATE_TEST` |
| 0x07 | `FACTORY_FRAME_BUFFER_TEST` |
| 0x08 | `FACTORY_DIAG_PST_UTS` |
| 0x0a | `FACTORY_SEMI_FINAL_TEST` |

### A.14 `OMADM_CMD` (0x14) — 25 sub-commands

| sub | name |
|---:|---|
| 0x01 | `OMADM_PRL_SIZE` |
| 0x02 | `OMADM_MODEL_NAME` |
| 0x03 | `OMADM_OEM_NAME` |
| 0x04 | `OMADM_SW_VER` |
| 0x05 | `OMADM_IS683_DATA` |
| 0x06 | `OMADM_PRL_READ` |
| 0x07 | `OMADM_PRL_WRITE` |
| 0x08 | `OMADM_PUZL_DATA` |
| 0x09 | `OMADM_ROOTCERT_READ` |
| 0x0a | `OMADM_ROOTCERT_WRITE` |
| 0x0b | `OMADM_MMC_OBJECT` |
| 0x0c | `OMADM_MIP_NAI_OBJECT` |
| 0x0d | `OMADM_CURRENT_NAI_INDEX` |
| 0x0e | `OMADM_MIP_AUTH_ALGO` |
| 0x0f | `OMADM_NAM_INFO` |
| 0x10 | `OMADM_START_CIDC` |
| 0x11 | `OMADM_START_CIFUMO` |
| 0x12 | `OMADM_START_CIPRL` |
| 0x13 | `OMADM_START_HFA` |
| 0x14 | `OMADM_START_REG_HFA` |
| 0x15 | `OMADM_SETUP_SESSION` |
| 0x16 | `OMADM_SERVER_START_SESSION` |
| 0x17 | `OMADM_CLIENT_START_SESSION` |
| 0x18 | `OMADM_SEND_DATA` |
| 0x19 | `OMADM_ENABLE_HFA` |

### A.17 `EMBMS_CMD` (0x17) — 9 sub-commands

| sub | name |
|---:|---|
| 0x01 | `EMBMS_SERVICE_MSG` |
| 0x02 | `EMBMS_SESSION_MSG` |
| 0x03 | `EMBMS_BSSI_MSG` |
| 0x04 | `EMBMS_COVERAGE_MSG` |
| 0x05 | `EMBMS_SESSION_LIST_MSG` |
| 0x06 | `EMBMS_SIGNAL_STRENGTH_MSG` |
| 0x07 | `EMBMS_SIB16_NETWORK_TIME_MSG` |
| 0x08 | `EMBMS_SAI_LIST_MSG` |
| 0x09 | `EMBMS_GLOBAL_CELL_ID_MSG` |

### A.20 `DOMESTIC_CMD` (0x20) — 36 sub-commands

| sub | name |
|---:|---|
| 0x01 | `DOMESTIC_CHANNEL_SETTING` |
| 0x02 | `DOMESTIC_LTE_RRC_SETTING` |
| 0x03 | `DOMESTIC_DELETE_STORED_CELL_LIST` |
| 0x04 | `DOMESTIC_ACTIVATION_DATE` |
| 0x06 | `DOMESTIC_SECURITY_MODE` |
| 0x07 | `DOMESTIC_HSDPA_ON_OFF` |
| 0x08 | `DOMESTIC_LAST_CALL` |
| 0x09 | `DOMESTIC_LTE_THROUGHPUT_TEST_MENU` |
| 0x0c | `DOMESTIC_CARD_TYPE` |
| 0x15 | `DOMESTIC_BAND_SEL` |
| 0x16 | `DOMESTIC_GCF_TEST_MODE` |
| 0x33 | `DOMESTIC_OTA_REG_MODE` |
| 0x35 | `DOMESTIC_NET_REG_STATUS_UI` |
| 0x3f | `DOMESTIC_NSRI_PROCESS` |
| 0x40 | `DOMESTIC_NSRI_TOAST_CMD` |
| 0x48 | `DOMESTIC_DISABLE_LTE_B7` |
| 0x4b | `DOMESTIC_MOBILE_QUALITY_INFO` |
| 0x4e | `DOMESTIC_ANDROID_ENTER_DIALER` |
| 0x4f | `DOMESTIC_KEEP_LTE_ICON_CSFB_SETTING` |
| 0x50 | `DOMESTIC_VOICE_CALL_STATUS` |
| 0x51 | `DOMESTIC_SYSTEM_INFO_FOR_LTE` |
| 0x55 | `DOMESTIC_SYSTEM_INFO_FOR_WIPI` |
| 0x59 | `DOMESTIC_NSRI_SECURE_CALL_MODE` |
| 0x5c | `DOMESTIC_IMSI_CHANGED_INFO` |
| 0x5d | `DOMESTIC_NETWORK_INFO_NOTI` |
| 0x60 | `DOMESTIC_PS_BARRING_FOR_VOLTE` |
| 0x61 | `DOMESTIC_KT_HD_VOICE_STATUS` |
| 0x62 | `DOMESTIC_UKNIGHT_INFO` |
| 0x63 | `DOMESTIC_LTE_WIDEBAND_INFO` |
| 0x6a | `IPC_DOMESTIC_LTE_ROAMING_STATUS` |
| 0x6b | `DOMESTIC_PROTOCOL_FEATURE` |
| 0x90 | `DOMESTIC_NSRI_ENCRYPT_SMS` |
| 0x91 | `DOMESTIC_NSRI_DECRYPT_SMS` |
| 0x92 | `DOMESTIC_NSRI_DECRYPTTX_SMS` |
| 0x93 | `DOMESTIC_NSRI_CHECK_SUSIM` |
| 0x94 | `DOMESTIC_NSRI_REQUEST_PROC` |

### A.30 `JPN_CMD` (0x30) — 4 sub-commands

| sub | name |
|---:|---|
| 0x01 | `IPC_MMB_NVINFO` |
| 0x02 | `IPC_DATA_SETTINGINFO` |
| 0x03 | `IPC_JPN_AC_BARRING_FOR_VOLTE` |
| 0x05 | `IPC_JAPAN_KDDI_SIMBLOB` |

### A.80 `GEN_CMD` (0x80) — 1 sub-commands

| sub | name |
|---:|---|
| 0x01 | `GEN_PHONE_RES` |

Mains without a sub-command table in this class: `0x15 RFS_CMD` (returns the
formatted fallback `IpcProtocol GR: 0x%04x`) and `0x16 IMS_CMD` (returns the
`UNDEFINED` fallback). Mains `0x18`–`0x24`, `0x40`, `0x70` exist only in
`IpcProtocol41` and are not catalogued here.
