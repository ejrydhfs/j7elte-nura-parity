# GPU31-DT — stock DT GPU surface: node list, property tables, driver reliance plan

Scope: enumerate exactly which GPU-related nodes/properties exist in the stock
DT, what the driver can rely on with **zero DT change** (stock `dt.img` is
never rebuilt — S-Boot hangs on rebuilt DTBs; new kernels boot against the
stock DTB), and the in-driver fallback for every property a new consumer
(panfrost / devfreq / power) might want but stock does not have.

## 0. Sources

- Decompiled stock DTBs: `/tmp/stockboot/stock_18804736.dts`,
  `stock_18907136.dts`, `stock_19009536.dts` (from stock `boot.img` DTBs).
- All three DTBs contain **identical GPU-relevant content** (verified by
  grep-diff; only line offsets shift, e.g. `mali` at :3845 / :~3890 / :3943).
  Line references below are `stock_19009536.dts` unless noted.
- Stock kbase consumer (read-only, shared tree — not modified):
  `drivers/gpu/arm/t72x/r5p0/{mali_kbase_core_linux.c,platform/gpu_exynos7580.c,mali_kbase_platform.c}`
  and tree DTS `arch/arm64/boot/dts/exynos7580{,-q}.dtsi` (same `mali` node —
  tree source and stock DT agree field-for-field).
- Ground truth that stock DT + stock driver already works: `/dev/mali0`,
  DDK `r15p0-00rel0`, UK 10.6, `Mali-T72x MP2 r1p0 0x0720`
  (`docs/reports/gpu-verify.md` §2, §4).

## 1. GPU-relevant node list (stock DT)

| # | Node | Lines | `compatible` | `status` prop? |
|---|------|-------|--------------|----------------|
| 1 | `mali@0x11400000` (root child) | 3943–3951 | `arm,mali` | **absent** (= enabled by default; kbase probes, `/dev/mali0` exists) |
| 2 | `pd-g3d@10464060` (root child) | 4358–4364 | `samsung,exynos-pd` | absent (= bound; `samsung,power-domain = <&pd_g3d>` resolves, phandle `0x60`) |
| 3 | `s2mpu03_pmic@66` → `regulators/BUCK4` | 3247–3268 / 3301–3309 | `samsung,s2mpu03-pmic` | n/a (PMIC node; `g3d_en = <1>` at :3255) |
| 4 | `bus_int` (root child) | 2771–2782 | `samsung,exynos7-devfreq-int` | absent (= active; INT/MIF buses vote targets, not GPU nodes) |
| 5 | `bus_mif` (root child) | 2784–2797 | `samsung,exynos7-devfreq-mif` | absent (= active) |
| 6 | `bus_isp` (root child) | 2799–2808 | `samsung,exynos7-devfreq-isp` | absent (= active; camera/ISP only, listed to close the set) |
| 7 | `tmuctrl@10030000/10060000/10040000` | 2906–2910 / 2912–2916 / 2942–2946 | `samsung,exynos7580-tmu` x3 | absent (= active; **no** GPU `cooling-device` / thermal-zone linkage anywhere) |
| 8 | `reserved-memory` | 22–82 | mixed (`fd-lib-shmem`, `exynos5433-ion,*`) | n/a — **no GPU carveout** (ion/video/mfc only; GPU uses its own MMU + normal DMA) |
| 9 | `iommu-domain_fimc-is` + `sysmmu@...` | 4194–4226 | `samsung,exynos-iommu-bus`, `samsung,exynos7420-sysmmu` | n/a — camera/ISP only; **no sysmmu/iommu for G3D** |
| 10 | `NOC_BE_GROUP1` (`init-desc` string) | 2378 | `samsung,exynos-noc` | n/a — `"G3D"` appears only as a NoC initiator name string; informational |

Deliberately **absent** (searched case-insensitively: `opp`, `mali-supply`,
`operating-points-v2`, `cooling`, `thermal.*gpu`, `gpu.*trip`, `clock-rates`,
`arm,mali-midgard`, `arm,malit6xx`, `mali-t72?`, second `compatible` entry,
`memory-region`, `iommus` on the mali node): **all of them**. The stock mali
node has exactly 6 properties and nothing else.

