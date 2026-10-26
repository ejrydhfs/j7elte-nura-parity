# SIPC5 RIL for j7elte — protocol recovered, transport + query set implemented

**Verdict: partial (transport and the SIM/network/IMEI query subset are
implemented and tested host-side; calls, SMS and data are not).**

This pass did three things:

1. extracted the stock telephony binaries and recorded their identity,
2. recovered the *message layer* that `libsec-ril.so` speaks on
   `/dev/umts_ipc0` — the 7-byte `IpcProtocol` header, the main/sub command
   numbering, the request bytes for the SIM/network/IMEI start-up set, and
   the reply layout for IMEI and registration state, every constant tied to a
   function address,
3. implemented and packaged `j7elte-ril`, a client that opens the kernel
   channels, brings the kernel's modem state to `ONLINE`, sends that request
   set and decodes the answers.

Calls/SMS/data are **not** implemented and no success is claimed for them:
their opcodes are known (they are in the same table) but their payload
layouts and state machines were not recovered. §9 lists exactly what is
missing.

**Not executed against modem hardware.** Verification is: byte-exact frame
vectors against the stock builders, a mock-CP end-to-end run over a socketpair,
a host and an aarch64 cross build, an aarch64 run under `qemu-aarch64`, the
APKBUILD `build()`/`package()` bodies, and a `git apply` check of the exported
patch. §8 has the details.

---

## 1. Stock binaries

Extracted with `debugfs` from `system.raw` (an ext4 image whose root *is*
`/system`, so the paths inside the image are `/bin/rild`, `/lib/…`). All are
ARM EABI5, Android API 23 bionic, dynamically linked against
`/system/bin/linker`, stripped; hashes are SHA-256 of the extracted files under
`parity/userspace/ril/stock/`.

| Path in image | Size | ELF | SHA-256 |
|---|---|---|---|
| `/bin/rild` | 22 072 | ELF32 ARM, EABI5, PIE (`DYN`), interp `/system/bin/linker`, BuildID md5 `c8726626…` | `64650e0b4ab11816539264b351da2d3dd965fd3753dd4f250c9191b0e18c57f4` |
| `/lib/libsec-ril.so` | 2 468 316 | ELF32 ARM, EABI5, `DYN`, Android 23 | `09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73` |
| `/lib/libsec-ril-dsds.so` | 2 468 316 | ELF32 ARM, EABI5, `DYN` (identical size; BuildID `0d15bba0…`) | `640f0ea0a7c6d650d3ede7e5410af72717db1fda35c6f78a6d07bfb78a67cfa0` |
| `/lib/libril.so` | 82 920 | ELF32 ARM, `DYN` | `ad8072ac410f914fa87942aabd58ef71de73fbec1d664429a66b8ff2672b5618` |
| `/lib/libreference-ril.so` | 38 416 | ELF32 ARM, `DYN` | `fc32b63443445fc563fbc5cb10858614ba2f1216d61406f5f355e6f1812c1807` |
| `/lib/librilutils.so` | 17 916 | ELF32 ARM, `DYN` | `066981be45065af9633015cabdcecf54bb726562d7957808cbcf8402a97dc894` |
| `/lib/libsecril-client.so` | 30 208 | ELF32 ARM, `DYN` | `0506f623307ea4dc039164b987b4042de3b829e60fbeffa0a3927b7eea8d8020` |
| `/lib/libaudio-ril.so` | 26 160 | ELF32 ARM, `DYN` | `5e9103221c2f940c63ef5fe1a3e351794ed62cda709ed20b4fde88ba48c1e0fa` |
| `/bin/at_distributor` | 103 812 | ELF32 ARM, `DYN` | `9d01e6167da24cf808fe976842552b9e3f1640a0faa72982c8b4c7d3f9a8435e` |

`/sbin/cbd` is not in `system.raw`; it is in the ramdisk and was already
extracted at `boot_out/rd/sbin/cbd` (handled by `j7elte-cbd`).

Which RIL is used: `boot_out/rd/default.prop:4-5`

```
rild.libpath=/system/lib/libsec-ril.so
rild.libpath2=/system/lib/libsec-ril-dsds.so
```

