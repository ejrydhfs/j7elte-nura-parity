# Packet data / PDP context on j7elte (Shannon SS310AP, `libsec-ril.so`)

Scope: the IPC messages that carry packet-data signalling between the AP RIL
(`/system/lib/libsec-ril.so`) and the CP over `/dev/umts_ipc0`, plus the
kernel/interface side of `rmnet0..7`.  The transport itself (SIPC5 link
framing, 7-byte `sipc_fmt_hdr`, INIT_END handshake, `IpcHistory`) is in
[`framing.md`](framing.md) and is not repeated here; this document starts at
the 7-byte payload header.

All evidence is static: ARM32 ELF disassembly/decompilation of
`libsec-ril.so`, the kernel tree in this repository, and the ROM DTB.  Nothing
was executed against a modem.  Addresses are **file/virtual addresses inside
the ARM32 ELF** (the library is PIE; Thumb functions have bit 0 set in the
symbol table, so `IpcTxDefinePdpContext` = `0x1b95bd` means entry `0x1b95bc`).
`[INFERENCE]` marks conclusions that follow from field usage but were not
proven by an independent trace.

---

## 1. Extracted binaries

`system.raw` is the `/system` filesystem itself, so in-image paths are
`/bin/rild`, `/lib/...`.  Extraction was repeated with
`debugfs -R "dump <path> /tmp/rilx/<name>" system.raw`; the SHA-256 of the
dumped file matches `parity/userspace/ril/stock/` byte for byte.

| Path in image | Size | SHA-256 | Used by this slice |
|---|---:|---|---|
| `/system/lib/libsec-ril.so` | 2 468 316 | `09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73` | **yes** (all addresses below) |
| `/system/lib/libsec-ril-dsds.so` | 2 468 316 | `640f0ea0a7c6d650d3ede7e5410af72717db1fda35c6f78a6d07bfb78a67cfa0` | no (different build; same addresses do not apply) |
| `/system/bin/rild` | 22 072 | `64650e0b4ab11816539264b351da2d3dd965fd3753dd4f250c9191b0e18c57f4` | no |
| `/system/lib/libril.so` | 82 920 | `ad8072ac410f914fa87942aabd58ef71de73fbec1d664429a66b8ff2672b5618` | no |
| `/system/lib/libsecril-client.so` | 30 208 | `0506f623307ea4dc039164b987b4042de3b829e60fbeffa0a3927b7eea8d8020` | no |
| `/system/lib/libreference-ril.so` | 38 416 | `fc32b63443445fc563fbc5cb10858614ba2f1216d61406f5f355e6f1812c1807` | no |
| `/system/lib/librilutils.so` | 17 916 | `066981be45065af9633015cabdcecf54bb726562d7957808cbcf8402a97dc894` | no |
| `/system/lib/libaudio-ril.so` | 26 160 | `5e9103221c2f940c63ef5fe1a3e351794ed62cda709ed20b4fde88ba48c1e0fa` | no |
| `/system/bin/at_distributor` | 103 812 | `9d01e6167da24cf808fe976842552b9e3f1640a0faa72982c8b4c7d3f9a8435e` | no |

`boot_out/rd/default.prop` selects `rild.libpath=/system/lib/libsec-ril.so`, so
the single-SIM library is the one that runs.

---

## 2. Which protocol class is live: `IpcProtocol41`, not 40

The builder function chosen at run time is the concrete `IpcProtocol4x`
object.  The config decides it:

* `ModemFactory::LoadModemConfiguration()` @ `0x0f754c` builds the single
  `ModemConfig` and calls `ModemConfig::Init(...)` at `0x0f786a`
  (`blx 0xc285c` = PLT `ModemConfig::Init`).
  Register/stack argument setup at `0x0f782c…0x0f7864`:
  `r1 = 0` (index), `r2 = 3` (ModemIntfType), `r3 = "/dev/umts_ipc0"`
  (pc-relative literal at `0x0f793c`), `[sp+0] = 4|6` (ModemType),
  `[sp+4] = 1` (ModemProtocol), **`[sp+8] = 2` (ModemProtoVer)**
  (`0x0f7830: movs r5,#2` / `0x0f7838: str r5,[sp,#8]`).
* `ModemConfig::Init` stores the 6th argument (ModemProtoVer) at `this+0x1c`
  (`0x0f70bd` decompilation).
* `IpcModem::CreateProtocol(ModemProtoVer)` @ `0x198b2c`: `param == 2`
  allocates `IpcProtocol41` and calls `IpcProtocol41::IpcProtocol41(ver=2,
  IpcModem*)` @ `0x1b2ef1`; `param == 0` would create `IpcProtocol40`.

