# j7elte-cbd / j7elte-ril deployed on hardware — CP does not power on (TZ/SMC blocker)

**Verdict: packages built and installed, services enabled, IPC path verified as far
as the hardware allows; the baseband never powers on, so ONLINE/IMEI/registration
were not reached.** Both binaries compile cleanly on the device with its own
`gcc (Alpine 15.2.0) 15.2.0`, pass their own self-tests, and do what the packages
say on the AP side: `/dev/umts_boot0` is opened and the BOOT image is pushed into
the SHMEM boot region, `/dev/umts_ipc0` + `/dev/umts_rfs0` are opened and the
kernel emits `INIT_END`.

The remaining failure is **below userspace**: the kernel's CP power path
(`ss310ap_on()` → `exynos_get_cp_power_status()` →
`exynos_smc(0x82000700, READ_CTRL=3)`) returns `8` on this unit, so
`exynos_set_cp_power_onoff()` aborts before writing `CP_PWRON|CP_START` and the CP
is never started. `mbx_cp_status` therefore stays `0`, `IOCTL_MODEM_BOOT_ON`
(`ss310ap_boot_on()`) times out after its 100×10 ms poll and returns `-EACCES`,
and `IOCTL_MODEM_BOOT_OFF` never sees `CMD_PHONE_START` (15 s timeout). No kernel
change was made — the assigned scope forbids it; see §5 for the exact evidence and
what a kernel/bootloader-side follow-up would have to check.

Everything below is from the phone (SM-J700M, Nura aarch64/musl, kernel
`3.10.108 #33`), not a host emulation.

---

## 1. Artifacts

Sources are the package directories verbatim:
`code/pmaports/device/archived/j7elte-ril/` and `.../j7elte-cbd/`.
All `sha512sums` in both APKBUILDs verify on the host before transfer.

| Installed file | SHA-512 / size |
|---|---|
| `/usr/bin/j7elte-ril` | `ed7bece3…ed73c`, 574 584 B, `ELF 64-bit LSB pie executable, ARM aarch64, static-pie linked` |
| `/usr/bin/j7elte-cbd` | `bda63cf6…2d1d4`, 302 096 B, `ELF 64-bit LSB pie executable, ARM aarch64, static-pie linked` |
| `/etc/init.d/j7elte-ril` | `7c470ff2…dd3d` (package `j7elte-ril.initd`) |
| `/etc/init.d/j7elte-cbd` | `1d486dd9…7b45` (package `j7elte-cbd.initd`) |
| `/etc/conf.d/j7elte-ril` | `3aadbf43…fb63b` (package `j7elte-ril.confd`) |
| `/etc/conf.d/j7elte-cbd` | `115903a6…1058c` (package `j7elte-cbd.confd`) |

`/usr/lib/systemd/system/j7elte-{ril,cbd}.service` were installed as the
APKBUILDs do; OpenRC is the active service manager.

## 2. Reproducible install

Run as root on the phone (`rootshell sh -c …`); sources are transferred
unmodified from the repo tree.

```
# host: pack the two package dirs (no edits)
cd code/pmaports/device/archived && tar cf /tmp/rildeploy.tar j7elte-ril j7elte-cbd
# device
mkdir -p /tmp/rilbuild && cd /tmp/rilbuild && tar xf -      # tar on stdin

# build exactly as the APKBUILD build() does, plus -static
cd /tmp/rilbuild/j7elte-ril
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -O2 -static \
    -o j7elte-ril j7elte-ril.c
cd /tmp/rilbuild/j7elte-cbd
gcc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -O2 -static \
    -o j7elte-cbd j7elte-cbd.c

# package() layout (OpenRC + systemd unit)
install -Dm755 /tmp/rilbuild/j7elte-ril/j7elte-ril   /usr/bin/j7elte-ril
install -Dm755 /tmp/rilbuild/j7elte-ril/j7elte-ril.initd /etc/init.d/j7elte-ril
install -Dm644 /tmp/rilbuild/j7elte-ril/j7elte-ril.confd /etc/conf.d/j7elte-ril
install -Dm644 /tmp/rilbuild/j7elte-ril/j7elte-ril.service \
    /usr/lib/systemd/system/j7elte-ril.service
install -Dm755 /tmp/rilbuild/j7elte-cbd/j7elte-cbd   /usr/bin/j7elte-cbd
install -Dm755 /tmp/rilbuild/j7elte-cbd/j7elte-cbd.initd /etc/init.d/j7elte-cbd
install -Dm644 /tmp/rilbuild/j7elte-cbd/j7elte-cbd.confd /etc/conf.d/j7elte-cbd
install -Dm644 /tmp/rilbuild/j7elte-cbd/j7elte-cbd.service \
    /usr/lib/systemd/system/j7elte-cbd.service

rc-update add j7elte-cbd default
rc-update add j7elte-ril default
rc-service j7elte-cbd start       # mounts EFS, runs the CP loader
rc-service j7elte-ril start       # after the CP is ONLINE
```

