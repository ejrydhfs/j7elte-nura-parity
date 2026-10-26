# j7elte-ril — CALL, SMS and GPRS expansion (0003/0004 applied, 0005 delta)

**Verdict: partial, verified subset implemented; nothing run against a
baseband.**
`0003` and `0004` are applied in the tree (canonical `j7elte-ril.c` =
SHA-512 `aef6d045…1069`, 3244 lines). This report covers those passes and the
**0005 delta** against them, which adds the SMS text encoder, SIM write,
automatic incoming-SMS ack and the remaining traced PDP/GPRS frames.

All opcodes, lengths and body offsets come from the stock `libsec-ril.so`
(ARM32, `09539011…`) instruction streams, read with Ghidra and cross-checked
against `parity/userspace/ril/spec/{calls,sms,data,messages}.md`. The
GSM 03.40 encoder is the one part that is new code rather than a traced
builder; it is validated against the spec's worked example and is called out
as such below.

---

## 1. Delivery state

| Patch | Base | Content |
|---|---|---|
| `0003-j7elte-ril-call-sms-pdp.patch` | frozen 1335-line source | call basics, SMSC get/set, GPRS get/define-PDP/PS-attach, unit `EnvironmentFile` fix — applied |
| `0004-j7elte-ril-extended-call-sms-pdp.patch` | applied 0003 | PDP activate/deactivate, SMS send/read/delete/ack + RX decoders, dial B, call-list upload, modify, call time, E911, SRVCC, block, transfer, manage-call, SND audio path — applied |
| `0005-j7elte-ril-sms-encoder-gprs-actions.patch` | applied 0004 (aef6d045) | SMS text→TPDU encoder, `sms-write`, auto MTSMS ack, PDP actions 2..5, IPv6 config, mobile-data — **not applied** |

0005 is 54 515 bytes / 1559 lines / 57 hunks and touches:

```
parity/userspace/ril/j7elte-ril.c
parity/pmaports/device/archived/j7elte-ril/j7elte-ril.c
parity/pmaports/device/archived/j7elte-ril/APKBUILD
```

Apply from the session root with `-p1`. New source:

```
8ab9c319b58b461fe1007613df4ef2b0f777478a7e03f3a846db256e2c78903c8bf6da65ca3c908026baa63c9996ec11ace67c07261cb968fc4d21c9942737fa
```

3742 lines. `0002-j7elte-ril-package.patch` must be regenerated after 0005
(package source and the APKBUILD hash change); it was regenerated for 0004
and is untouched here.

---

## 2. Messages and behaviour

Header: `u16 len, u8 seq, u8 ack, u8 main, u8 sub, u8 type`. `msg_seq` is
filled by the send path (`SetMsgSequence` 0x1a42d2 / `GenerateMsgSequence`
0x1a34fe), matching the stock code. Builders are the IpcProtocol41 copies
(the live class; `IpcModem::CreateProtocol` 0x198b2c).

### 2.1 CALL (main 0x02) and call-adjacent (0x0c, SND 0x09)

