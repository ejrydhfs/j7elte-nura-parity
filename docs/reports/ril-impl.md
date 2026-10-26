# j7elte-ril — implemented SIPC5/IPC client; calls/SMS/data still open

**Verdict: implemented for the transport and the traced GET/EXEC subset;
partial for telephony.** `j7elte-ril` opens the FMT and RFS channels (which
completes the AP side of the CP handshake), speaks the Samsung message layer
with the exact framing used by `libsec-ril.so`, and sends/decodes the
protocol-41 messages that were recovered. Calls, SMS and data/PDP are **not
complete**; no success is claimed for them and no hardware was available.

This pass took over a concurrently written `j7elte-ril.c` (protocol-41 proxy,
mock-CP self-test) and fixed the defects below. Detailed reverse-engineering
addresses and the per-opcode Ghidra listings are in
`parity/reports/ril-re.md`; this report covers the implementation, build,
packaging, validation and the remaining gap.

Final source state: `parity/userspace/ril/j7elte-ril.c`, 1335 lines, sha512
`5ef663ec…8b4f` (full hash in the APKBUILD), cross-built and self-tested as
described in §4.

---

## 1. Transport

`/dev/umts_ipc0` is io_device 235 (`SIPC5_CH_ID_FMT_0`), `IPC_FMT`,
`attrs = 0x02` (`ATTR_SIPC5`, no `ATTR_NO_LINK_HEADER`;
`arch/arm64/boot/dts/modem-isla-pdata.dtsi:53-58`). The kernel owns the SIPC5
link layer:

* `misc_write()` prepends the 4-byte link header (`cfg`, `ch=235`, `len`) to
  every `write()` (`modem_io_device.c:879-885,939-946`);
* `rx_fmt_ipc()` strips it before queueing (`modem_io_device.c:257-278`);
* `misc_read()` returns one payload and waits 100 ms when empty
  (`modem_io_device.c:974-991`);
* `misc_write()` returns `-EPERM` unless `mc->phone_state == STATE_ONLINE`
  (`modem_io_device.c:869-876`, `modem_prj.h:660-665`).

Userspace therefore writes **IPC messages only**. The message header is
`struct sipc_fmt_hdr` (7 bytes, `modem_prj.h:205`): `u16 len; u8 msg_seq;
u8 ack_seq; u8 main_cmd; u8 sub_cmd; u8 cmd_type`.

Opening `/dev/umts_rfs0` after `/dev/umts_ipc0` makes `shmem_init_comm()`
(`link_device_shmem.c:1463-1524`, RFS case) call `send_ipc_irq(CMD_INIT_END)`
immediately, because the peer FMT io_device is already open. The
`rild_ready()`/CP_START path is only the retry fallback. The program holds
both descriptors for its lifetime and never writes RFS.

A RIL started after `j7elte-cbd` can find the kernel still reporting
`STATE_OFFLINE` even though the CP is already online (the state notification
loop in `ss310ap_boot_off()` skips iodevs that were not open at boot). The
program therefore re-issues `IOCTL_MODEM_BOOT_OFF` on its open FMT descriptor;
`mc->init_cmpl` is already complete, so the ioctl returns immediately and
re-notifies every open iodev. This is an AP-side bookkeeping action; it does
not boot or reset the CP.

## 2. Protocol version and framing evidence

This build uses **IpcProtocol41**, not 40:

* `IpcModem::CreateProtocol(ModemProtoVer)` @`0x198b2c`: `0` →
  `IpcProtocol40` (`0x1a4418`), `2` → `IpcProtocol41(2)` (`0x1b2ed0`),
  `8` → `IpcProtocol41Iil`, any other non-zero → `IpcProtocol41`.
* `ModemFactory::LoadModemConfiguration()` @`0xf754c` is the only caller of
  `ModemConfig::Init()`. The parsed-entry path takes the proto version from
  entry+16 (the built-in template at `0x26318c` holds `4`); the fallback path
  at `0xf7830` stores `2` in the `ModemProtoVer` stack slot before the call at
  `0xf786a`. Both values select `IpcProtocol41`.

Consequences for this implementation:

* IMEI request is the 8-byte protocol-41 form
  (`IpcProtocol41::IpcTxGetSerialNumber` @`0x1c21f4` →
  `Ipc41PacketBuilder::BuildIpcMiscMobileEquipmentSerialNumber` @`0x1d3bec`):
  `len=8, [4]=0x0a, [5]=0x03, [6]=GET, [7]=kind`. Protocol 40's 10-byte form
  with a `0xff` prefix would be wrong here.
