# SIPC5 framing and transport on j7elte (Shannon SS310AP)

Scope: everything between "the CP is running" (after `j7elte-cbd`, see
[`../reports/ril-userspace.md`](../../../reports/ril-userspace.md)) and a RIL
message crossing `/dev/umts_ipc0` / `/dev/umts_rfs0`.  It documents the kernel
link layer (which owns the wire framing), the userspace payload framing used by
the stock `libsec-ril.so`, the byte-level layout of both, and the minimal
handshake that brings the CP to `INIT_END`.

Evidence is static only: kernel sources in this tree, `system.raw` binaries and
the ROM kernel.  Nothing was run against modem hardware.  Addresses are
**file/virtual addresses inside the ARM32 ELF**, not runtime addresses (the
library is a PIE loaded at an arbitrary base).

Companion artifacts (this slice):

| Path | What |
|---|---|
| `parity/userspace/ril/sipc5/sipc5.h` | codec + transport API |
| `parity/userspace/ril/sipc5/sipc5.c` | implementation |
| `parity/userspace/ril/sipc5/sipc5-probe.c` | `selftest`, `decode`, `monitor`, `send`, `status` |
| `parity/reports/ril-framing.md` | short pointer/report |

---

## 1. Extracted artifacts

`system.raw` is the `/system` filesystem itself (root contains `bin/`, `lib/`,
`build.prop`), so the paths inside the image are `/bin/rild`, `/lib/...`.
Extraction used `debugfs -R "dump <path> ..." system.raw`; the hashes below are
of those dumped files and match `parity/userspace/ril/stock/` byte for byte.

| Path in image | Size | SHA-256 | ELF | Build ID |
|---|---:|---|---|---|
| `/system/bin/rild` | 22 072 | `64650e0b4ab11816539264b351da2d3dd965fd3753dd4f250c9191b0e18c57f4` | ELF32 ARM EABI5, ET_DYN (PIE), interpreter `/system/bin/linker` | `c8726626fe82c7c4e7a40ecf749e0ea3` |
| `/system/lib/libril.so` | 82 920 | `ad8072ac410f914fa87942aabd58ef71de73fbec1d664429a66b8ff2672b5618` | ELF32 ARM, ET_DYN | `5e3e23b15f39ef8c1e45604cfc78f227` |
| `/system/lib/libsec-ril.so` | 2 468 316 | `09539011c74683e7781a33e62515ed08cd556ffa6b5067edf29dfa89b0a9aa73` | ELF32 ARM, ET_DYN | `8d7c7e152d01b1d46c79629c4d9b9424` |
| `/system/lib/libsec-ril-dsds.so` | 2 468 316 | `640f0ea0a7c6d650d3ede7e5410af72717db1fda35c6f78a6d07bfb78a67cfa0` | ELF32 ARM, ET_DYN | `0d15bba06f8f222cc30091e40697880d` |
| `/system/lib/libsecril-client.so` | 30 208 | `0506f623307ea4dc039164b987b4042de3b829e60fbeffa0a3927b7eea8d8020` | ELF32 ARM, ET_DYN | `c62956628431b04114b97e23b299233b` |
| `/system/lib/libreference-ril.so` | 38 416 | `fc32b63443445fc563fbc5cb10858614ba2f1216d61406f5f355e6f1812c1807` | ELF32 ARM, ET_DYN | `9bb9fae39555777ea1417c34d6b783f2` |
| `/system/lib/libaudio-ril.so` | 26 160 | `5e9103221c2f940c63ef5fe1a3e351794ed62cda709ed20b4fde88ba48c1e0fa` | ELF32 ARM, ET_DYN | `5b16c17b4344d13275e1c240b9ae2f1f` |
| `/system/lib/librilutils.so` | 17 916 | `066981be45065af9633015cabdcecf54bb726562d7957808cbcf8402a97dc894` | ELF32 ARM, ET_DYN | `bd42e0199c8dedaa1f2c61e7dba83678` |
| `/system/bin/at_distributor` | 103 812 | `9d01e6167da24cf808fe976842552b9e3f1640a0faa72982c8b4c7d3f9a8435e` | ELF32 ARM, ET_DYN | `075a551c0ff1f66443a9a0f3e9800402` |

`libsec-ril.so` and `libsec-ril-dsds.so` have the same size but differ in
2 126 295 bytes: they are two builds of the same source (single-SIM vs DSDS).
`boot_out/rd/default.prop:4-5` selects
`rild.libpath=/system/lib/libsec-ril.so` (this slice analysed that one) and
`rild.libpath2=/system/lib/libsec-ril-dsds.so`.

`libsec-ril.so` is **not stripped**: 9 577 dynamic symbols are present,
including full Itanium C++ mangled names (`IpcModem`, `IpcProtocol40`,
`IpcProtocol41`, `IoChannel`, `SockUnixIoChannel`, `ModemProxy`, ...), functions
are Thumb-2 (symbol values have bit 0 set), and `.ARM.attributes` reports
`ARM v7`, Thumb-2, VFPv4.  That is what makes the frame builders traceable
instruction by instruction.

`/sbin/cbd` (ramdisk: `boot_out/rd/sbin/cbd`, 668a22542ec65abaafd3e7d3441666711ffde1eb016b77717ea6bd15f51a5b08,
ELF32 ARM static, Android 23) is the BOOT-channel user and is already covered
by the cbd report; it is only referenced here for the shared SIPC5 link header.

---

## 2. Stack overview