| Command | main/sub/type | len | Builder |
|---|---|---:|---|
| `call-originate NUM [CLIR] [TYPE] [ID]` | 02/01/EXEC | 99 | 0x1b5335 |
| `call-dial-b NUM [CLIR] [TYPE] [FLAG] [COND]` | 02/01/EXEC | 101 | 0x1b543d |
| `call-answer` | 02/04/EXEC | 7 | 0x1b58dc |
| `call-release` | 02/03/EXEC | 7 | 0x1b5b38 |
| `call-list` | 02/06/GET | 7 | 0x1b5568 |
| `call-list-set TYPE,CID,F25,W4,F24,NUM …` | 02/06/SET | 8+Σ | 0x1b55d5 |
| `call-hold` | 02/13/EXEC | 7 | 0x1b6421 |
| `call-dtmf TONE\|stop` | 02/08/SET | 9 | 0x1b5db5 / 0x1b5e31 |
| `call-burst-dtmf DIGITS` | 02/07/EXEC | 91 | 0x1b5cc1 |
| `call-flash STRING` | 02/11/SET | 90 | 0x1b5f91 |
| `call-voice-privacy get\|set [TYPE]` | 02/0c/GET, 02/0c/SET | 8, 9 | 0x1b5f29 / 0x1b5ea5 |
| `call-modify ID TYPE STR…` | 02/30/EXEC | var | 0x1b5949 |
| `call-modify-confirm ID TYPE STR…` | 02/32/EXEC | var | 0x1b5a45 |
| `call-time get` / `set SECS` | 02/0d/GET, 02/0d/SET | 9, 37 | 0x1b6171 / 0x1b60ed |
| `call-total-time get` / `set V1..V7` | 02/0d/GET, 02/0d/SET | 9, 37 | 0x1b62a5 / 0x1b61fd |
| `e911-cb MODE` | 02/10/SET | 8 | 0x1b6075 |
| `e911-status STATUS RAT` | 02/17/SET | 9 | 0x1b6319 |
| `call-srvcc STATE` | 02/14/SET | 8 | 0x1b63a5 |
| `call-block STATUS` | 02/18/SET | 8 | 0x1b6609 |
| `call-transfer ARG NUM1 [NUM2] [NUM3] [NUM4]` | 0c/81/SET | 358 | 0x1b6679 |
| `call-cancel-transfer ARG` | 0c/82/SET | 8 | 0x1b6865 |
| `call-manage TYPE INDEX NUMBER` | 0c/07/SET | 92 | 0x1b5ba5 |
| `audio-path PATH MODE` | 09/03/SET | 9 | 0x1ce085 (p41 only) |

Key constants: `ConvertCallTypeToIpc` 0x1b5319 is a 21-entry int16 table at
**0x2392f6** (`0000 0100 0300 0105 0106 0301 0306 0302 0303 0304 0305 0307
0700 0702 0100 0801 0308 0309 030a 030b 030c`; out-of-range → 0x0100;
CallType 13 = emergency on the p41 path). Dial A uses `number[0]` for TOA
(`0x11`/`0x21`); dial B computes `[11]` from `(FLAG && COND==1)` and does not
look at the number. Manage-call maps its type through the s8 table at
0x239352 (1→3 … 8→10, 9..17→-1, 18→4, 19..21→0). The call-list `W4` byte is
the unresolved `table[CallDetails[+8]]` and is a caller input.

RX: call status (raw u16 state word + four bytes), call signal (three bytes),
incoming indication (no payload fields), call-list payload raw.

### 2.2 SMS (main 0x04)

| Command | main/sub/type | len | Builder |
|---|---|---:|---|
| `sms-send NUMBER TEXT [SMSC] [DOMAIN]` | 04/01/EXEC | 11+N | 0x1cd280 + encoder |
| `sms-send-tpdu TPDU_HEX SMSC [DOMAIN]` | 04/01/EXEC | 11+N | 0x1cd280 |
| `sms-write INDEX STATUS TPDU_HEX SMSC` | 04/04/EXEC | 268-byte write | 0x1cd110 |
| `sms-read INDEX STORAGE` | 04/03/GET | 10 | 0x1af9c6 |
| `sms-delete INDEX STORAGE` | 04/05/EXEC | 10 | 0x1af934 |
| `sms-ack TPID ok\|fail [CAUSE]` | 04/06/EXEC | 12 | 0x1cd340 |
| `sms-smsc` | 04/0a/GET | 7 | 0x1cd594 |
| `sms-smsc-set ADDR` | 04/0a/SET | 19 | 0x1afa10 |

Send body: `[7]=2`, `[8]=1` GSM path, `[9]=ConvertDomainToIPCE(domain)`
(0→2, 1→3, 3→1, else 0), `[10]=N`, `[11..]=SCA blob + TPDU`; write size is
exactly `11+N` (the 267-byte memset belongs to the p40 twin only).
SCA (`BuildScaPdu` 0x181340): leading `+` required by this tool (the stock
uses `GetToA()` 0x1812c5 otherwise, unresolved), `k=(digits+1)/2 ≤ 11`,
`out[0]=k+1`, `out[1]=0x91`, BCD nibble-swap with `0xF` fill.
When SMSC is omitted, `sms-send` first sends `sms-smsc` and captures the
`04/0a` reply (the stock `SmsManager::DoSendSms` substitutes the SIM SMSC
when the request string is empty).