Both builds are warning-free. Note the APKBUILD and both init scripts install and
exec **`/usr/bin/...`**, not `/usr/sbin/...`; the assignment's `/usr/sbin` wording
would break the shipped `start-stop-daemon --exec` lines, so the package layout was
followed. `-static` is the only build-flag deviation (the APKBUILD passes
`$LDFLAGS`); the result is static-PIE and needs no musl loader at runtime.

The cbd init script mounts `/dev/disk/by-partlabel/EFS` on `/efs`; on this unit it
is `mmcblk0p3`, ext4, and `/efs/nv_data.bin` (1 048 576 B) is present, so the real
NV is sent (not the zero-filled default).

CP image check (`j7elte-cbd --dump-toc`), read from
`/dev/disk/by-partlabel/RADIO` = `mmcblk0p14`:

```
TOC: data_start=0x00000200 num_images=1
name     id       offset       size  load_addr        crc
BOOT     0    0x00000200 0x00001820 0x40000000 0x54d582a3
MAIN     2    0x00001a20 0x02141498 0x40010000 0xb64516b6
NV       3    0x00000000 0x00100000 0x44900000 0x00000000
OFFSET   4    0x0007aa00 0x00085600 0x00000000 0x00000000
```

## 3. Service state (left enabled)

```
# rc-update show default
            j7elte-bt | default
           j7elte-cbd | default
          j7elte-gpsd | default
           j7elte-ril | default
       j7elte-sensors | default
           j7elte-wifi | default
```

```
# rc-service j7elte-cbd start / status
 * Mounting /dev/disk/by-partlabel/EFS on /efs ... [ ok ]
 * Booting the SS310AP baseband ... [ ok ]          # start() returns 0 …
 * status: crashed                                    # … the loader exits ~5 s later

# rc-service j7elte-ril start / status
 * Starting the SIPC5 RIL client ... [ ok ]
 * status: started          (immediately after start)
 * status: crashed          (40 s later, after the CP probe fails)
```

Both services are enabled in `default`. `j7elte-cbd` deliberately exits non-zero
when the CP handshake fails (so the service manager can restart the sequence);
`j7elte-ril -d` exits with status 1 after it gives up on a CP that never reaches
ONLINE, so the pidfile goes stale and OpenRC reports `crashed`.

## 4. Tool output and kernel evidence

### 4.1 cbd (foreground, verbose)

```
j7elte-cbd: using CP image /dev/disk/by-partlabel/RADIO
j7elte-cbd: NV image: /efs/nv_data.bin (1048576 bytes)
j7elte-cbd: BOOT image: 0x1820 bytes @file 0x200 -> shmem boot region
j7elte-cbd: ioctl MODEM_XMIT_BOOT ok
j7elte-cbd: ioctl MODEM_ON ok
j7elte-cbd: ioctl MODEM_BOOT_ON failed: Permission denied
cbd_rc=1
```

Corresponding kernel log (`/proc/kmsg` capture):

```
mif: misc_open: umts_boot0 (opened 1) by j7elte-cbd
mif: misc_ioctl: umts_boot0: IOCTL_MODEM_ON
mif: ss310ap_on: +++
mif: ss310ap_on: cp_active:0 cp_status:0
mif: init_mailbox_regs: sys_rev:1, ds_det:0 (0x1)
CP Power status: 8
ERR! Get CP Power status: 8
exynos_set_cp_power_onoff: mode[0]
ERR! read Fail: 8
mif: ss310ap_on: change_mr_reset -> 0
mif: misc_ioctl: umts_boot0: IOCTL_MODEM_BOOT_ON
mif: ss310ap_boot_on: +++
mif: io_dev_modem_state_changed: ss310ap->state changed (OFFLINE -> BOOTING)
mif: misc_release: umts_boot0 (opened 0) by j7elte-cbd      # -EACCES, ~2 s later
```

`j7elte-cbd --status` → `modem state: 3` (`STATE_BOOTING`, left behind by the
failed `ss310ap_boot_on()`; it is **not** a sign that the CP started).

