# Modem CP power from a non-Android kernel: TrustZone verdict

**Verdict: IMPOSSIBLE on this unit without the stock TrustZone — the CP power
switch is a secure-only register and the resident S-Boot/Kinibi monitor refuses
the power WRITE. Possible only with the stock Android TZ stack (paired S-Boot +
TZSW), or by replacing the bootloader chain — not from any non-Android kernel
alone.**

Phone: SM-J700M (j7elte), Exynos 7580, Shannon SS310AP. Stock kernel `3.10.61`
(`/tmp/stocksb/kernel`, raw ARM64 Image, 15 987 880 B) disassembled with capstone;
running tree kernel `3.10.108` (`android_kernel_samsung_universal7580@2a746c1`,
`arch/arm64/mach-exynos/`). TEE: Kinibi (`/dev/mobicore`, `t-base-EXYNOS64-
Android-302A-V015`), `libMcClient.so`/`libMcRegistry.so`/`mcDriverDaemon`
extracted from `system.raw` via `debugfs`. AP tar (`AP.tar.md5`, 3.3 GB)
contains only `boot.img`/`recovery.img`/`system.img`/`userdata.img` — **no
`sboot.bin`/TZSW payload, so the resident monitor cannot be replaced from
stock files**. Device link was down for the whole session (ping 100% loss), so
all evidence below is read-only RE of stock images; no hardware probe was run.

---

## 1. Stock cp-power path: exact SMC ABI (recovered from the binary)

All in `exynos_smc(0x82000700, sub, arg2, arg3)` → `smc #0`
(`arch/arm64/kernel/exynos-smc.S:19-23`; caller setup `mov x0,#0x700` +
`movk x0,#0x8200,lsl#16` at 17 sites in the stock Image, e.g. fileoff
`0x17d0c`, `0x18064`). VA==fileoff holds for analysis (kallsyms-adjacent
`adrp` targets resolve to the correct `exynos_cp_*` name strings at
`0x76edb8+`).

| Sub | Name (pmu.h:21-25) | Kernel callers (stock fileoff → semantic) |
|---|---|---|
| 3 | `READ_CTRL` | `exynos_cp_release` (`0x17ce0`), `exynos_cp_active_clear` (`0x17dbc`), `exynos_clear_cp_reset` (`0x17e9c`), `exynos_get_cp_power_status` (`0x17f7c`), `exynos_set_cp_power_onoff` (`0x17fdc`), dump (`0x17bf0`) |
| 4 | `WRITE_CTRL` | release: `(4, CP_START=0x8)` (`0x17d38`); active_clear: `(4, 0x40)` (`0x17e18`); clear_reset: `(4, 0x100)` (`0x17ef8`); power-on: `(4, 0xA)` (`0x18070`); power-off: `(4, (ctrl & ~2)=…&0xfffd, 1)` (`0x180b0-0x180c8`); reset asserts `(4, CP_RESET_SET=0x4)` (`0x1820c-0x1821c`, wrapped by `exynos_cp_reset`) |

Return-code convention (every reader identical):
`ands w1,w0,#0xffff; b.ne <read-fail>` — success is `(CP_CTRL<<16)|0`.
`exynos_get_cp_power_status` tests **`ubfx x0,x19,#0x11,#1`** (`0x17fb8`):
bit 17 = bit 1 of the high half = `CP_PWRON` — i.e. the ABI is literally
*PWRON at register-bit 1, shifted to bit 17*. Security path wraps the same
ID with the SSS clock: `exynos_smc(0x82001011, ENABLE=0,…)` →
`exynos_smc(0x82000700, mode, size, addr)` →
`exynos_smc(0x82001011, DISABLE=1,…)` (`0x31f310-0x31f354` =
`shmem_security_request`, strings `mode=%u, size=%lu, addr=%lu` /
`return_value=%d` at `0x9954be/0x9954e6`).

`CP_CTRL` bit words (`regs-pmu-exynos7580.h:1377-1384`):
`PWRON b1, RESET_SET b2, START b3, ACTIVE_REQ_EN b5, ACTIVE_REQ_CLR b6,
RESET_REQ_EN b7, RESET_REQ_CLR b8, PWRDN_DONE b9, RTC_OUT_EN b10,
SLEEP_START_REQ b12, SW_SLEEP_START_REQ b13, CLEANY_BYPASS_END b16`.