* PIN status and NET registration requests are byte-identical between 40 and
  41; the program uses the 41 builders.
* `cmd_type` is 1 EXEC, 2 GET, 3 SET, 4 CFRM, 5 EVENT on requests; responses
  use 1 INDI, 2 RESP, 3 NOTI (`GetCmdTypeName`/`GetResponseTypeName`).
* `msg_seq` is overwritten at send time by
  `IpcProtocol::SetMsgSequence()` @`0x1a42d2` from
  `GenerateMsgSequence()` @`0x1a34fe`, an incrementing 8-bit counter. The
  program now does the same (first message 0, then 1, 2, …); the builders
  themselves leave the byte zero.

## 3. Implemented messages

`--print-table` prints the same list with the evidence addresses. The three
messages required by the ticket:

### IMEI — `--imei` / `imei`

```
MISC_CMD(0a) / sub 03 / GET, len 8
08 <seq> 00 0a 03 02 <kind>      kind 1 = IMEI
```

`IpcProtocol41::IpcTxGetSerialNumber` @`0x1c21f4` /
`BuildIpcMiscMobileEquipmentSerialNumber` @`0x1d3bec`. The reply
(`IpcProtocol41::IpcRxMiscSerialNumber` @`0x1c4d6c` →
`Ipc41RilDataBuilder::BuildRilDataMiscMobileEquipmentSerialNumber` @`0x1d3d04`,
layout same as `IpcProtocol40` @`0x1ac220`) is decoded as:

* kind 1 (and any non-hex kind): `[8]` = length, `[9..]` = ASCII → `imei:`;
* kind 2 or 7: four bytes at `[9..12]` as hex → `meid:`;
* kind 3: seven bytes at `[10..16]` as hex → `esn:`.

This corrects the concurrent draft, which labelled kind 3 (DeviceInfoType 8,
ESN) as IMEI. The real `--imei` request uses kind 1, which takes the ASCII
path.

### SIM lock/PIN status — `--sim` / `pin`

```
SEC_CMD(05) / sub 01 / GET, len 7
07 <seq> 00 05 01 02
```

`IpcProtocol41::IpcTxGetPinStatus` @`0x1cb844`;
`IpcProtocol41::IpcRxSecPinStatus` @`0x1cc4e0`:

* `[7]` = raw lock type → 139-entry table `0x23bcc0` (index raw-1):
  1..11 identity, `0x80..0x84` → 12..16, `0x88..0x8b` → 17..20, else 0;
* `[8]` = lock key, used directly (1..6, identity table `0x23bd58`);
* `[6]==EVENT` additionally sets the "pin state changed" flag in
  `IpcSimCache+0x21`; the stock transmit path refuses to send the GET until
  that flag is set. That is an AP-side cache gate, not part of the wire
  message; this program sends the traced GET regardless.

The draft had `[7]`/`[8]` swapped and omitted the `0x80`/`0x88` mappings;
fixed.

### Network registration — `--net` / `reg`

```
NET_CMD(08) / sub 05 / GET, len 9   voice: 09 <seq> 00 08 05 02 ff 02
NET_CMD(08) / sub 05 / GET, len 9   data:  09 <seq> 00 08 05 02 ff 03
```

Builders `IpcProtocol41::IpcTxNetGetVoiceRegState` @`0x1c74bc` and
`IpcTxNetGetDataRegState` @`0x1c7534`. Parser
`IpcProtocol41::IpcRxNetRegState` @`0x1c8bb4`; fields are printed by their
payload offsets because the semantic names of the `NetRegState` constructor
arguments were not recovered: `stat`[7], `f8`[8], `rat`[9], `f10`[10],
`f0b` u16[11..12], `f0d` u32[13..16], `f11`[17], `f12` u16[18..19],
`f14..f16`[20..22], `f17` u16[23..24]. Partial replies print the fields that
are present rather than being discarded.

### Other traced requests

`SMS_DEVICE_READY`, `MISC_GET_BASEBAND_VERSION`, `MISC_GET_ERI_VERSION`,
`MISC_GET_TIME_INFO`, `MISC_GET_DEVICE_CAPA`, `SEC_SET_SIM_POWER`,
`NET_GET_OPERATOR`, `PWR_RADIO_POWER`, `PWR_MODEM_POWER_OFF` are implemented
and byte-verified by the self-test. Responses for them are printed as a header
plus hex dump unless a decoder exists; no semantics are guessed.

