# Modem CP power: TZ SMC ABI mismatch (status 8) — root cause, patch, hardware proof

**Verdict: read-side blocker removed and verified on hardware; the CP still
does not power on because the TZ also rejects the power WRITE (returns 8).
`ONLINE` / IMEI were not reached. The kernel now gets past every CP-power
_read_ and issues the `WRITE_CTRL`, and the exact TZ refusal is captured in
dmesg. This report documents the evidence, the patch, and the next step.**

Phone: SM-J700M, Nura Alpine aarch64/musl. Kernels: stock `3.10.61`
(`boot.img`, dpi@SWDG9701, Mar 2018) for disassembly reference; running
tree kernel `3.10.108` built from
`android_kernel_samsung_universal7580@2a746c1` (`code/kernel/build.sh`).
New kernel in this report: `#34` (read-tolerant) and `#34`+probe (MMIO
comparison logging). Userspace `/usr/bin/j7elte-cbd` + `j7elte-ril`
unchanged in behaviour except cbd now also drives the two stock security
handshakes (MODEM_RESET + SECURITY_REQ mode 2 / mode 0) in stock order.

---

## 1. Root cause

### 1.1 The ABI contract

`arch/arm64/mach-exynos/pmu-cp.c` drives CP power through the EL3 monitor:

```
exynos_smc(0x82000700 /* SMC_ID */, sub, arg2, arg3)
  sub 3 = READ_CTRL, sub 4 = WRITE_CTRL   (mach/pmu.h:21-25)
```

The specified convention is `return = (CP_CTRL << 16) | 0`: the CP_CTRL
register word in the high half, zero in the low half. All five readers
(`exynos_get_cp_power_status`, `exynos_set_cp_power_onoff`,
`exynos_cp_release`, `exynos_cp_active_clear`, `exynos_clear_cp_reset`)
test `!(ret & 0xffff)`, shift right 16 on success, and treat any non-zero
low half as an SMC error. Relevant CP_CTRL bits (`regs-pmu-exynos7580.h`):

```
BIT(1) CP_PWRON   BIT(2) CP_RESET_SET   BIT(3) CP_START
BIT(5) CP_ACTIVE_REQ_EN  BIT(6) CP_ACTIVE_REQ_CLR
BIT(7) CP_RESET_REQ_EN   BIT(8) CP_RESET_REQ_CLR  ...
```

### 1.2 What this unit's TZ returns

On this unit `READ_CTRL` returns **`8`** (`CP_START` alone: CP held in
reset/start, powered down). Two hypotheses:

* (a) the TZ echoes the raw CP_CTRL register instead of `(val<<16)|0`;
* (b) `8` is a TZ error code (unknown sub-command / denied).