The observed unit behavior (`READ_CTRL→8`, `WRITE_CTRL(0xA)→8`, i.e. raw
`CP_START` echoed and PWRON never latched) is therefore the **TZ refusing
both the read encoding and the write** — return value 8 is not a valid
`(val<<16)|0` word (low half 8 ≠ 0) and a post-write readback of 8 proves
`CP_PWRON` (b1) was not set.

## 2. TEE surface: no modem TA, nothing reachable from the normal world

- Stock `system.raw` root (via `debugfs`): `libMcClient.so`, `libMcRegistry.so`
  (`/system/lib`), `mcDriverDaemon` + `rild` + `tzdatacheck` (`/system/bin`,
  `tee` symlink). **No `*.tlbin` TA bundle, no `mcRegistry/` directory, no
  CP/modem trustlet anywhere** in `lib/`, `vendor/{firmware,lib,etc}`,
  `app/`, `priv-app/`, `lib/drm/`.
- `mcDriverDaemon` strings: `<t-base Driver Daemon …>`,
  `t-base-EXYNOS64-Android-302A-V015-681_681`; registry paths are App-domain
  only (`/data/app/mcRegistry/…`, `*.rootcont`, `*.authtokcont`,
  `<uuid>.tlbin` filename *format* with zero modem UUIDs present); TA lookup
  is UUID-keyed `mcRegistryMemGetServiceBlob` → `processOpenTrustlet` — there
  is no static command-ID table to replay, and the client endpoint is
  `/dev/mobicore-user` (Kinibi daemon protocol), **not** an SMC callable from
  a kernel driver.
- `libsec-ril.so` (2.47 MB, ARM32) links no `mcOpenTrustlet`/`mobicore`/`tee`
  symbol; RIL↔CP runs over `/dev/umts_ipc0`+`/dev/umts_rfs0` (SIPC5), never
  through TrustZone. `tzdatacheck` is the Android tzdata updater, unrelated.
- `CP_CTRL` (PMU `0x105C0030`) is **never mapped non-secure**: the stock dump
  function (`0x17bf0`) prints the SMC return as `CP_CTRL` and reads only
  `+0x34/+0x38/+0x40/+0x6C` (`CP_STAT/CP_DEBUG/CP_SEMAPHORE/AUD_PATH_CFG`)
  via direct MMIO at VA `0xFFFFFF80F8180000+off` (PA `0x105C0000+off`,
  `pmu_cal_sys_exynos7580.h:22`). The power switch has no normal-world alias.

## 3. What differs under Nura vs Android (and what does not gate)

- **Not a gate:** bootmode/`/proc/cmdline`/ATAG (`console=ttySAC1…` stock DT
  `chosen:4826` vs `console=tty0 loglevel=7` Nura `/tmp/bimg/cmdline`; cbd's
  `FLB_GET_BOOTMODE` handler (`ril.cbd.modechan` strings) runs *on the CP
  after* power-on, so it cannot gate power-on), sec-boot/warranty fuses (no
  such check in the cp-power path — only generic `sec_debug`/`knox` strings
  elsewhere), `sys_rev`/`ds_det` (mailbox info word only,
  `modem_ctrl_ss310ap.c:182-194`), `CP_RST_N` (kernel drives gpio `out hi`,
  already correct), SHMEM/mailbox DT (verified identical in `modem.md` P2),
  regulators (no `S2MPU01A` LDO calls in the modem path;
  `CONFIG_REGULATOR_S2MPU01A` unset, `change_cp_pmu_manual_reset()` is a
  no-op returning 0; the modem DT node has no regulator phandles).
- **The actual difference:** the stock Android boot chain (S-Boot → TZSW
  Kinibi → kernel `3.10.61`) leaves a monitor that implements SMC
  `0x82000700` subs 3/4 with the `(val<<16)|0` contract; the Nura chain
  (same S-Boot, replacement kernel `3.10.108`) issues the identical SMC
  sequence (`RESET→SEC(2)→XMIT_BOOT→SEC(0)→ON→BOOT_ON`, verified byte-identical
  WRITE constant `0xA` at `0x18070`) and the monitor answers `8`/`8` — raw
  `CP_START`, power denied. Stock cbd (`/sbin/cbd` in ramdisk, ARM32 static)
  confirms the required order: `Request Security : non-secure mode` →
  `Send BIN` → `Request Security : secure mode` → `MODEM ON : pda_active 1,
  cp power on` → `BOOT ON`, and the ACCEPT table (`{0x00FEED02,0x00FEED04,
  0xFEED0002}`) already replays green on this unit (mode-2→`0x0`,
  mode-0→`0xFEED0002`) — the handshake passes, **then** the power WRITE is
  refused. S-Boot holds the CP in reset (`CP_CTRL=0x8`) until the TZ it
  trusts says otherwise.