```
   libsec-ril.so (userspace, bionic)          kernel (drivers/misc/modem_v1)
   --------------------------------          ------------------------------
   IpcProtocol40::IpcTx*()                    misc_write()      misc_read()
     builds 7-byte FMT header + body            |                  ^
   IpcModem::SendMessage()                      v                  |
     -> DoIoChannelRoutingTx()                 builds/strips the 4..6-byte
        -> IoChannel::Write()  --write(2)-->   SIPC5 *link* header
                                               |
                                     +---------+---------+
                                     | FMT circ queue    |  head/tail in
                                     | 4096 B each way   |  4 MiB SHM map
                                     +---------+---------+
                                               |
                                        mailbox doorbell
                                       mcu_ipc@0x104B0000
                                               |
                                              CP
```

Two independent "frames" exist and must not be confused:

1. **SIPC5 link frame** – `{cfg, ch, len[, ctrl|ext_len]}` + payload + optional
   padding.  Exists only on the SHM wire.  Added by `misc_write()`, removed by
   `rxq_read()`/`rx_frame_with_link_header()`.  Userspace never sees it on
   `/dev/umts_ipc0`.
2. **IPC message** – `struct sipc_fmt_hdr` (7 bytes) + command body.  This is
   the userspace payload and is what `libsec-ril.so` builds.

---

## 3. Kernel transport

### 3.1 io devices

From `arch/arm64/boot/dts/modem-isla-pdata.dtsi:14-233` (identical to the ROM
DTB `boot_out/dtbs/000_800.dts:4403-4636`, see `modem.md`), modem `ss310ap`,
`mif,ipc_version = <50>` (SIPC 5.0), `mif,link_name = "shmem"`,
`mif,link_attrs = <0x7C8>`.

`0x7C8` = `LINK_ATTR_DPRAM_MAGIC` (1<<3) | `MEM_BOOT` (1<<6) | `MEM_DUMP`
(1<<7) | `BOOT_ALIGNED` (1<<8) | `DUMP_ALIGNED` (1<<9) | `XMIT_BTDLR` (1<<10)
(`modem_v1.h:229-247`).  **`LINK_ATTR_SBD_IPC` (1<<0) and
`LINK_ATTR_IPC_ALIGNED` (1<<1) are clear**, so this device does *not* use the
MIPI-LLI SBD rings and does *not* pad IPC frames; it uses the "legacy" 4 MiB
SHM map with byte circular queues.

Relevant io devices (`iod,attrs` uses `enum iodev_attr_bit`, `modem_v1.h:249-262`):

| node | id | format | io_type | attrs | meaning |
|---|---:|---|---|---|---|
| `umts_ipc0` | 235 (FMT_0) | 0 IPC_FMT | 0 IODEV_MISC | `0x2` | ATTR_SIPC5 only → link header built by kernel |
| `umts_ipc1` | 236 | IPC_FMT | MISC | `0x402` | ATTR_DUALSIM + SIPC5 |
| `umts_rfs0` | 245 (RFS_0) | 2 IPC_RFS | MISC | `0x2` | remote EFS channel |
| `umts_boot0` | 215 (BOOT_0) | 4 IPC_BOOT | MISC | `0x2` | UDL boot channel |
| `umts_ramdump0` | 225 (DUMP_0) | 5 IPC_DUMP | MISC | `0x202` | ATTR_NO_CHECK_MAXQ + SIPC5 |
| `multipdp` | 0 | 3 IPC_MULTI_RAW | 2 DUMMY | `0x2` | PS demux (net) |
| `rmnet0..7` | 10..17 | 1 IPC_RAW | 1 IODEV_NET | `0x2` / `0x402` | data netdevs |
| `umts_csd`, `umts_router`, `umts_dm0`, `ipc_loopback0` | 1, 25, 28, 244 | RAW | MISC | `0x2` | other misc channels |

The RIL only needs `umts_ipc0` and `umts_rfs0`.

### 3.2 File operations (`modem_io_device.c`)

* `misc_open()` (`:487`) increments `iod->opened` and calls
  `ld->init_comm(ld, iod)` → `shmem_init_comm()` (see 3.5).  There is no
  permission or state check beyond that; opening while the CP is offline is
  allowed.
* `misc_write()` (`:845`):
  - rejects `format <= IPC_RFS && id == 0` (`-EINVAL`) and, for IPC channels
    (`sipc5_ipc_ch(id)`), requires `cp_online(mc)` (`-EPERM`), which is
    `phone_state == STATE_ONLINE` exactly (`modem_prj.h:660-665`).  A write
    before the CP reaches `STATE_ONLINE` fails with `EPERM`; the link layer
    additionally checks `ipc_active()` before queuing.
  - if `iod->link_header` (true here because ATTR_NO_LINK_HEADER is clear),
    computes `cfg` and reserves 4..6 header bytes, copies the user payload,
    then fills `{cfg, ch=iod->id, len=hdr+tx_bytes[, ctrl]}` and appends
    padding if `ld->aligned`.
  - **one `write(2)` is one SIPC5 frame** as long as `iod->max_tx_size` is 0.
    `max_tx_size` comes from the DT property `iod,ul_buffer_size`
    (`modem_main.c:162`), which none of the j7elte nodes declare, so no
    multi-frame chunking ever happens for `umts_ipc0`.
  - returns `count` (never a short count) or a negative errno.
* `misc_read()` (`:974`): waits up to 100 ms for a queued skb, then copies
  `min(skb->len, count)` bytes.  If the user buffer was smaller, the remainder
  stays at the head of the queue and is returned by the next read.  Returns 0
  on timeout.