## 2. Property tables (decoded)

### 2.1 `mali@0x11400000` — the whole node, verbatim + decoded

| Property | Stock value | Decoded / notes |
|----------|-------------|-----------------|
| `compatible` | `"arm,mali"` (single string) | Matches `exynos_mali_match[]` (`mali_kbase_core_linux.c:3056`; that table — not `kbase_dt_ids[]` — is wired to `.of_match_table`, :3522). `arm,malit6xx`/`arm,mali-midgard`/`arm,mali-t720` are **not** present and not needed by kbase. |
| `reg` | `<0x00 0x11400000 0x00 0x5000>` | 20 KiB G3D SFR at `0x11400000`. Kbase takes `IORESOURCE_MEM,0` (`:3165`). |
| `interrupts` | `<0 0x11a 0> <0 0x11b 0> <0 0x119 0>` | GIC SPI 282, 283, 281 → Linux IRQ 314, 315, 313 (SPI+32), flags 0. **Order is the ABI**: kbase reads `IORESOURCE_IRQ, i` with `irqtag = i` (`:3134–3162`); the `interrupt-names` string match is compiled out (`#if 0`). |
| `interrupt-names` | `"JOB", "MMU", "GPU"` | Present but **ignored** by the driver (see above). Index 0=JOB, 1=MMU, 2=GPU. |
| `clocks` | `<&clock 1> <&clock 10> <&clock 160> <&clock 161> <&clock 162> <&clock 163>` | `&clock` = `clock-controller@0x10010000` (`samsung,exynos7580-clock`, :2763, phandle `0x0f`). IDs per tree `exynos7580.dtsi:1066+`: 1=`fin_pll`, 10=`fout_g3d_pll`, 160=`aclk_g3d`, 161=`mout_g3d`, 162=`dout_aclk_g3d`, 163=`aclk_g3d_400`. |
| `clock-names` | `"fin_pll", "fout_g3d_pll", "aclk_g3d", "mout_g3d", "dout_aclk_g3d", "aclk_g3d_400"` | **These strings are the real clock ABI**: the exynos7580 backend does `clk_get(kbdev->dev, "<name>")` for all six (`gpu_exynos7580.c`); any one failing aborts clock init (`gpu_get_clock`). |
| `samsung,power-domain` | `<0x60>` = `&pd_g3d` | Consumed by the Samsung PM-domain core (not by kbase, which additionally bangs `EXYNOS_PMU_G3D_CONFIGURATION/STATUS` raw MMIO in `gpu_power_set_reg`/`gpu_is_power_on`). |

### 2.2 `pd-g3d@10464060`

| Property | Value |
|----------|-------|
| `compatible` | `samsung,exynos-pd` |
| `reg` | `<0x00 0x10464060 0x00 0x20>` (G3D PMU block) |
| `pd-option` | `<0x01>` |
| phandle | `0x60` (`pd_g3d`), referenced once — by the mali node |

Sibling PD nodes (`pd-mfcmscl`, `pd-aud`, `pd-disp`, `pd-isp`) are untouched by
GPU work; listed only so integrators do not renumber/alias them.

### 2.3 PMIC rail: `g3d_en` + `BUCK4/vdd_g3d`

| Item | Value |
|------|-------|
| `g3d_en = <0x01>` (:3255, `s2mpu03_pmic@66`) | G3D rail enable flag for the s2mpu03 driver |
| `BUCK4 regulator-name` | `"vdd_g3d"` — **this string is the regulator ABI**: backend does `regulator_get(NULL, "vdd_g3d")` (`gpu_exynos7580.c:712`), i.e. global lookup, no DT phandle involved |
| `regulator-min/max-microvolt` | 600000 / 1300000 (`0x927c0` / `0x13d620`), `always-on` + `boot-on` |