so `rild` (generic, `libril.so`) `dlopen()`s `libsec-ril.so` and talks to it
through the `RIL_Init`/`RIL_…` C API; `libreference-ril.so` is present but
unused. `libsec-ril.so`'s `DT_NEEDED` set is `libcutils, libril, libnetutils,
libsqlite, libhardware_legacy, libcrypto, librilutils, libxml2,
libsecnativefeature, libc++, libdl, libprotobuf-cpp-full, libc, libm` — i.e.
even if bionic were available it would drag in most of the Android stack, which
is why a native reimplementation is the only route on musl.

Both `libsec-ril{,-dsds}.so` are byte-different but size-identical (different
BuildID) — dual-SIM variant, same code layout for the parts analysed here.

---

## 2. Kernel side: what the ABI actually is

Everything below is from
`android_kernel_samsung_universal7580-…/drivers/misc/modem_v1` (referred to as
`modem_v1/`), which `parity/reports/modem.md` established is at P1–P4 parity
with the ROM kernel.

### 2.1 Device nodes

| Node | `iod,id` | `iod,format` | `iod,attrs` | DT line (`modem-isla-pdata.dtsi`) |
|---|---|---|---|---|
| `/dev/umts_ipc0` | 235 (`SIPC5_CH_ID_FMT_0`) | 0 `IPC_FMT` | `0x2` `ATTR_SIPC5` | :53-59 |
| `/dev/umts_ipc1` | 236 | 0 | `0x402` DUAL_SIM\|SIPC5 | :60-67 |
| `/dev/umts_rfs0` | 245 (`SIPC5_CH_ID_RFS_0`) | 2 `IPC_RFS` | `0x2` | :68-75 |
| `/dev/umts_boot0` | 215 (`SIPC5_CH_ID_BOOT_0`) | 4 `IPC_BOOT` | `0x2` | :180-187 |
| `/dev/umts_ramdump0` | 225 | 5 `IPC_DUMP` | `0x202` | :188-195 |
| `rmnet0…7` | 10…17 | 1 `IPC_RAW` | `0x2`/`0x402` | :108-171 |
| `/dev/smd4`, `/dev/umts_ciq0` | 33, 26 | 1 | `0x802` | :196-211 |

Channel numbering: `modem_v1.h:113-195` (`SIPC5_CH_ID_FMT_0 = 235`,
`…RFS_0 = 245`, `…BOOT_0 = 215`, `…DUMP_0 = 225`; PDP data channels 10–24).

### 2.2 Link layer — owned by the kernel, invisible to userspace

* The SIPC5 link header is `struct sipc5_link_header`
  (`modem_v1/include/sipc5.h:22-30`): `u8 cfg; u8 ch; u16 len;` plus an
  optional control/ext field. `cfg` bit layout at `sipc5.h:32-41`
  (`START_MASK = 0xf8`, `PADDING_EXIST = 0x04`, `EXT_FIELD_EXIST = 0x02`,
  `CTL_FIELD_EXIST = 0x01`, `MULTI_FRAME_CFG = 0x03`).
* **There is no CRC or checksum in the SIPC5 link layer on this platform.**
  Frames are length-delimited (`sipc5_get_frame_len`, `sipc5.h:121-134`) with
  optional 4-byte padding (`sipc5_calc_padding_size`, `sipc5.h:93-97`). The
  only CRC in the whole modem path is the CP-image CRC of the boot stage,
  which `j7elte-cbd` already sends.
* `misc_write()` (`modem_io_device.c:845-975`) prepends **one** link header per
  `write(2)`: `cfg = SIPC5_START_MASK` (plus `PADDING_EXIST` when
  `ld->aligned`), `ch = iod->id` (`sipc5_build_header`,
  `modem_io_device.c:1290-1341`), `len = hdr_len + count`. So for
  `/dev/umts_ipc0` every `write()` becomes exactly one frame with `ch = 0xeb`
  and `len = 4 + count`.
* `ld->aligned` changes state exactly once in the boot, and the FMT channel
  is affected because the flag lives on the shared `link_device`:
  at probe it is set from `LINK_ATTR_IPC_ALIGNED` (`link_device_shmem.c:2295-2300`),
  whose bit (0x2) is clear in `mif,link_attrs = <0x7C8>`
  (`modem-isla-pdata.dtsi:48`), so it starts **false** (`cfg = 0xf8`);
  `shmem_start_download()` — `IOCTL_MODEM_DL_START`, which `cbd` issues —
  then sets it from `LINK_ATTR_BOOT_ALIGNED` (0x100, set)
  (`link_device_shmem.c:1771-1775`), and nothing clears it afterwards.
  **From the first DL_START on, FMT frames are `cfg = 0xfc`
  (`PADDING_EXIST`) and the kernel pads each frame to a multiple of 8 bytes**
  (`misc_write()` tailroom, `modem_io_device.c:892-898`; stripped again on RX
  by `skb_trim()` in `io_dev_recv_skb_single_from_link_dev`,
  `modem_io_device.c:407-410`). This never changes what userspace reads or
  writes — the padding is inside the frame, not part of `len` — but it is why
  a userspace implementation must not try to parse or produce link headers.
* The FMT and RAW rings are shared-memory buffers of fixed size: FMT TX/RX
  `4096` bytes each, RAW TX `2 084 864`, RAW RX `2 097 152`
  (`link_device_memory.h:62-65`), bound by
  `shmem_init_ipc_map()` (`link_device_shmem.c:2134-2196`).
  `dev_id(ch)` (`link_device_memory.h:584-587`) maps only channels 235–244
  (`sipc5_fmt_ch`) to the FMT ring; everything else — including RFS at 245 —
  rides the RAW ring. This is the hard upper bound for one IPC message on
  `umts_ipc0` (~4 KiB minus header).
* `iod->max_tx_size = io_t->ul_buffer_size` (`modem_main.c:162`); the DTS sets
  no `iod,ul_buffer_size`, so it is 0 and `sipc5_build_config()` never selects
  multi-frame mode for AP→CP writes (`modem_io_device.c:1301-1305`).
* On read, `rx_fmt_ipc()` → `rx_frame_with_link_header()`
  (`modem_io_device.c:273-279`, `:257-271`) strips the link header and (for
  multi-frame `cfg`) reassembles per control-field id before queueing; a
  message that still spans several frames is delivered as several `read()`s.
* `misc_read()` (`modem_io_device.c:974-1026`) returns **0, not EOF**, when its
  queue is empty after a 100 ms wait — a client must treat `read() == 0` as
  "nothing yet".
* Registration: `misc_open()` (`modem_io_device.c:487-514`) bumps `iod->opened`
  and calls `ld->init_comm`; `sipc5_init_io_device()`
  (`modem_io_device.c:1343-1360`) sets `link_header = true` unless
  `ATTR_NO_LINK_HEADER` is set.

### 2.3 The handshake that matters

* CP → AP `CMD_PHONE_START` → `cmd_phone_start_handler()`
  (`link_device_shmem.c:322-370`): resets the IPC link, completes
  `mc->init_cmpl`, and **only if `rild_ready(ld)`** sends `CMD_INIT_END`.
* `rild_ready()` (`link_device_shmem.c:261-288`) is true when
  `umts_ipc0.opened > 0 && umts_rfs0.opened > 0`.
* `shmem_init_comm()` (`link_device_shmem.c:1463-1500`) re-sends `INIT_END`
  when the second of the two channels is opened and `cp_boot_done` is still 0.
  **Consequence: opening both devices is the AP half of the handshake.** With
  neither open, the CP keeps repeating `PHONE_START` (`mif: shmem: CP_START <-`)
  and telephony never comes up — this is the state the previous pass left the
  device in.
* `ss310ap_boot_off()` (`modem_ctrl_ss310ap.c:326-352`) is what publishes
  `STATE_ONLINE`: after `wait_for_completion_timeout(&mc->init_cmpl, …)` it
  walks `mc->modem_state_notify_list` and calls `modem_state_changed(ONLINE)`
  **only for io devices with `opened > 0`**; `io_dev_modem_state_changed()`
  (`modem_io_device.c:442-451`) is what writes `mc->phone_state`.
* `misc_write()` rejects every IPC message while the modem is not online:
  `if (!cp_online(mc) && sipc5_ipc_ch(iod->id)) return -EPERM;`
  (`modem_io_device.c:873-877`), and `cp_online()` is
  `mc->phone_state == STATE_ONLINE` (`modem_prj.h:664`).
* The stock RIL's read path polls the same state first:
  `DevIoctlIoChannel::Read` (0x0d9fcc) issues `ioctl(fd, 0x6f27)` and only
  proceeds when the return value is 4; otherwise it reports the state through
  `Modem::NotifyRegistrant(RegistrantType 0x57, …)`.
* `IOCTL_MODEM_STATUS = _IO('o',0x27)` (`modem_prj.h:84`) returns
  `mc->phone_state` as the ioctl *return value*
  (`modem_io_device.c:657-677`); `STATE_ONLINE = 4` (`modem_prj.h:143-151`).

**Therefore a RIL started after `j7elte-cbd` must re-issue
`IOCTL_MODEM_BOOT_OFF = _IO('o',0x23)` on one of its own fds to make the kernel
publish `STATE_ONLINE` to its just-opened channels** — `mc->init_cmpl` is
already complete, so the ioctl returns immediately. `j7elte-ril` does this
(see §6); without it every write fails with `EPERM` even though the baseband is
running. This is a real requirement that neither report had recorded before.

---

## 3. Message layer (recovered from `libsec-ril.so`)

### 3.1 Header

`IpcProtocol::GetIpcMessage` (0x001a2ef4) reassembles messages on the u16 at
offset 0 and clamps messages to 0x2000 bytes. The header is exactly the kernel's
`struct sipc_fmt_hdr` (`modem_v1/modem_prj.h:205-212`), 7 bytes, little-endian:

```
0  u16  len       total length including this header
2  u8   msg_seq   zero in every builder; stamped by the send path (§3.1a)
3  u8   ack_seq   always 0
4  u8   main_cmd
5  u8   sub_cmd
6  u8   cmd_type
```

The builders only set len/main/sub/type and leave `msg_seq`/`ack_seq` zero:
verified on `IpcTxSetDeviceReady` (0x1afa78), `IpcTxGetSapStatus` (0x1ae8a4),
`IpcTxModemPowerOff` (0x1ae6a8) and the shared builder
`Ipc41PacketBuilder::BuildIpcMiscMobileEquipmentSerialNumber` (0x1d3bec), all
of which start from a zeroed stack buffer.

**§3.1a — `msg_seq` is stamped on the way out.** `IpcProtocol40::SetMsgSequence
(char *)` (0x1a42d2) calls `IpcProtocol::GenerateMsgSequence()` (0x1a34fe) and
stores the result in `msg[2]`. `GenerateMsgSequence()` reads the byte at
`this+0x4c`, increments it modulo 255 (`sdiv 255` then re-add, i.e. wrap after
255) and writes it back, so the value is a per-protocol, per-direction 8-bit
counter, not a request id. The method is virtual (the `IpcProtocol41` copy is
0x1a42d3) and is invoked from the send path (`IpcModem::SendMessage`, 0x1a1804,
calls through the protocol object's vtable before handing the buffer to
`DoIoChannelRoutingTx`); the vtable itself lives in REL-relocated
`.data.rel.ro`, so the slot index could not be resolved statically in this
pass. `j7elte-ril` mirrors the behaviour: `ipc_send()` writes
`r->seq++` into byte 2, starting at 0. Nothing in the reply decoders depends
on it — the two handlers traced in §3.5 read only the length and the payload —
so a peer that ignores the byte is still compatible.

### 3.2 `cmd_type` — `IpcProtocol41::GetCmdTypeName` (0x1bb0fc)

| value | name | string address |
|---|---|---|
| 1 | `EXEC` | 0x228727 |
| 2 | `GET` | 0x22872c |
| 3 | `SET` | 0x228730 |
| 4 | `CFRM` | 0x228734 |
| 5 | `EVENT` | 0x228739 |
| other | `UNDEFINED` | 0x22871d |

`GetResponseTypeName` (0x1bb0c8) is the RX-side spelling of the same byte:
1 → (reserved), 2 → `RESP` (0x228713), 3 → `NOTI` (0x228718). The handlers
dispatch on `message[4]` (`IpcProtocol40::GetRxData`, 0x1a4100) and use
`message[6]` to tell a reply from a notification — e.g.
`IpcRxMiscSerialNumber` treats type 3 as "no identity here".

### 3.3 `main_cmd` — `IpcProtocol41::GetMainCommandName` (0x1bb148)

| value | name | value | name |
|---|---|---|---|
| 0x01 | `PWR_CMD` | 0x11 | `GPS_CMD` |
| 0x02 | `CALL_CMD` | 0x13 | `FACTORY_CMD` |
| 0x04 | `SMS_CMD` | 0x15 | `RFS_CMD` |
| 0x06 | `PB_CMD` | 0x17 | `EMBMS_CMD` |
| 0x08 | `NET_CMD` | 0x18 | `MMS_PROVISION_CMD` |
| 0x0a | `MISC_CMD` | 0x1f | `MODEM_TEST_CMD` |
| 0x0c | `SS_CMD` | 0x20 | `DOMESTIC_CMD` |
| 0x0e | `SAT_CMD` | 0x22 | `QMI_HIDDENMENU_CMD` |
| 0x0f | `CFG_CMD` | 0x24 | `PROSE_CMD` |
| 0x10 | `IMEI_CMD` | 0x30 | `JPN_CMD` |
| | | 0x40 | `QMIIMS_CMD` |
| | | 0x70 | `IIL_CMD` |
| | | 0x80 | `GEN_CMD` |

Two main commands are used but not in that switch: 0x05 (`SEC_CMD`, see
`SEC_SUB_CMD_UNDEFINED` at 0x2227f2) and 0x12 (`SAP_CMD`, `SAP_SUB_CMD_UNDEFINED`
at 0x222960). `GetMainCommandName` returns `MAIN_CMD_UNDEFINED` (0x228825) for
them; the sub-command formatter has a separate table per main command, e.g.
`PWR_SUB_CMD_UNDEFINED(0x%x)` at 0x22277b.

### 3.4 Sub-commands used by this implementation

| message | main/sub/type | request bytes | stock builder |
|---|---|---|---|
| `SMS_DEVICE_READY` | 04 / 07 / 3 | `07 00 00 00 04 07 03` | `IpcTxSetDeviceReady` 0x1afa78 (string `SMS_DEVICE_READY` 0x220dc1) |
| `MISC_GET_BASEBAND_VERSION` | 0a / 01 / 2, `ff` | `08 00 00 00 0a 01 02 ff` | `IpcTxBaseBandVersion` 0x1c22b4 → `BuildIpcMiscMobileEquipmentVersion` 0x1d3e60 |
| `MISC_GET_ERI_VERSION` | 0a / 01 / 2, `40` | `08 00 00 00 0a 01 02 40` | `IpcTxGetERIVersion` 0x1c2340 / 0x1ab89c |
| `MISC_GET_SERIAL_NUMBER` | 0a / 03 / 2, kind | `08 00 00 00 0a 03 02 kk` | `IpcTxGetSerialNumber` 0x1c21f4 → builder 0x1d3bec |
| `MISC_GET_TIME_INFO` | 0a / 05 / 2 | `07 00 00 00 0a 05 02` | `IpcTxGetTimeInfo` 0x1c2830 |
| `MISC_GET_DEVICE_CAPA` | 0a / 2d / 2 | `07 00 00 00 0a 2d 02` | `IpcTxGetDeviceCapa` 0x1c5708 |
| `SEC_GET_PIN_STATUS` | 05 / 01 / 2 | `07 00 00 00 05 01 02` | `IpcTxGetPinStatus` 0x1cb844 |
| `SEC_SET_SIM_POWER` | 05 / 0d / 3, `0|1` | `08 00 00 00 05 0d 03 pp` | `IpcTxSetSimPower` 0x1ccb10 |
| `NET_GET_OPERATOR` | 08 / 03 / 2 | `07 00 00 00 08 03 02` | `IpcTxNetGetOperator` 0x1c75b0 (identical in `IpcProtocol40` at 0x1acb30) |
| `NET_GET_VOICE_REG_STATE` | 08 / 05 / 2, `ff 02` | `09 00 00 00 08 05 02 ff 02` | `IpcTxNetGetVoiceRegState` 0x1c74bc |
| `NET_GET_DATA_REG_STATE` | 08 / 05 / 2, `ff 03` | `09 00 00 00 08 05 02 ff 03` | `IpcTxNetGetDataRegState` 0x1c7534 |
| `PWR_RADIO_POWER` on | 01 / 07 / 1, `01 00` | `09 00 00 00 01 07 01 01 00` | `IpcTxRadioPower` 0x1cb350 |
| `PWR_RADIO_POWER` off | 01 / 07 / 1, `02 02` | `09 00 00 00 01 07 01 02 02` | same, `RadioPowerState == 2` branch |
| `PWR_MODEM_POWER_OFF` | 01 / 02 / 1 | `07 00 00 00 01 02 01` | `IpcTxModemPowerOff` 0x1ae6a8 |

`GetSerialNumber`'s kind byte is the `DeviceInfoType` mapped by the shared
builder (0x1d3bec): 1,2 → 1; 4 → 2; 8 → 3; 0x10 → 4; 0x20 → 5; 0x40 → 6;
0x80 → 7. Kind 1 is the IMEI request.

The bytes above are the *builder* output, i.e. with `msg_seq = 0`; on the
wire byte 2 carries the send-path counter described in §3.1a. `IpcProtocol40` and `IpcProtocol41` contain separate copies of these builders;
the ones compared byte-for-byte (`NetGetOperator`, `NetGetVoiceRegState`,
`GetTimeInfo`, `SetDeviceReady`, `ModemPowerOff`) are identical, so the
subset above is protocol-version independent. Version selection:
`IpcModem::CreateProtocol(ModemProtoVer)` 0x198b2c → `IpcProtocol40` only for
`ver == 0`, `IpcProtocol41Iil` for `ver == 8`, `IpcProtocol41` for everything
else; `IpcModem::Init` 0x1a1bd8 passes `ModemConfig[+0x1c]`
(`ModemConfig::Init` 0x0f70bc stores `ModemProtoVer` at +0x1c).

### 3.5 Reply layouts recovered

**IMEI / MEID / ESN** — `IpcProtocol40::IpcRxMiscSerialNumber` 0x1ac220
(the `IpcProtocol41` copy at 0x1c4d6c is a two-instruction forwarder to the
shared implementation at 0x1f4b5c):

```
[6] == 3 (EVENT)      -> no identity in this message
[7] = wire kind, i.e. the DeviceInfoType mapped by the request builder
kind 1 (IMEI)         -> [8] = length, ASCII bytes at [9..9+len)
kind 2 or 7 (MEID)    -> 4 bytes at [9..12],  formatted "%02X" each (0x22ab5a)
kind 3 (ESN)          -> 7 bytes at [10..16], formatted "%02X" each (0x22ab3d)
kind map (0x2383a8)   = {1,4,8,0,0,0,0x80} for kind 1..7
```

The wire kind is the same mapping the request builder applies
(1/2 -> 1, 4 -> 2, 8 -> 3, 16 -> 4, 32 -> 5, 64 -> 6, 128 -> 7), so a reply to
`MISC_GET_SERIAL_NUMBER(1)` (IMEI) carries kind 1 and takes the ASCII path;
the hex-formatted kinds are the MEID/ESN variants, not IMEI.

**Registration state** — `IpcProtocol41::IpcRxNetRegState` 0x1c8bb4 reads
`[7]` (raw stat, 1..0x30), `[8]`, `[9]` (radio tech), `[10]`, u16 `[0x0b]`,
u32 `[0x0d]`, `[0x11]`, u16 `[0x12]`, `[0x14]`, `[0x15]`, `[0x16]`, u16
`[0x17]` and passes them positionally to `NetRegState::NetRegState`. The
constructor's parameter names were not recovered, so `j7elte-ril` prints these
as raw fields instead of guessing (see §6); the message must be ≥ 25 bytes.

**PIN / lock status** — `IpcProtocol41::IpcRxSecPinStatus` 0x1cc4e0:

```
[7] = raw lock type, passed through the 139-entry table at 0x23bcc0
      (index value-1): identity for 1..11, 0 for everything else
