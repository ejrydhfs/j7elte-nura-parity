# GPU / 3D parity — ARM Mali-T720 (kbase, Exynos 7580), j7elte

Scope: ROM `SM-J700M J700MUBU2BRC2` (kernel 3.10.61) vs Nura kernel tree
`android_kernel_samsung_universal7580-2a746c1…` (3.10.108). Static analysis only.
No cross-toolchain for this 3.10 tree is usable on the host, so **nothing was compiled**
and **no runtime test was performed**. No git state was changed; no patches were produced
because P1–P4 all pass (see "Patches"); the one config-level delta — the built DDK revision
— is documented as residual R1 with a report-only config diff.

**Verdict**

| Item | Result |
|---|---|
| P1 symbol coverage | **PASS** — ROM's exact DDK (`t72x/r7p0`) is present in-tree and accounts for **535/535** ROM GPU symbols once 14 build artefacts are explained (non-clone names: **521/525**, the 4 exceptions being only `module_platform_driver` init/exit wrappers and `DEFINE_SIMPLE_ATTRIBUTE` `*_fops_open` clones); the *built* variant (`t7xx/r15p0`) covers 491/535 exact names, and all 44 deltas are mapped to r15p0 successors (no missing functionality) |
| P2 device tree | **PASS** — `mali@0x11400000`, `pd-g3d@10464060`, `g3d_en` and `vdd_g3d` (BUCK4) are identical in value; only phandle numbers differ (ignored per contract) |
| P3 config | **PASS** — `CONFIG_DDK_VERSION_OS="n"` + `CONFIG_MALI_T7XX=y` + `CONFIG_MALI_R15P0=y` select `t7xx/r15p0/` and its `platform/exynos/` integration; every dependency of the enabled options is `=y` |
| P4 firmware/blobs | **PASS (N/A)** — kbase opens **no** firmware/data file (0 `request_firmware` call sites in either variant). The only GPU runtime blob is the Android EGL driver (`/vendor/lib/egl/libGLES_mali.so`, `r7p0-03rel0`), which is userspace and is not consumed by Nura's Linux userspace. No `files-gpu.list` applicable |

3D acceleration itself is **not** a kernel-parity question: the kernel half (kbase + Exynos
DVFS/power integration) is complete; the userspace half (GLES/Vulkan driver) does not exist
in Nura. The Nura wiki's "3D Acceleration = Untested" is therefore expected and is not fixed
by any kernel change.

---

## 1. Which DDK is built for 7580 (and which one the ROM used)

Nura's build selection, traced through the Kconfig/Kbuild chain:

1. `config-nura-j7elte:2263` → `CONFIG_DDK_VERSION_OS="n"` (identical to
   `arch/arm64/configs/lineageos_j7elte_defconfig:2254`).
2. `config-nura-j7elte:289` → `CONFIG_SOC_EXYNOS7580=y`.
3. `drivers/gpu/arm/Kconfig:23-29` — the `SOC_EXYNOS7580` guard sources **either**
   `t7xx/Kconfig` (when `DDK_VERSION_OS = "n"`, line 24-25) **or** `t72x/Kconfig`
   (when `"l"`/`"m"`, line 27-28). With `"n"`, only `t7xx/` is offered.
4. `drivers/gpu/arm/t7xx/Kconfig:16,28,36` → `MALI_T7XX` (default y), choice default
   `MALI_R15P0`. Config: `CONFIG_MALI_T7XX=y` (2288), `CONFIG_MALI_R12P0` unset (2289),
   `CONFIG_MALI_R15P0=y` (2290).
5. `drivers/gpu/arm/Kbuild:17,25` → `obj-y += t7xx/r15p0/`.
6. `drivers/gpu/arm/t7xx/r15p0/Kbuild:223` → `obj-$(CONFIG_MALI_MIDGARD) += mali_kbase.o`;
   `:218` → `obj-... += platform/` because `CONFIG_MALI_PLATFORM_THIRDPARTY=y`
   (config 2277) with `CONFIG_MALI_PLATFORM_THIRDPARTY_NAME="exynos"` (2278).