* `misc_poll()` (`:536`): `POLLIN|POLLRDNORM` when the RX queue is non-empty;
  for `IPC_FMT` returns `POLLHUP` when the CP is in crash/CRASH_RESET/
  NV_REBUILDING/CRASH_WATCHDOG (`:568-583`).  A RIL must treat `POLLHUP` as
  "modem died", not as EOF.

### 3.3 SIPC5 link header

`include/sipc5.h:20-30`, `modem_v1.h:197-200`:

```
offset  size  field
0       1     cfg
1       1     ch
2       2     len        (little-endian; header + payload, padding excluded)
4       1     ctrl       } only if (cfg & 0x02) && (cfg & 0x03) == 0x03
4       2     ext_len    } only if (cfg & 0x02) && (cfg & 0x03) == 0x02
```

`cfg` bits (`include/sipc5.h:29-41`):

| bit | name | meaning |
|---|---|---|
| 7..3 | `SIPC5_START_MASK` | must read `0b11111` (frame start marker) |
| 2 | `SIPC5_PADDING_EXIST` | padding present; length padded to 8 bytes |
| 1 | `SIPC5_EXT_FIELD_EXIST` | ctrl/ext_len field present |
| 1..0 | `SIPC5_EXT_FIELD_MASK` | `0b11` = multi-frame (5-byte header), `0b10` = extended 32-bit length (6-byte header), else 4-byte header |

Header length (`sipc5_get_hdr_len`): 4 normally, 5 for multi-frame, 6 for
extended length.  This device only emits 4-byte headers: `sipc5_build_config()`
(`modem_io_device.c:1290-1313`) sets `0xF8`, adds `0x04` when `ld->aligned`,
and adds `0x03` only when `max_tx_size > 0` and the payload would exceed it;
the extended-length branch is `#if 0`.

Multi-frame control byte (`modem_v1.h:197`, `modem_io_device.c:1330-1341`):
bit 7 = `more`, bits 6..0 = reassembly `id` (`info_id`, incremented per frame).
Header length fields:

```
frame_len = len field                       (excludes padding)
pad       = (cfg & 0x04) ? (8 - frame_len % 8) % 8 : 0   // sizeof(size_t) == 8 on arm64
total_len = frame_len + pad
```

The kernel's `sipc5_calc_padding_size()` uses `sizeof(size_t)`; on this arm64
kernel the granularity is **8 bytes**, not 4.

`ld->aligned` is a single flag on the shared `shmem` link device, and its
timeline on j7elte is:

* `create_sbd_link_device()`/`shmem_create_link_device()` (`:2298`) sets it
  only if `LINK_ATTR_IPC_ALIGNED` is present — it is **not** (3.1), so it
  starts false;
* `shmem_start_download()` (`:1762`, called from `IOCTL_MODEM_DL_START`)
  sets it **true** because `LINK_ATTR_BOOT_ALIGNED` is present;
* nothing on this code path clears it again (`ld->aligned = false` appears
  only in the SBD/IOSM init handlers and in the `else` branches of the same
  download/upload functions, none of which run here).

Therefore, **after `cbd` has issued `IOCTL_MODEM_DL_START`, every FMT/RFS
frame the kernel builds for `umts_ipc0`/`umts_rfs0` has `cfg = 0xFC`
(`SIPC5_PADDING_EXIST`) and is padded to a multiple of 8 bytes**.  Before
`DL_START` (e.g. a RIL started against a manually powered CP) the same frames
would use `cfg = 0xF8` and no padding.  A decoder must accept both, and a
capture taken during normal operation will show the padding bit set.  RX is
tolerant either way: `io_dev_recv_skb_single_from_link_dev`
(`modem_io_device.c:399-416`) trims to `sipc5_get_frame_len()` when
`lnk_hdr && ld->aligned`.

`rxq_read()` (`link_device_shmem.c:1057-1120`) validates the received frame:
`cfg` start bits must be `0xF8`, `sipc5_get_total_len()` must be ≤ the ring
occupancy, then it copies `total_len` bytes and advances the ring tail.  A bad
header forces a CP crash (`shmem_forced_cp_crash`), so a CP that emits one bad
frame is treated as fatal — there is no resynchronisation.

### 3.4 Shared-memory ring transport

`shmem@0x90000000` (`samsung,exynos7580-shm_ipc`):
`reg = <0 0x90000000 0 0x05c00000>`, `shmem,ipc_offset = <0x5800000>`,
`shmem,ipc_size = <0x400000>`.  Therefore:

* boot region: `0x90000000 .. 0x95800000` (CP boot image, `cbd` writes here);
* IPC region: `0x95800000 .. 0x95C00000` (4 MiB), mapped by
  `shmem_setup_ipc_map()` (`link_device_shmem.c:2125-2190`) as
  `struct shmem_4mb_phys_map` (`link_device_memory.h:59-83`).

Map offsets, relative to `0x95800000`:

| off | field | notes |
|---:|---|---|
| 0x000 | `magic` | `MEM_IPC_MAGIC` = `0xAA` |
| 0x004 | `access` | must be 1 (`ipc_active()`, `link_device_shmem.c:85-120`) |
| 0x008 | `fmt_tx_head` | byte offset into `fmt_tx_buff` |
| 0x00c | `fmt_tx_tail` | written by CP |
| 0x010 | `fmt_rx_head` | written by CP |
| 0x014 | `fmt_rx_tail` | written by AP |
| 0x018 | `raw_tx_head` | |
| 0x01c | `raw_tx_tail` | |
| 0x020 | `raw_rx_head` | |
| 0x024 | `raw_rx_tail` | |
| 0x028 | `reserved[4056]` | |
| 0x1000 | `fmt_tx_buff[4096]` | AP→CP IPC (FMT and RFS frames) |
| 0x2000 | `fmt_rx_buff[4096]` | CP→AP IPC |
| 0x3000 | `raw_tx_buff[2084864]` | AP→CP PS data |
| 0x200000 | `raw_rx_buff[2097152]` | CP→AP PS data |