[8] = lock key, used directly (the 6-entry table at 0x23bd58 is the identity
      for 1..6 and 0 otherwise)
[6] == EVENT also sets "pin state changed" in IpcSimCache+0x21 (0x1cc504)
```

Both mapped values are what `IpcSimCache::UpdateLockInfo(cache, lock_type,
lock_key)` receives; `j7elte-ril` prints `lock_type`/`lock_key` as raw byte plus
mapped value.

**Operator** — `IpcProtocol41::IpcRxNetIdentity` 0x1c9bf0 carries MCC/MNC,
several encoded name variants (GSM 7-bit `UnPackToGsm7bit`, UCS2
`SwapMChar`+`Unicode2UTF`, raw) and a length-prefixed selector at `[0xb]`;
decoding it needs the full encoding helper chain, which was out of scope for
this pass — the daemon prints it as a hex dump.

---

## 4. Boot / handshake sequence as it now stands

```
cbd (j7elte-cbd)                  kernel                          CP
  IOCTL_MODEM_ON  ───────────────▶ cp power on
  XMIT_BOOT / stages ────────────▶ /dev/umts_boot0 frames ───────▶ boot ROM
  IOCTL_MODEM_BOOT_OFF ──────────▶ wait init_cmpl ◀─────────────── CMD_PHONE_START
                                     │  (INIT_END only if rild_ready)
                                     │
