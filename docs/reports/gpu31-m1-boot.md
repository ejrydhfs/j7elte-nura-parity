# GPU31 M1 boot verification — pre-flash gate + live #35 baseline

Date: 2026-11-19. Slice: `gpu31-m1-boot` (owner: G31_M1Verify).
Device: SM-J700M (j7elte), Nura Alpine aarch64. Running kernel at baseline:
`3.10.108 #35 SMP PREEMPT Wed Nov 18 23:36:05 EST 2026` (aarch64, gcc 16.2.0).

**Flash status: NOT FLASHED. No flash was performed in this slice.**
Per Main: no flash until (1) this offline verification lands, (2) the
recovery plan is reviewed, (3) Main gives an explicit go. A bad boot image
requires physical access we do not have, and the phone serves two live
workstreams (GPS GLME trace, FM audio) — no reboot was performed either.

## 1. Offline verification (host-side, no device writes)

### 1.1 Image header / size / argv match (`/tmp/kernel-boot.img`)

Parsed with Python `struct` (`ANDROID!` header v0, page 2048):

| Field | `/tmp/kernel-boot.img` (M1 candidate) | `/tmp/bindboot.img` (known-good) |
|---|---|---|
| kernel size | 18 110 712 B | 17 234 120 B |
| ramdisk size | 8 822 392 B | 8 822 392 B (identical) |
| dt size | 313 344 B | 313 344 B (identical) |
| second size | 0 | 0 |
| kernel addr | `0x10008000` | `0x10008000` (identical) |
| cmdline | `console=tty0 loglevel=7` | `quiet splash` |
| total | 27 250 688 B < 33 554 432 B (BOOT p10 = 32 MiB) | 26 374 144 B < 32 MiB |
| ARM64 Image magic | `0x644d5241` OK | `0x644d5241` OK |
| sha256 | `cf25ed70…51cb68be726` (rebuilt 2026-11-19 01:26) | `fcec651e…f038` (matches `code/kernel/recovery/known-good.sha256`) |

Payload-hash comparison (split at page boundaries, sha256 per section):

| Section | M1 candidate | Known-good | Match? |
|---|---|---|---|
| kernel | `9a38de83…` (19 Nov img) / rebuilt img carries new kernel | `52170889…` | **differ (expected — new kernel)** |
| ramdisk | `ac0b6f8b…c8491a18` | `ac0b6f8b…c8491a18` | **identical** |
| dt | `352bda9b…69d8f937` | `352bda9b…69d8f937` | **identical** |
| stock parts | `/tmp/bimg/dt.img` = same `352bda9b…`; `/tmp/bimg/ramdisk-new.gz` = same `ac0b6f8b…`; `/tmp/bimg/cmdline` = `console=tty0 loglevel=7\n` | — | **image reuses stock dt + ramdisk-new.gz + cmdline** |

Kernel-payload string probes (uncompressed ARM64 `Image`, M1 candidate):

- `exynos-drm`, `exynos_drm`, `drm_get_platform_dev`,
  `exynos_drm_device_register`, `samsung,exynos5250-g2d`, `mali_kbase`
  all PRESENT.
- bindboot kernel: `mali_kbase` present, `exynos-drm` /
  `drm_get_platform_dev` ABSENT (DRM is the M1 delta).
- `check-readiness.sh /tmp/kernel-boot.img` → **READY** (size < 32 MiB,
  `ANDROID!` magic, known-good matches record).

NOTE: the sha in `gpu-drm.md` M1 (`53acd61a…`, 27 234 304 B, 18 Nov 23:56
build) is stale — the image was rebuilt 19 Nov 01:26
(`cf25ed70…`, 27 250 688 B, kernel +16 384 B). Payload composition is
unchanged (same ramdisk/dt/cmdline); only the kernel payload differs
(Integrator rebuild, likely the sibling `0001-cp-power-raw-ctrl.patch`
documented in `gpu-drm.md` §M1). The value recorded here is ground truth
from the file on disk.

### Expected post-flash string table (`m1-verify.sh` §2 reference)

| Probe | #35 baseline (expect ABSENT) | M1 flashed (expect PRESENT) |
|---|---|---|
| `dmesg \| grep -ci drm` | `0` (verified live, §2) | ≥ 1 (`exynos … on minor 0`-class init line) |
| `dmesg \| grep -iE 'exynos-drm\|exynos_drm'` | empty | ≥ 1 line |
| `dmesg \| grep -i mali` (rootshell) | latency lines only (`state restore/save latency exceeded`) | same kbase lines + no new kbase errors |
| `/dev/dri` | absent (`ls: /dev/dri: No such file or directory`) | `card0` + `controlD64`, no `renderD128` (M2 scope) |

## 2. Live #35 baseline (read-only SSH, short `flock` holds, no reboot)

All commands read-only (`cat`, `ls`, `dmesg | grep`, `xdpyinfo`, `ip`).
No `dd`, no flash, no reboot.