**Text encoder (new code).** `sms-send NUMBER TEXT` builds a single-part GSM
03.40 SMS-SUBMIT with no VP field: `01 00 <DA-len> <TOA> <DA-BCD> 00 00
<UDL> <packed septets>`; TP-MR is 0, TP-PID 0, TP-DCS 0 (7-bit default).
It implements the GSM 03.38 default alphabet for printable ASCII, including
the extension table (`^ { } \ [ ~ ] |` become ESC + code), and rejects any
character without a mapping. Limits: destination 1..20 digits, text
1..160 septets, one part only (no UDH/concatenation, no national-language
tables). Its selftest vector is the spec example: number/text
`+15551234567` / `hi` with SMSC `+15551234567` must produce
`22 00 … 04 01 01 02 01 02 17` + `07915155214365f7` +
`01000b915155214365f7000002e834`, exactly.

SIM write (`IpcTxWriteSmsToSim` 0x1cd110): the frame is **always written as
268 bytes**; the length field is `12+N` (u16), `[7]=2`,
`[8..9]=u16(index==255 ? 0xffff : index-1)`, `[10]=(status+1)&0xff`,
`[11]=N`, `[12..]=SCA+TPDU`.

Ack: status 0 on success; failure maps cause 0x80→0x8080, 0x81→0x8081,
22→0x8016, else 0x806f; `tpid` is the incoming `[11]` and must be ≤ 255.
**Auto-ack**: `listen` watches `04/02` INDI; for an SMS (`[7]==2`) it records
the tpid and sends `04/06` with status 0 on the next loop iteration — well
inside the 60 s window the CP allows. `--no-auto-ack` disables it. The
delayed-timer form (send at 60 s) was intentionally not used: acking late
risks the CP retransmitting.

RX: send result 04/01 (u16 status, third field, PDU length, raw PDU),
incoming 04/02 (type, tpid, SCA+PDU total, SCA length, raw TPDU), SMSC 04/0a,
raw 04/06. TPDU field decoding (sender/time/text) is not implemented.

### 2.3 GPRS (main 0x0d)

| Command | main/sub/type | len | Builder |
|---|---|---:|---|
| `pdp-get` | 0d/04/GET | 7 | 0x1a7a4c |
| `pdp-define CID APN [PROTO]` | 0d/01/SET | 134 | 0x1b95bc |
| `pdp-activate CID APN [AUTH]` | 0d/04/SET | 112 | 0x1b96ac (action 1) |
| `pdp-deactivate CID [REASON]` | 0d/04/SET | 112 | 0x1b96ac (action 0) |
| `pdp-action CID ACTION` | 0d/04/SET | 112 | 0x1b96ac (actions 2..5) |
| `ps-attach on\|off [REASON]` | 0d/03/SET | 10 | 0x1b9828 |
| `ipv6-config CID ADDR_HEX` | 0d/09/SET | 24 | 0x1b9c68 |
| `mobile-data B0 B1` | 0d/1d/SET | 9 | 0x1a7b68 |

PDP SET layout: `[7]` action, `[8]` cid, `[9..12]` u32 flags
(1 APN, 2 second string, 0x10 auth), `[13..44]` APN[32], `[45..76]` second
string[32], `[109]` auth (table 0x239b10 = {1,2,1}, indexed AUTH-1),
`[111]` detach reason 1..4. The builder's switch table at **0x1b970c** maps
its int argument directly to the wire action for 0..5; only action 1 enters
the string-copy block and only action 0 stores a reason, so actions 2..5 are
a 112-byte frame with just `[7]` and `[8]` set. `pdp-action` exposes 2..5.

IPv6 configuration: `[7]=cid`, `[8..23]` = 16 address bytes.
Mobile data: `[7]`, `[8]` booleans.

RX: PDP list for `type != EVENT` (`[7]` count, `{cid, active}` pairs),
IP configuration 0d/09 (cid + raw u16 flags), GPRS call status 0d/10
(cid, connected, cause), PS state 0d/03.

---

## 3. Validation performed (no hardware)

* **Host build.** `cc -std=c11 -D_GNU_SOURCE -Wall -Wextra
  -Wno-unused-parameter -Werror -O2` — clean.
* **Cross build.** aarch64 GCC 10.3.1, same flags + `-static`: clean,
  `ELF 64-bit LSB executable, ARM aarch64, statically linked` (4 413 192 B).