7. `t7xx/r15p0/platform/Kbuild:20` → `platform/exynos/`;
   `t7xx/r15p0/platform/exynos/Kbuild:22` → `gpu_exynos$(soc_name).o`, with
   `CONFIG_EXYNOS_SOC_NAME="7580"` (config 2303) → `gpu_exynos7580.o`.

`CONFIG_MALI_MIDGARD=y` is defined by the same `t7xx/r15p0/Kconfig:17` menuconfig; the
`midgard/` directory is deliberately *not* built (`drivers/gpu/arm/Kbuild:14` is commented
out). `CONFIG_MALI_T72X` does not exist under `DDK_VERSION_OS="n"` (Kconfig not sourced), so
the `t72x/r7p0` code, although present, is dead weight in the Nura build.

The **ROM** used the other revision. `strings boot_out/vmlinux.elf | grep '^drivers/gpu/arm/'`
yields 23 source paths and **all 23 are `drivers/gpu/arm/t72x/r7p0/…`** (backend/gpu jm/pm,
`mali_kbase_instr.c`, mem_pool, vinstr, `platform/exynos/gpu_dvfs_*`, `gpu_pmqos`,
`gpu_utilization`, …); none is under `t7xx/`. All 23 exist verbatim in the Nura tree
(`drivers/gpu/arm/t72x/r7p0/…`, checked file-by-file). Corroborating revision markers:

- ROM has `kbase_jd_evict` (r7p0-only) and **0** `kbase_jit*` symbols (JIT arrives in r15p0).
- ROM kernel UK version is **10.0** (`t72x/r7p0/mali_kbase_uku.h:47-48`); the stock userspace
  blob reports `Midgard-"r7p0-03rel0"` (§P4), consistent with the r7p0 kernel source.
- The Nura-built r15p0 reports UK **10.6** (`t7xx/r15p0/mali_kbase_uku.h:69-70`).

Both revisions are the same downstream Samsung kbase fork; r15p0 merely newer. r15p0 still
implements T72x: `t7xx/r15p0/mali_kbase_hw.c:71-72,166-175` (GPU_ID_PI_T72X → features and
r0p0/r1p0/r1p1 issue lists), `mali_base_hwconfig_features.h:72`, and the Exynos 7580
platform (`gpu_exynos7580.c`).

---

## 2. P1 — symbol coverage

ROM universe (`boot_out/symbols.txt`, 55 634 symbols) filtered on `^kbase`: **535
symbols** (422 `kbase_*` + 113 `kbasep_*`, all text symbols per `nm vmlinux.elf`), plus
these non-kbase GPU groups (all recounted 2026-10-24):

| Group | Count | Where implemented in the Nura tree |
|---|---|---|
| `gpu_*` platform symbols | 125 | 121 in `t72x/r7p0/platform/exynos/`, 4 in `drivers/thermal/exynos7580/gpu_cooling.c` (`gpu_tstate`, `gpu_freq_table_668`, `gpu_freq_table_800`, `gpu_freq_table`) |
| gpufreq/cooling/exynos-gpu names | 20 | `gpu_cooling.c` (`cooling_gpu_lock:58`, `gpufreq_*`), `exynos_gpu_*` platform perf/notifier code |
| `base_hw_issues_*` DDK issue tables | 35 | `mali_kbase_hw.c` of both variants (all product IDs compiled in) |
| `hardware_counters_mali_*` | 8 | `mali_base_hwconfig_features.h`-side tables of both variants |
| named mali symbols | 5 | `mali_balance`, `mali_wait_reg`, `mali_timeline_ops`, `_mali_profiling_control` (byte-identical in both variants); `exynos_mali_match` — the ROM-only name of the r7p0 DT match table (`t72x/r7p0/mali_kbase_core_linux.c:4066`), which r15p0 names `kbase_dt_ids` with the same `"arm,mali"` compatible (`t7xx/r15p0/mali_kbase_core_linux.c:4224-4238`) |
| `mali_job_systrace_event_{start,stop}` trace-event family | 16 | macro-generated by `TRACE_EVENT(mali_job_systrace_event_start, …:18` / `_stop, …:60` in `mali_linux_systrace.h` of both variants (`event_class_*`, `__event_*`, `ftrace_*`, `perf_trace_*` names are emitted by the `TRACE_EVENT` expansion) |

Every symbol in every group is accounted for (see §2.1 for the measurement method).