### Refused

`--send-sms` exits 2 with an explicit message. `SMS_DEVICE_READY` is sent by
`info`, but the SMS submit frame (`SMS_CMD/sub 01/EXEC`, 267 bytes,
`IpcProtocol41::IpcTxSendSms` @`0x1cd280`) contains a PDU and a conversion
that were not traced end to end. Unknown `main/sub` combinations are never
sent by the normal commands; `raw` is deliberately an escape hatch that
requires the user to supply a complete message.

## 4. Build and validation (no hardware)

* **Host build.** `cc -std=c11 -D_GNU_SOURCE -Wall -Wextra
  -Wno-unused-parameter -Werror -O2` — clean.
* **Cross build.** `aarch64-none-linux-gnu-gcc` 10.3.1 (arm-10.29), same
  flags plus `-static`: clean; `ELF 64-bit LSB executable, ARM aarch64,
  statically linked` (4 361 544 bytes). Runs under `qemu-aarch64`.
* **Frame self-test.** `j7elte-ril selftest` first compares 14 complete
  message byte vectors against the traced builders (including the 8-byte IMEI
  frame `080000000a030201`, PIN `07000000050102`, voice
  `09000000080502ff02`, data `09000000080502ff03`), then starts a mock CP on
  a socketpair and decodes its replies:
  - kind-1 ASCII IMEI `353438353132370` (24-byte message) → `imei:` +
    `kind: 1 (DeviceIdentityType 1)`;
  - 25-byte registration state → every field printed;
  - 9-byte PIN status → `lock_type=0x01 (LockType 1) lock_key=0x01 (1)`.
  Host and static-aarch64-under-qemu both exit 0.
* **`--dry-run`.** Prints the 14 vectors; `--dry-run --imei|--sim|--net`
  selects the corresponding command in the trailing summary line.
* **Flags.** `--imei`, `--sim` (PIN status), `--net` (voice + data
  registration) are aliases for the `imei`, `pin`, `reg` subcommands.
  `--send-sms` exits 2 before opening any device.
* **Packaging.** `bash -n APKBUILD`, `sha512sum -c` over all four sources,
  and the `build()`/`package()` bodies with the cross compiler all pass; the
  package installs `/usr/bin/j7elte-ril`, the OpenRC script and conf.d, and
  the systemd unit. Cross binary size in the staged package is 4 361 544 B.
* **Not done:** `abuild` (not installed), execution against any modem, any
  observation of a CP response.

## 5. Remaining blockers (exact)

1. **Calls.** `CALL_CMD` release (`02 03 01`), answer (`02 04 01`, generic —
   the CallType argument is unread in the builder) and get-call-list
   (`02 06 02`) are 7-byte requests and are traced; they are queued for the
   integration owner but not in this source. Originate (`02 01 01`, 99 bytes)
   needs `ConvertCallTypeToIpc` @`0xd59c0` and ToA derivation and is not
   implemented. No call-state machine (incoming/status notifications,
   `IpcHistory` transaction matching) is implemented, so call control as a
   whole is absent.
2. **SMS.** GET SMSC (`04 0a 02`) and SET SMSC (`04 0a 03`, raw address
   string) are traced and pending integration. SEND SMS (`04 01 01`, 267
   bytes) still has an unresolved conversion of argument 5 and no traced PDU
   encoder; SMS is therefore not implemented and `--send-sms` refuses.
3. **Data.** GET PDP (`0d 04 02`, 7 bytes), DEFINE PDP (`0d 01 03`, 134
   bytes, traced) and PS ATTACH (`0d 03 03`, 10 bytes, traced) are pending
   integration. SET PDP (`0d 04 03`, 112 bytes) is complex and not
   implemented. No PDP context is activated and no `rmnet` data path is
   exercised.
4. **Response matching.** The current program waits for the next inbound
   message, which is sufficient for one outstanding GET. The stock RIL keeps
   an `IpcHistory` keyed by main/sub/type and `msg_seq`; production call/SMS
   work will need that to survive interleaved notifications.
5. **Hardware.** No CP acknowledgement, INIT_END effect or response payload
   has been observed. All opcode/length evidence is static.

## 6. Hardware test plan