* **`--selftest`** (positional command; host and `qemu-aarch64` of the static
  binary): **rc=0**. It checks **44 exact frames** — 21 fixed vectors plus 23
  parameterised builders, including the SMS text encoder against the spec
  example, the 268-byte SIM write (length field 0x23), PDP actions/activate/
  deactivate, IPv6 config, mobile-data, dial A/B, DTMF, flash, SMS ack ok/
  fail. It also runs the mock CP through the query/send paths and exercises
  the auto-ack helper (one real ack frame, second call a no-op).
* **`--dry-run`** prints, among the rest:

  ```
  sms-text      220000000401010201021707915155214365f701000b915155214365f7000002e834
  sms-write     23000000040401020000011707915155214365f7…0000
  pdp-action    700000000d04030301000000…00
  ipv6-config   180000000d09030120010db8000000000000000000000001
  mobile-data   090000000d1d030100
  ```
* **Patch.** `0005` passes `git apply --check -p1` and `patch --dry-run`
  against a mirror of the current applied tree; applying it reproduces the
  staged tree byte for byte. The applied source builds with `-Werror` and
  passes `--selftest` on host and aarch64 under qemu; APKBUILD `sha512sums`
  verify in the applied tree.
* **Argument paths.** Missing/invalid arguments exit 2 before sending;
  `--no-auto-ack` is accepted.

---

## 4. Remaining work (exact)

### 4.1 SMS
* Text encoder is **single-part GSM 7-bit only**: no concatenated SMS/UDH,
  no UCS2 or national-language shift tables, 160-septet cap. Multi-part
  sending is not implemented.
* TPDU field decoding (originating address, SCTS, text) is not implemented;
  incoming/send-result PDUs are printed raw.
* Auto-ack is immediate (inside the 60 s window), only for `[7]==2` GSM
  messages, and only while `listen` runs; there is no persistent ack queue.
* `sms-write` encodes the traced 268-byte frame but SIM-record semantics
  (status byte, record numbering) have not been exercised on a CP/SIM.
* Remaining SMS builders: 04/09 count, 04/0c memory status, 04/0e CB config,
  and the p41-only builders in spec §5.13.

### 4.2 GPRS
* PDP SET actions 2..5 are emitted as the traced zero-body frames; their
  CP-side meaning was not proven (the stock jump table only shows that the
  string block is skipped).
* 13/0x14 LTE attach profile (199 B), 13/0x1b and the other non-traced GPRS
  entries are not implemented. Define-PDP's optional auth/IPv6/IPv4 blocks
  are always absent.
* IPv6 config and mobile-data frames are built but no CP response semantics
  were verified; rmnet interface configuration remains kernel-owned.

### 4.3 CALL
* Call-list upload `W4` is the unresolved table lookup and must be supplied
  by the caller; the rest of the call-list record semantics are raw.
* E911 status RAT is a raw byte (converter slot 0x8e4 unresolved);
  manage-call does not apply `NeedCallIdPlusMinus()`'s product-dependent
  index increment; total-time values are raw u32s.
* Test-call 02/01 diagnostic and end-test-call are not exposed.
* RX call-list records, call-time responses and the `ConvertToIpcCmd` state
  word are printed raw or only as verified sub-fields.

### 4.4 Transport / hardware
* Response matching is still "any message after a GET", not the stock
  `IpcHistory` `(main,sub,type)` + `msg_seq` lookup (0x1a42de).
* No CP acknowledgement, INIT_END effect or payload has ever been observed.
  All evidence is static; the CP may reject an inferred sub-command.

---

## 5. Artifacts

| Path | Content |
|---|---|
| `parity/patches/userspace/ril/0003-j7elte-ril-call-sms-pdp.patch` | first pass, applied |
| `parity/patches/userspace/ril/0004-j7elte-ril-extended-call-sms-pdp.patch` | second pass, applied |
| `parity/patches/userspace/ril/0005-j7elte-ril-sms-encoder-gprs-actions.patch` | this delta |
| `parity/reports/ril-impl-2.md` | this report |

The tree is at the 0004 state (`aef6d045…1069`); 0005 is produced against
exactly that state. No shared device APKBUILD, kernel config or `j7elte-cbd`
file was modified.