The state word is also the conclusion of `--dry-run`: stage 0..15 frames, the
`MODEM_BOOT_OFF` wait and `MODEM_BOOT_DONE` are all printed correctly
(`stage 3 frame 512/512 cmd=0x9301`, `stage 15: START cmd=0x00009f00`,
`ioctl MODEM_BOOT_OFF(0x6f23)`), and `--selftest` exits 0 with the mock-CP frame
checks (`CALL/02/06 CFRM`, `SMS/04/0a CFRM smsc "+15551234567"`, `GPRS/0d/04 CFRM`).

### 4.2 RIL

```
# j7elte-ril -v state
BOOTING (3)

# j7elte-ril -t 5 info
j7elte-ril: state BOOTING (3); re-issuing IOCTL_MODEM_BOOT_OFF
j7elte-ril: IOCTL_MODEM_BOOT_OFF: Resource temporarily unavailable
j7elte-ril: state BOOTING (3), waiting        (repeats)
j7elte-ril: CP did not reach ONLINE (last state BOOTING (3))
j7elte-ril: boot the baseband first: j7elte-cbd -d
```

`--selftest` on the device binary: rc 0 (exact frame vectors + mock CP; the same
44-frame set documented in `ril-impl-2.md`). `-n imei` prints the protocol-41
8-byte request and the full dry-run vector list.

**INIT_END path (verified):** simply opening the two channels produces the
designed kernel effect, independent of the CP being alive:

```
mif: misc_open: umts_ipc0 (opened 1) by j7elte-ril
mif: shmem_init_comm: umts_rfs0 is not opened yet
mif: shmem_init_comm: shmem: umts_rfs0->INIT_END->ss310ap
mif: misc_open: umts_rfs0 (opened 1) by j7elte-ril
```

With the CP down there is no `CP_START <- ss310ap` / `CMD_PHONE_START` reply, so
`IOCTL_MODEM_BOOT_OFF` blocks for its 15 s `MIF_INIT_TIMEOUT` and returns
`-EAGAIN`:

```
mif: misc_ioctl: umts_ipc0: IOCTL_MODEM_BOOT_OFF
mif: ss310ap_boot_off: +++
mif: ss310ap_boot_off: T-I-M-E-O-U-T
mif: mif_disable_irq: cp_fail(#50) is not active <ss310ap_boot_off>
```

### 4.3 CP power path, root cause

Reading the driver source of the tree that produced kernel #33
(`arch/arm64/mach-exynos/pmu-cp.c`, `drivers/misc/modem_v1/modem_ctrl_ss310ap.c`):

* `ss310ap_on()` first asks `exynos_get_cp_power_status()` (= SMC sub-command
  `READ_CTRL`) whether the CP is already powered.
* On this unit that SMC returns **8** (logged twice: `CP Power status: 8`,
  `ERR! Get CP Power status: 8`). The kernel ABI expects
  `(value << 16) | 0`; a non-zero low half is treated as an error.
* `ss310ap_on()` ignores the failure and continues, but
  `exynos_set_cp_power_onoff(CP_POWER_ON)` repeats the same read and `goto exit`s
  on it, so the `WRITE_CTRL(CP_PWRON|CP_START)` is **never issued**.
* `ss310ap_boot_on()` then polls `mbx_cp_status` 100 × 10–20 ms; the CP never runs
  its boot ROM, the mailbox stays 0, and the ioctl returns `-EACCES`.

A PMU dump forced through `IOCTL_SECURITY_REQ` (the kernel prints its registers at
the end of `shmem_security_request()`) confirms the values:

```
CP_CTRL:   0x00000008    # the SMC read result, not a register value
CP_STAT:   0x00000010    # CP_ACCESS_MIF (direct PMU read)
CP_DEBUG:  0x00000000
CP_SEMAPHORE: 0x00000000
AUD_PATH_CFG: 0x00000000
```

`CP_RST_N` (`gpio-150`) is deasserted (`out hi`), the SHMEM node is correct
(`reg=<0x90000000 0x05c00000>`, `ipc_offset=0x5800000`, `ipc_size=0x400000`), the
mailbox IDs match the stock DT (`mbx_cp2ap_status=5`, `mbx_ap2cp_status=4`), and
`IOCTL_MODEM_XMIT_BOOT` reports success.

The stock `cbd` extracted from the ROM's `boot.img` (`/sbin/cbd`, ARM32/Thumb-2)
was disassembled for comparison. It uses the same ioctl set our loader uses
(0x6f19/0x6f22/0x6f23/0x6f24/0x6f28/0x6f29/0x6f40) **plus two calls the package
does not make**, in `shannon_normal_boot` (calls at `0x21e1e`, `0x21e50`,
`0x21e88`, `0x21eb8`, `0x21eec`):