Stock DVFS table voltages (compiled into `gpu_exynos7580.c`,
`gpu_dvfs_table_default[]`: 800 MHz→1100000; 734/668→1100000;
534/440/350→1000000; 266/160→900000) all sit inside [600000, 1300000].
The rail is up before kbase probes; `gpu_regulator_init` only re-enables and
sets the ASV-matched voltage.

### 2.4 Devfreq bus nodes (DVFS-adjacent, not GPU nodes — do not retarget)

`bus_mif` votes the MIF QoS the GPU backend requests per OPP
(`mif_min_table[]` in C: 416/559/667/728/825 MHz tiers; stock `bus_mif`:
`downthreshold 0x3c`, `upthreshold 0x46`, `default_qos 0x65900`=416000 kHz,
`initial_freq 0xc96a8`=825000 kHz, `cal_qos_max 0xc96a8`=825000 kHz).
`bus_int` similarly (`default_qos 0x186a0`=100000 kHz,
`initial 0x61a80`=400000 kHz). The kbase backend drives these via
pm_qos-style calls from C (`GPU_BUS_DEVFREQ=1`, `GPU_PMQOS_*`
attributes), **not** via DT phandles — no DT edit is needed or wanted here.
`bus_isp` is camera-only.

### 2.5 CPU `operating-points` (NOT a GPU table — do not copy the pattern)

`cpus/cpu@000` and `cpu@100` carry legacy `operating-points` (freq,uV pairs
1600000@1300000 down to 300000@850000). They belong to `cpu-cluster.0/1`
(`BUCK2/BUCK1`) and prove only that this DT predates `operating-points-v2`.
There is **no** GPU OPP table anywhere in stock.

## 3. What the driver can rely on without any DT change

The stock kbase probe path needs exactly the six mali properties plus the two
support nodes, all present and proven by the shipping `/dev/mali0`:

1. `compatible = "arm,mali"` → OF match (`exynos_mali_match`).
2. `reg` 0x11400000/0x5000 → register mapping.
3. Three `interrupts` **in index order** JOB, MMU, GPU (names unused).
4. Six `clock-names` resolvable through the exynos7580 clock driver
   (`fin_pll`, `fout_g3d_pll`, `aclk_g3d`, `mout_g3d`, `dout_aclk_g3d`,
   `aclk_g3d_400`); `gpu_set_clock` reparents `mout_g3d → fout_g3d_pll` and
   gates `aclk_g3d_400`.
5. `samsung,power-domain = <&pd_g3d>` bound by the PM-domain core, plus the
   backend's own raw PMU writes (works even if generic PD binding lags).
6. `vdd_g3d` regulator by global name (`BUCK4`, always-on/boot-on,
   0.6–1.3 V window covers every stock OPP).
7. Everything timing/voltage/policy lives in C, not DT: 8-entry DVFS table
   (160–800 MHz), `mif_min_table`, ASV (`mach/asv-exynos`), TMU throttle
   ladder (`GPU_TEMP_THROTTLING1..8` → 800..160 MHz, `GPU_TMU_CONTROL=1`
   via thermal notifier, no DT cooling map), governor/interactive tunables.

Net: **kbase needs zero DT additions — stock DT is already sufficient.**
Any new consumer MUST meet the same bar (section 4), because `dt.img` is frozen.

## 4. Driver reliance plan for new consumers (panfrost / devfreq / power)

Rule: new code probes the stock node **as-is**. Each missing property below
has an in-driver default; none may become a hard DT requirement.