Which of the two rings is used is decided by `dev_id(ch)`
(`link_device_memory.h:584`): `sipc5_fmt_ch(ch)` is true only for 235..244, so

* FMT channels (235..244, including `umts_ipc0`) use the **FMT ring**
  (4 KiB per direction) — this is the hard upper bound on an FMT payload;
* every other IPC channel, **including RFS (245..254)**, maps to `IPC_RAW` and
  is therefore carried in the **RAW ring** (2 MiB per direction, shared with
  PS data and `multipdp` traffic).

The destination channel is always the `ch` byte in the link header, and the
receiver routes on it (`rx_frames_from_dev()` → `link_get_iod_with_channel`).
So an RFS message is not limited to 4 KiB, but it competes for space with
uplink PS traffic in `raw_tx_buff`.

`circ_queue.h` defines the ring primitives: head/tail are **monotonic byte
offsets modulo 2^32** interpreted against `size`; writes wrap with
`circ_write()`; usage = `in - out`.  `txq_write()` (`link_device_shmem.c:473`)
checks space, copies, then publishes with `set_txq_head()` after an `smp_mb()`.

Doorbell (`link_device_memory.h:88-128, 417-449`, `mcu_ipc@0x104B0000`,
SPI/AP interrupt 0x17f = 383):

| value | meaning |
|---|---|
| `0x0080` | `MASK_INT_VALID` – interrupt word valid |
| `0x0040` | `MASK_CMD_VALID` – low 6 bits are a command |
| `0x003f` | `MASK_CMD_FIELD` |
| `0x0020` / `0x0010` | `MASK_REQ_ACK_FMT` / `MASK_REQ_ACK_RAW` |
| `0x0008` / `0x0004` | `MASK_RES_ACK_FMT` / `MASK_RES_ACK_RAW` |
| `0x0002` / `0x0001` | `MASK_SEND_FMT` / `MASK_SEND_RAW` |

Commands (`link_device_memory.h:113-126`): `INIT_START=1`, `INIT_END=2`,
`REQ_ACTIVE=3`, `RES_ACTIVE=4`, `REQ_TIME_SYNC=5`, `CRASH_RESET=7`,
`PHONE_START=8`, `CRASH_EXIT=9`, `CP_DEEP_SLEEP=0xA`, `NV_REBUILDING=0xB`,
`EMER_DOWN=0xC`, `PIF_INIT_DONE=0xD`, `SILENT_NV_REBUILD=0xE`,
`NORMAL_POWER_OFF=0xF`.

TX: `misc_write → shmem_send → xmit_ipc → xmit_ipc_to_dev` queues the skb and
starts `tx_timer`; `tx_frames_to_dev()` (`:503`) drains the queue into
`fmt_tx_buff` and the timer raises `MASK_SEND_FMT|MASK_INT_VALID`
(`:568-620`).  RX: the CP raises `MASK_SEND_FMT`; `shmem_irq_handler` →
`shmem_rx_task` → `ipc_rx_func` → `recv_ipc_frames` → `rx_frames_from_dev`
reads whole frames from `fmt_rx_buff`, demuxes on `ch`, and (for an ACK
request) sends `MASK_RES_ACK_FMT`.

Because `ld->sbd_ipc == false`, none of the SBD/MIPI-LLI machinery
(`include/sbd.h`, `link_device_memory_sbd.c`) is used on this device.  It is
present in the tree for other modems and is a red herring for j7elte.

### 3.5 Boot/CP handshake

`shmem_init_comm()` (`link_device_shmem.c:1463-1520`) runs on every
`open()` of an FMT/RFS channel:

* Early return if `cp_boot_done` is already 1.
* FMT (`ch 235..244`): look up the matching RFS id (`ch + 10`).  If that
  channel is already open → `send_ipc_irq(CMD_INIT_END)` and set
  `cp_boot_done`.
* RFS (`ch 245..254`): same check against `ch - 10`.

`cmd_phone_start_handler()` (`:319-368`) runs when the CP sends
`CMD_PHONE_START`:
if `rild_ready(ld)` (both `fmt_iod->opened > 0` **and** `rfs_iod->opened > 0`,
`:261-285`) it answers `CMD_INIT_END`; otherwise it just moves the link to
`LINK_STATE_IPC` and completes `mc->init_cmpl`.  The CP re-sends
`CMD_PHONE_START` periodically until it sees `INIT_END`, which is the dmesg
`CP_START <-` retry loop.

Consequences for a RIL:

1. Open **both** `umts_ipc0` and `umts_rfs0`.  Any order works; the second
   open triggers `INIT_END`.  Opening only the FMT channel leaves the CP
   retrying forever.
2. Do not close either fd while running: `misc_release()` decrements `opened`
   and purges the RX queue.
3. `IOCTL_MODEM_BOOT_OFF` (used by `cbd`) blocks until the CP sends
   `PHONE_START`, so it is legitimate for `cbd` to return before a RIL has
   opened anything.

### 3.6 Ioctls a RIL may use

`_IO('o', n)` = `0x6f00 | n` on ARM (`modem_prj.h:74-113`):