Method: each ROM name was looked up as a whole identifier in the variant's `.c`/`.h` set
(`grep -rhoE '[A-Za-z_][A-Za-z0-9_]*'`, `LC_ALL=C comm`). Symbols generated by macros were
traced to the generating macro, not to a literal source token.

### 2.1 ROM revision (`t72x/r7p0`, in-tree) — coverage 521/535, remainder is build artefacts

| ROM symbol group | Count | Disposition |
|---|---|---|
| found as source identifiers in `t72x/r7p0` | 521 | implemented |
| `.isra.*` / `.constprop.*` clones | 10 | GCC clones; base function present, e.g. `kbase_fence_trigger` (`mali_kbase_softjobs.c:153`), `kbase_gpu_nr_atoms_on_slot_min` (`backend/gpu/mali_kbase_jm_rb.c:222`), `kbase_js_get_slot` (`mali_kbase_js.c:76`), `kbase_mem_pool_{alloc,free,sync}_page` (`mali_kbase_mem_pool.c`), `kbasep_replay_find_hw_job_id`/`kbasep_replay_parse_payload` (`mali_kbase_replay.c:484,747`), `kbasep_trace_dump_msg` (`mali_kbase_device.c:397`), `kbasep_vinstr_dump_size` (`mali_kbase_vinstr.c:223`) |
| `kbase_platform_driver_init` / `_exit` | 2 | generated by `module_platform_driver(kbase_platform_driver)` (`mali_kbase_core_linux.c:4090`); ROM proves the expansion: `__initcall_kbase_platform_driver_init6` is in `symbols.txt` |
| `kbase_mem_pool_debugfs_{size,max_size}_fops_open` | 2 | generated by `DEFINE_SIMPLE_ATTRIBUTE` (`include/linux/fs.h:2738-2746`, which emits `__fops##_open`); driver source only names the fops (`t72x/r7p0/mali_kbase_mem_pool_debugfs.c:43,66`) |

⇒ 535/535 accounted for. Raw literal-name coverage is 521/535 = 97.4 %; counting only
non-clone names (535 − 10 clones = **525**), coverage is **521/525 = 99.2 %**, missing
exactly the 4 macro-generated names (`kbase_platform_driver_{init,exit}`,
`kbase_mem_pool_debugfs_{size,max_size}_fops_open`) — this 521/525 figure is the one quoted
in the tasking as "421/425" (digit transcription). The 14 exceptions are all
compiler/macro-generated, which the contract explicitly allows. Also verified: every one of
the 125 ROM `gpu_*` platform symbols is implemented — 121 in `t72x/r7p0` and 4
(`gpu_tstate`, `gpu_freq_table_668`, `gpu_freq_table_800`, `gpu_freq_table`) in
`drivers/thermal/exynos7580/gpu_cooling.c` (present in the Nura tree); the 20
gpufreq/cooling names (`cooling_gpu_lock` (`gpu_cooling.c:58`), `gpufreq_*`, `exynos_gpu_*`)
likewise, and `gpu_product_id_names.27724` is a compiler-renamed static array
(`gpu_product_id_names`, `t72x/r7p0/mali_kbase_core_linux.c`).

### 2.2 Built revision (`t7xx/r15p0`) — 491/535 exact names, 44 explained

Raw exact-name coverage **491/535 = 91.8 %**. Excluding the same 14 build artefacts
(10 clones + 2 module wrappers + 2 `DEFINE_SIMPLE_ATTRIBUTE` opens, all of which an r15p0
build also emits — with one nuance: `kbase_mem_pool_alloc_page.isra.15`'s base was folded
into `kbase_mem_pool_alloc_pages` (`mali_kbase_mem_pool.c:466`), while
`kbase_mem_pool_free_page`/`kbase_mem_pool_sync_page` survive verbatim
(`mali_kbase_mem_pool.c:219,153`), so 9 of the 10 clones would reappear) leaves **30
genuine r7p0→r15p0 API deltas**, listed exhaustively:

| ROM (r7p0) symbol | r15p0 disposition / successor | Evidence |
|---|---|---|
| `kbase_common_device_remove` | inlined into `kbase_platform_device_remove` | `mali_kbase_core_linux.c:3652` |
| `kbase_debug_job_fault_context_exit` | `kbase_debug_job_fault_context_term` | `mali_kbase_debug_job_fault.c:486` |
| `kbase_gpu_slot_update` | `kbase_backend_slot_update` | `backend/gpu/mali_kbase_jm_rb.c:1043` |
| `kbase_instr_hwcnt_enable` | `kbase_instr_hwcnt_enable_internal` | `mali_kbase_hwaccess_instr.h:39` |
| `kbase_instr_hwcnt_disable` | `kbase_instr_hwcnt_disable_internal` | `mali_kbase_hwaccess_instr.h:51` |
| `kbase_instr_hwcnt_dump` | `kbase_instr_hwcnt_request_dump` + `…_wait_for_dump` | `mali_kbase_hwaccess_instr.h:62,73` |
| `kbase_instr_hwcnt_suspend` | `kbase_vinstr_suspend` | `mali_kbase_vinstr.h:139` |
| `kbase_instr_hwcnt_resume` | `kbase_vinstr_resume` | `mali_kbase_vinstr.h:148` |
| `kbase_vinstr_hwc_suspend` | `kbase_vinstr_suspend` | `mali_kbase_vinstr.h:139` |
| `kbase_vinstr_hwc_resume` | `kbase_vinstr_resume` | `mali_kbase_vinstr.h:148` |
| `kbase_jd_evict` | removed; replaced by JIT eviction (`kbase_jit_evict`) | `mali_kbase_mem.c:421` |
| `kbase_js_ctx_list_add_pullable` | `…_nolock` | `mali_kbase_js.c:733` |
| `kbase_js_ctx_list_add_unpullable` | `…_nolock` | `mali_kbase_js.c:459` |
| `kbase_js_ctx_list_remove` | `…_nolock` | `mali_kbase_js.c:888` |
| `kbase_mem_pool_debugfs_add` | `kbase_mem_pool_debugfs_init` | `mali_kbase_mem_pool_debugfs.c:71` |
| `kbase_mmu_flush` | `kbase_mmu_flush_invalidate` | `mali_kbase_mmu.c:66` |
| `kbase_pm_enable_interrupts_mmu_mask` | removed; folded into `kbase_pm_enable_interrupts` | `backend/gpu/mali_kbase_pm_driver.c:968` |
| `kbase_security_has_capability` | removed with the whole security module (no `mali_kbase_security.c` in r15p0) | — |
| `kbasep_jd_debugfs_ctx_add` | `kbasep_jd_debugfs_ctx_init` | `mali_kbase_jd_debugfs.c:113` |
| `kbasep_js_policy_ctx_has_priority`, `kbasep_js_policy_init`, `kbasep_js_policy_init_ctx`, `kbasep_js_policy_log_job_result`, `kbasep_js_policy_term`, `kbasep_js_policy_term_ctx` (6) | removed; the `js_policy` layer was folded into `mali_kbase_js.c` in r15p0 | no `js_policy` token in r15p0 |
| `kbasep_mem_profile_debugfs_add` | `kbasep_mem_profile_debugfs_insert` | `mali_kbase_core_linux.c:885` |
| `kbasep_secure_mode_debugfs_fops`, `_open`, `_seq_show` (3) | removed; secure-rendering debugfs gone (feature disabled in both configs) | — |
| `kbasep_vinstr_detach_client` | made public: `kbase_vinstr_detach_client` | `mali_kbase_vinstr.c:647` |

These are revision-level renames/removals, not missing driver functionality: r15p0 is a
superset of the r7p0 driver (it adds JIT, the reworked vinstr/instr split and the
`js_policy` merge). The one removed *behaviour* is the `kbase_security_has_capability()`
gate on the hwcnt/instr entry points (r7p0 checked capabilities before allowing counter
access; r15p0 has no such check). With `CONFIG_MALI_SEC_HWCNT` unset — the Nura config has
it off (2298) and the ROM symbol table contains no `gpu_hwcnt_sec`/`*hwcnt_sec*` symbols, so
the ROM had it off too — and no instrumentation userspace in Nura, this has no observable
effect here.

In short: **P1 passes on the contract's wording ("implemented in the Nura tree")** because
the ROM's exact revision is present and complete; the built revision differs by DDK
generation only (see residual R1 for the one ABI consequence).