| Needed by new consumer | In stock DT? | Rely on | Fallback if missing (driver default) |
|------------------------|--------------|---------|--------------------------------------|
| OF match | `arm,mali` only | Match `arm,mali` as an Exynos7580/T720 alias (SoC-gated, commented) in addition to `arm,mali-t720`; never require the new string in DT | Probe by `reg` address `0x11400000` match as last resort; refuse other `arm,mali` SoCs by SoC gate, not by DT |
| `reg` 0x11400000/0x5000 | yes | `platform_get_resource(IORESOURCE_MEM, 0)` | Hard requirement (present); `dev_err` + `-ENOENT` if absent — no default possible, fail loudly |
| 3 IRQs JOB/MMU/GPU | yes, in order | Index-based mapping (0/1/2), verify count == 3 | Same as kbase: ignore `interrupt-names`; `-ENOENT` unless exactly 3 IRQ resources |
| 6 clocks | yes, by name | `devm_clk_get(dev, "<stock name>")` with the six stock `clock-names` | All six are required (present in stock). Optional 7th (panfrost-style `bus` clock): `devm_clk_get_optional`, tolerate `NULL`; never spin on `-EPROBE_DEFER` — bound retries, continue degraded-but-alive |
| `mali-supply` / `vdd_g3d` | **no phandle** (rail exists, name-bound) | `devm_regulator_get_optional(dev, "mali")` first, then legacy `regulator_get(NULL, "vdd_g3d")` path | Tolerate `-ENODEV`: rail is `always-on`+`boot-on` in stock; assume powered, keep the stock voltage ladder in C, log that voltage writes are skipped |
| OPP table (`operating-points-v2`) | **no** | Ship the stock ladder as a static C table (MHz: 160, 266, 350, 440, 534, 668, 734, 800; uV: 900k, 900k, 1000k, 1000k, 1000k, 1100k, 1100k, 1100k; MIF QoS kHz: 416, 416, 559, 667, 728, 825, 825, 825) | Parse `operating-points-v2` **opportunistically** (`devm_pm_opp_of_add_table`); on `-ENODEV`/absent, fall back to the static table. Cap top at 800 MHz regardless of source |
| Power domain | yes (`pd-g3d`, `pd-option 1`) | Generic `samsung,power-domain` binding (already works) | kbase-style raw PMU `LOCAL_PWR_CFG` sequence as backup; never leave the domain off at probe end |
| Devfreq governor linkage (`bus_mif`/`bus_int`) | nodes exist, no GPU phandle | Reuse the C-side pm_qos pattern (MIF tiers above); only reference `bus_mif`/`bus_int` by existing compatible, never add phandles | If a bus node lookup fails, clamp to the 416 MHz MIF floor tier in-driver and continue |
| Thermal / TMU cooling | TMU nodes exist, **no GPU cooling map** | Keep the C throttle ladder (800→…→160 MHz) driven by the existing TMU notifier path | No DT cooling cells required; if TMU notify registration fails, pin to <=534 MHz and log (safe static derate, matches `GPU_TEMP_THROTTLING4`) |
| IOMMU / `memory-region` | **none for G3D** (correct: T720 has its own MMU) | Use the GPU-internal MMU; DMA via normal coherent/CMA pools | Require nothing from DT; `-ENODEV` on any `iommus` lookup is the expected path, not an error |
| `status` | absent (= okay) | Treat absent as enabled; only honour explicit `"disabled"` | n/a |
| `clock-rates`, `arm,malit6xx` quirks, `#cooling-cells`, `opp-shared`, `sram` | all absent | Ignore unconditionally | n/a — must not be referenced by new code |

Cross-slice notes: G31_Power owns the `vdd_g3d`/BUCK4 window (do not narrow
`regulator-min/max` assumptions beyond [600000, 1300000]); G31_Devfreq owns
the static OPP/governor tables (single copy — power/panfrost slices consume,
do not duplicate); G31_Kms/G31_Gem must not add `memory-region`/`iommus`
phandles (frozen DT, and unneeded). Any patch that parses a property not in
section 2 MUST implement the section 4 fallback in the same patch.

## 5. Notes / traps

- Decompile artifact, ignore: `LDO1 regulator-min-microvolt = "", "\f5";`
  (:3313) is a dtc string-decode glitch on an unrelated sensor LDO, not GPU.
- `interrupts` flag cell 0 on all three SPIs is verbatim stock and works with
  the shipping kbase (`IRQF_SHARED`, flags from resource); do not "fix" it.
- `mali` has no `status = "okay"` and no `aliases` entry; both are fine —
  absent status enables the node, and nothing looks the GPU up by alias.
- G3D PMU address `0x10464060` (PD node `reg`) vs G3D SFR `0x11400000`
  (mali `reg`) — different blocks, do not conflate in patches.