| Item | Verdict | Evidence (verbatim) |
|---|---|---|
| Kernel identity | **PASS (baseline)** | `Linux nura-j7elte 3.10.108 #35 SMP PREEMPT Wed Nov 18 23:36:05 EST 2026 aarch64`; `/proc/version`: `Linux version 3.10.108 (asdf@asdf-82ju) (gcc version 16.2.0 (GCC) ) #35 …` |
| Uptime (no cold boot consumed) | **PASS (baseline)** | `06:22:42 up 9 min` at capture; later `80.80 422.39` in `/proc/uptime` |
| `/dev/dri` absent (pre-M1) | **PASS (baseline)** | `ls: /dev/dri: No such file or directory` |
| exynos-drm absent (pre-M1) | **PASS (baseline)** | `dmesg \| grep -ci drm` → `0` (rootshell); `grep -iE exynos-drm` → empty |
| fbdev alive | **PASS (baseline)** | `/dev/fb0` char `29, 0`; `virtual_size` = `720,1280` |
| X at 720×1280 | **PASS (baseline)** | `dimensions: 720x1280 pixels (191x339 millimeters)`; `Xorg :0 vt1 -config …/99-fbdev.conf -dpms -s 0 -noreset` (PID 4132) |
| USB-net returned | **PASS (baseline)** | `rndis0: <BROADCAST,MULTICAST,UP,LOWER_UP> … state UP`; `default via 172.16.42.2 dev rndis0` |
| GPS tty stock parity | **PASS (baseline)** | `/dev/ttySAC0` char `204, 64`; `GPS_PWR_EN/value` readable. NOTE: open as `user` → `PermissionError EACCES` (user not in `dialout`: `groups=10(wheel),28(netdev),102(plugdev),300(abuild),10000(user)`); parity signal is open-as-root, encoded in `m1-verify.sh` §6 |
| kbase intact | **PASS (baseline)** | `/dev/mali0` char `10, 59`; `dmesg \| grep -i mali` shows live PM latency lines (`mali 11400000.mali: state restore latency exceeded …`) |
| Kernel cmdline (proc) | **INFO (baseline)** | `/proc/cmdline` = `console=ttySAC1` (boot cmdline ≠ mkboot header cmdline `console=tty0 loglevel=7`; recorded so post-flash diff does not false-alarm) |
| dmesg ring | **INFO (baseline)** | Early-boot lines already rotated out (ring head at `[161s]` sshd noise, not `Linux version` banner); display-probe grep (`decon`, `fb0`, `frame.buffer`) returns nothing at this uptime — post-flash asserts use `grep -i drm` + node existence, not banner text |

Stale-report correction (per Main): the phone IS UP — `gpu-drm.md` M1
"device link down / 100% loss" and `gpu31-master.md` §5 risk 1 are stale
as of this baseline (SSH + RNDIS verified working 2026-11-19 ~06:2x UTC
window; one transient `Connection timed out` mid-window recovered on
retry — link is flaky, holds must stay short).

## 3. `m1-verify.sh` improvements (this slice)

`code/kernel/m1/m1-verify.sh` rewritten (still runs on the phone only,
never reboots): 7 numbered groups, each printing `PASS:`/`FAIL:`/`INFO:`,
exit 0 only if all pass (`M1-VERIFY-PASS (7/7)`), else `M1-VERIFY-FAIL (N)`
with exit 1:

1. `/dev/dri` nodes: require `card0` + `controlD64` char nodes; `renderD128`
   reported INFO-only (absent expected on M1, present tolerated).
2. `dmesg` probe: user `dmesg`, rootshell fallback; prints `grep -i drm`
   lines; requires `exynos-drm|exynos_drm`.
3. fbdev: require `/dev/fb0` + `virtual_size` exactly `720,1280`.
4. X: `pgrep Xorg` + `xdpyinfo` dimensions contain `720x1280`.
5. USB-net: `ip route show default` non-empty (proves RNDIS path back).
6. GPS: require `/dev/ttySAC0` char node; open-as-user PASS, else
   open-as-root PASS with the EACCES note (§2 GPS row); require
   `GPS_PWR_EN/value` readable.
7. kbase: require `/dev/mali0`.

`sh -n` syntax check: OK.

## 4. Readiness list (for Main)

- [x] Offline verification done (§1: header/dtb/ramdisk/cmdline/size/sha
      recorded; kernel-payload DRM string delta confirmed; readiness READY).
- [x] `m1-verify.sh` hardened with per-item PASS/FAIL + expected-output
      table (§3, §1 expected-output table).
- [x] Live #35 baseline captured for post-flash comparison (§2).
- [ ] Recovery plan reviewed by Main (owner: Main; doc:
      `code/kernel/recovery/RECOVERY.md`; fallback `/tmp/bindboot.img`
      `fcec651e…f038` verified present §1).
- [ ] Explicit go from Main (NOT RECEIVED — no flash, no reboot performed).
- [ ] Physical-recovery acknowledgement: TWRP + `adb push` restore (§3 of
      RECOVERY.md) needs a human at the device; confirm who before flashing.

Suggested flash sequence once approved (unchanged from RECOVERY.md, M1Verify
only): `scp` image → `dd … bs=512 conv=fsync; sync` → `sync; reboot`
(never sysrq-b) → 5-min SSH wait → `sh code/kernel/m1/m1-verify.sh` →
report back here with dmesg excerpts, `/dev/dri` listing, X check.