Disambiguating evidence (all from hardware dmesg on kernel #34):

1. `WRITE_CTRL(CP_PWRON|CP_START = 0xA)` **also returns 8**
   (`ERR! write Fail: 8`). A genuine register write of `0xA` cannot read
   back `8` if PWRON was accepted, so the TZ refused the power-up.
2. The failure persists through the complete stock handshake
   (`MODEM_RESET → SECURITY mode 2 → XMIT_BOOT → SECURITY mode 0 →
   MODEM_ON → BOOT_ON`): mode 2 returns `0`, mode 0 returns the accepted
   code `0xFEED0002` (matches the stock cbd accept-table at `0x62b74`:
   `{0xFEED02, 0xFEED04, 0xFEED0002}`), yet the subsequent `WRITE_CTRL`
   still returns 8 and `ss310ap_boot_on()` times out (`-EACCES`, CP boot
   ROM never runs, `mbx_cp_status` stays 0).
3. The old code aborted *before* the write (`ERR! read Fail: 8` /
   `ERR! Get CP Power status: 8`); after the read-tolerant fix the write
   is issued and refused. So hypothesis (b) governs the WRITE path
   regardless of how the READ value is interpreted.

Conclusion: **this unit's TZ does not implement the CP-power SMC
sub-commands 3/4 as this kernel lineage expects** (returns 8 for both
READ and WRITE). The stock `3.10.61` kernel disassembled from `boot.img`
contains the *identical* mask/shift/branch sequence and the same WRITE
constant `0xA` (`mov x2,#0xa` before `bl exynos_smc` at file offset
`0x18070`), so stock Android relied on its (fused, S-Boot-paired) TZ
returning the shifted encoding — which this unit's TZ does not provide.

### 1.3 What status 8 means, precisely

Numerically `8 == BIT(3) == CP_START`. The direct-MMIO probe (below)
compares the SMC return against the non-secure PMU alias; until that
kernel boots, both readings (a) and (b) remain live for the READ path,
but the WRITE refusal (`ERR! write Fail: 8`) is unambiguous: **CP power
is blocked in the TZ/EL3 firmware on this unit, not in the AP kernel
logic, the DT, the regulators the kernel owns, or the userspace
sequence** (all of which were verified: `CP_RST_N` gpio-150 `out hi`,
SHMEM `reg=<0x90000000 0x05c00000>`, mailbox IDs match stock DT,
`CP_STAT=0x10` = `CP_ACCESS_MIF`).

---

## 2. The patch

### 2.1 Kernel: `0001-cp-power-raw-ctrl.patch` (wired into APKBUILD)

New file `code/pmaports/device/archived/linux-samsung-j7elte/0001-cp-power-raw-ctrl.patch`,
listed in `APKBUILD source=` with sha512, applies with `patch -p1`
(verified: applies clean to pristine `2a746c1`, result identical to the
flashed tree).

It adds one helper and uses it in all five readers:

```c
static u32 cp_ctrl_normalize(u32 raw, const char *who)
{
        u32 known = CP_PWRON | CP_RESET_SET | CP_START |
                    CP_ACTIVE_REQ_EN | CP_ACTIVE_REQ_CLR |
                    CP_RESET_REQ_EN | CP_RESET_REQ_CLR |
                    MASK_CP_PWRDN_DONE | RTC_OUT_EN |
                    MASK_SLEEP_START_REQ | SET_SW_SLEEP_START_REQ |
                    CLEANY_BYPASS_END;
        if (!(raw & 0xffff))
                return raw >> 16;          /* canonical encoding, as before */
        if ((raw & ~known) == 0) {         /* raw CP_CTRL word: accept */
                u32 direct = __raw_readl(EXYNOS_PMU_CP_CTRL);
                ...                        /* read-only MMIO comparison */
                return raw;
        }
        return 0xffffffff;                 /* genuine error, as before */
}
```

* Canonical `(val<<16)|0` input behaves bit-for-bit as before.
* A raw value with only known CP_CTRL bits set is accepted (and logged
  with a direct-MMIO comparison of `CP_CTRL`/`CP_STAT`/`CP_DEBUG`, which
  are non-secure-readable — the existing dump path already reads them).
* Anything with unknown bits set is still an error (`-1` paths kept).

GPS driver untouched (no shared files; `git status` shows only the cbd,
APKBUILD, and new patch + report).

### 2.2 Userspace: stock-order security handshake in `j7elte-cbd`

`code/pmaports/device/archived/j7elte-cbd/j7elte-cbd.c` (+66 lines):

* `IOCTL_MODEM_RESET (0x6f21)` + `IOCTL_SECURITY_REQ (0x6f53)` with
  `struct modem_sec_req` (`modem_prj.h:178`, 16 B, `_Static_assert`ed).
* `boot_cp()` now runs the exact stock order: `RESET → SEC(mode 2) →
  XMIT_BOOT → SEC(mode 0, {BOOT size, TOC data_start}) → ON → BOOT_ON →
  DL_START → stages`.
* `security_req()` accepts the stock codes `0`, `0xFEED02`, `0xFEED04`,
  `0xFEED0002` (the kernel returns the TZ word verbatim; the mode-0
  accept code arrives as a negative int32 and must not be treated as
  `-errno`).

---

## 3. Hardware proof (kernel #34, `3.10.108 #34 SMP PREEMPT`)

Built with `code/kernel/build.sh` (GCC 16, `-Wno-error`), packed with
`/tmp/mkboot.py`, flashed to `/dev/mmcblk0p10` (`dd bs=1M; sync`),
rebooted via `sysrq b`. Previous GPS-parity kernel was `#33`; `#34`
boots identically (GPS driver untouched).

### 3.1 Read path fixed — write now issued and refused by TZ

`j7elte-cbd -v` (foreground, stock order), then two pure-read probes:

```
j7elte-cbd: ioctl MODEM_RESET ok
j7elte-cbd: ioctl SECURITY_REQ(mode2) ok (0x0)
j7elte-cbd: ioctl MODEM_XMIT_BOOT ok
j7elte-cbd: ioctl SECURITY_REQ(mode0) ok (0xfeed0002)
j7elte-cbd: ioctl MODEM_ON ok
j7elte-cbd: ioctl MODEM_BOOT_ON failed: Permission denied
```

```dmesg
mif: misc_ioctl: umts_boot0: IOCTL_MODEM_RESET
mif: ss310ap_reset: +++
CP Power status: 8
exynos_get_cp_power_status: raw CP_CTRL 0x8 (TZ without (val<<16)|0 encoding)
mif: ss310ap_reset: ---
mif: misc_ioctl: umts_boot0: IOCTL_SECURITY_REQUEST
mif: shmem_security_request: mode=2, size=0, addr=0
mif: shmem_security_request: shmem: return_value=0
mif: misc_ioctl: umts_boot0: IOCTL_SECURITY_REQUEST
mif: shmem_security_request: mode=0, size=6176, addr=512
mif: shmem_security_request: shmem: return_value=-18022398     (= 0xFEED0002)
mif: misc_ioctl: umts_boot0: IOCTL_MODEM_ON
mif: ss310ap_on: +++
mif: ss310ap_on: cp_active:0 cp_status:0
mif: init_mailbox_regs: sys_rev:1, ds_det:0 (0x1)
CP Power status: 8
exynos_get_cp_power_status: raw CP_CTRL 0x8 (TZ without (val<<16)|0 encoding)
exynos_set_cp_power_onoff: raw CP_CTRL 0x8 (TZ without (val<<16)|0 encoding)
ERR! write Fail: 8
mif: ss310ap_on: change_mr_reset -> 0
mif: ss310ap_boot_on: +++
mif: io_dev_modem_state_changed: ss310ap->state changed (OFFLINE -> BOOTING)
```

Versus kernel #33 (for the record): `CP Power status: 8` →
`ERR! Get CP Power status: 8` / `ERR! read Fail: 8`, MODEM_ON aborts
before the write. On #34 the read is accepted and the write is issued —
the failure moved from the AP kernel into the TZ, which is the precise
new fact.

### 3.2 Not reached (blocked by the TZ write refusal)

* `STATE_ONLINE`, `CP_START <- ss310ap`, `CMD_PHONE_START`, IMEI via
  `j7elte-ril` (RIL still reports `BOOTING (3)` → 15 s `BOOT_OFF`
  timeout, as designed).
* The direct-MMIO comparison kernel (#34+probe, same Image + 3-line
  `__raw_readl` log) was built, packed to `/tmp/kernel-boot.img`
  (26 308 608 B) and flashed, but the device link dropped during the
  reboot (`rndis_host ... failed with error -110`, phone enumerates as
  `18d1:d001 SAMSUNG_Android` CDC without an RNDIS function) before the
  post-flash dmesg could be collected. The probe adds no state change
  (read-only), so it is safe to collect on next access; the patch file
  in the repo already contains it.

---

## 4. What remains (next step, not this ticket)

The TZ on this unit must be made to power the CP, or bypassed:

1. Collect the probe-kernel line (`direct CP_CTRL=... CP_STAT=...`)
   to confirm the SMC echoes the register; if direct `CP_CTRL`
   differs from 8, the SMC path is fully synthetic.
2. If the non-secure PMU alias is writable, a fallback
   `__raw_writel(CP_PWRON|CP_START)` + readback (with the SSS_CLK
   wrapper the security path uses) is the next experiment — or a
   different SMC function ID if this TZ moved CP power (needs TZ
   reverse engineering; the stock kernel binary only proves the old
   ABI, not the new one).
3. Never toggle PMIC LDO38/39 rails from the kernel (per-device rule);
   CP power here means the SoC `CP_CTRL` switch + `CP_RST_N` (already
   `out hi`), not the PMIC rails.

## 5. Files changed

* `code/pmaports/device/archived/linux-samsung-j7elte/0001-cp-power-raw-ctrl.patch` (new)
* `code/pmaports/device/archived/linux-samsung-j7elte/APKBUILD` (source + sha512)
* `code/pmaports/device/archived/j7elte-cbd/j7elte-cbd.c` (stock RESET + SECURITY_REQ order)
* `docs/reports/modem-cp-power.md` (this file)