| ioctl | value | effect |
|---|---:|---|
| `IOCTL_MODEM_STATUS` | `0x6f27` | returns `mc->phone_state` |
| `IOCTL_MODEM_FORCE_CRASH_EXIT` | `0x6f34` | forces CP crash/ramdump state |
| `IOCTL_MODEM_DL_START` | `0x6f28` | BOOT only (cbd) |
| `IOCTL_MODEM_XMIT_BOOT` | `0x6f40` | BOOT only (cbd) |
| `IOCTL_MODEM_GET_SHMEM_INFO` | `0x6f41` | shared-memory geometry |
| `IOCTL_MODEM_RAMDUMP_START/STOP` | `0x6fce/0x6fcf` | ramdump |

The stock RIL itself only uses `0x6f34`: `DoModemCrash()` @ `0x197da8`
opens `/dev/umts_ipc0` (`open(path, 2)`, path xref at `0x197dd6` → string
`0x209072`), calls `ioctl(fd, 0x6f34)` and closes again.  No other ioctl is
issued on the IPC paths by `libsec-ril.so`.

---

## 4. Stock userspace (`libsec-ril.so`)

### 4.1 `IoChannel` – the transport object

Constructor `IoChannel::IoChannel` @ `0xd9c56` area; the layout inferred from
`IoChannel::Init` @ `0xd9c56` (Ghidra: "IoChannel::Init(char const*, Modem*)")
and `IoChannel::Open` @ `0xda13c`:

```
+0x00  vtable
+0x04  int fd                  (-1 before open; Open stores the result here)
+0x08  char *path              (strdup'ed by Init)
+0x0c  Modem *owner
```

`IoChannel::Init(path, modem)` (`0xd9c56`): `operator_delete(this->path)`,
`strlen`, `operator_new`, `strcpy`, store `modem` at +0xc, return 0; a NULL
path returns -1.

`IoChannel::Open()` (`@0xda13c`, Thumb): loops up to 0x33 = 51 attempts:
`fd = open(this->path, O_RDWR)` (PLT stub at `0xda148`, mode 2), storing the
result at `this+4`; on failure it sleeps and retries; success returns 0.

`IoChannel::Read(buf, len)` (`@0xd9e2c`):
```
if (!buf || this->fd < 0) return -1;
for (;;) {
    n = read(this->fd, buf, len);
    if (n >= 0) break;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(10000); continue;
    }
    return -1;
}
return n;
```
(asm: `0xd9e78` read, `0xd9e70` `usleep(10000)`, comparisons against 4 and 11.)

`IoChannel::Write(buf, len)` (`@0xd9ee4`):
```
if (this->fd < 0) return -1;
done = 0;
while (done < len) {
    n = write(this->fd, buf + done, len - done);   // 0xd9f5a/0xd9f60
    if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(10000); continue; }
        return -1;
    }
    done += n;
}
return 0;
```
Note the mismatch with the kernel: `misc_write()` accepts the whole buffer at
once, so the stock loop is defensive; a partial write would produce a second
frame, which is why `sipc5_write_msg()` in this slice treats a short write as
an error.

Virtual method order (derived from call sites, not from a resolved vtable —
the vtable slots are filled by `R_ARM_RELATIVE` relocations at load time and
read as zero in the file):

| vtable off | method | call sites |
|---:|---|---|
| +0x08 | `Init(char const*, Modem*)` | `Modem::Init` @ `0x102166`/`0x1021a2` |
| +0x0c | `Open()` | `Modem::OpenIoChannel` @ `0x102364`, `OpenRfsIoChannel` @ `0x10239c` |
| +0x10 | `Close()` | `Modem::CloseIoChannel` @ `0x102374` |
| +0x14 | `Read(char*, int)` (inferred: the only free slot between `Close` and `Write`, and every subclass overrides `Read`) | `IoChannelReader` thread body (not symbolised) |
| +0x18 | `Write(char*, int)` | `DoIoChannelRoutingTx` @ `0x198bb4`/`0x198c00`, `Modem::SendRfsMessage` @ `0x1020d4` |
| +0x1c | `Write(char*, int, int)` | `IpcModem::SendMessage` @ `0x1a1848` |
| +0x20 | unnamed virtual used by the RX/debug path | `DoIoChannelRoutingRx` @ `0x1a1684` |

`Modem` object fields used by the transport (`Modem::Init` @ `0x10212c`):

```
+0x1c  IoChannel *fmt     (path = ModemConfig+0x0c, typically "/dev/umts_ipc0")
+0x20  IoChannel *debug   (only if ModemConfig::HasDebugInterface(), path = config+0x10)
+0x24  IoChannel *rfs     (created by Modem::CreateRfsIoChannel, path from Nv's config,
                           "/dev/umts_rfs0")
+0x2c  Nv *
```

`IpcModem` adds `+0x34 ModemProxy *` (set post-construction),
`+0x38 IpcHistory *`, `+0x3c IoChannelReader *`, `+0x40 IpcDump *`,
`+0x44 protocol selector` (constructor `IpcModem::IpcModem` @ `0x198aa0`).

Channel class selection (`IpcModem::CreateIoChannel` @ `0x198a18`) switches on
`ModemConfig+0x8`:

| value | class |
|---:|---|
| 0, default | base `IoChannel` (direct `open()` of the device node) |
| 1, 0xb | `IoChannel` with an alternate vtable (specialised device) |
| 2 | `SockInetIoChannel` |
| 3 | `DevIoctlIoChannel` |
| 4 | `SockUnixIoChannel` (unix socket, see `IPCDEBUG_UNIX_SOCKET` @ `0x209081`) |

