# RIL call control — message catalog (spec pass)

**Verdict: implemented (spec + evidence) — calls are now specified at the byte
level; no code and no hardware run in this pass.**

Full catalog: [`parity/userspace/ril/spec/calls.md`](../userspace/ril/spec/calls.md).

## What was established

* Stock binaries re-extracted from `system.raw` and hashed; hashes match
  `parity/userspace/ril/stock/` (`libsec-ril.so`
  `09539011…0a9aa73`, `libsec-ril-dsds.so` `640f0ea0…7cfa0`, `rild`
  `64650e0b…c57f4`). `.dynsym` is intact, so every function below is a symbol,
  not a guess.
* **30 TX messages** byte-exact with builder address, `main/sub/cmd_type`,
  length and field offsets: dial (two variants, 99/101 B), answer (7 B),
  release (7 B), call-list get/set, hold, burst/continuous DTMF, voice privacy
  get/set, flash, emergency-callback mode, call-time and total-call-time
  get/set, E911 status, SRVCC, block status, modify/modify-confirm,
  test call, manage-call, transfer/cancel-transfer, and the **voice-call audio
  path** (`SND_CMD`, 9-byte 41 layout vs 8-byte 40 layout).
* **Emergency dialling** is `CallType 13 → ipc word 0x700` (21-entry s16 table
  at `0x2392f6`, `ConvertCallTypeToIpc` 0x1b5319; the 40-era builder hardcodes
  `CallType 12` instead); the normal voice call is `CallType 1 → 0x100`.
* **RX dispatch**: `IpcRxCall` (0x1a6673) switches on payload byte 5 and routes
  subs 2, 5, 6, 9, 10, 12, 13, 14, 15, 16, 18, 25, 49. Call-status and
  call-list payload layouts were decoded field by field
  (`IpcRxCallStatus` 0x1b6aed, `IpcRxCallList` 0x1b6ba1), including the
  `ConvertToIpcCmd` state-word decoder (0x1b509c).
* **No positive ack**: no call parser acts on `cmd_type == 2 (RESP)`; every
  indication is `NOTI (3)`. A client must drive state from sub 5 / sub 6
  (and sub 2 for MT) with a timeout, not wait for an ack.
* **Minimum sequence** (dial / answer / audio path / release) and the kernel
  preconditions (both `umts_ipc0`+`umts_rfs0` open, `IOCTL_MODEM_BOOT_OFF` to
  reach `STATE_ONLINE`, else `EPERM`) are documented with kernel source lines.

## Honest gaps (marked in the spec)

1. Vtable slot → parser binding is unresolved: `.rel.dyn` is Android packed
   (`ANDROID_REL`/APS2), which GNU binutils does not apply. Sub **numbers** and
   the **parser list** are both evidenced; the association for
   subs 10/12/13/14/15/16/18/25/49 is inference.
2. `CallStatus` field *names* (`+0x5c/+0x60/+0x6c/+0x70`) are not recovered —
   only offsets and sources. Use the raw values.
3. Audio-path enum values and the E911 RAT converter (vtable slot 0x8e4) are
   untraced.
4. Name↔number mapping for subs 20/23/24/48/50 is inferred from adjacent
   strings; the numbers themselves are read from builder immediates.
5. Nothing executed against a CP (no hardware).

No source, package or shared config file was modified by this pass; only
`spec/calls.md` and this report were written.