Therefore every data message below uses the **IpcProtocol41** builder.  The
IpcProtocol40 variants are listed only where they differ; they are dead code
on this device.

Call chain (all direct virtual dispatches, resolved from the vtables):

```
Dca (ContextActivationDca)            ContextActivationDca::DefineContext   @0x15027c
  -> SecRil facade vtable+0x158       (Dca+0x18 object)
  -> IpcModem::DefinePdpContext       @0x19a318   (IpcModem vtable+0x164)
  -> protocol object at IpcModem+0x34 : vtable+0xc4
  -> IpcProtocol41::IpcTxDefinePdpContext @0x1b95bc
```

The protocol object pointer is `*(IpcModem+0x34)`; `IpcModem::DefinePdpContext`
loads its vtable and branches to `+0xc4` (`0x19a354: ldr.w r7,[r1,#0xc4]`,
`0x19a35a: blx r7`).  Slot numbers were confirmed from the `IpcProtocol41`
vtable at `0x259200` (`vptr = vtable+8`):

| vtable slot | IpcProtocol41 entry | function |
|---|---|---|
| `+0xc4` | `0x2592cc` → `0x1b95bc` | `IpcTxDefinePdpContext` |
| `+0xc8` | `0x2592d0` → `0x1b96ac` | `IpcTxSetPdpContext` |
| `+0xcc` | `0x2592d4` → `0x1a7a4c` | `IpcTxGetPdpContext` |
| `+0xd0` | `0x2592d8` → `0x1b9828` | `IpcTxPsAttach` |
| `+0xdc` | `0x2592dc` → `0x1b9b04` | `IpcTxSetLteAttachProfile` |
| `+0xe0` | `0x2592e0` → `0x1b9894` | `IpcTxSetDataProfile` |
| `+0xe4` | `0x2592e4` → `0x1b9c68` | `IpcTxIpv6Configuration` |

All IpcModem entry points use the same transaction IDs (first argument of
`AllocatedTransaction`): `0x514` define, `0x515` activate/deactivate, `0x516`
get list, `0x517` PS attach (`0x19a318`, `0x19a586`, `0x19a5eb`, `0x19a37b`).

---

## 3. Message catalog (main command 13 = `GPRS_CMD`)

Direction is AP→CP for TX builders, CP→AP for RX parsers.  `type` is the
request table (`EXEC=1, GET=2, SET=3, CFRM=4, EVENT=5`) for TX and the
response table (`INDI=1, RESP=2, NOTI=3`) for RX (`framing.md` §4.2).

### 3.1 AP → CP

| # | main / sub / type | len | builder (entry) | purpose |
|---|---|---:|---|---|
| 1 | `0x0d / 0x01 / 3` | 134 (`0x86`) | `IpcTxDefinePdpContext` @ `0x1b95bc` | define one PDP profile (APN + protocol + requested IP) |
| 2 | `0x0d / 0x04 / 3` | 112 (`0x70`) | `IpcTxSetPdpContext` @ `0x1b96ac` | activate (act=1) / deactivate (act=0) PDP context |
| 3 | `0x0d / 0x04 / 2` | 7 | `IpcTxGetPdpContext` @ `0x1a7a4c` | read the PDP context list |
| 4 | `0x0d / 0x03 / 3` | 10 (`0x0a`) | `IpcTxPsAttach` @ `0x1b9828` | PS attach / detach |
| 5 | `0x0d / 0x14 / 3` | 199 (`0xc7`) | `IpcTxSetLteAttachProfile` @ `0x1b9b04` | LTE initial-attach APN profile |
| 6 | `0x0d / 0x1b / 3` | 203 (`0xcb`) | `IpcTxSetDataProfile` @ `0x1b9894` | full data profile (APN type, strings, auth, IPs) |
| 7 | `0x0d / 0x09 / 3` | 24 (`0x18`) | `IpcTxIpv6Configuration` @ `0x1b9c68` | push a static IPv6 address for a context |
| 8 | `0x0d / 0x1d / 3` | 9 | `IpcTxSetMobileDataSetting` @ `0x1a7b68` | mobile-data enable/roaming flags |
| 9 | `0x0f / 0x26 / 3` | 8 | `IpcTxCfgSetMobileApStatus` @ `0x1b7b90` | AP (tethering) status (main 15 = `CFG_CMD`) |

Notes:
* `IpcTxGetPdpContext` is one implementation shared by IpcProtocol40/41
  (same address `0x1a7a4c` in both vtables).
* `IpcTxSetMobileDataSetting` is also shared (`0x1a7b68` in both vtables).
* `IpcTxSetPdpContext` doubles as the **deactivate** message: byte 7 is the
  action.  There is no separate "deactivate" builder.

