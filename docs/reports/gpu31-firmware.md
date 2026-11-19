# GPU31-Firmware — T720 needs no firmware blob; OPP + perfcnt probe init

Scope: determine whether panfrost on the j7elte (Exynos 7580, Mali-T720)
needs a Mali firmware blob, locate any mali firmware in the stock ROM,
and implement or skip the `mali_fw` loader — plus the devfreq/perfcnt
initialisation panfrost performs at probe.

**Verdict: SKIP the loader. No firmware blob exists for this chip, on any
side. T720 is Midgard: no MCU, no firmware interface — kbase and panfrost
drive the Job Manager registers directly.**

Commit: this slice's files (`code/gpu31/firmware/panfrost7580_fw.{c,h}`,
`fw_test.c`). Phone is UP (kernel `#35`, link verified 2026-11-19,
`Mali-T72x MP2 r1p0 0x0720` re-read live); no reboot, no flash, no device
writes were made — all evidence below is host-side plus read-only sysfs.

## 0. Sources

- Shared tree (read-only): `drivers/gpu/arm/t7xx/r15p0/` — `mali_midg_regmap.h`
  (PRFCNT block :60-100, PRFCNT_CONFIG values :406-413, GPU_STATUS
  PRFCNT_ACTIVE, GPU_COMMAND CLEAR/SAMPLE), `mali_kbase_defs.h:1319`
  (SMC function IDs — Trusted *firmware* call gate for power, unrelated to
  a GPU blob), `backend/gpu/mali_kbase_devfreq.c` (devfreq target/status,
  init 60–125/180–260), `backend/gpu/mali_kbase_instr_backend.c:100-200`
  (PRFCNT enable preamble), `mali_kbase_core_linux.c:3270-3360`
  (regulator/clock/OPP probe), `platform/exynos/gpu_exynos7580.c`
  (8-entry DVFS table :40-54), Kconfig (`MALI_MIDGARD_DVFS`).
- Stock ROM: `system.raw` (ext4, J700MUBU2BRC2) via `debugfs`, full-vendor
  scan of `/vendor/lib[/egl]`, `/vendor/firmware`, `/lib[/egl,/hw,/64]`,
  `/app`, `/priv-app`, `/framework`, `/media`, `/etc`.
- Mainline panfrost reference: `linux-7.3` design baseline per
  `code/userspace/gpu/panfrost/design.md`; public Mesa/panfrost docs
  (Mali-CSF `mali_csffw.bin` is Valhall-only).
- Prior repo evidence: `docs/reports/gpu-panfrost.md` (M4 scope),
  `docs/reports/mali-blob-re.md` (r7p0 DDK is blob-free of firmware),
  `docs/reports/gpu-verify.md` + `code/userspace/gpu/dvfs-power/POWER.md`
  (live GPU ID, OPP table), `docs/reports/gpu31-dt.md` (stock DT has no
  OPP/mali-supply nodes).
- Live check 2026-11-19: `cat /sys/devices/11400000.mali/gpuinfo` →
  `Mali-T72x MP2 r1p0 0x0720`, `uname -r` → kernel `#35` (proves the
  `GPU_ID 0x07201000` the harness asserts is current silicon, not stale).

## 1. Stock-blob evidence (all negative, as expected)

| Search | Result |
|---|---|
| `grep -rn request_firmware\|release_firmware drivers/gpu/arm/` (all of kbase r5p0–r15p0, t6xx–t7xx, midgard) | **zero calls**. Only "firmware" hit in r15p0 is the SMC/TrustZone comment (`mali_kbase_defs.h:1319`), not a blob load |
| `/vendor/firmware` in stock image | 6 files, none GPU: `bcm43438A1…hcd` (BT), `fimc_is_{fd,fw2_3l2}.bin` + `setfile_*` (camera), `mfc_fw.bin` (video). Nura ships the same set in `firmware.tar.gz` |
| `/vendor/lib/egl` | exactly one file: `libGLES_mali.so` (the r7p0 DDK userspace blob — compiler + JM submit client, loads via kbase ioctls, loads no firmware) |
| Case-insensitive sweep for `mali\|g3d\|t72\|midgard\|panfrost\|csffw` over `/vendor/lib`, `/vendor/media`, `/lib`, `/lib64`, `/lib/egl`, `/lib/hw`, `/app`, `/priv-app`, `/framework`, `/media` | **no hits** (only unrelated `libion`, `libLLVM`, qmage strings) |
| Upstream panfrost on Midgard | loads **no** blob on any Midgard part; the only panfrost-adjacent firmware in the ecosystem is Valhall-CSF `mali_csffw.bin`, which postdates this GPU by a generation |

