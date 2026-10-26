# j7elte packet data / PDP — spec pointer

**Verdict: spec complete for the GPRS (main `0x0d`) data path; no code
implemented here.**

The full catalog, byte layouts, addresses and the minimal default-bearer
sequence are in [`../userspace/ril/spec/data.md`](../userspace/ril/spec/data.md).

What it establishes, all from static analysis of
`/system/lib/libsec-ril.so` (`sha256 09539011…aa73`, extracted from
`system.raw` with `debugfs`):

* The live protocol class on this device is **IpcProtocol41**, not 40:
  `ModemFactory::LoadModemConfiguration` passes `ModemProtoVer = 2`
  (`0x0f7830/0x0f7838` to `ModemConfig::Init` at `0x0f786a`) and
  `IpcModem::CreateProtocol` @ `0x198b2c` maps `2` to `IpcProtocol41`.
* Data signalling uses main command `0x0d` (`GPRS_CMD`) with:
  define `13/1/SET` (134 B, `0x1b95bc`), activate/deactivate `13/4/SET`
  (112 B, `0x1b96ac`, action byte), get list `13/4/GET` (7 B, `0x1a7a4c`),
  PS attach `13/3/SET` (10 B, `0x1b9828`), LTE attach profile `13/0x14/SET`
  (199 B, `0x1b9b04`), IPv6 config `13/9/SET` (24 B, `0x1b9c68`).
* CP→AP: PDP list `13/4` (`0x1a8700`), IP configuration `13/9`
  (`0x1ba4ba`), data-call state + fail cause `13/0x10` (`0x1ba5d0`, cause
  table at `0x239a40`), PS state `13/3` (`0x1ba88c`).
* Minimum default bearer: define → (list response) → activate → IP config
  notification → data-call-state notification; `OnDefineContextDone`
  (`0x150df4`) calls `ActivateContext` (`0x150cf8`) only after a successful
  define.
* `rmnet0..7` are created by the kernel from the DT io_device list
  (`modem-isla-pdata.dtsi:108-172`, `alloc_netdev`/`register_netdev` at
  `modem_io_device.c:1405-1416`); the RIL only reports the name in
  `RIL_Data_Call_Response` (`DataCallRespBuilder::MakeData` @ `0x18896d`) and
  can configure it through `IntfConf` (`SIOCSIFADDR` 0x8916, `SIOCADDRT`
  0x890b, `SIOCSIFMTU` 0x8922).

Open items (protocol-byte table base, `13/0x1b` tail offsets, the
`DataCall+0x10` field ambiguity, ifname source) are listed in the spec.
Nothing was executed against a modem.