### 3.2 CP → AP (all dispatched from `IpcRxGprs` @ `0x1baef8`)

`IpcRxGprs` reads `buf[5]` (sub command) and tail-branches through the
protocol vtable (`0x1baef8`: `tbb [pc, ip]` with `ip = sub-3`, table at
`0x1baf10`, 26 entries).  Resolved mapping for IpcProtocol41:

| sub | vtable slot | parser (entry) | meaning |
|---:|---|---|---|
| `0x03` | `+0x93c` | `IpcRxGprsPsState` @ `0x1ba88c` | PS attach state / detach cause |
| `0x04` | `+0x684` | `IpcRxPdpContext` @ `0x1a8700` | PDP context list (response to #1/#2/#3) |
| `0x09` | `+0x67c` | `IpcRxIpConfiguration` @ `0x1ba4ba` | IP configuration (addresses, DNS, MTU) |
| `0x10` | `+0x680` | `IpcRxGprsCallStatus` @ `0x1ba5d0` | data-call state + fail cause |
| `0x16` | `+0x950` | `IpcRxGprsEpdgStatus` @ `0x1bac88` | ePDG/IMS status |
| `0x18` | `+0x964` | `IpcRxGprsPcoStatus` @ `0x1bacc8` | PCO/IMS APN info |
| `0x1a` | `+0x968` | `IpcRxGprsBackoffTimer` @ `0x1ba65c` | PDP retry/back-off timer |
| `0x0c`, `0x0f`, `0x19`, `0x1c` | — | HSDPA status / DUN pin control / LTE CA status / traffic channel | not data-bearer related |

IpcProtocol40 additionally lacks sub 3/0xc/0xf/0x16/0x18/0x19/0x1a.

---

## 4. Byte layouts

Buffer is the full payload starting with the 7-byte `sipc_fmt_hdr`; offsets
below are from the start of that header (`len` at 0, `main` at 4, `sub` at 5,
`type` at 6).  Builders `memset` the whole frame to 0 first, so unused bytes
are zero.

### 4.1 Define PDP context — `13/1/SET`, len 134

`IpcProtocol41::IpcTxDefinePdpContext` @ `0x1b95bc`, disassembly
`0x1b95c0…0x1b969e`:

```
 0  u16 len = 0x86
 2  u8  msg_seq (0 from this builder)
 3  u8  ack_seq (0)
 4  u8  main  = 0x0d
 5  u8  sub   = 0x01
 6  u8  type  = 3 (SET)
 7  u8  count = 1                      ; 0x1b9608/0x1b960c (constant 1)
 8  u8  cid                            ; 0x1b95f6 (arg2)
 9  u8  protocol byte                  ; 0x1b9612…0x1b9620 = table[arg3-2]
10  char apn[100]                      ; strncpy @0x1b963c, max 100, rejects >100
110 u8  0
111 u8  requested_ipv6[16]             ; arg6 != NULL: 16 bytes copied @0x1b9664
127 u8  requested_ipv4[4]              ; arg5 != NULL: 4 bytes  copied @0x1b9676
131 u8  0, 132 u8 0
133 u8  ip_version_indicator           ; only if arg4(bool): arg3∈{2,3} ? arg3 : 1
```

* Argument order (`this` in r0): `r1 = apn`, `r2 = cid`, `r3 = DataProtocol`,
  `[sp+0] = bool`, `[sp+4] = uchar* (IPv4, 4 bytes)`,
  `[sp+8] = uchar* (IPv6, 16 bytes)`.
* The 4-byte/16-byte blocks come from `ContextActivationDca::DefineContext`
  @ `0x15027c`: `DataCall+0x118` (which is `DataLink+0xc`, the IPv4 address)
  and a 16-byte copy of `DataCall+0x1a4` / `DataCall+0x128`
  (`DataLink+0x98` / `DataLink+0x1c`, the IPv6 address).  `[INFERENCE]` from
  the call site plus the `DataLink` field use in `IpcRxIpConfiguration`.
* The protocol byte table is a 3-entry signed-byte table reached from
  `0x1b9612` (`ldr.w lr,[pc,#0x94]` → literal `0x1b96a8` = `0x000804c6`,
  `add lr,pc` at `0x1b9616`).  The exact base resolves to `0x239ae0`
  ([INFERENCE]: ±2 bytes due to Thumb PC rounding) where the three bytes are
  `05 06 04`.  Outside `2..4`, `DataProtocol` is sent as `2`.
* `byte 133` is only written when the 5th argument (bool) is set; on the
  normal `DefineContext` path (`0x15027c`) that bool is
  `0 < *(int *)(DataCall+0x100)`, i.e. a "static IP requested" flag
  `[INFERENCE]`.

**IpcProtocol40 variant** (`0x1a7830`) is byte-identical for offsets 0…130
but writes byte 133 as `2` (`proto∈{2,3}`) or `1` (`proto=4`), instead of
copying the protocol value.

### 4.2 Activate / deactivate PDP — `13/4/SET`, len 112

`IpcProtocol41::IpcTxSetPdpContext` @ `0x1b96ac`, disassembly
`0x1b96c0…0x1b9808`:

```
 0  u16 len = 0x70
 2  u8  msg_seq (0)
 3  u8  ack_seq (0)
 4  u8  main = 0x0d
 5  u8  sub  = 0x04
 6  u8  type = 3 (SET)
 7  u8  action                         ; arg2: 0=deactivate, 1..5=activate flavours
 8  u8  cid                            ; arg1
 9  u32 flags                          ; bit0 = apn string present, bit1 = 2nd string
                                       ; present, bit4 = auth type != 0
13  char apn[32]                       ; arg3, max 32 bytes, copied verbatim
45  char string2[32]                   ; arg4, max 32 bytes
77  u8  zero[32]                       ; never written by this builder
109 u8  auth mapped                    ; table[arg5-1] for arg5∈1..3, else 0
110 u8  0
111 u8  reason mapped                  ; only when action==0:
                                       ;  -9→1, -10→2, -15→3, -18→4, else 0
```

* `action` comes from `ContextActivationDca::ActivateContext` @ `0x150cf8`:
  it maps `DataCall+0xe8` (DataProfile) to `3` (profile 2), `4` (profile
  `0x3e9`), `5` (profile `0x3ea`), else `1`.  `DeactivateContext`
  @ `0x150f74` passes action `0` and `DataCall+0xf8` (fail cause) as the
  reason.
* `ContextActivationDca::DeactivateContext`'s reason is a RIL/PDP fail cause
  (`DataCall+0xf8`), and the builder only encodes four negative values
  (`-9,-10,-15,-18`) in byte 111.
* The two 32-byte strings are `DataCall+0x79` and `DataCall+0xab`
  (`0x150d18` region) — APN/username slots `[INFERENCE]`; authentication is
  `DataCall+0xf4`.
* Flags are written at `0x1b9762…0x1b97c8` as a 32-bit little-endian value.
* **IpcProtocol40 variant** (`0x1a7918`) accepts only actions `0,1,2`;
  anything else returns `-1` without sending.  Its auth table is at
  `0x239b10` (3 signed bytes `01 02 01` at the resolved base).

### 4.3 Get PDP context list — `13/4/GET`, len 7

`0x1a7a4c` (shared): `len=7`, `main=0x0d`, `sub=0x04`, `type=2 (GET)`, no
body.  The response is the same `13/4` PDP context list as §4.9.

### 4.4 PS attach / detach — `13/3/SET`, len 10

`IpcProtocol41::IpcTxPsAttach` @ `0x1b9828`:

```
 0  u16 len = 0x0a
 4  u8  main = 0x0d
 5  u8  sub  = 0x03
 6  u8  type = 3
 7  u8  attach_flag         ; arg2(bool) ? 1 : 0
 8  u8  secondary_flag      ; arg3(bool) ? 1 : 0
 9  u8  reason              ; only when attach_flag == 0: table[arg4-1], arg4∈1..4
```

`IpcModem::PsAttach` @ `0x19a37b` sends `0x517` and forwards
`(uchar arg1, bool arg2, bool arg3, DataDetachReason arg4)` to vtable+0xd0.
`ContextActivationDca::AttachCall` @ `0x1512e8` calls it with
`(cid=DataCall+0x278, 1, 0, reason)`; `DetachCall` @ `0x1513f4` with
`(cid, 0, reason==2, reason)`.  Note: arg1 (the uchar) is **not** encoded by
the IpcProtocol41 builder (only IpcProtocol40 puts it in byte 8).

IpcProtocol40 variant (`0x1a7a82`, len 9): `[7] = arg2?1:0`, `[8] = arg1`
(no attach-flag/reason bytes).

### 4.5 LTE initial-attach profile — `13/0x14/SET`, len 199

`IpcProtocol41::IpcTxSetLteAttachProfile` @ `0x1b9b04` (only exists in 41):

```
 0  u16 len = 0xc7
 4  u8  main = 0x0d
 5  u8  sub  = 0x14
 6  u8  type = 3
 7  u8  1
 8  u8  cid
 9  u8  protocol byte (table[arg7-2], default 2)
10  char apn[100]
110 u8 0
111 u8  requested_ipv6[16]        ; arg9, 16 bytes
127 u8  requested_ipv4[4]         ; arg8, 4 bytes
131 u8 0, 132 u8 0
133 u8 ip_version_indicator      ; if arg6(bool): arg7∈{2,3} ? arg7 : 1
134 char string2[32]              ; from arg3 (3rd pointer arg)
166 char string3[32]              ; from arg4
198 u8  auth mapped              ; table[arg5-1]
```

Caller: `LteDataCallManager::DoSetInitialAttachApn` @ `0x157d59` →
`ContextActivationDca::OnPsAttach` @ `0x151149` (vtable+0x160 with
`(cid, apn, user, pwd, auth, ...)`).

### 4.6 Full data profile — `13/0x1b/SET`, len 203

`IpcProtocol41::IpcTxSetDataProfile` @ `0x1b9894`.  Only entered when
`*(this+4) == 2` (protocol version 2).  Layout from the decompilation
(`0x1b98ae…0x1b9aac`), offsets relative to the frame:

```
 0  u16 len = 0xcb
 4  u8  main = 0x0d
 5  u8  sub  = 0x1b
 6  u8  type = 3
 7  u8  profile action          ; 1 (profile 2), 2 (3), 3 (default/4), 4, 5,
                                ; 0xb (0x3eb), 2 (0x3ec? see table) — see builder
 8..27   APN-type string, 19 bytes max, NUL padded ("profile name")
28  u8  protocol byte (table[proto-2], default 2)
29  char apn[100]
129 char user[31]
160 char password[31]
191 u8  auth mapped
192 u8  ip version indicator
193 u8  flags/…
194 u16, 196 u16, 198 u16      ; three stack arguments (ports/ids)
```

The exact offsets in the tail are `[INFERENCE]`: the builder is 554 bytes and
uses three inline `u16` stack arguments plus a bool; line-by-line offset
recovery was not completed.  The 203-byte length, the `0x0d/0x1b/3` id, the
`profile` byte at 7 and the 19-byte APN-type string at 8 are certain (they
are the first stores in `0x1b98…`).

### 4.7 Static IPv6 configuration — `13/9/SET`, len 24

`IpcProtocol41::IpcTxIpv6Configuration` @ `0x1b9c68`:

```
 0  u16 len = 0x18
 4  u8  main = 0x0d
 5  u8  sub  = 0x09
 6  u8  type = 3
 7  u8  cid                      ; buf[0] of the argument block
 8  u8  ipv6[16]                 ; argument+1
```

### 4.8 Mobile-data setting — `13/0x1d/SET`, len 9

`0x1a7b68` (shared): `len=9`, `main=0x0d`, `sub=0x1d`, `type=3`,
`[7] = arg1(bool)`, `[8] = arg2(bool)`.

### 4.9 PDP context list response — `13/4`, CP→AP

`IpcProtocol41::IpcRxPdpContext` @ `0x1a8700` (also IpcProtocol40's):

```
 0  u16 len
 4  u8  main = 0x0d
 5  u8  sub  = 0x04
 6  u8  type  ; 3 (NOTI) is rejected -> parser returns NULL
 7  u8  count ; number of entries
 8  entry[0] { u8 cid, u8 active }      ; active==1 -> DataCall state 4 (connected)
10  entry[1] { u8 cid, u8 active }
...
```

Each entry is copied into a `DataCall` object:
`DataCall+0xc = active ? 4 : 0`, `DataCall+0x10 = cid`
(`0x1a874a…0x1a8792`).  The response is therefore an **array of (cid,
connected) pairs**, not a per-request echo; the same parser serves define,
activate, deactivate and get-list.

### 4.10 IP configuration — `13/9`, CP→AP

`IpcProtocol41::IpcRxIpConfiguration` @ `0x1ba4ba`.  Rejects `type != 3`
(NOTI only).  Payload (`len` > 0xa4):

```
 7      u8  cid
 8      u16 flags
        bit15 (0x8000) IPv4 block present:
 0x0a   u32 ipv4 address      -> DataLink+0x0c
 0x0e   u32 dns1              -> DataLink+0x14
 0x12   u32 dns2              -> DataLink+0x18
        (DataLink+0x10 = same value as the address; prefix length 0x18 = /24
         stored at DataLink+0xa8)
        bit6 (0x0040) IPv6 block present:
 0x2a   u8[16] ipv6 address   -> DataLink+0x1c   (GenerateIpv6Gateway)
 0x6a   u8[16] ipv6 dns1      -> DataLink+0x2c
 0x7a   u8[16] ipv6 dns2      -> DataLink+0x3c
        bit13 (0x2000) MTU present:
 0x9f   u32 (low 16 bits)     -> DataLink+0xac
        bit14 (0x4000) present:
 0xa3   u8                    -> DataLink+0xae
 always:
 0x1e   u32 -> DataLink+0x5c   (P-CSCF v4 #1)
 0x22   u32 -> DataLink+0x60   (P-CSCF v4 #2)
 0x26   u32 -> DataLink+0x64   (P-CSCF v4 #3)
 0x3a   u8[16] -> DataLink+0x68
 0x4a   u8[16] -> DataLink+0x78
 0x5a   u8[16] -> DataLink+0x88
```

Evidence: `0x1ba4d8…0x1ba55c` (IPv4 + flags), `0x1ba560…0x1ba5c4`
(MTU/0xae and the three 16-byte blocks), and the `DataLink` accessors
`DataLink::HasIpAddress` @ `0x17b27d`, `HasIpv6Address` @ `0x17b29d`,
`HasPcscfAddress` @ `0x17b2e1`.

`[INFERENCE]` field names: `DataLink+0xc/+0x14/+0x18` are used as
address/dns1/dns2 by `LteDataCallManager::SetDnsProperty` @ `0x1575c4`
(`net.%s.dns1`, `net.%s.dns2`), and `DataLink+0xac` is the MTU read by
`DataCallManager::UpdateMtu` @ `0x126ce1`.

IpcProtocol40's parser (`0x1a8088`) is the same shape but stores IPv4 prefix
length 32 (`0x20`) at `DataLink+0xa8` and uses slightly different IPv6 block
offsets (`0x22`, `0x5a`, `0x6a`, `0x7a`).

### 4.11 Data-call state / fail cause — `13/0x10`, CP→AP

`IpcProtocol41::IpcRxGprsCallStatus` @ `0x1ba5d0` (NOTI only):

```
 7     u8  cid                          -> DataCallStatus+0x08
 8     u8  state flag; 1 -> state 4 (connected)  -> DataCallStatus+0x0c
 9     u8  fail cause; only when state != 4 and value < 0x41:
           DataCallStatus+0x10 = cause_table[cause]   (table at 0x239a40,
           65 signed bytes; e.g. 0->0, 1->14, 4->26 … 45->50, 62->-11,
           63->-8, 64->66)
10     u8  flag                         -> DataCallStatus+0x18 (bool)
0x0b   u8                             -> DataCallStatus+0x14
0x0c   u32 (if != 0)                    -> DataCallStatus+0x1c  (retry/back-off)
       if len > 0x10:
0x10   u8                             -> DataCallStatus+0x20
```

Proto40's version (`0x1a81dc`) uses a 48-entry table at `0x243e08` and
`DataCallStatus+0x1c = u16(buf+0xc) * 1000` (milliseconds).

### 4.12 PS attach state — `13/3`, CP→AP

`IpcProtocol41::IpcRxGprsPsState` @ `0x1ba88c`; rejects `type == 2` (RESP):

```
 7  u8 attached          ; 1 = attached -> PsStateInfo+8 (bool)
 8  u8 flag              ; ==0 -> PsStateInfo+9 (bool)
 9  u8 cause             ; 1..7 -> table lookup, else 0  -> PsStateInfo+0xc
```

`GsmDataCallManager::OnPsStateChanged` @ `0x15b811` then compares the
interface name against `"rmnet"` (5 chars) and `"gsm_rmnet"` (9 chars)
(`0x15b82a`, `0x15b898`) and raises `RIL_UNSOL_RESPONSE_PS_NETWORK_STATE_CHANGED`
(`0x2b02`).

### 4.13 Other CP→AP data-related notifications

| sub | parser | payload highlights |
|---:|---|---|
| `0x1a` | `IpcRxGprsBackoffTimer` @ `0x1ba65c` | `[7]` cid, `[8]` APN string (100 B), `[0x6d]` cause (table, ≤2), `[0x6e]` u32 timer |
| `0x18` | `IpcRxGprsPcoStatus` @ `0x1bacc8` | `[7]` len, `[8…]` PCO/IMS APN blob |
| `0x16` | `IpcRxGprsEpdgStatus` @ `0x1bac88` | `[7]` status, `[8]` flag |
| `0x1c` | `IpcRxTrafficChannel` @ `0x1a86c8` | traffic-channel state (`RIL_UNSOL_DATA_CALL_LIST_CHANGED` path) |

---

## 5. Minimum sequence for a default data bearer

Derived from the Dca state machine, not from a hardware trace.  The RIL-side
entry is `ContextActivationDca::OnSetupDataCall` @ `0x150061` (called from
`Dca::HandleEvent`), followed by:

| step | RIL action | IPC message | evidence |
|---:|---|---|---|
| 0 | (context for the APN already stored in `DataCall` by `AllocateCall`) | — | `ContextActivationDca::AllocateCall` @ `0x1505d5` |
| 1 | `DefineContext` | `13/1/SET` len 134 | `0x15027c` → `IpcModem::DefinePdpContext` `0x19a318` → `0x1b95bc` |
| 2 | response `13/4` (list) completes transaction 0x514 | — | `OnDefineContextDone` @ `0x150df4` checks `result==0` |
| 3 | `ActivateContext` (immediately after a successful define) | `13/4/SET` len 112, action=1 | `OnDefineContextDone` → `0x150cf8` → `0x1b96ac` |
| 4 | CP `13/9` IP configuration notification → addresses/DNS/MTU | — | `IpcRxIpConfiguration` `0x1ba4ba`, `Dca::OnIpConfigured` `0x12c909` |
| 5 | CP `13/0x10` data-call state notification → connected/failed | — | `IpcRxGprsCallStatus` `0x1ba5d0`, `ContextActivationDca::OnDataCallStateChanged` `0x151a04` |
| 6 | (optional, LTE) attach profile / PS attach before or between 1 and 3 | `13/0x14/SET` 199 / `13/3/SET` 10 | `OnPsAttach` `0x151149`, `AttachCall` `0x1512e8` |
| 7 | tear-down | `13/4/SET` len 112, action=0 | `DeactivateContext` `0x150f74` |

Notes:
* Steps 1→3 are strictly sequential: `OnDefineContextDone` calls
  `ActivateContext` only when the define transaction returned 0.
* The IP configuration and the connected state arrive asynchronously; a RIL
  must not assume `13/9` precedes `13/0x10` (the parser/state machine keeps
  separate timers: `SetCallStateTimeout` @ `0x150b25`, IPv6 config timer
  @ `0x14fa21`).
* On failure, `OnDataCallStateChanged` maps `DataCallStatus+0x10` through
  `ContextActivationDca::CustomizeFailCause` @ `0x1523f1` and returns a RIL
  `DataCallFailCause`.
* PS attach is normally a separate RIL request
  (`GsmDataCallManager::DoGprsAttchOrDetach` @ `0x15b6c0`,
  `LteDataCallManager::DoAttach` @ `0x156a68`); it is not required before a
  PDP activation on all networks.  Which of the two the framework calls first
  is a policy decision outside the IPC layer.

---

## 6. `rmnet0..7`: who creates and configures what

**Kernel creates the interfaces at probe time.**  Each PDP channel is a DT
io_device of `io_type = IODEV_NET`:

* `arch/arm64/boot/dts/modem-isla-pdata.dtsi:108-172`:
  `rmnet0..3` = ids `10..13`, attrs `0x2`; `rmnet4..7` = ids `14..17`,
  attrs `0x402` (`ATTR_DUALSIM | ATTR_SIPC5`); all `format = <1>`
  (`IPC_RAW`), `links = <0x200>`.
* `drivers/misc/modem_v1/modem_io_device.c:1405-1416`:
  `alloc_netdev(sizeof(struct vnet), iod->name, …)` followed by
  `register_netdev()`, i.e. the netdevice names come from the DT strings.
  The comment at `:1125-1128` describes the Samsung bridge topology
  (`rmnet0` bridge → real `xxxx_rmnet0`) when `iod->use_handover` is set.
* Data travels over the RAW ring (`link_device_memory.h` ring selection;
  `framing.md` §3.4), not the 4 KiB FMT ring, so bulk PS traffic does not
  share the signalling queue.

**Userspace reports the name; it does not create the device.**  The RIL
builds `RIL_Data_Call_Response_v6` in `DataCallRespBuilder::MakeData` @
`0x18896d`:

```
+0x00 status        <- DataCall+0xf8 (0 when state==4/connected)
+0x04 cid           <- DataCall+0xfc
+0x08 field2        <- DataCall+0x10   (the same field ActivateContext logs as
                       "Activate PDP context for CID(%d)"; AOSP names this
                       slot "active" -- see risk 8)
+0x0c protocol      <- 2 (IPV4V6) if DataCall+0x288 != 0, 1 (IPV6) for the
                       connected/IPv6 state, else 0 (IP)
+0x10 type string   <- "IPV6" (0x2139cc) / "IPV4V6" (0x2139d1) / "IP"
+0x14 ifname        <- *(int *)(DataCall+0xe0) + 4   (string object at +0xe0)
+0x18 addresses, +0x1c gateways, +0x20 dnses  (inet_ntop of DataLink fields)
```

The struct has an extra 32-bit protocol enum at `+0x0c` that AOSP's
`RIL_Data_Call_Response_v6` does not have, so offsets after it are shifted
relative to AOSP by 4 bytes.

The interface-name prefix is selected once in `RIL_Init` @ `0x0db300`
(`SecNativeFeature` string copied with `strncpy(..., 0xf)`), and
`GsmDataCallManager::OnPsStateChanged` @ `0x15b811` recognises both `rmnet…`
(`strncmp(...,5)` at `0x15b82a`) and `gsm_rmnet…` (`strncmp(...,9)` at
`0x15b898`).  `[INFERENCE]`: on this device the name is `rmnet<cid>`; the
exact `strncpy` source string was not resolved.

**The RIL can also configure the interface directly.**  `IntfConf`
(`vtable +0x10 = Open`, `+0x14 = sockaddr conversion`) issues classic socket
ioctls, which work on the kernel-created `rmnetN`:

| function | ioctl | effect |
|---|---|---|
| `IntfConf::SetAddress` @ `0x18ec74` | `0x8916` (`SIOCSIFADDR`) | set IPv4 address (retries on `EADDRNOTAVAIL`? errno 0x11 handled) |
| `IntfConf::AddRoutes` @ `0x18e73d` | `0x890b` (`SIOCADDRT`) | add route; flags `RTF_UP|RTF_GATEWAY`, `RTF_HOST` for /32 |
| `IntfConf::SetMtu` @ `0x18e9f0` | `0x8922` (`SIOCSIFMTU`), `0x8921` to read | MTU |
| `IntfConf::Up` @ `0x18ee70` | `SIOCSIFFLAGS` via `SetFlags` @ `0x18eda9` | `IFF_UP` |
| `IntfConf::SetPrefixLength` @ `0x18ea89`, `SetTxQueueLength` @ `0x18eb51` | — | prefix / txqueuelen |

On the normal Android path the framework (netd) still owns interface
configuration; these helpers are the Samsung side channels used for the
attach/IMS/bridge cases.  `ContextActivationDca::OnIpConfigured` @ `0x1539cc`
also sets `net.<apn>.mtu`-style properties and runs
`/system/bin/am broadcast …` (`Am::Execute`) rather than configuring the link
itself for the default bearer.

---

## 7. Open items / risks

1. **Response pairing for `13/1` and `13/3`.** `IpcRxGprs` only dispatches
   subs `3,4,9,0xc,0xf,0x10,0x16,0x18,0x19,0x1a,0x1c`.  A CP reply carrying
   the request's own sub (1 or 3-with-RESP) is not handled by these parsers;
   completion must come from the generic `IpcHistory` transaction matcher
   (framing.md risk R1) or from the `13/4` list.  Which one the CP does is
   unknown without a trace.
2. **Protocol byte tables.** The `DataProtocol`→wire mapping is a signed-byte
   table addressed from `0x1b9612`/`0x1b9bd2`; the resolved base is
   `0x239ae0 = 05 06 04` but Thumb PC rounding permits a ±2-byte alternate
   base, which would change the values.  Verify on hardware by observing the
   byte the CP accepts.
3. **`13/0x1b` (SetDataProfile) tail.** Length and header fields are certain;
   the tail offsets (191-202) are reconstructed from the decompiler and not
   byte-verified.
4. **`ip_version_indicator` (byte 133).** Its semantic (IPv4/IPv6 request
   flag vs. protocol echo) is inferred from the caller's bool.
5. **`DataLink+0x10` in the IPv4 block** receives the same value as the
   address; whether it is a gateway, netmask or a decompiler artifact is
   unresolved.
6. **`ifname` string.** `DataCall+0xe0` is a pointer whose `+4` is the name;
   the object type and the exact `strncpy` source in `RIL_Init` were not
   resolved.  The kernel guarantees `rmnet0..7` exist regardless.
7. **DSDS library.** `libsec-ril-dsds.so` has the same size but a different
   build; none of the addresses here apply to it.
8. **`DataCall+0x10` vs the response "active" slot.** `DataCallRespBuilder::
   MakeData` @ `0x18896d` copies `DataCall+0x10` into response offset 8
   (`0x188994: ldr r2,[r5,#0x10]; str r2,[r4,#8]`), while
   `ContextActivationDca::ActivateContext` @ `0x150cf8` passes the same
   `DataCall+0x10` as the context id to the activation IPC (its log string
   is "Activate PDP context for CID(%d)").  Either the RIL struct slot at
   `+8` is not `active`, or the object layout differs from the decompiler's
   model.  Only a hardware/framework trace can settle it; the IPC messages
   are unaffected.