Net: there is no blob to find because the architecture has no blob consumer.
Kbase r15p0 brings the GPU up (live `/dev/mali0`, soft-job proof in
gpu-verify.md §4) with clocks + regulator + register writes only.

## 2. Loader decision: SKIP with a documenting stub

`pf7580_fw_required()` (`panfrost7580_fw.c`) returns 0 for every GPU_ID and
names the reason; a non-T720 ID logs a warning but still loads nothing. This
keeps the probe path honest (the "firmware step" is visibly considered and
dismissed) without inventing an interface the silicon lacks.

## 3. What panfrost *does* need at probe (implemented)

### 3.1 Devfreq / OPP init

- Stock DT has **no** `operating-points(-v2)` for the GPU (gpu31-dt §2–4);
  frequencies live in C. `pf7580_opp_table[]` ports the stock ladder
  1:1 (`gpu_exynos7580.c:40-54`): 160/266/350/440/534/668/734/800 MHz at
  900/900/1000/1000/1000/1100/1100/1100 mV — all inside the BUCK4
  600 mV–1300 mV window (`vdd_g3d`, always-on/boot-on).
- `pf7580_register_opps()` registers one OPP per entry via 3.10-era
  `opp_add(dev, Hz, uV)` (`include/linux/opp.h:43,396`; Hz/uV units at
  `drivers/base/power/opp.c:55-67,383-396`) so the stock `simple_ondemand`
  governor (`drivers/devfreq/governor_simpleondemand.c`, profile struct in
  `include/linux/devfreq.h:56-90`) and a kbase-style `target()` can resolve
  freq→voltage. Voltage ordering (up-before-`clk_set_rate` on the way up,
  down-after on the way down) follows `kbase_devfreq_target`
  (`mali_kbase_devfreq.c:60-125`) and belongs to the devfreq slice that owns
  `target()` — this file registers data only.
- The exynos7580 backend drives MIF/INT QoS via C-side pm_qos tiers
  (`mif_min_table`, gpu31-dt §2.4), not DT phandles — no DT edit needed
  (frozen `dt.img`), consistent with the gpu31-dt reliance plan.

### 3.2 Perfcnt init

`pf7580_perfcnt_probe()` reproduces the kbase enable preamble
(`mali_kbase_instr_backend.c:145-181`) without submitting work:
`PRFCNT_CONFIG <- MODE_OFF`, `BASE_LO/HI` + JM/SHADER/TILER/MMU_L2 masks for
the address space, then `CONFIG <- MODE_MANUAL`; returns `-EBUSY` while
`GPU_STATUS` bit 2 (`PRFCNT_ACTIVE`) reads set. Register offsets
(0x060/0x064/0x068/0x06C/0x070/0x074/0x07C) and config values verified
against `mali_midg_regmap.h:74-90,406-490`; `mali_hw.h` already carries the
IRQ bit (`PERFCNT_DONE` = bit 16) and `PRFCNT_CLEAR`/`SAMPLE` commands.
Sampling (GPU_CMD + IRQ 16) and the 8186 tiler-enable quirk belong to the
perfcnt ioctl slice, next to the sampling path — noted in code.

## 4. Delivered code + verification

Files (staged, own files only): `code/gpu31/firmware/panfrost7580_fw.{h,c}`,
`code/gpu31/firmware/fw_test.c`.

- Host harness `fw_test.c` (verdict + OPP table, kernel stubs): **ALL PASS**
  — T720 r1p0 ID → no blob with reason string; non-T720 ID → still no-load;
  8 OPPs sorted, 160–800 MHz, 900 mV–1100 mV, endpoints exact.
- Kernel C (`panfrost7580_fw.c`): `-fsyntax-only` clean under `-std=gnu89`
  against the prepared tree's generated headers
  (`j7k/gpu31/scratch`, `include/generated/{autoconf.h,bounds.h,...}`).
  No `make` in the shared tree; no device writes; no flash.
- Harness binary was removed after the run (not committed); rerun with
  `gcc -Wall -Wextra -o /tmp/fw_test fw_test.c && /tmp/fw_test`.

## 5. Cross-slice notes

- G31_Devfreq owns `target()`/governor wiring (voltage ordering lives
  there); G31_Power owns the `vdd_g3d` window (do not widen beyond
  600–1300 mV); perfcnt ioctl slice owns sampling + IRQ + 8186 quirk.
- Integrator: files are new and self-contained; `git apply` the staged
  additions. No DT change, no Kconfig change, no shared-tree touch.