j7elte-ril                        │
  open /dev/umts_ipc0  ───────────▶ misc_open → shmem_init_comm
  open /dev/umts_rfs0  ───────────▶   → CMD_INIT_END ─────────────▶ CP: link up
  ioctl(0x6f27) == 4 ?             │
  ioctl(0x6f23) BOOT_OFF ─────────▶ ss310ap_boot_off → state ONLINE
  07…04 07 03 (DEVICE_READY) ─────▶ misc_write → SIPC5 ch 0xeb ───▶ CP
  GET queries / responses ◀─────── read() ◀─────────────────────── CP
```

Two orderings are possible and both are handled: the RIL may start before the
CP reaches `PHONE_START` (then `init_comm` sends `INIT_END` when the second
device is opened, and `BOOT_OFF` waits inside the kernel), or after (then
`BOOT_OFF` returns immediately and republishes `ONLINE`).

---

## 5. What the daemon does with the transport

* one `write(2)` per message, retrying `EINTR`/`EAGAIN`/`ENOSPC`
  (`IoChannel::Write` 0x0d9ee4 does the same);
* `read(2)` returns the message bytes with the SIPC5 link header already
  stripped; capacity 8 KiB (`GetIpcMessage`'s clamp);
* messages are reassembled on the u16 length, and several messages can arrive
  in one buffer (`rx_feed()`);
* `read() == 0` means "queue empty", not EOF;
* messages are bounded by the 4 KiB FMT ring (`link_device_memory.h:62-65`),
  not by the 8 KiB protocol clamp;
* `POLLHUP` on the FMT device means a CP crash/reset state
  (`misc_poll()`, `modem_io_device.c:536-599`), which the daemon reports and
  turns into a non-zero exit so the service manager restarts it.

---

## 6. Implementation: `j7elte-ril`

`parity/userspace/ril/j7elte-ril.c` (~1 340 lines, C11, POSIX only, libc only),
packaged as `j7elte-ril` with OpenRC + systemd integration.

Commands: `info`, `imei`, `version`, `time`, `operator`, `reg`, `pin`,
`radio on|off`, `sim-power on|off`, `modem-off`, `raw HEX`, `listen`, `state`,
`selftest`, `print-table`.

Design points that come straight from the evidence:

| Behaviour | Why |
|---|---|
| Opens `umts_ipc0` **and** `umts_rfs0`, keeps them open | `rild_ready()` `link_device_shmem.c:261`; `shmem_init_comm()` `:1463` |
| Re-issues `IOCTL_MODEM_BOOT_OFF` when the kernel state is `OFFLINE`/`BOOTING` | `ss310ap_boot_off()` only notifies already-open iodevs (`modem_ctrl_ss310ap.c:341-347`); `misc_write()` is `-EPERM` otherwise (`modem_io_device.c:873`) |
| Waits for `IOCTL_MODEM_STATUS == 4` before the first write | `DevIoctlIoChannel::Read` 0x0d9fcc does the same check |
| Sends `SMS_DEVICE_READY` first, then the MISC/NET/SEC queries | first message the stock SMS manager sends after boot; all bytes from the builders |
| Prints unknown replies as header + hex | avoids inventing field semantics for messages whose layout was not traced |
| `--json` emits one object per inbound message | interface for an oFono plugin or a control script |
| Exits non-zero on `POLLHUP` | CP crash detection used by both stock `cbd` and this service |

`--dry-run` prints every frame it would send; `--print-table` prints the
recovered opcode table with the stock function address of each builder.

---

## 7. Verification performed

All of this was re-run today in this environment; no hardware was involved.

| Check | Result |
|---|---|
| Host build `cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Werror -O2` | clean |
| `j7elte-ril --dry-run` | prints the 14 frames listed in §3.4, byte-identical to the disassembly |
| `j7elte-ril -v selftest` — frame vectors | 14/14 exact matches against the expected byte strings; a mismatch fails the run |
| `j7elte-ril -v selftest` — mock CP over `socketpair` | `MISC/0a/03` request sent, mock `CFRM` reply with `kind 1` (15 ASCII bytes) decoded as `imei: 353438353132370` (`kind 1`, DeviceIdentityType 1); `NET/08/05` request sent, 25-byte reg-state reply decoded field by field; `SEC/05/01` request sent, 9-byte lock-status reply decoded as `lock_type=0x01 (LockType 1) lock_key=0x01 (1)` |
| Cross build `aarch64-none-linux-gnu-gcc` 10.3.1 `-static` | clean; `ELF 64-bit LSB executable, ARM aarch64, statically linked` (4 353 244 B) |
| `qemu-aarch64` run of the cross-built binary: `--dry-run`, `selftest` | identical output, exit 0 |
| APKBUILD `build()` + `package()` run with `CC=<cross gcc> CFLAGS=-O2 -pipe LDFLAGS=-static srcdir=… pkgdir=…` | both return 0; installs `/usr/bin/j7elte-ril`, `/etc/init.d/j7elte-ril`, `/etc/conf.d/j7elte-ril`, `/usr/lib/systemd/system/j7elte-ril.service` |
| `sha512sum -c` over the APKBUILD `sha512sums` block | passes |
| `sh -n j7elte-ril.initd`, `bash -n APKBUILD` | pass |
| `systemd-analyze verify --root=<staged root>` | exits 0 |
| `0002-j7elte-ril-package.patch` | `git apply --check` passes against pmaports `ba682bd`; after applying, the tree is byte-identical to `parity/pmaports/device/archived/j7elte-ril` (the shared repo was restored with `git reset` for that path). The patch was generated from source sha512 `5ef663ec…a8b4f`, the revision that the checks above ran against. |

The mock CP is *not* hardware evidence: it exercises the daemon's framing,
reassembly and decoders, and it produces exactly the frame shapes the stock
handlers expect, but a real SS310AP must still confirm the wire behaviour
(§9, risks).

---

## 8. Kernel changes

None. Nothing in §2 needed a patch: the missing `ONLINE` publication is solved
from userspace with an ioctl the kernel already exposes, and the FMT/RFS
plumbing is complete.

---

## 9. What is still missing (why this is "partial")

The message *numbering* for the remaining domains is fully enumerated in the
library (the name table is printable with `--print-table` for the implemented
subset and readable in §3.3 for the main commands). What is missing is the
payload layouts and state machines:

1. **Calls** — `IpcTxCallOriginate` 0x1b5334 / 0x1a52e0, `IpcTxCallAnswer`
   0x1b58dc, `IpcTxCallRelease` 0x1b5b38, `IpcTxCallGetCallList` 0x1b5568,
   `IpcTxCallSetCallList` 0x1b55d4, `IpcTxCallModify` 0x1b5948,
   `IpcTxCallSendBurstDtmf` 0x1b5cc0, `IpcTxCallFlash` 0x1b5f90 … Each takes
   `CallDetails`/`CallList`/`ClirType`/`UusInfo` arguments whose wire encoding
   (call id width, CLIR enum values, UUS length prefix) was not traced, and
   the RX side (`IpcRxCall…`, e.g. `IpcRxCallSrvcc`, `IpcRxCallCallTime`)
   needs the same treatment plus the call-state machine that maps IPC
   indications to `RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED`.
2. **SMS** — `IpcTxSendSMS`-family (`ModemProxy::SendDeliveryReportWithPdu`
   0x0f7cca / `IpcModem::SendDeliveryReportWithPdu` 0x19c568, `IpcTxWriteSmsToSim` 0x1cd110, `IpcTxSendRPSMMAE` 0x1b2e90),
   plus the PDU/RP framing done in `SmsManager` and the storage bookkeeping
   (`SMS_DEVICE_READY` alone only enables the domain).
3. **Data / PDP** — `IpcTxSetDataProfile` 0x19a410, the `rmnet0…7` netdevs
   exist and are created by `modem_io_device.c`, but a PDP context is
   activated by an IPC request whose layout (`DataProfile`, `DataAuth`,
   `DataProtocol`, IPv4v6 address blobs) was not recovered; the DCA state
   machine (`Dca::OnIpConfigured` 0x44286, `ContextActivationDca`) is the
   other half.
4. **SIM** — `IpcTxGetRSIM`/`IpcTxGetRsimWithAid` (APDU transport),
   `IpcTxGetSimLockInfo`, `IpcTxWriteSmsToSim`, plus the `IpcSimCache`
   invalidation rules. `SEC_GET_PIN_STATUS` and `SEC_SET_SIM_POWER` are
   implemented but the PIN *submission* path (`IpcTxSetPinStatus` 0x1cb974)
   needs the pin-type/enum encoding.
5. **RFS (EFS/NV)** — the channel must be opened, but its file protocol
   (`RFS_OPEN_FILE`, `RFS_READ_FILE`, `RFS_NV_READ_ITEM`, …) is a separate
   request/response layer for `/efs`; it is only needed for NV/EFS access, not
   for registration.
6. **Unsolicited indication catalogue** — the stock RIL registers handlers for
   ~200 `IpcRx…` entry points; only the two layout decoders above were
   recovered. Everything else is currently logged as hex.

Effort estimate for the next slice, in the order that unlocks the most:
registration-state enum mapping + operator-name decoding (small, pure
decoding) → SIM APDU/`SEC` pin submission (medium, unlocks SIM unlock and
phonebook) → call control (medium) → SMS (medium) → data (large, needs the DCA
state machine and `rmnet` wiring).

---

## 10. Mapping to oFono / standalone control

There is still no AT channel on this platform (`parity/reports/ril-userspace.md`
§7 stands: no `umts_calibcmd` iodev, no UART mux driver, no `AT+` strings in
`libsec-ril.so`), so oFono's `atmodem` cannot be used. What `j7elte-ril`
provides is the transport + opcode layer a plugin would sit on:

| oFono atom | current state | would use |
|---|---|---|
| `netreg` | **available**: `NET_GET_OPERATOR`, `NET_GET_{VOICE,DATA}_REG_STATE` + reg-state decoder | `operator`, `reg` |
| `sim` (state/PIN) | **partially available**: `SEC_GET_PIN_STATUS`, `SEC_SET_SIM_POWER`; PIN submit and APDU missing | `pin`, `sim-power` |
| `sim` (IMEI/IMEISV) | **available**: `MISC_GET_SERIAL_NUMBER` + decoder | `imei` |
| `devinfo`/`radio-settings` | **available**: baseband/ERI version, `PWR_RADIO_POWER` | `version`, `radio on/off` |
| `voicecall` | **missing** (§9.1) | — |
| `sms` | **missing** (§9.2) | — |
| `gprs`/`context` | **missing** (§9.3) | — |

Practical integration options, cheapest first:

1. **standalone tool** (implemented): `j7elte-ril -j info` / `listen` gives
   JSON on stdout; an init script or a small `ofono`-external monitor can
   consume it today.
2. **ofono `netreg` plugin**: a plugin that links the same framing code and
   pushes `OFONO_NETREG_…` on `NET_GET_*_REG_STATE` indications; needs the
   reg-state enum table (the 48-entry table at 0x23bb28/0x23bb70 that maps the
   raw stat byte) finished first.
3. **full RIL**: continue the extraction in §9 until `rild`-equivalent
   coverage exists, then either implement `libril`'s socket protocol
   (`RIL_register_socket`, the `rild` control socket) so `ofono`'s `rilmodem`
   could attach, or keep the C API and write an `ofono` `ril`-style plugin.

The daemon deliberately has no Android/ril socket layer: adding one before the
opcode coverage exists would only move the gap.

---

## 11. Risks and limits

* **No hardware run.** Nothing was executed against an SS310AP. Frame bytes are
  proven against the stock binary; the CP's agreement (does it answer
  `04 07 03` with a `CFRM`, does it require the `SET_DEVICE_READY` before any
  other query, is `MISC/01/GET` really the version query on this CP build) is
  not proven.
* **`STATE_ONLINE` republish** (§2.3) is inferred from source; if the ROM
  kernel differs from the Nura tree, `IOCTL_MODEM_BOOT_OFF` may return
  `-EAGAIN`. The daemon logs the ioctl error and keeps waiting, so the failure
  is diagnosable, not silent.
* **Reply correlation.** The stock library keeps per-domain state machines;
  `j7elte-ril` matches replies only by `main`/`sub` and prints everything else.
  A CP that answers out of order is still printed correctly, but the tool
  cannot tell "the answer to query X" from "an indication of the same opcode"
  beyond the type byte.
* **`msg_seq`** is generated by the stock send path (§3.1a) but its exact
  semantics (wrapping counter vs. per-domain sequence, and whether the CP
  validates it) were not established; `j7elte-ril` sends an incrementing
  counter from 0, which matches the recovered generator. `ack_seq` is always
  0 in the send path. Multi-frame SMS is the most likely consumer of these
  bytes and was not reached.
* **Reg-state field names** are not recovered; the decoder prints raw offsets
  rather than mislabeling LAC/CID.
* **Protocol version.** `IpcProtocol41` is the default branch of
  `IpcModem::CreateProtocol`; the configured value lives at
  `ModemConfig+0x1c` and comes from `ModemFactory::LoadModemConfiguration`,
  whose default template (in `.data` at 0x263198) is relocated at load time, so
  the numeric version could not be read statically. The implemented subset is
  identical in the 40 and 41 copies that were compared.
* **Multi-SIM.** `umts_ipc1` (`ATTR_DUALSIM`) and `libsec-ril-dsds.so` were
  not analysed; `j7elte-ril` handles slot 0 only.

---

## 12. Artifacts

| Path | Content |
|---|---|
| `parity/reports/ril-re.md` | this report |
| `parity/userspace/ril/j7elte-ril.c` | daemon/CLI (canonical source) |
| `parity/userspace/ril/j7elte-ril.{initd,confd,service}` | service integration |
| `parity/userspace/ril/stock/` | the extracted stock binaries (§1) |
| `parity/pmaports/device/archived/j7elte-ril/` | APKBUILD + source copies |
| `parity/patches/userspace/ril/0002-j7elte-ril-package.patch` | package as a `git diff` against pmaports `ba682bd` |

Not touched: `device-samsung-j7elte/APKBUILD`, the kernel tree, the shared
`device/archived/linux-samsung-j7elte` config, and the existing
`j7elte-cbd` package.