| stock step | call | ioctl |
|---|---|---|
| before power | `std_boot_modem_reset` (`0x1ccdc`) | `IOCTL_MODEM_RESET` 0x6f21 |
| before power | security wrapper (`0x1dc8c`) with mode 2 | `IOCTL_SECURITY_REQ` 0x6f53 |
| before power | per-stage loader loop (`0x21a58` → `0x218cc`) | `IOCTL_MODEM_XMIT_BOOT` 0x6f40 |
| before power | security wrapper with mode 0, args `{mode, BOOT size, TOC data_start}` | `IOCTL_SECURITY_REQ` 0x6f53 |
| power | `std_boot_modem_on` (`0x1cca8`) | `IOCTL_MODEM_ON` 0x6f19 |

The mode-0 security request returns `0xFEED0002` and stock treats that as an
**accepted code** (the wrapper's table at `0x62b74` is
`{0x00FEED02, 0x00FEED04, 0xFEED0002}`, and `0xFEED0002` is positive, so the
caller's `bge` continues); the kernel's own log prints it as `-18022398`
(signed). The exact stock order, including the reset and both security requests,
was replayed by hand on this unit
(`0x6f21 → 0x6f53(mode 2) → 0x6f40(BOOT) → 0x6f53(mode 0) → 0x6f19 → 0x6f22`)
and still ends in `CP Power status: 8` / `ERR! read Fail: 8` and `-EACCES`, so
the missing calls are a fidelity gap worth fixing for a future bring-up but are
**not** the cause of the failure on this unit.

The OEM SMC ID is therefore implemented by the secure monitor: `SECURITY_REQ`
mode 2 returns 0 and mode 0 returns `0xFEED0002` (stock's accepted code). The
sub-command this kernel needs for power, `READ_CTRL` (`exynos_smc(0x82000700, 3,
0, 0)`), returns `8`, and every kernel path to `WRITE_CTRL` reads first, so the
CP-control writes are never issued. This cannot be fixed from
`j7elte-cbd`/`j7elte-ril`: the CP is behind the TZ-owned `CP_CTRL` register,
`CP_PWRON` is never asserted, and with `/dev/mem` disabled (`CONFIG_DEVMEM` off)
there is no userspace MMIO path. It needs a kernel/bootloader-side investigation
(SMC ABI vs the phone's TZ/bootloader version, or the non-secure CP power path),
which is out of the assigned scope. No kernel/DT/config file was modified.

## 5. Verified subset vs. unverified

Verified on hardware:

* both packages build on the device (gcc 15, musl, `-Wall -Wextra`, static) and
  their packaged init/conf files are installed verbatim and enable cleanly;
* `j7elte-cbd` parses the real RADIO TOC, uploads BOOT with
  `IOCTL_MODEM_XMIT_BOOT`, drives `MODEM_ON`/`BOOT_ON` in the stock order, and
  fails at the precise ioctl the kernel fails at (not earlier, not later);
* `j7elte-ril` opens `umts_ipc0`+`umts_rfs0` in the order that makes
  `shmem_init_comm()` send `INIT_END`, queries `IOCTL_MODEM_STATUS`, re-issues
  `IOCTL_MODEM_BOOT_OFF` when the state is not ONLINE, and reports the 15 s
  timeout as an error;
* `j7elte-ril selftest` rc=0 on the device binary; `--dry-run` emits the traced
  protocol-41 vectors.

Not verified (blocked by §4.3):

* `modem state` 4 `ONLINE`;
* `CP_START <- ss310ap` / `CMD_PHONE_START` in `dmesg`;
* IMEI, PIN state, operator, voice/data registration, SMS, calls, PDP.

## 6. What remains unimplemented in the packages themselves

Per the `j7elte-ril` APKBUILD header: “Calls, SMS and data/PDP are NOT
implemented; the gap and the exact missing pieces are documented in the report.
Do not expect telephony to work yet.” The source has since grown the traced
call/SMS/GPRS frames (see `ril-impl-2.md`), but end-to-end telephony is still
absent; its own header lists: the unresolved call-list entry field table, the E911
RAT converter slot, the remaining GPRS switch arms, SIM APDU/phonebook/STK and the
RFS file protocol, and raw-only decoding for call-list/PDP replies. There is no
call-state machine and no end-to-end SMS/PDU decode, so ringing, calls, texts and
data remain unimplemented beyond the frames that were traced.

`j7elte-cbd` intentionally has no ramdump collection: on a CP crash the watcher
exits non-zero so the service can restart the sequence.