For the SS310AP configuration used on j7elte the FMT channel is a base
`IoChannel` opened directly on `/dev/umts_ipc0` — corroborated by the two
device-path strings at `0x209072` (`/dev/umts_ipc0`) and `0x209096`
(`/dev/umts_rfs0`) being fed into `ModemConfig::Init` from
`ModemFactory::LoadModemConfiguration` @ `0x0f754c` (xrefs at `0x0f7868`,
`0x0f784e`), and by `DoModemCrash()` opening the very same node.

### 4.2 FMT message header (userspace payload)

`modem_prj.h:205-212`:

```c
struct __packed sipc_fmt_hdr {
    uint16_t len;      /* total incl. this header */
    uint8_t  msg_seq;
    uint8_t  ack_seq;
    uint8_t  main_cmd;
    uint8_t  sub_cmd;
    uint8_t  cmd_type;
};                     /* 7 bytes */
```

Confirmed byte-for-byte from four stock builders (all Thumb-2, stack buffer):

`IpcProtocol40::IpcTxModemPowerOff()` @ `0x1ae6a8` (52 B):
```
memset(sp, 0, 7)                 @ 0x1ae6b4 (memset via PLT)
strh  #7        -> [sp+0]        @ 0x1ae6be
strb  #1        -> [sp+4]        @ 0x1ae6c8   main_cmd  = 1  (PWR_CMD)
strb  #2        -> [sp+5]        @ 0x1ae6c6   sub_cmd   = 2  (PWR_OFF)
strb  #1        -> [sp+6]        @ 0x1ae6cc   cmd_type  = 1  (EXEC)
-> 07 00 00 00 01 02 01          len 7, no body
```

`IpcProtocol40::IpcTxRadioPower(RadioPowerState)` @ `0x1ae65e` (74 B):
```
str  #0 -> [sp+0]  (clears len/seq/ack)   @ 0x1ae668
strh #8 -> [sp+0]                         @ 0x1ae670
strb #1 -> [sp+4]                         @ 0x1ae674   main = 1 PWR
strb #7 -> [sp+5]                         @ 0x1ae678   sub  = 7
strb #1 -> [sp+6]                         @ 0x1ae67c   type = EXEC
strb state (3 for ON, 4 for OFF) -> [sp+7] @ 0x1ae68e
-> 08 00 00 00 01 07 01 <state>          len 8, body 1 byte
```

`IpcProtocol40::IpcTxBaseBandVersion()` @ `0x1ab831` (108 B):
```
07..08 bytes: [0..1]=8, [4]=0x0A (MISC_CMD), [5]=1, [6]=2 (GET), [7]=0xFF
-> 08 00 00 00 0a 01 02 ff
```

`IpcProtocol40::IpcTxGetSapStatus()` @ `0x1ae8a4` (52 B):
```
[0..1]=7, [4]=0x12 (SAP_CMD), [5]=2, [6]=2 (GET)
-> 07 00 00 00 12 02 02
```

RX confirms the same offsets: `IpcProtocol40::IpcRxGetSapStatus` @ `0x1aea80`
tests `buf[6]` against 2 and 3 and reads the first body byte at `buf[7]`;
`IpcProtocol40::IpcRxGeneral` @ `0x1ab69d` loads a u16 at `buf+9` (= body
offset 2).  `IpcProtocol40::IpcTxRawIpc` @ `0x1a4341` verifies
`*(u16*)buf == len_arg` before handing the buffer to the channel writer, i.e.
the length field is authoritative and must match the write size.

`msg_seq`/`ack_seq` are left 0 by every studied builder; `IpcModem::SendMessage`
@ `0x1a1804` then calls (a) `ModemProxy` vtable +0x08 and (b) +0x30 with the
protocol selector `this+0x44`, before `DoIoChannelRouting`.  The proxies are
the transaction-history layer (`IpcHistory`), not the device writer.  Nothing
in the traced path rewrites bytes 2/3, so a from-scratch RIL may send seq=0;
whether the CP cares is a hardware question (risk R2 below).

`cmd_type` is direction-dependent; the RIL names it from two tables:

* request (AP→CP) names at `0x228727`: `EXEC=1, GET=2, SET=3, CFRM=4, EVENT=5`
  (`IpcProtocol40::GetCmdTypeName` @ `0x1a87ed`);
* response (CP→AP) names at `0x22870e`: `INDI=1, RESP=2, NOTI=3`
  (`IpcProtocol40::GetResponseTypeName` @ `0x1a87b9`).

So one numeric value has two meanings depending on direction; a decoder must
know which side produced the frame.

`main_cmd` is resolved by `IpcProtocol40::GetMainCommandName` @ `0x1a8839`
(a switch on the byte; the name strings start at `0x22873f`).  Verified
mapping from the branch targets:

```
1 PWR, 2 CALL, 3 CDMA_DATA, 4 SMS, 5 SEC, 6 PB, 7 DISP, 8 NET, 9 SND,
10 MISC, 11 SVC, 12 SS, 13 GPRS, 14 SAT, 15 CFG, 16 IMEI, 17 GPS, 18 SAP,
19 FACTORY, 20 OMADM, 21 RFS, 22 IMS, 23 EMBMS,
32 GEN, 48 DOMESTIC, 128 JPN
```

(`0x22873f` .. is the string blob; the three out-of-sequence values 0x20/0x30/
0x80 come from explicit `cmp r1,#0x20/#0x30/#0x80` branches at `0x1a88c0`.)

### 4.3 TX path

`IpcModem::SendMessage(char *msg, int len)` @ `0x1a1804`:

```
if (!msg) return -1;
proxy->vtable[+0x08](proxy, msg, len);            // history/log hook
proxy->vtable[+0x30](proxy, msg, 0, this->proto); // protocol-specific hook
ret = this->DoIoChannelRouting(msg, len, NULL);   // 0x1a1710
if (!ret) this->AddIpcDumpRecord(msg, len, 0);
return ret;
```

`IpcModem::DoIoChannelRouting(msg, len, chan)` @ `0x1a1710`:
requires `this+0x1c` (FMT channel) and, if the debug channel exists, that it is
open too; then `DoIoChannelRoutingTx()` @ `0x198b88` or
`DoIoChannelRoutingRx()` @ `0x1a1684` (RX path called from the reader).

`IpcModem::DoIoChannelRoutingTx(msg, len)` @ `0x198b88`:

```
if (!debug_chan_present) return fmt->vtable[0x18](fmt, msg, len) == 0;
if (debug->mode != 1 || fmt write succeeds) { ... }
```

i.e. the FMT payload is handed to `IoChannel::Write` → `write(fd, ...)` → a
7-byte-header payload becomes one SIPC5 frame generated by `misc_write()`.

The `IpcProtocol40::IpcTx*` family is the top of this chain; e.g.
`IpcTxModemPowerOff` builds a 7-byte stack buffer and calls
`(*(chan->vtable + 0x18))(chan, buf, len)` directly (`0x1ae69a`), and
`IpcTxRawIpc` @ `0x1a4341` does the same with a caller-supplied buffer after
validating its length field.

### 4.4 RX path

`IpcModem::ReceiveMessage(char *buf, int len, IoChannel *chan)` @ `0x1a17a8`
rejects `len == 0`, logs, and tail-calls the dispatcher at `0x1f4a8c` (a long
branch veneer; the real dispatcher receives `(buf, len, chan)`).

`IpcModem::DoIoChannelRoutingRx(buf, len, chan)` @ `0x1a1684`:
`chan == this+0x24` (RFS) → RFS handler; `chan == this+0x20` (debug) → debug
handler; `chan == this+0x1c` (FMT) → dispatch by `chan+0x18` state; otherwise
dispatch on `(this, buf, len)`.

The reader is `IoChannelReader` (object at `IpcModem+0x3c`; ctor takes the
`Modem*`).  `IoChannelReader::Start()` @ `0x1a1fc8` opens a command pipe and
starts the `Thread` stored at `+0x4`; the thread loop reads from every added
`IoChannel` (`AddIoChannel` @ `0x1a2070`, `GetIoChannelMaxFd` @ `0x1a20b8`)
and calls `Modem::ReceiveMessage(buf, n, chan)`.

`IoChannelReader` selects with those fds, so a RIL needs either one thread per
channel or a `poll()`/`select()` loop over both fds.  `misc_read()` returns 0
on its own 100 ms timeout, so a blocking-loop implementation must not treat 0
as EOF.

Two delivery details that shape a reimplementation:

* One `read()` returns **at most one skb**.  If the caller's buffer is
  smaller than the skb, `misc_read()` returns the prefix and keeps the rest
  for the next call (3.2).  Use a buffer ≥ 4096 (the FMT ring size).
* For a multi-frame message the kernel strips each link header and queues each
  fragment as its own skb (`rx_frame_with_link_header` →
  `gather_multi_frame`, `modem_io_device.c:216-278`).  Userspace therefore
  sees fragment-sized reads; only the first fragment starts with the 7-byte
  `sipc_fmt_hdr` whose `len` gives the total.  `sipc5_asm_feed()` implements
  exactly this reassembly.

### 4.5 Minimal handshake after CP boot

```
cbd: BOOT_OFF (blocks until CP sends CMD_PHONE_START) -> BOOT_DONE
     CP is now in LINK_STATE_IPC; INIT_END not yet sent
RIL: fd_fmt = open("/dev/umts_ipc0", O_RDWR)
     fd_rfs = open("/dev/umts_rfs0", O_RDWR)      // triggers INIT_END
     ... keep both fds open for the lifetime of the process ...
     loop: read(fd_fmt) -> sipc5_fmt_parse -> handle notification/response
```

`STATE_ONLINE` (required by `misc_write()` on IPC channels) is produced by
`ss310ap_boot_off()` — i.e. by `cbd`'s `IOCTL_MODEM_BOOT_OFF`, which waits for
`mc->init_cmpl`, enables `irq_cp_wdt` and calls
`iod->modem_state_changed(STATE_ONLINE)` for every already-open iod
(`modem_ctrl_ss310ap.c:327-346`; the state itself is stored by
`io_dev_modem_state_changed`, `modem_io_device.c:442-461`).  A RIL that starts
before `cbd` finishes `BOOT_OFF` will see `EPERM` from `misc_write()` until the
state flips.

No version query, no reset, no capability negotiation is observable before the
CP starts sending; the CP is the one waiting (`CP_START` retries).  The first
messages after `INIT_END` are CP-initiated indications (`PWR_*`, `SEC_*`,
`NET_*`), and only then should a RIL send requests.

---

## 5. Pseudo-code

### TX (AP→CP)

```
build(msg):
    body = command-specific encoding
    total = 7 + len(body)
    hdr = { total, seq, ack, main_cmd, sub_cmd, cmd_type }
    return hdr || body                       // e.g. 07 00 00 00 01 02 01

send(msg):
    // kernel: misc_write() prepends cfg (0xF8 cold, 0xFC after cbd's
    // IOCTL_MODEM_DL_START), ch=235, len=11, then pads to 8 bytes
    // (len 11 -> 16 total) when the pad bit is set
    n = write(fd_fmt, msg, len(msg))         // exactly one frame
    if n != len: desync -> fatal
```

