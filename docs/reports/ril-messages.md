# RIL message catalog — delivered as spec, one cross-check defect found

**Verdict: spec complete, static evidence only (no hardware).**
The Samsung SIPC5/IPC message catalog that `/dev/umts_ipc0` carries is written up in
[`parity/userspace/ril/spec/messages.md`](../userspace/ril/spec/messages.md) (975 lines,
415 catalogued sub-commands). This report is the pointer plus the findings that matter for
the implementation and one defect found in a sibling package by cross-checking.

## What was done

* Extracted `rild`, `libril.so`, `libsec-ril.so`, `libsec-ril-dsds.so` (plus
  `at_distributor`, `libsecril-client.so`, `librilutils.so`, `libaudio-ril.so`) from
  `system.raw` with `debugfs` and recorded SHA-256 for each. The copies already in
  `parity/userspace/ril/stock/` hash identically; nothing there was modified.
* Disassembled `libsec-ril.so` (ARM32/Thumb-2, Capstone 5, 480 243 instructions over the
  whole `.text`, function extents from `.ARM.exidx`, PLT stubs resolved through the 7 771
  `.rel.plt` entries).
* **Recovered the ID tables by executing the stock switch functions**, not by guessing:
  `IpcProtocol40::GetMainCommandName` @0x1a8838 and
  `IpcProtocol40::GetSubCommandName` @0x1a89b0 were interpreted for every (main, sub) pair
  (compare trees, `tbh`/`tbb` jump tables, `ldr r0,[pc]; add r0,pc` leaves resolved into
  `.rodata`); `IpcProtocol41::GetMainCommandName` @0x1bb148 likewise.
* Decoded the frame header from the kernel's own `struct sipc_fmt_hdr`
  (`modem_prj.h:205-212`) and confirmed it against every builder/parser touched.
* Derived payload layouts for the SIM/IMEI, network-registration, signal, call, SMS and
  PDP/GPRS families where the builder or parser exposes the offsets.

## Key results

| Item | Result |
|---|---|
| FMT frame | 7-byte header `{u16 len; u8 msg_seq; u8 ack_seq; u8 main; u8 sub; u8 type}`, payload at +7; `len` is the whole frame |
| Request types | 1 `EXEC`, 2 `GET`, 3 `SET`, 4 `CFRM`, 5 `EVENT` (`GetCmdTypeName` @0x1a87ec) |
| Response types | 1 `INDI`, 2 `RESP`, 3 `NOTI` (`GetResponseTypeName` @0x1a87b8) |
| Main commands | 26 named IDs in p40 (0x01–0x17, 0x20, 0x30, 0x80); p41 adds 8 (0x18, 0x1f, 0x21–0x24, 0x40, 0x70) → 34 total. Full table in the spec |
| Sub commands | 415 names for p40, complete per main (PWR 7, CALL 24, SMS 18, NET 19, GPRS 22, SEC 11, IMEI 8, CFG 49, GPS 54, …) |
| Channel | `umts_ipc0` = iodev id 235, `ATTR_SIPC5` only; kernel adds/strips the SIPC5 link header, no padding (`ld->aligned` false for IPC), `write()` = one frame, `read()` returns one payload |
| Vendor gate | `DevIoctlIoChannel::Read` @0xd9fcc does `ioctl(fd, 0x6f27)` (`IOCTL_MODEM_STATUS`) and only reads when the result is 4 (`STATE_ONLINE`) |
| Layering | `rild` = dlopen+RIL_Init only; `libril.so` = AOSP socket/request framework; **all IPC IDs live in `libsec-ril.so`** |
| Registration minimum | `PWR_PHONE_PWR_UP/3` (CP→AP) → `PWR_PHONE_STATE/1` → `NET_REGIST/2 {ff,02}` → `NET_REGIST` resp → `NET_SERVING_NETWORK/2` + `NET_IDENTITY/3` → `SEC_PIN_STATUS/2` → `MISC_ME_IMSI/2`; signal is a CP `CALL_SIGNAL/3` notification with no AP request |
| DSL note | `libsec-ril-dsds.so` is a materially different build (2 126 295 of 2 468 316 bytes differ), not a flag-flipped copy; the catalog covers `libsec-ril.so`, the one `rild.libpath` names |

### Verified request/response pair

`NET_REGIST`: request built by `IpcProtocol40::IpcTxNetGetVoiceRegState` @0x1aca3c
(`09 00 <seq> 00 08 05 02 ff 02`), response parsed by
`IpcProtocol40::IpcRxNetRegState` @0x1ad38c (state @7, domain @8, radio tech @9,
LAC u16 @0xb, cell id u32 @0xd, extras @0x11/@0x12). Both sides disassembled; the
semantic names of the two enum tables (0x238550, 0x2386e0) are not recovered.

## Cross-check defect in `j7elte-ril` (not edited by me)

`parity/reports/ril-impl.md` §3.1 documents the IMEI request as a **10-byte** frame
`0a 00 <seq> 00 0a 03 02 ff 01 00`. The stock builder
`IpcProtocol40::IpcTxGetSerialNumber` @0x1ab7a0 emits **8 bytes**:
`08 00 <seq> 00 0a 03 02 <type>` (`mov.w ip,#8; strh ip,[sp,#8]`, `strb r0(2),[sp,#0xe]`,
`strb r4,[sp,#0xf]`, then `movs r2,#8` as the channel-write length). The p41 equivalent
(`Ipc41PacketBuilder::BuildIpcMiscMobileEquipmentSerialNumber` @0x1d3bec) also builds
exactly 8 bytes, and the DSDS build's copy at 0x1ab5b8 is the same code. The response
parser @0x1ac220 reads `[7]=type, [8]=len, [9..]=value`, i.e. the request body is only the
requested type. The 10-byte frame in the sibling package therefore has two extra payload
bytes and a wrong `len` field; it will very likely be rejected by the CP. Owner notified;
the spec is authoritative here.

## Unknowns (all restated in spec §11)

Protocol variant actually selected (CSC-driven, `CreateProtocol` @0x198b2c), `ack_seq`
semantics, registration/call/PDP field semantics beyond the byte offsets, SMS PDU
construction, p41 sub-name tables, RFS/IMS sub-commands, and the input namespace of
`ConvertToIpcCmd` @0x1a4440.

## Artifacts

| Path | Content |
|---|---|
| `parity/userspace/ril/spec/messages.md` | the catalog (binaries/hashes, layering, channel ABI, header, main+sub tables, payload layouts, registration sequence, safety list, evidence index, full appendix) |
| `parity/reports/ril-messages.md` | this pointer report |

Nothing outside `spec/` and this report was written; `parity/userspace/ril/j7elte-*.c`,
the APKBUILDs and `parity/pmaports` were not touched.

## How to verify

Every claim in the spec carries a vaddr in `libsec-ril.so`; `parity/userspace/ril/stock/`
has the exact bytes. The ID tables can be regenerated by interpreting the two
`GetMainCommandName`/`GetSubCommandName` functions; the frame layouts by reading the
`IpcTx*/IpcRx*` builder/parser bodies at the quoted addresses. No hardware run was
possible; nothing here is a runtime observation.
