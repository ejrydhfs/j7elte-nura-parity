# SMS slice — message catalog delivered (static evidence only)

**Verdict: spec complete; no runtime verification (no hardware).**
The `SMS_CMD` (main 0x04) catalog for j7elte is written up in
[`parity/userspace/ril/spec/sms.md`](../userspace/ril/spec/sms.md):
send, send result, incoming SMS, MTSMS ack, delivery report, SIM storage
(write/read/delete/count), SMSC address, memory status and cell broadcast,
with byte offsets and vaddrs for every field.

## What was done

* Re-extracted all 9 telephony binaries from `system.raw` with `debugfs`
  and compared them byte-for-byte with `parity/userspace/ril/stock/`
  (identical). SHA-256 recorded in the spec §1.
* Disassembled `libsec-ril.so` (ARM32/Thumb-2) and decoded its Android
  APS2 packed relocations (SLEB128 group format) so that the C++ vtables
  could be resolved symbolically. This step was necessary to prove which
  handler each `SMS_CMD` sub-command reaches and which protocol class is
  built at runtime.
* Traced every SMS builder and parser end to end, including the argument
  provenance from `SecRil` request data through `SmsManager` →
  `ModemProxy` → `IpcModem` → `IpcProtocol41`.

## Findings that matter

| Item | Result |
|---|---|
| Active protocol | `IpcProtocol41`, not 40. `CreateProtocol` maps 0→p40 and everything else→p41; every `ModemConfig` produced by `LoadModemConfiguration` carries proto ver 1 or 2. `libsec-ril.so` never selects p40 on this image. |
| Send | `04/01/EXEC`, `len = 11 + N`; `[7]=2` (GSM), `[8]=1`, `[9]=ConvertDomainToIPCE(GetDomain())`, `[10]=N`, `[11..]=SCA+TPDU`. p41 writes exactly `len` bytes (`0x1cd30a/0x1cd316`). The p40 twin writes 267 — different. |
| SCA | `SmsCenter::BuildScaPdu` @`0x181340`: `[n_octets][toa][BCD digits]`; empty SMSC still emits `01 <toa>` (2 bytes), not `00`. |
| Send result | `04/01`: `u16[8..9]` status → `GetSmsResult` (0/11/12-13/18-19/0x8000+ mapping), `[10]` third field, `[11]` PDU length, `[12..]` PDU. |
| Incoming | `04/02`: `[7]=2` GSM; `[8]`→msg+0x18, `[11]`→msg+0x20, `[12]`=SCA+PDU total, `[13]`=SCA len octet; PDU raw binary after SCA. Registrant 57. |
| Ack | `04/06/EXEC`, 12 bytes: `[7]=2`, `[8..9]`=0x0000 on success (or 0x8080/0x8081/0x8016/0x806f on failure), `[10]`=tpid, `[11]=0`. 60 s local timer; on expiry `IpcModem::SmsIncomingAckTimeout` is a **stub** that never tells the CP. |
| Delivery report | `04/06`: `u16[7]` status → `GetSmsResult`; RIL emits event 7 (or 0), does not forward TP-Status. |
| SIM storage | write `04/04` → `04/04` result; read `04/03` → `04/03` message; delete `04/05` → `04/05` result; count `04/09`; 0-based record index on the wire. |
| Concatenation | No reassembly in the RIL. UDH header is parsed into `GsmSmsMessage+0x54`, but each segment is an independent `04/01` indication and is acked independently; framework does the joining. |
| RFS | Not used for SMS. Only `/dev/umts_rfs0` must be *opened* for the INIT_END handshake. |

## Notes for the implementation owner

* The send frame is shorter than the stock p40 constant: write `11+N`
  bytes (not 267). `N` = SCA prefix + raw TPDU bytes; `[10]` must equal
  `N` and the `u16 len` must equal `11+N`.
* `[8]=1` for a GSM single-part send (the GSM branch forces it).
  `[9]` is `ConvertDomainToIPCE(DomainSelector::GetDomain())`; the runtime
  domain depends on CSC/IMS, so start with `02` (the `0→2` mapping) and
  adjust from a CP trace if the frame is rejected.
* Ack `tpid` (`[10]`) is the `GsmSmsMessage+0x20` value echoed from the
  incoming frame byte `[11]`; `0x100` is rejected by the stock builder.
* Response sub IDs echo the request sub (send 1→1, incoming 2→2,
  write 4→4, read 3→3, delete 5→5, ack/status 6→6). Match on `(main,sub)`
  and `cmd_type`; the two directions share numbers for 1 and 6.

## Discrepancies with earlier documents

* `spec/messages.md` §A.04 and its layout row describe the **p40** send
  builder (267-byte write, body starting at `[0xb]`). That class is not
  selected on this device; the p41 layout above supersedes it for
  `j7elte-ril`.
* `spec/messages.md` says the IPC channel gets no SIPC5 padding, while
  `spec/framing.md` says `ld->aligned` is set after `IOCTL_MODEM_DL_START`
  and post-boot frames are `cfg=0xFC`, 8-byte padded. SMS payload layout is
  unaffected (userspace supplies payloads only), but the framing owner
  should resolve the contradiction before the first write test.

## Unknowns (full list in spec §11)

Send bytes `[8]`/`[9]` semantics and CP validation; incoming bytes
`[8]`,`[9]`,`[10]`,`[11]`; CP retransmission of unacked indications;
TP-MR matching for status reports; `SMS_SEL_MEM` request layout (no builder).

## Verification performed

* `debugfs` extraction + `sha256sum` + `cmp` against the stock copies
  (9/9 identical).
* Every byte offset quoted in the spec was read from the disassembly of
  the cited function; vtable slot → handler mappings were resolved from
  the APS2 relocations (e.g. `IpcProtocol41` vtable slot `0x34c` →
  `IpcTxSendSms`, slot `0x6a0` → `IpcRxSms`, sub-table slot `0xaac` →
  `IpcRxIncomingMsg`).
* Not performed: any execution against the CP, `qemu` replay of SMS
  frames, or hardware trace.

## Artifacts

| Path | Content |
|---|---|
| `parity/userspace/ril/spec/sms.md` | the catalog (hashes, protocol selection, TX/RX layouts, worked PDU example, minimum sequence, SIM storage, unknowns, evidence index) |
| `parity/reports/ril-sms.md` | this pointer report |

No code, package, APKBUILD, kernel file or shared device config was
modified.