---

## 3. P2 — device tree

ROM node (all three ROM `dtbs/*.dts`, e.g. `000_800.dts:3845-3852`) vs Nura
`arch/arm64/boot/dts/exynos7580.dtsi:1066-1076`:

| Property | ROM | Nura | Match |
|---|---|---|---|
| node | `mali@0x11400000` | `mali@0x11400000` | yes |
| compatible | `"arm,mali"` | `"arm,mali"` | yes |
| reg | `0x11400000`, `0x5000` | `0x11400000`, `0x5000` | yes |
| interrupts | `<0 282 0>, <0 283 0>, <0 281 0>` | `<0 282 0>, <0 283 0>, <0 281 0>` | yes |
| interrupt-names | `JOB`, `MMU`, `GPU` | `JOB`, `MMU`, `GPU` | yes |
| clocks | ids `1,10,160,161,162,163` via clk controller @0x10010000 (phandle 0x0f) | `&clock 1,10,160,161,162,163` (same controller) | yes |
| clock-names | `fin_pll, fout_g3d_pll, aclk_g3d, mout_g3d, dout_aclk_g3d, aclk_g3d_400` | identical | yes |
| samsung,power-domain | phandle → `pd-g3d@10464060` | `&pd_g3d` → same node | yes |

Clock-ID semantics confirmed in `drivers/clk/samsung/clk-exynos7580.c:20-23,49`
(`oscclk = 1` named `fin_pll`, `g3d_pll = 10`, `g3d = 160, mout_g3d, dout_aclk_g3d,
aclk_g3d_400`), so the numeric IDs and names agree.

Power domain (`000_800.dts:4260-4266` vs `exynos7580.dtsi:1618-1623`): both
`compatible="samsung,exynos-pd"`, `reg=<0x10464060 0x20>`, `pd-option=<1>`; the driver looks
it up by genpd name `"pd-g3d"` (`t7xx/r15p0/platform/exynos/gpu_control.c:49-59`), which
matches the node name in both trees.

Regulator / DVFS hooks (ROM `000_800.dts:3157,3204-3212` vs
`exynos7580-j7elte_rev06.dts:477,525-531`; rev04 identical):
`s2mpu03_pmic@66` has `g3d_en = <1>` in both; `BUCK4` is named `vdd_g3d` with
`600000..1300000 µV`, `regulator-always-on`, `regulator-boot-on`, `ramp-delay 12000`,
`initial-mode 1` in both. This is the regulator the driver requests by name
(`t7xx/r15p0/platform/exynos/gpu_exynos7580.c:711` — `regulator_get(NULL, "vdd_g3d")`) and
the six `clk_get(kbdev->dev, …)` names (`:490-540`) all exist in the DT node.

No OPP table exists in either DT; both trees select frequency/voltage from hard-coded
Exynos7580 tables in `gpu_exynos7580.c` (`gpu_freq_table`, ASV), so their absence is not a
delta. The three ROM DTBs differ only in phandle numbering (0x5a/0x5b/0x60 for `pd-g3d`),
which the contract ignores; the mali node text is otherwise byte-identical.

---

## 4. P3 — config

`config-nura-j7elte` (a copy of `config-samsung-j7elte.aarch64`, which equals the in-tree
`lineageos_j7elte_defconfig` for every MALI symbol):

