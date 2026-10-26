# Telephony userspace for j7elte — CP loader implemented, RIL blocked

**Verdict: partial.**
A CP (baseband) boot loader — the missing piece that keeps the modem powered
down — is implemented, builds for aarch64, and reproduces the stock `cbd` boot
sequence from evidence. **Calls/SMS/data are NOT implemented and cannot be
implemented from open sources alone**: the production RIL path on this device is
Samsung's proprietary binary IPC over `/dev/umts_ipc0`; there is no AT command
channel in normal operation. Details and the exact missing piece are in
[§7](#7-ofono-assessment).

**Exact remaining blocker for calls/SMS/data:** after this loader runs, the CP
is alive but telephony stays down because nothing opens `/dev/umts_ipc0` and
`/dev/umts_rfs0`; the kernel only completes the CP handshake with `INIT_END`
when both are open (`rild_ready()`, `link_device_shmem.c:261`). A RIL must then
speak Samsung's proprietary, undocumented `IpcProtocol` request/response
opcodes over the SIPC5 FMT channel. There is no AT channel to fall back on:
`libsec-ril.so` contains no `AT+` strings, the CP UART is not routed to the AP
(no `umts_calibcmd` iodev, no `check_cpboot` driver) and `/dev/umts_rfs0` is a
remote-filesystem protocol (EFS/NV), not AT. So `ofono`'s `atmodem` has nothing
to connect to, and calls/SMS/data need a from-scratch reimplementation of that
IPC layer — explicitly out of scope here.

No hardware was available: everything below is static analysis of the kernel
sources, the ROM kernel (`boot_out/vmlinux.elf`, `kernel.bin`), the ROM
userspace (`system.raw`, ramdisk) and the CP image, plus cross-compilation and
host-side (native and `qemu-aarch64` user-mode) dry-run validation. Nothing was
executed against real modem hardware.

Kernel side is unchanged and needs no change — `parity/reports/modem.md` already
established P1–P4 parity for `drivers/misc/modem_v1` + `drivers/misc/mcu`.

---

## 1. What the kernel exposes (the ABI the loader must use)

All from the Nura tree; the ROM kernel (`boot_out/vmlinux.elf`) implements the
same ioctls (jump table for `'o'` commands at `0xffffffc00051de24…0x51e594`,
where `IOCTL_MODEM_XMIT_BOOT` is dispatched at `0x51de88/0x51e0b4`).

| Item | Source | Notes |
|---|---|---|
| `IOCTL_MODEM_ON` `_IO('o',0x19)` = 0x6f19 | `modem_prj.h:74` | `ss310ap_on()`: CP power on, `pda_active=1`, `exynos_cp_release()` |
| `IOCTL_MODEM_BOOT_ON` `_IO('o',0x22)` | `modem_prj.h:77` | `ss310ap_boot_on()`: `ld->boot_on`, then waits ≤100×10 ms for `mbx_cp_status != 0`, enables `irq_cp_fail` |
| `IOCTL_MODEM_DL_START` `_IO('o',0x28)` | `modem_prj.h:86` | `shmem_start_download()` (`link_device_shmem.c:1762`) + `mc->ops.modem_boot_confirm()` → `mbx_ap_status = 1` |
| `IOCTL_MODEM_FW_UPDATE` `_IO('o',0x29)` | `modem_prj.h:87` | `shmem_update_firm_info()` (`:1808`) copies `struct std_dload_info` into `mld->img_info` |
| `IOCTL_MODEM_XMIT_BOOT` `_IO('o',0x40)` | `modem_prj.h:103` | `shmem_xmit_boot()` (`:1683`): copies `mf.len` bytes from `mf.binary` to `boot_start + m_offset` when `mf.mode == 0` |
| `IOCTL_MODEM_BOOT_OFF` `_IO('o',0x23)` | `modem_prj.h:78` | `ss310ap_boot_off()`: **blocks** up to `MIF_INIT_TIMEOUT` = 15 s (`modem_ctrl_ss310ap.c:35`) for `mc->init_cmpl`, which `cmd_phone_start_handler()` completes on `CMD_PHONE_START` (`link_device_shmem.c:366`) |
| `IOCTL_MODEM_BOOT_DONE` `_IO('o',0x24)` | `modem_prj.h:79` | `ss310ap_boot_done()`: log only |
| `IOCTL_MODEM_STATUS` `_IO('o',0x27)` | `modem_prj.h:84` | returns `mc->phone_state` |
| `struct modem_firmware` | `modem_prj.h:169` | `__packed`, 28 bytes: `u64 binary; u32 size, m_offset, b_offset, mode, len` |
| `struct std_dload_info` | `include/sipc5.h:250` | `__packed`, 12 bytes: `size, mtu, num_frames` |
| BOOT channel = one SIPC5 frame per `write()` | `misc_write()` `modem_io_device.c:845` → `shmem_send()` `:1524` → `xmit_udl()` `:946` → `udl_write()` `:917` | `umts_boot0` has DT `iod,attrs = <0x02>` = `ATTR_SIPC5` and **not** `ATTR_NO_LINK_HEADER` (bit 8), so `sipc5_init_io_device()` (`modem_io_device.c:1344`) leaves `iod->link_header = true` and `misc_write()` prefixes **every write()** with a 4-byte SIPC5 link header (`cfg=0xF8`, plus `SIPC5_PADDING_EXIST` once `ld->aligned` is set by `IOCTL_MODEM_DL_START`; `ch=0xD7`; `len=4+payload`). `xmit_udl()` then copies that frame verbatim. Userspace owns the UDL *payload* (16-byte header + data), not the wire framing; **one UDL frame must be one `write()`**. `misc_read()` strips the header again (`rx_raw_misc()`, `:281`). |
| Boot region geometry | `shm_ipc.c:40` (`shm_get_boot_size`) + `link_device_shmem.c:2400` | `boot_size = shmem,ipc_offset` = 0x5800000, `boot_start = shmem` base — the 0x1820-byte BOOT image has ample room |
| Crash/state notification | `misc_poll()` `modem_io_device.c:~540` | `POLLHUP` on the BOOT device for crash states — used by `--daemon` |

DT evidence for the BOOT channel: `arch/arm64/boot/dts/modem-isla-pdata.dtsi:181-186`
(`iod,name = "umts_boot0"`, `id = 215`, `format = "IPC_BOOT"`, `attrs = 0x02`);
the ROM DTB carries the same values (`boot_out/dtbs/000_800.dts:4590-4596`),
and the link is declared `mif,link_attrs = <0x7C8>` (`:49`) which includes
`BOOT_ALIGNED`. The kernel's per-write framing is therefore stock behaviour,
not a translation made by this loader.

The `mmc` partitions have **no names in the kernel** (`PARTNAME` exists in
`block/partition-generic.c:232` but nothing sets it for a PIT, and neither the
Nura nor the ROM kernel contains the string `by-name`), so the Android
`/dev/block/platform/…/by-name/RADIO` links come from ueventd, not from the
kernel. The loader therefore probes several paths (see §5).

## 2. CP image format (`parity/firmware/modem.bin`, 34 887 024 B)

Reverse engineered from `cbd` and confirmed by dumping the blob. `cbd` reads
0x80 bytes and `strcmp`s four names (instructions `0x1df3e` read, `0x1df84`
`"TOC"`, `0x1df90` `"BOOT"`, `0x1dfa0` entry 2, `0x1dfbc` `"NV"`).

```
0x00  char  magic[4]      "TOC\0"
0x04  u32   reserved[4]
0x14  u32   data_start    0x200    (first image offset == header size)
0x18  u32   reserved
0x1c  u32   num_images    1
0x20  entry BOOT   id 0    off 0x200     size 0x1820      load 0x40000000  crc 0x24d1e187
0x40  entry MAIN   id 2    off 0x1a20    size 0x02143a38  load 0x40010000  crc 0x124a5825
0x60  entry NV     id 3    off 0          size 0x00100000  load 0x44900000  crc 0
0x80  entry OFFSET id 4    off 0x7aa00    size 0x00085600  load 0         crc 0
```

Entry layout (0x20 B): `char name[12]; u32 offset; u32 load_addr; u32 size;
u32 crc; u32 id;` — the `offset` field (not a computed concatenation) is what
`cbd` seeks to (`0x1e116` loads `[fp+0x58]` = MAIN `offset`, `0x1e12a` stores it
as the stage file offset). The `OFFSET` entry is only consumed by Samsung
factory tools; `cbd` never reads past 0x80 at boot.

`--dump-toc` prints exactly the table above from the real blob.

## 3. Stock boot sequence (evidence)

The SS310 path is `shannon_normal_boot()` (string at `0x1e4d4`, code from
`0x1e3f0`); it calls, in this order (`cbd`, ARM32/Thumb-2):

| # | Call | Instruction | ioctl / action |
|---|---|---|---|
| 0 | `std_boot_prepare_args(dev, 5)` | `0x1de7c` | allocates the 0xc24-byte CP context (`0x1cac4`: `movw r2,#0xc24`, arg 5 = stage count), opens `/dev/umts_boot0` O_RDWR, opens the CP image, reads+validates the TOC, opens `/efs/nv_data.bin` (creating a default one of the TOC NV size), builds the 5 stage descriptors at `ctx+0x20c` stride 0x28 |
| 1 | `std_boot_load_loader(cp, 0)` | `0x1cb70` (called `0x1e4c2`, gated on `boot_link == 9`) | reads BOOT (`size`/`offset` from descriptor 0) and `ioctl(0x6f40, &modem_firmware)` — **before** power-on |
| 2 | `std_boot_modem_on` | `0x1cca8` | `ioctl(0x6f19)` |
| 3 | `std_boot_dload_on` | `0x1cd64` | `ioctl(0x6f22)` |
| 4 | `std_boot_dload_start` | `0x1cd98` | `ioctl(0x6f28)` |
| 5 | `std_boot_dload(cp)` | `0x1cdcc` | the 5-stage frame loop below |
| 6 | `std_boot_dload_off` | `0x1d1f4` | `ioctl(0x6f23)` – waits for `CMD_PHONE_START` |
| 7 | `std_boot_finalize` | `0x1d228` | `ioctl(0x6f24)` |

The stage table built at `0x1e0de…0x1e1cc` (descriptor base `ctx+0x20c`):

| stage | start | data | crc frame | done | payload |
|---|---|---|---|---|---|
| 0 (BOOT) | no | no | (unused) | yes | image already in shmem (step 1) |
| 1 (TOC) | yes | yes | **no** | yes | file[0 .. 0x200), 1 frame |
| 2 (MAIN) | yes | yes | **yes** (`main.crc`) | yes | file[0x1a20 .. +0x2143a38), 17 032 frames |
| 3 (NV) | yes | yes | **no** | yes | local `/efs/nv_data.bin`, 0x100000 B, 512 frames |
| 15 | yes | no | no | no | bare START handshake (`0x9f00`) |

Frame protocol (`std_udl_req_resp` `0x1c628`, `std_udl_poll` `0x1c58c`,
`std_udl_stage_start` `0x1c764`, `std_udl_stage_done` `0x1c71c`,
data path `0x1ce96…0x1d006`):

```
ctrl  : write u32 (stage<<8)|0x9000   -> poll(fd, 30 000 ms) -> read u32, expect (stage<<8)|0xA000
data  : write 16-byte header + payload  header = { (stage<<8)|0x9001, num_frames, seq, len }
        len <= 0x800, seq starts at 1, num_frames = ceil(size/0x800), memset padding to 0
        after the LAST frame, write nothing and wait for (stage<<8)|0xA001
crc   : write { (stage<<8)|0x900C, crc }  -> wait for (stage<<8)|0xA00C
done  : write u32 (stage<<8)|0x900D      -> expect (stage<<8)|0xA00D
before each data stage: ioctl(0x6f29, &{size, mtu=0x810, num_frames})
```

`cbd` reports `mtu = 0x810` (16 + 2048) to the kernel and never reads a
per-frame ack, but it does read two further response words per data stage:
the data-completion ack `0xA001` (after the last frame, no request written)
and the CRC ack `0xA00C` (after the CRC frame). Both are
`std_udl_req_resp(fd, 0, resp)` calls at `0x1d016` and `0x1d0c6` in the
disassembly; a loader that skips them desynchronises the response queue and
the following DONE handshake reads the wrong word.

The commands above are the **payload** passed to `write(2)`/`read(2)`. On the
wire each write is one SIPC5 link frame: `misc_write()` adds a 4-byte header
(`cfg`, `ch = 0xD7`, `len = 4 + payload`) and `xmit_udl()` copies it verbatim
(§1); `misc_read()` strips it again, which is why `cbd` never sees it. Since
the response words (`0xA0xx`) go through the same path, userspace reads the
payload only. After `IOCTL_MODEM_DL_START`, `ld->aligned` is true because of
`LINK_ATTR_BOOT_ALIGNED`, so `cfg` has the padding bit set; every payload used
here is a multiple of 4 bytes, so no padding is added. This is also the reason
the loader issues exactly one `write()` per UDL frame: `misc_write()` turns
each write into one SIPC5 frame (the BOOT iodev has no `iod,ul_buffer_size`,
so `max_tx_size == 0` and no multi-frame chunking happens).

## 4. What was implemented

`parity/userspace/ril/j7elte-cbd.c` (~1 160 lines, C11, POSIX only, no
dependencies beyond libc) + OpenRC init script, `conf.d`, systemd unit.

It performs §3 steps 0–7 for real, including:

* TOC parse/validate and `--dump-toc`;
* BOOT upload with `IOCTL_MODEM_XMIT_BOOT` (28-byte packed ABI, asserted at
  compile time together with the ioctl numbers);
* the full stage table with the exact frame layout above, `ENOSPC`/`EAGAIN`
  retry on the UDL ring, and 30 s response timeouts (same value as `cbd`'s
  `poll(..., 0x7530)`);
* NV from `/efs/nv_data.bin`, or a zero-filled default of the TOC-declared size
  when missing (stock behaviour, loudly warned);
* `--daemon`: after boot, `poll()`s `/dev/umts_boot0` and exits non-zero on
  `POLLHUP` so the service manager can restart the sequence (no ramdump
  collection — stock `cbd` supports it, this does not);
* `--dry-run` / `-n`: prints the complete ioctl + frame sequence without
  touching any device (used for the validation below).

Divergences from stock, each deliberate:

| Stock | Here | Why |
|---|---|---|
| `-P platform/…/by-name/RADIO` only | `-P`, `-C`, or probe `by-partlabel/RADIO`, `by-name/RADIO`, `/dev/mbin0`, `mmcblk0p5`, `/lib/firmware/postmarketos/samsung-j7elte/modem.bin` | the kernel creates no partition names; softlinks are Android-only |
| 32-bit struct passed to `IOCTL_MODEM_XMIT_BOOT` | kernel's `__packed` 28-byte ABI (`u64 binary` at 0, `size` at 8, `m_offset` 0xc, `mode` 0x14, `len` 0x18) | the ROM kernel reads 28 bytes (`mov x2,#0x1c`) and a 64-bit `binary` (`ldr x22,[x29,#0x60]`); see risk R1 |
| ramdump on CP crash | exit + supervisor restart | out of scope, documented |

**Validation performed (no hardware; re-run 2026-10-24):**

* **UAPI review.** Ioctl numbers, `struct modem_firmware` (28 B on aarch64)
  and `struct std_dload_info` (12 B) were re-checked against
  `modem_prj.h:74-176` and `include/sipc5.h:250`; the compile-time asserts
  cover both. The BOOT channel's per-`write()` SIPC5 framing (§1) was traced
  through `misc_write()`/`xmit_udl()` and confirmed against both the Nura and
  the ROM device trees. The loader's one-frame-per-write rule is therefore
  required by the ABI, and its `ENOSPC` retry is safe because `misc_write()`
  returns either the full count or an error and failed writes are not queued.
* **Host build.** `cc -std=c11 -D_GNU_SOURCE -Wall -Wextra
  -Wno-unused-parameter -Werror -O2` — clean.
* **Cross build.** `aarch64-none-linux-gnu-gcc` 10.3.1 (arm-10.29) with the
  same flags plus `-static` — clean;
  `ELF 64-bit LSB executable, ARM aarch64, statically linked` (3 779 392 B,
  debug info not stripped). A dynamic aarch64 link also builds. The static
  binary was additionally executed under `qemu-aarch64` (user-mode emulation,
  still no modem/hardware): `--dump-toc` and `--dry-run -v` produce the same
  output as the host build, so the cross-compiled artifact itself is exercised.
* **Packaging.** `build()` and `package()` from the APKBUILD were executed
  with `CC=<cross gcc> CFLAGS=-O2 -pipe LDFLAGS=-static srcdir=… pkgdir=…`:
  both return 0 and install `/usr/bin/j7elte-cbd`, `/etc/init.d/j7elte-cbd`,
  `/etc/conf.d/j7elte-cbd` and `/usr/lib/systemd/system/j7elte-cbd.service`.
  `sha512sum -c` over the APKBUILD `sha512sums` block passes.
* **Service files.** `sh -n j7elte-cbd.initd` and `bash -n APKBUILD` pass;
  `systemd-analyze verify --root=<staged root>` on the unit exits 0.
* **TOC.** `--dump-toc` on `parity/firmware/modem.bin` prints BOOT/MAIN/NV and
  the factory-only OFFSET entry exactly as §2. (`--dump-toc` now reads the
  extra 0x20-byte OFFSET entry at 0x80 for display only; the boot payload is
  still file[0 .. 0x200), like `cbd`.)
* **Dry run.** `--dry-run -v` end to end: stage-0 DONE, stage-1 START +
  1 TOC frame + `0xA101` ack + DONE, stage-2 START + 17 032 MAIN frames +
  `0xA201` ack + CRC `0x124a5825` + `0xA20C` ack + DONE, stage-3 START +
  512 NV frames + `0xA301` ack + DONE, stage-15 START `0x9f00`, then
  `BOOT_OFF`/`BOOT_DONE`; no device is opened.
* **Patch.** `0001-j7elte-cbd-package.patch` is `git -C parity/pmaports diff
  HEAD -- device/archived/j7elte-cbd`, passes `git apply --check` against a
  pristine extraction of pmaports `ba682bd`, and after applying that patch the
  extracted package is byte-identical to the one in `parity/pmaports` (only
  the untracked build artifact `j7elte-cbd` is excluded). The shared
  `parity/pmaports` repo was not modified by that check.

This pass also fixed two defects found during the review, one of them a
protocol bug:

* **Missing CP acknowledgements.** `cbd` reads a `(stage<<8)|0xA001` word
  after the last data frame of every data stage and a `(stage<<8)|0xA00C` word
  after the CRC frame (`std_udl_req_resp(fd, 0, resp)` at `0x1d016` and
  `0x1d0c6`) — see §3. The loader did not read either, so the response queue
  would have been left with two stale words per stage and the following
  `DONE` handshake would have read `0xA001`/`0xA00C` instead of `0xA00D`.
  Fixed with a `wait_resp()` helper used after the frame loop and after the
  CRC frame.
* **Wrong comment about kernel framing.** The file header claimed the kernel
  adds no SIPC5 framing to the BOOT channel; `misc_write()` does add it
  (BOOT has `ATTR_SIPC5` and not `ATTR_NO_LINK_HEADER`). Comment corrected;
  the code was already correct because it writes payloads only.

Earlier in the same effort the OpenRC script was fixed to not report a
successful non-daemon boot as a failure (`J7ELTE_CBD_DAEMON=no` skips the
liveness check and removes the stale pidfile), and a stale comment mentioning
a non-existent `--nv-create` option was removed.

**Limit of verification:** no aarch64/musl cross toolchain and no `abuild`
are installed in this environment, so the package was not built by `abuild`
itself; the compile above uses the contract's glibc cross compiler and the
APKBUILD shell bodies were run by hand with the same variables. Nothing was
executed against modem hardware.

## 5. Residual risks (what a hardware run must settle)

* **R1 — `IOCTL_MODEM_XMIT_BOOT` argument width.** Stock `cbd` builds an 8-byte
  prefix `{ptr, size}` at `sp+8` and passes `&prefix`, while the 64-bit kernel
  copies 28 bytes (`sizeof(struct modem_firmware)`) and dereferences `binary` as
  64-bit. Either that code path never executes on this device (`boot_link != 9`)
  or stock relies on stale stack contents. This implementation uses the
  kernel's ABI, which is the only self-consistent option; if the CP refuses to
  start, test with `--no-xmit-boot` (and then the bootloader would have to come
  from somewhere else — evidence for that does not exist).
* **R2 — stage 15.** The 5th descriptor is a START-only handshake with stage
  number 15 (`0x9f00`). Its meaning is not evidenced beyond the descriptor
  values; if the CP rejects it, the response word will differ and the loader
  will abort before `BOOT_OFF`.
* **R3 — CRC policy.** CRC frames are sent for stage 2 only, exactly as the
  descriptor flags say (stage 3 has `crc_en = 0` even though the TOC CRC is 0).
* **R4 — NV.** A zero-filled default NV makes the modem boot without IMEI. A
  real `/efs/nv_data.bin` (EFS partition) is required for a usable device; the
  init script mounts EFS when it can find the partition, but the partition
  number is not in the PIT-less postmarketOS tree, so `J7ELTE_EFS_DEV` may have
  to be set by hand.
* **R5 — `IOCTL_MODEM_BOOT_OFF` timing.** It blocks up to 15 s inside the
  kernel. If the CP never sends `CMD_PHONE_START`, the ioctl returns `-EAGAIN`
  and the loader reports failure even though the image transferred fine.
* **R6 — CP acknowledgement words.** The `0xA001`/`0xA00C`/`0xA00D`/`0xA000`
  response words and the 30 s timeout come from the stock `cbd` binary, not
  from a hardware trace. If the CP on this unit emits a different (or no)
  acknowledgement, the loader aborts with a logged `got 0x…, expected 0x…`
  at the offending stage; the frame/CRC data itself has already been written,
  so the point of failure is unambiguous in the log.

## 6. Device nodes after the loader runs

`/dev/umts_boot0` (boot channel, raw), `/dev/umts_ipc0` (SIPC5 FMT — the RIL
control channel), `/dev/umts_rfs0` (SIPC5 RFS — remote EFS file access),
`/dev/umts_ramdump0`, `/dev/smd4`, `/dev/multipdp` + `rmnet0…7` (data),
plus `ipc_loopback0`, `umts_csd`, `umts_router`, `umts_dm0`, `umts_ciq0`
(20 iodevs from the DT list, see `parity/reports/modem.md` P2).

Note `rild_ready()` (`link_device_shmem.c:261`) requires **both** `umts_ipc0`
and `umts_rfs0` to be open before the kernel answers the CP's `PHONE_START`
with `INIT_END`. Without a RIL, the CP retries `PHONE_START` (visible in
`dmesg` as `mif: shmem: CP_START <- …`) and telephony never comes up — even
though the firmware is running.

## 7. ofono assessment

**Blocked: no usable AT command channel exists on this platform, and the
proprietary IPC protocol is the only path.**

Evidence:

* The ROM's `at_distributor` (factory/test AT daemon) talks on
  `/dev/ttySAC2` and `/dev/umts_calibcmd`; `umts_calibcmd` is **not** in this
  device's 20-iodev DT list, and the CP UART is behind a board mux
  (`cbd`: `"Fix CP-UART path to CMC during CP Booting"`, it writes
  `/sys/class/sec/switch/check_cpboot`). Neither the Nura tree nor the ROM
  kernel contains a driver for that node (`grep -r check_cpboot` finds
  nothing; the only `sec_switch_data` users are MUIC headers), and nothing
  consumes `mc->gpio_flm_uart_sel` (`modem_v1.h:415` is dead platform data),
  so userspace cannot route the CP UART to the AP.
* `libsec-ril.so` (2.4 MB, the actual RIL) opens `/dev/umts_ipc0`,
  `/dev/umts_rfs0`, `/dev/umts_boot0`, `/dev/smd0`, `/dev/mbin0` and speaks a
  binary request/response protocol: strings `IpcProtocol`,
  `ProcessIpcMessageReceived`, `IpcRxRfs`, `IpcModem::ProcessIpc regType=%d`,
  and a message enum (`IPC_DATA_SETTINGINFO`, `IPC_IMS_CP_STATE`,
  `IPC_MMB_NVINFO`, …). There is **no `AT+` string in `libsec-ril.so`** — the
  production path never sends AT commands.
* `/dev/umts_rfs0` is a *remote file system* protocol, not AT: the RIL's RFS
  client implements `RFS_OPEN_FILE`, `RFS_READ_FILE`, `RFS_WRITE_FILE`,
  `RFS_NV_READ_ITEM`, `RFS_NV_RESTORE`, `RFS_UNLINK_FILE`, … (strings in
  `libsec-ril.so`). It is used for EFS/NV, exactly like the kernel side's
  `SIPC5_CH_ID_RFS_0` channel.
* Data would use `rmnet0…7` (created by `modem_io_device.c`) but they only carry
  traffic after a PDP context is set up through the same IPC protocol.

What an ofono plugin would need, in order of difficulty:

1. **Transport** — available now: `/dev/umts_ipc0` is a SIPC5 multiplexer;
   the kernel already does the link framing, userspace just reads/writes
   channel frames (4-byte SIPC5 link header + payload, `include/sipc5.h:24-80`).
2. **Samsung IPC opcode layer** — *missing*: the request/response IDs, payload
   layouts and state machines of `libsec-ril.so`'s `IpcProtocol`. This is
   proprietary and undocumented; nothing in the kernel sources or the DT
   describes it. Without it, only the transport is reachable.
3. **RFS client for EFS/NV** — *missing but well-shaped*: the command set is
   enumerable from strings and the framing is the same SIPC5 channel, so a
   reimplementation is feasible; it is not needed for calls/SMS.
4. **AT fallback** — *not available* (see above: no AT in the RIL, no UART mux
   driver, no `umts_calibcmd` iodev).

Consequently ofono's `atmodem` cannot be pointed at anything on this device, and
a `shannon`/`ss310` plugin would be a from-scratch reimplementation of Samsung's
proprietary IPC — a separate, much larger project. What *is* now possible is
bringing the baseband up and observing it (`dmesg | grep mif:`), and exercising
the transport with a small SIPC5 client.

Also worth stating explicitly: `libsamsung-ipc` (used for older Exynos
Samsung phones with XMM modems over a raw IPC device) does **not** cover this
platform — different modem vendor (Shannon), different transport (SIPC5 over
shared memory), different boot protocol.

## 8. Hardware test plan

Prerequisites: `linux-samsung-j7elte` with `CONFIG_SEC_MODEM_V1=y`,
`CONFIG_SEC_MODEM_SS310AP=y`, `CONFIG_LINK_DEVICE_SHMEM=y` (all `=y` in
`config-nura-j7elte:1095,1096,1108`), `j7elte-cbd` installed, RADIO partition
untouched from stock (heimdall-bootimg only writes BOOT).

1. **Probe / nodes**
   `dmesg | grep mif:` → expect `mif: MODEM:ss310ap LINK:shmem` and
   `mif: boot_start=…, boot_size=92274688` (`0x5800000`, from
   `link_device_shmem.c:2402`); `/dev/umts_boot0` must exist.
   `j7elte-cbd --status` → prints `mc->phone_state` (0 = `STATE_OFFLINE`,
   `modem_prj.h:145-146`).
2. **Image discovery**
   `j7elte-cbd --dump-toc` (no `-C`) → must print the TOC from the RADIO
   partition; if it reports "no RADIO partition with a CP image found", the
   partition is not `mmcblk0p5` on this PIT — pass `-P` explicitly.
3. **Dry run on the device**
   `j7elte-cbd -n -v` → prints the same plan as §4 (no device access).
4. **Real boot**
   `rc-service j7elte-cbd start` (or `j7elte-cbd -v -d` in the foreground with
   `-v`), then watch `dmesg`:
   - `mif: umts_boot0: IOCTL_MODEM_ON` (`modem_io_device.c:613`) followed by
     `mif: ss310ap: cp_active:0 cp_status:0` and `change_mr_reset -> 0`,
   - `mif: umts_boot0: IOCTL_MODEM_BOOT_ON` and `cp_status=1` (the CP boot
     ROM is up),
   - `mif: umts_boot0: IOCTL_MODEM_DL_START` and `mif: ss310ap: ap_status=1`,
   - the loader's own stage logs (`stage 2: 17032 frame(s) sent`),
   - `mif: shmem: CP_START <- ss310ap` within 15 s (this is `BOOT_OFF`
     returning), then `CP boot complete`.
5. **Expected failure without a RIL** — after ~15 s, repeated
   `mif: shmem: CP_START <- …` and `mif: shmem: umts_ipc0.opened=0,
   umts_rfs0.opened=0` (from `rild_ready()`), i.e. the CP is alive but never
   gets `INIT_END`. That is the correct, expected end state of this work.
6. **Regression checks** — `cat /sys/class/net/rmnet0/uevent` (netdev exists),
   `ls /dev/umts_*`, `dmesg` free of `copy_from_user fail` / `Invalid args`
   (if R1 is wrong these appear immediately after `XMIT_BOOT`).
7. **Crash path** — `echo 1 > /sys/…` is not needed: kill the loader while the
   CP is booting, or force a CP crash via the kernel's
   `IOCTL_MODEM_FORCE_CRASH_EXIT` (not exposed by this tool) and check that
   `poll()` on `/dev/umts_boot0` returns `POLLHUP` and the service restarts.

## 9. Artifacts

| Path | Content |
|---|---|
| `parity/userspace/ril/j7elte-cbd.c` | the loader (canonical source) |
| `parity/userspace/ril/j7elte-cbd.{initd,confd,service}` | service integration |
| `parity/pmaports/device/archived/j7elte-cbd/` | APKBUILD + copies of the sources |
| `parity/patches/userspace/ril/0001-j7elte-cbd-package.patch` | the package as a `git diff` against pmaports `ba682bd` |
| `parity/reports/ril-userspace.md` | this report |

This change does not modify `device-samsung-j7elte/APKBUILD`,
`linux-samsung-j7elte/config-*` or the kernel tree (other tasks in the tree may
have their own pending edits there), and no `firmware-samsung-j7elte` change is
made — the 33 MB `modem.bin` stays on the RADIO partition rather than being
packaged. The only pmaports additions are the five files under
`device/archived/j7elte-cbd/` contained in the patch.