Prerequisites: `j7elte-cbd` installed and the CP booted; kernel with
`CONFIG_SEC_MODEM_V1=y`, `CONFIG_SEC_MODEM_SS310AP=y`,
`CONFIG_LINK_DEVICE_SHMEM=y`; `/dev/umts_ipc0` and `/dev/umts_rfs0` present.

1. `j7elte-cbd -v -d`, wait for `CP boot complete`.
2. `j7elte-ril state` → must print `ONLINE (4)`. If it prints `OFFLINE` while
   the CP is up, the program re-issues `IOCTL_MODEM_BOOT_OFF` on the next
   command; check `dmesg | grep mif:` for the state transition.
3. Start `j7elte-ril -v --imei`; expect
   `mif: shmem: umts_rfs0->INIT_END->ss310ap` when the RFS fd opens, then the
   TX bytes `08 00 00 00 0a 03 02 01` and an RX message with main 10/sub 3
   whose ASCII payload is the IMEI. Compare with the value in the stock
   Android `RIL_REQUEST_GET_DEVICE_IDENTITY` / the device label. If it times
   out, do not change the opcode until a CP log/trace shows the frame
   arriving; try `-t 60`.
4. `j7elte-ril -v --sim`: expect lock type/key from the CP. Compare with the
   stock SIM lock state. A NOTI (`cmd_type 3`) also sets the stock cache gate
   and is printed as `changed`.
5. `j7elte-ril -v --net`: voice then data. Compare `stat/f8/rat/f0b/f0d/...`
   with the stock RIL's registration-state arrays for the same network. The
   field names are deliberately raw until the constructor semantics are
   proven.
6. `j7elte-ril -v info`: full query set; look for responses to every request
   and for `SMS_DEVICE_READY`.
7. Negative tests: `j7elte-ril --send-sms` → exit 2 and no device opened;
   unknown command → usage/exit 2.
8. Crash path: kill the process (or force a CP crash) and confirm `listen` /
   `-d` exits on `POLLHUP` so the service can restart. Closing the channels
   does not power the CP down.

## 7. Artifacts and state

| Path | Content |
|---|---|
| `parity/userspace/ril/j7elte-ril.c` | canonical source, 1335 lines |
| `parity/pmaports/device/archived/j7elte-ril/` | APKBUILD, source copy (same hash), initd, confd, systemd unit |
| `parity/patches/userspace/ril/0002-j7elte-ril-package.patch` | package diff; regenerated against this source revision, `git apply --check` passes on pmaports `ba682bd`, and the applied tree is byte-identical to the package directory |
| `parity/reports/ril-impl.md` | this report |
| `parity/reports/ril-re.md` | reverse-engineering report and per-address evidence (separate artifact) |

No shared device APKBUILD, kernel config or `j7elte-cbd` file was modified.
The package copy is byte-identical to the final source, its APKBUILD hash is
updated, `sha512sum -c` passes, and the 0002 patch was regenerated and
apply-checked. Further edits to the source/package/patch are owned by the
integration agent for the CALL/SMS/PDP consolidation.

## 8. CALL/SMS/GPRS consolidation

`parity/patches/userspace/ril/0003-j7elte-ril-call-sms-pdp.patch` has since
been applied: the canonical source, package copy and APKBUILD are now at
SHA-512 `57480ed5…8c83` (2147 lines), and 0002 was regenerated. 0003 added
CALL answer/release/hold/dial-A/DTMF/flash/voice-privacy and call get-list,
SMSC get/set, GPRS get/define-PDP and PS attach, plus a systemd
`EnvironmentFile` order fix.

A further delta, `0004-j7elte-ril-extended-call-sms-pdp.patch`, added PDP
activate/deactivate, SMS send/read/delete/ack with SCA encoding, dial
variant B, call-list upload, modify/modify-confirm, call time, E911, SRVCC,
block, transfer/cancel, manage-call and the SND audio path. It has since been
applied too (canonical `aef6d045…1069`, 3244 lines), and 0002 was regenerated.

A third delta, `0005-j7elte-ril-sms-encoder-gprs-actions.patch`, adds a
GSM 03.40 SMS-SUBMIT text encoder for `sms-send NUMBER TEXT`, SIM write
04/04, automatic incoming-SMS ack in `listen`, PDP actions 2..5, IPv6
configuration and mobile-data. It is **not applied** here. Details, evidence,
the new hash and the remaining unsupported messages are in
`parity/reports/ril-impl-2.md`.
