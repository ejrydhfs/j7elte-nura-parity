# RIL framing/transport (SIPC5) — spec + codec library

**Verdict: implemented (static evidence only; no hardware run).**
The full SIPC5 framing/transport path for the j7elte SS310AP is documented and
a tested userspace codec/transport library now exists.  No telephony feature
was added: command opcodes belong to the RIL client slice.

* Spec (structures, constants, addresses, pseudo-code):
  [`parity/userspace/ril/spec/framing.md`](../userspace/ril/spec/framing.md)
* Code: [`parity/userspace/ril/sipc5/`](../userspace/ril/sipc5/) —
  `sipc5.h`, `sipc5.c`, `sipc5-probe.c` (selftest/decode/monitor/send/status),
  `Makefile`.

## Findings that matter

1. **Two layers, different owners.**  The SIPC5 *link* header
   (`{cfg,ch,len[,ctrl]}`, `include/sipc5.h:20-30`) is built and stripped by
   `drivers/misc/modem_v1`; userspace only ever writes/reads the IPC payload
   (`modem_io_device.c:845-1030`).  The payload starts with the 7-byte
   `struct sipc_fmt_hdr` (`modem_prj.h:205-212`), confirmed byte-for-byte
   against stock builders (`IpcTxModemPowerOff` @0x1ae6a8 →
   `07 00 00 00 01 02 01`, `IpcTxRadioPower` @0x1ae65e,
   `IpcTxBaseBandVersion` @0x1ab831, `IpcTxGetSapStatus` @0x1ae8a4).
2. **The link device is aligned after boot.**  `ld->aligned` is set true by
   `IOCTL_MODEM_DL_START` (`link_device_shmem.c:1762`, `BOOT_ALIGNED` in
   `link_attrs=0x7C8`) and never cleared on this path, so post-boot FMT/RFS
   frames are `cfg=0xFC` and padded to 8 bytes (`sizeof(size_t)` on arm64).
   Before that they are `cfg=0xF8` with no padding.  `cbd`'s "no padding"
   assumption is wrong for BOOT frames (harmless, CP skips padding).
3. **Transport is the legacy 4 MiB SHM map, not SBD.**  `LINK_ATTR_SBD_IPC`
   is clear, so `umts_ipc0` uses `fmt_tx_buff`/`fmt_rx_buff` (4096 B each) at
   `0x95801000`/`0x95802000` with head/tail byte offsets and the mcu_ipc
   mailbox doorbell; `include/sbd.h` is unused on this device.  RFS (ch 245)
   maps to `dev_id() == IPC_RAW` (`sipc5_fmt_ch()` only covers 235..244), so
   RFS frames ride the 2 MiB RAW ring, not the 4 KiB FMT ring.
4. **Handshake:** the kernel answers the CP's `CMD_PHONE_START` with
   `INIT_END` only when *both* `/dev/umts_ipc0` and `/dev/umts_rfs0` have been
   opened (`rild_ready()`, `shmem_init_comm()`, `link_device_shmem.c:261-285,
   1463-1520`).  Opening the second one triggers it; either order works.
5. **RX is fragment-oriented.**  `misc_read()` returns at most one skb and a
   multi-frame message is queued as several stripped fragments, so a reader
   must reassemble using the `len` field (implemented as `sipc5_asm_feed()`).
6. **`cmd_type` is direction-dependent**: request table EXEC/GET/SET/CFRM/EVENT
   (1..5, `0x228727`), response table INDI/RESP/NOTI (1..3, `0x22870e`).
   `main_cmd` enum (`GetMainCommandName` @0x1a8839) is 1=PWR … 23=EMBMS,
   32=GEN, 48=DOMESTIC, 128=JPN.

## Verification performed

* `cc -O2 -g -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror` — clean.
* `sipc5-probe selftest` — 45 checks, 0 failures: link-header lengths for
  cfg 0xF8/0xFB/0xFA/0xFC, 8-byte padding, multi-frame ctrl byte, extended
  length, malformed start byte, the four byte-exact stock FMT messages,
  FMT round-trip, fragment reassembly and rejection of a bad length.
* `aarch64-none-linux-gnu-gcc 10.3.1 -static` build + `qemu-aarch64`
  selftest — 45/45 pass (the shipped-arch artifact is exercised, no hardware).
* `sipc5-probe decode` verified on both a `cfg=0xF8` and a padded `cfg=0xFC`
  frame carrying `07 00 00 00 01 02 01`.

## Not verified / risks

No CP interaction: sequence numbers (`msg_seq`/`ack_seq` are 0 in every stock
builder), CP-initiated multi-frame behaviour, and the debug/unix-socket channel
are untested.  Addresses are for `libsec-ril.so` build id
`8d7c7e152d01b1d46c79629c4d9b9424` only, not for the DSDS variant.