## 4. Alternatives evaluated

1. **PMIC register path bypassing TZ — rejected (unsafe + wrong layer).**
   CP power is the SoC `CP_CTRL` switch (secure-only), not a PMIC rail; the
   driver owns no CP regulator and `CONFIG_REGULATOR_S2MPU01A` is unset.
   Toggling LDO rails by hand risks the SoC/CP and violates the per-device
   rule — do not attempt.
2. **Ask TZ with the exact stock sequence — already exhausted.** Read-tolerant
   kernel + stock-order cbd (`RESET, SEC mode 2/0, XMIT_BOOT, ON, BOOT_ON`)
   still ends at `WRITE_CTRL→8`, `-EACCES`, `mbx_cp_status=0`. There is no
   further stock argument to try: all five readers and all WRITE constants
   are enumerated above.
3. **Non-secure MMIO write to `CP_CTRL` — expected to fault, untested.**
   `CP_CTRL` has no non-secure mapping in the stock mapping (dump reads
   everything *except* `+0x30`); `__raw_writel(PA 0x105C0030)` from EL1
   should abort (TZPC-decorated). It is the only non-flash experiment left,
   but success is unlikely and an abort in early boot can hang the board
   until a reflash — run only with a known-good boot image on hand.

## 5. Single most promising experiment

Because the device link is down, do this first with any known-good image;
read-only until the last step:

1. Boot a kernel with `CONFIG_DEVMEM=y` (or a `debugfs` u32 poke) and,
   from a late-init console **after full boot**, read PA `0x105C0030`
   (`CP_CTRL`), `0x105C0034` (`CP_STAT`), `0x105C0038` (`CP_DEBUG`).
2. **Expected output:** `CP_CTRL=0x00000008 CP_STAT=0x00000010
   CP_DEBUG=0x00000000` — matching the `8` the SMC returns and the
   `CP_STAT=0x10 (CP_ACCESS_MIF)` in `modem-cp-power.md` §4.3 — proving the
   SMC echoes the register and the CP is held in reset by S-Boot/TZ.
3. Only then attempt `WRITE_CTRL` once more via the existing tolerant path
   and capture the one line `ERR! write Fail: 8`; any value other than 8
   (or a data-abort on the direct read) is the new fact that reopens this
   ticket. Do **not** write the PMIC or flash anything for this ticket.

## Evidence chain (files & addresses)

- SMC wrapper: tree `arch/arm64/kernel/exynos-smc.S:19-23`; stock caller
  pattern `0x17d0c/0x18064/…` (17 sites), sub IDs in `pmu.h:21-25`.
- Readers/writers: stock `0x17ce0/0x17dbc/0x17e9c/0x17f7c/0x17fdc/0x181d8`
  ↔ tree `arch/arm64/mach-exynos/pmu-cp.c` (full function map §1);
  ABI proof `ubfx …,#0x11,#1` at `0x17fb8`; WRITE `0xA` at `0x18070`,
  power-off mask `&0xfffd` at `0x180b4-0x180c8`.
- PMU PA/VA: `pmu_cal_sys_exynos7580.h:22` (`0x105C0000`),
  `regs-pmu-exynos7580.h:25-40`; stock MMIO VA `0xFFFFFF80F8180000+off`.
- Security wrap: `link_device_shmem.c:1747-1756`, stock `0x31f310-0x31f354`;
  stock `cbd` order strings (`Request Security…MODEM ON…BOOT ON`).
- TEE inventory: `system.raw` (`debugfs -R "ls -l {lib,bin}"`),
  `/tmp/tee/{mcreg.so,mccli.so,mcdd,tzc,secril.so}`, daemon version string
  `t-base-EXYNOS64-Android-302A-V015-681_681`; AP tar listing (4 images,
  no sboot/tzsw). Ramdisk `init.baseband.rc` (`cpboot-daemon /sbin/cbd
  -d -tss310 …`).