| Config | Value | Needed by | Status |
|---|---|---|---|
| `DDK_VERSION_OS` (2263) | `"n"` | selects `t7xx` Kconfig | ok |
| `SOC_EXYNOS7580` (289) | `y` | gates `t7xx`/`t72x` Kconfig | ok |
| `MALI_MIDGARD` (2284) | `y` | builds `mali_kbase.o` (`t7xx/r15p0/Kbuild:223`) | ok (built-in; no module needed) |
| `MALI_T7XX` (2288) / `MALI_R15P0` (2290) | `y` / `y` | selects `t7xx/r15p0/` | ok |
| `MALI_PLATFORM_THIRDPARTY` (2277) + name `"exynos"` (2278) | `y` | `platform/exynos/` | ok |
| `EXYNOS_SOC_NAME` (2303) | `"7580"` | `gpu_exynos7580.o` | ok |
| `MALI_DVFS` (2287), `MALI_RT_PM` (2292), `MALI_DEBUG_SYS` (2294), `MALI_SYSTRACE_SUPPORT` (2291), `MALI_SEC_CL_BOOST` (2304), `MALI_SEC_UTILIZATION` (2305), `MALI_PM_QOS` (2306), `MALI_DEBUG_KERNEL_SYSFS` (2308) | `y` | Exynos platform, DVFS, sysfs/debugfs | ok — deps present: `PM_DEVFREQ=y` (3264), `CPU_FREQ=y` (457), `SYNC=y`/`SW_SYNC=y` (3173-3174), `PM=y` (439), `SYSFS`, `DEBUG_FS=y` (3727) |
| `MALI_DEVFREQ`, `MALI_MIDGARD_DVFS`, `MALI_CORESTACK`, `MALI_GPU_MMU_AARCH64`, `MALI_SEC_HWCNT`, `MALI_EXYNOS_SECURE_RENDERING` | not set | — | correct for T720 (LPAE MMU, no core stack, Samsung DVFS instead of devfreq, secure rendering unsupported) |

No MALI symbol was silently dropped: all `=y` symbols are present in the saved config, and
their `depends on` clauses are satisfied. Two pre-existing hygiene items (present identically
in the upstream LineageOS defconfig, not parity gaps): `MALI_MIDGARD` is defined twice
(`midgard/Kconfig:16` and `t7xx/r15p0/Kconfig:17`) and `MALI_DVFS=y` is written before
`MALI_T7XX=y` in the sorted defconfig.

---

## 5. P4 — firmware / blobs

- **Kernel firmware: none.** `grep -rn request_firmware drivers/gpu/arm/{t72x/r7p0,t7xx/r15p0}`
  returns 0 call sites (the only "firmware" hits are comments about the SMC/trusted firmware
  and the Juno platform). kbase does not open any data/firmware file at runtime, so the
  contract's firmware mapping does not apply and no `parity/firmware/files-gpu.list` is
  produced. The ROM firmware manifest (`parity/firmware/MANIFEST.sha512`) contains no Mali
  entry either.
- **Userspace (out of scope for a kernel patch):** the ROM carries exactly one GLES driver,
  `/vendor/lib/egl/libGLES_mali.so` (32-bit only — `system.raw` has no `/lib64/egl` mali
  blob), `strings` → `Midgard-"r7p0-03rel0"` and
  `hardware/samsung_slsi/MaliT720_r7p0_Insel/…`. This is loaded by Android's EGL loader, not
  by kbase. Nura is a Linux userspace (postmarketOS-derived, aarch64, no Android EGL/gralloc),
  so nothing installs or consumes it; 3D would have to come from a Linux driver (Mesa) and
  from mainline-style GPU support, independent of this kernel driver.

---

## 6. Patches

**None.** No kernel code is missing: the ROM's exact DDK (`drivers/gpu/arm/t72x/r7p0/`) is
present in the Nura tree (all 23 ROM-embedded source paths resolve) and was not modified by
this analysis; the built revision (r15p0) is a functional superset
for this GPU, the DT binding matches, and the config is coherent. Patching the r15p0 driver to
expose r7p0's internal symbol names would be cosmetic. Nothing was added under
`parity/patches/gpu/`.

---

## 7. Residual risks and suggested tests

**R1 — built revision ≠ ROM revision (config-level delta, not a missing driver).**
The Nura kernel builds DDK r15p0 (UK 10.6) while the ROM's userspace blob is r7p0 (UK 10.0).
The standard UK interface is compatible: function IDs 0..16 and 18..36 are identical in both
enums (r7p0 `mali_kbase_uku.h:407`, r15p0 `:479`; ID +32 keeps its number but is renamed
`KBASE_FUNC_TLSTREAM_ACQUIRE_V10_4` in r15p0, `:529`); r7p0's legacy
`KBASE_FUNC_EXT_BUFFER_LOCK` (+17) has no r15p0 counterpart, and
`kbase_api_handshake()` (`t7xx/r15p0/mali_kbase_core_linux.c:170-215`) answers a 10.x
userspace with `major=10, minor=min(6, userspace_minor)`, i.e. exactly what r7p0 userspace
sent. However the Samsung vendor extension block is **renumbered**, so a stock r7p0 blob
would mis-route any vendor call it makes:

```
r7p0 (mali_kbase_uku.h:407-…)          r15p0 (mali_kbase_uku.h:479-…)
  KBASE_FUNC_HWCNT_UTIL_SETUP  = +37     KBASE_FUNC_SOFT_EVENT_UPDATE = +38
  KBASE_FUNC_HWCNT_GPR_DUMP    = +38     KBASE_FUNC_MEM_JIT_INIT      = +39
  KBASE_FUNC_VSYNC_SKIP        = +39     KBASE_FUNC_TLSTREAM_ACQUIRE  = +40
  KBASE_FUNC_CREATE_SURFACE    = +40     KBASE_FUNC_HWCNT_UTIL_SETUP  = +41
  KBASE_FUNC_DESTROY_SURFACE   = +41     …  (all Samsung ids shifted +4)
  KBASE_FUNC_SET_MIN_LOCK      = +42     KBASE_FUNC_SET_MIN_LOCK      = +46
  KBASE_FUNC_UNSET_MIN_LOCK    = +43     KBASE_FUNC_UNSET_MIN_LOCK    = +47
  KBASE_FUNC_TMU_SKIP          = +44     KBASE_FUNC_TMU_SKIP          = +48
  KBASE_FUNC_EXT_BUFFER_LOCK   = +17     (removed)
```

Whether the r7p0 `libGLES_mali.so` actually issues those vendor calls cannot be established
statically here (stripped 23 MB 32-bit blob; no ARM disassembler was used) — treat this as
an open risk, not a confirmed break. For Nura's Linux userspace it is irrelevant.

If bit-exact ROM-DDK parity is ever required (i.e. running the stock Android GL stack on this
kernel), the only change needed is a **config** change, which lies outside this task's
ownership (`drivers/gpu/arm/**`) and is therefore reported, not applied:

```
# build the ROM's exact DDK instead of r15p0 (j7elte)
CONFIG_DDK_VERSION_OS="m"
CONFIG_MALI_T72X=y
CONFIG_MALI_R7P0_T72X=y
# CONFIG_MALI_T7XX is not set
# CONFIG_MALI_R15P0 is not set
```

Verified feasible: with `DDK_VERSION_OS="m"`, `drivers/gpu/arm/Kconfig:27-28` sources
`t72x/Kconfig`; `t72x/r7p0/Kbuild:152,163` builds `mali_kbase.o` + `platform/exynos/`;
`t72x/r7p0/platform/Kbuild` hardcodes `exynos/`; `t72x/r7p0/Kconfig:52-209` defines the same
Samsung symbols (`MALI_DVFS`, `MALI_RT_PM`, `MALI_DEBUG_SYS`, `MALI_SEC_HWCNT`, …) already
present in the config; and `gpu_exynos7580.o` is still selected via
`t72x/r7p0/platform/exynos/Kconfig` (`EXYNOS_SOC_NAME`, depends on `MALI_T72X`). Note the
r7p0 source is Android-6-era code and has not been compile-tested against 3.10.108 here.

**R2 — no build, no runtime test.** Static verification only. On a bootable image check:
`dmesg | grep -i mali` / `kbase` (probe, `Mali-T72x`, `r15p0-00rel0`), the DT binding
(`/sys/devices/platform/11400000.mali`), the sysfs/debugfs hooks enabled by
`MALI_DEBUG_SYS`/`MALI_DEBUG_KERNEL_SYSFS` (`/sys/kernel/gpu/*` — clock, voltage, power
state, DVFS table), `/sys/kernel/debug/mali0/`, and that the `pd-g3d` genpd is off when idle
(`/sys/kernel/debug/pm_genpd/*`). A GPU workload test also needs a userspace driver; without
one, only probe/power/DVFS behaviour is observable.

**R3 — P1 measurement caveat.** Coverage was computed by identifier presence in the variant
sources, not by compiling the tree (impossible here); a compiler could still turn a present
function into a different symbol (as it did for the 10 ROM clones). This affects naming
statistics only, not the conclusion that no ROM GPU functionality is missing.