### RX (CP→AP)

```
kernel:
    cp raises MASK_SEND_FMT
    read one SIPC5 frame from fmt_rx_buff:
        require cfg & 0xF8 == 0xF8
        require total_len <= ring usage
        ch = frame[1]; route to io_device with that id
        strip header (+padding)
        if multi-frame: append to per-id list until ctrl.more == 0
        queue skb(s) on iod->sk_rx_q
userspace:
    n = read(fd_fmt, buf, 4096)              // 0 = 100 ms timeout, -1 EIO...
    if reassembler needs more: continue
    parse {len, seq, ack, main, sub, type}; body = buf+7 .. len
    dispatch on (main, sub); interpret `type` with the response table
```

---

## 6. Constant reference

```
SIPC5 start mask            0xF8      cfg & 0xF8 == 0xF8
padding bit                 0x04      8-byte granularity (arm64 sizeof(size_t))
ext-field bit               0x02
multi-frame cfg             0x03      5-byte header, ctrl = more:1 | id:7
ext-length cfg              0x02      6-byte header, 32-bit len (compiled out)
header sizes                4 / 5 / 6
FMT/RFS channel IDs         235..244 / 245..254
BOOT/DUMP channel IDs       215..224 / 225..234
FMT ring (each direction)   4096 bytes at 0x95801000 / 0x95802000
SHM magic/access            0xAA / 1
MAX_SKB_TXQ_DEPTH           1024      per-device TX skb queue
MAX_IOD_RXQ_LEN             2048      per-io_device RX skb queue (drop above)
MAX_DM_RXQ_LEN              256       CPLOG1/CPLOG2 RX queue
misc_read timeout           100 ms
IOCTL_MODEM_STATUS          0x6f27
IOCTL_MODEM_FORCE_CRASH     0x6f34
```

---

## 7. Implementation and verification (this slice)

`parity/userspace/ril/sipc5/` is a dependency-free C11 library plus a probe
tool.  It is deliberately a *codec/transport* layer: it does not implement any
Samsung command opcode (that belongs to the RIL client).

* `sipc5_hdr_len/frame_len/pad_len/total_len`, `sipc5_link_parse`,
  `sipc5_link_build` mirror `include/sipc5.h` exactly, including the 5/6-byte
  variants and 8-byte padding.
* `sipc5_fmt_build/parse` implement the 7-byte header; `sipc5_asm_feed`
  reassembles fragments the way `misc_read()` delivers them.
* `sipc5_write_msg/read_msg` implement the `misc_write`/`misc_read` contract
  (one frame per write; partial write = error; 0 = timeout; `POLLHUP` =
  `-ECONNRESET`), `sipc5_open_channels()` enforces opening FMT+RFS.

Verification performed here:

* host build `cc -O2 -g -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror` — clean;
* `./sipc5-probe selftest` — 45 checks pass, including the four byte-exact
  stock FMT messages quoted in 4.2 and link-header/padding/multi-frame/ext-len
  vectors derived from the kernel macros;
* cross build `aarch64-none-linux-gnu-gcc 10.3.1 -static` and
  `qemu-aarch64 ./sipc5-probe selftest` — 45/45 pass, so the artifact that
  would ship on the device is exercised (no device/hardware involved).

Not verified: anything requiring the CP.  The tool's `monitor`/`send`/`status`
modes have never been run against `/dev/umts_ipc0`.

---

## 8. Open questions / risks

* **R1 – seq/ack.** Every stock builder leaves `msg_seq`/`ack_seq` at 0 and no
  traced function stamps them; the CP may still require monotonically
  increasing `msg_seq`.  Only hardware can settle this.
* **R2 – multi-frame policy.** With `max_tx_size == 0` the AP never sends
  multi-frame FMT; the CP could still do so.  The reassembler handles it, but
  no trace confirms the CP ever sets `more`.
* **R3 – 8-byte padding.** `sipc5_calc_padding_size()` uses `sizeof(size_t)`
  (8 on arm64), and `ld->aligned` becomes permanently true after
  `IOCTL_MODEM_DL_START`.  `cbd`'s assumption "payload multiple of 4 → no
  padding" is therefore wrong: the kernel pads BOOT frames whose length is not
  a multiple of 8, and RIL frames after boot are padded too (`cfg = 0xFC`).
  The pad bytes are whatever was in the freshly allocated skb tail (no explicit
  zeroing), so they must not be interpreted.  The CP skips them via the cfg
  bit; a userspace reader never sees them (the kernel strips/trims).
* **R4 – debug channel.** `Modem::Init` can create a second FMT-like channel
  (`this+0x20`) whose path comes from `ModemConfig+0x10`; on j7elte it may be
  the unix socket named by `IPCDEBUG_UNIX_SOCKET` (`0x209081`).  Whether it is
  enabled for `ss310ap` was not established; it does not affect the normal
  `umts_ipc0` path.
* **R5 – `libsec-ril-dsds.so`.** Same size, different content (2 126 295
  differing bytes) and a different build id; only `libsec-ril.so` was traced.
  Addresses in this document are not valid for the DSDS library.
* **R6 – stock `open()` loop.** `IoChannel::Open` retries 51 times; the
  library's `sipc5_open()` does not retry, because the device node exists as
  soon as `modem_probe()` has run.  A RIL starting before the kernel driver is
  probed must add its own retry.
