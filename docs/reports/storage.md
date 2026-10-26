# Storage parity — `dw_mmc-exynos` (eMMC / SD), j7elte

**Verdict: PASS (P1–P4). No patch required.**

| | |
|---|---|
| ROM baseline | SM-J700M `J700MUBU2BRC2`, kernel `3.10.61-13213628`, Exynos 7580 |
| Nura baseline | `android_kernel_samsung_universal7580` @ `2a746c1`, git baseline `b5e363a`, kernel 3.10.108 |
| Covered paths | `drivers/mmc/host/dw_mmc.c`, `dw_mmc-exynos.c`, `dw_mmc-pltfm.c` (+ `dw_mmc.h`, `dw_mmc-exynos.h`, `dw_mmc-pltfm.h`) |
| ROM evidence | `boot_out/symbols.txt`, `boot_out/vmlinux.elf`, `boot_out/dtbs/000_800.dts`, `/tmp/rom_000_800.sorted.dts` |
| Nura evidence | source tree at `2a746c1`, `/tmp/nura_j7.sorted.dts`, `config-nura-j7elte` |
| Method | static analysis only — no aarch64 kernel toolchain is installed (contract limitation); no kernel build was attempted |

---

## P1 — Symbol coverage: PASS (113/113 accounted for)

`grep -E 'dw_mci' boot_out/symbols.txt` returns exactly **113** symbols (the same set the task quotes).

| Result | Count | Share |
|---|---|---|
| Resolved to a definition/identifier in `drivers/mmc/host/` of the Nura tree | 106 | 93.8% |
| Macro- or linker-generated names (explained below) | 7 | 6.2% |
| **Effective coverage** | **113** | **100%** |

Raw hit rate 93.8% is below the 98% bar, but *every* exception is explained and is not a
source gap; the source constructs that emit them are identical in both trees.

Distribution of the 106 located symbols:

| File | Symbols |
|---|---|
| `drivers/mmc/host/dw_mmc.c` | 73 |
| `drivers/mmc/host/dw_mmc-exynos.c` | 24 |
| `drivers/mmc/host/dw_mmc-pltfm.c` | 9 |

Samsung-specific state/objects that would be easy to lose are all present:
`dw_mci_lpa_host` (`dw_mmc-exynos.c:37`), `dw_mci_host_count` (`:38`), `dw_mci_save_sfr`
(`:39`), `dw_mci_exynos_misc_control` (`:1664`), `dw_mci_exynos_register_dump` (`:490`),
`dw_mci_regs_fops` / `dw_mci_req_fops` (`dw_mmc.c:558` / `:533`), `dw_mci_tp_mon`
(`dw_mmc.c:3601`). GCC clone suffixes in the ROM list (`dw_mci_exynos_set_enable_shift.isra.1`,
`dw_mci_exynos_set_sample.constprop.11`) are compiler artifacts of the ROM's gcc; the base
functions exist in the Nura source (`dw_mmc-exynos.c:880`, `:927`).

### The 7 "missing" symbols are all macro/linker generated

| ROM symbol | Why it is not a literal in the Nura source | ROM address (`nm` of `vmlinux.elf`) |
|---|---|---|
| `dw_mci_pltfm_driver_init` | expansion of `module_platform_driver(dw_mci_pltfm_driver)` at `drivers/mmc/host/dw_mmc-pltfm.c:148` | `ffffffc000e76a4c t` |
| `dw_mci_pltfm_driver_exit` | same expansion (`__driver##_exit`) | `ffffffc000e92478 t` |
| `dw_mci_exynos_pltfm_driver_init` | expansion of `module_platform_driver(dw_mci_exynos_pltfm_driver)` at `drivers/mmc/host/dw_mmc-exynos.c:1770` | `ffffffc000e76a68 t` |
| `dw_mci_exynos_pltfm_driver_exit` | same expansion | `ffffffc000e92494 t` |
| `__initcall_dw_mci_pltfm_driver_init6` | linker-visible token paste `__initcall_##fn##id` | `ffffffc000ea3c68 t` |
| `__initcall_dw_mci_exynos_pltfm_driver_init6` | same | `ffffffc000ea3c70 t` |
| `__initcall_dw_mci_init6` | from `module_init(dw_mci_init)` at `drivers/mmc/host/dw_mmc.c:5198` | `ffffffc000ea3c60 t` |

Evidence that these names are produced by the *same* source construct in the Nura tree:

- `include/linux/platform_device.h:208-210` — `module_platform_driver(drv)` → `module_driver(drv, platform_driver_register, platform_driver_unregister)`.
- `include/linux/device.h:1145-1155` — `module_driver()` defines `static int __init <drv>_init(void)` and `static void __exit <drv>_exit(void)`, then `module_init()/module_exit()`. Substituting the two driver struct names (`dw_mmc-pltfm.c:138` `dw_mci_pltfm_driver`, `dw_mmc-exynos.c:1759` `dw_mci_exynos_pltfm_driver`) reproduces all four `_init`/`_exit` names exactly.
- `include/linux/init.h:190-192` — `__define_initcall(fn, id)` emits `static initcall_t __initcall_##fn##id __used __section(".initcall"#id".init")`, i.e. the `__initcall_*6` names are produced by token pasting and never appear literally in driver source.

`dw_mci_init` / `dw_mci_exit` themselves **are** present in the Nura source
(`dw_mmc.c:5188` definition, `:5194` exit, `:5198-5199` `module_init/module_exit`), so
`__initcall_dw_mci_init6` is covered for the same reason.

Why the ROM image nevertheless contains `*_driver_exit` **and** `*_driver_init` as local
text symbols: whether the `__exit` bodies survive is decided by the build (linker discarding
`.exit.text`) and by the compiler not folding tiny wrappers, i.e. it is a property of the ROM's
toolchain/config, not of the driver sources. Static verification cannot enumerate the local
symbol table of a Nura image that was not built here; the source constructs are identical.

Residual P1 caveat: symbols prove the ROM's function inventory is present in the Nura tree;
they cannot prove byte-level behavioural identity between 3.10.61 and 3.10.108.

---

## P2 — Device tree: PASS (dwmmc nodes are byte-identical)

Comparison of the two *compiled* DT blobs (ROM `000_800.dtb` vs Nura j7elte build), sorted,
storage node ranges:

```
diff -u <(sed -n '642,790p' /tmp/rom_000_800.sorted.dts) \
        <(sed -n '632,780p' /tmp/nura_j7.sorted.dts)
→ no differences (identical)
```

All three nodes and their `slot@0` children are identical: `compatible`, `reg`,
`interrupts`, `clocks`/`clock-names`, `status`, `num-slots`, all capability flags,
`fifo-depth`, `card-detect-delay`, `data-timeout`, `desc-size`, `qos_int_level`,
`samsung,dw-mshc-*-timing`, delay lines, `clk_pin`/`clk_addr`/`clk_val`/`clk_str_num`,
`num-ref-clks`, `ciu_clkin`, `tp_mon_depth`/`tp_mon_table`, `pinctrl-names`/`pinctrl-0`/`pinctrl-1`.

| Item | dwmmc0 (eMMC) | dwmmc1 (SDIO/WLAN) | dwmmc2 (SD card) |
|---|---|---|---|
| ROM DT offset | `dtbs/000_800.dts:3442` | `:3512` | `:3557` |
| Nura source | `arch/arm64/boot/dts/exynos7580-j7elte_rev04.dts:36` | `:84` | `:123` |
| base node | `arch/arm64/boot/dts/exynos7580.dtsi:766` | `:791` | `:802` |
| compatible | `samsung,exynos7580-dw-mshc` | same | same |
| reg / irq | `0x13540000+0x2000` / `<0 0xf5 0>` | `0x13550000` / `0xf6` | `0x13560000` / `0xf7` |
| bus-width | `<8>` | `<4>` | `<4>`, `disable-wp` |
| card detect | `broken-cd` | `cd-type-external` | `cd-type-gpio`, `cd-gpio = <&gpa3 4 0xf>`, `sec-sd-slot-type = <1>` |
| CD pinmux | — | — | `dwmmc2_cd_ext_irq` = `gpa3-4`, fn `0x0f`, pud `0x3`, drv `0x0` (`exynos7580-j7elte_rev04.dts:256`; `000_800.dts:386`) — identical in both |
| timing (sdr / ddr) | `<3 0 2 0>` / `<3 0 2 2>` | `<3 0 2 1>` / `<3 0 2 1>` | `<3 0 2 0>` / `<3 0 2 1>` |
| HS signalling | `supports-highspeed`, `supports-hs200-mode`, `supports-ddr200-mode`, `supports-ddr200-enhanced-strobe`, ddr200 delay lines 40/40 | `supports-highspeed` | `supports-highspeed`, `supports-sdr104-mode` |
| regulators | `fixed_volt`; no `vmmc-supply`/`vqmmc-supply` on any node in either tree (the `vmmc`/`vqmmc` fixed-regulator nodes exist at `000_800.dts:1292`/`:1247` and nura `1293`/`1250` but are not wired to the mmc nodes) | same | same |

Note on "mmc-hs timing": neither ROM nor Nura uses the upstream `mmc-hs200-1_8v` /
`mmc-ddr-1_8v` bindings; high-speed modes are expressed by `supports-*` flags plus
`samsung,dw-mshc-*-timing` tuples, which are identical.

Driver-side consumers: every property present in the ROM nodes is read by the Nura driver
(`dw_mmc.c` / `dw_mmc-exynos.c`), checked individually — `only_once_tune`, `enable-ulp-mode`,
`use-enable-shift`, `supports-poweroff-notification`, `enable-cache-control`,
`bypass-for-allpass`, `ignore-phase`, `use-fine-tuning`, `qos_int_level`, `desc-size`,
`tp_mon_depth`, `tp_mon_table`, `clk_pin`, `clk_addr`, `clk_val`, `clk_str_num`,
`num-ref-clks`, `ciu_clkin`, `cd-type-gpio`, `cd-type-external`, `sec-sd-slot-type`,
`cd-gpio`, `caps-control`, `use-broken-voltage`, `pm-ignore-notify`,
`supports-sdr104-mode`, `use-smu`, `bypass-smu`, `fixed_volt`, `clock-gate`,
`enable-cclk-on-suspend`, `data-timeout`, `supports-ddr200-enhanced-strobe`,
`supports-ddr200-mode`, `supports-hs200-mode`, `num-slots`. The 7580 compatible string is
matched at `dw_mmc-exynos.c:1740`.

DT deltas found anywhere in the storage-adjacent set: **one, additive, non-driver** — the Nura
DT contains an extra `firmware/android/fstab/system` node
(`dev = "/dev/block/platform/13540000.dwmmc0/by-name/SYSTEM"`, `/tmp/nura_j7.sorted.dts:1057`)
that the ROM DT does not have. It is an Android/init concern, references the same eMMC
controller (`dwmmc0`), and does not alter driver binding.

DTB generation is intact: `arch/arm64/boot/dts/Makefile:13` lists
`exynos7580-j7elte_rev00/rev04/rev06.dtb` under `CONFIG_CAMERA_J7`, which is `=y`
(`config-nura-j7elte:2176`), and all three `.dts` files exist; pmaports builds `dt.img` from
`*j7elte*.dtb` (`parity/pmaports/device/archived/linux-samsung-j7elte/APKBUILD:44-46`), so a
j7elte DTB with the nodes above is present in the boot image.

---

## P3 — Config: PASS

From `config-nura-j7elte` (copy of `config-samsung-j7elte.aarch64`):

| Option | Value | Line |
|---|---|---|
| `CONFIG_MMC` | `y` | 2842 |
| `CONFIG_MMC_BLOCK` | `y` | 2853 |
| `CONFIG_MMC_DW` | `y` | 2871 |
| `CONFIG_MMC_DW_PLTFM` | `y` (also `select`ed by `MMC_DW_EXYNOS`) | 2876 |
| `CONFIG_MMC_DW_EXYNOS` | `y` | 2877 |
| `CONFIG_MMC_DW_IDMAC` | `y` | 2874 |
| `CONFIG_MMC_DW_64_IDMAC` | `y` | 2875 |
| `CONFIG_FMP` / `MMC_DW_FMP_DM_CRYPT` / `MMC_DW_FMP_ECRYPT_FS` | `y` / `y` / `y` | 1156 / 2872 / 2873 |
| `CONFIG_MMC_CLKGATE`, `CONFIG_MMC_BLOCK_DEFERRED_RESUME`, `CONFIG_MMC_UNSAFE_RESUME` | `y` | 2846 / 2856 / 2845 |
| `CONFIG_MMC_SDHCI*` | not set (ROM also uses dw_mmc only) | 2867-2869 |

Dependencies hold: `MMC_DW` depends on `ARM || ARM64` (`drivers/mmc/host/Kconfig:520-522`),
`MMC_DW_EXYNOS` depends on `MMC_DW` and selects `MMC_DW_PLTFM` (`Kconfig:580-584`),
`MMC_DW_IDMAC` depends on `MMC_DW` (`Kconfig:550-553`), `FMP` depends on `MMC_DW`
(`Kconfig:528-531`) and `=y` is set. Objects are built-in via
`drivers/mmc/host/Makefile:42-44` (`dw_mmc.o`, `dw_mmc-pltfm.o`, `dw_mmc-exynos.o`).

---

## P4 — Firmware / blobs: PASS (none required)

- No `request_firmware*` call anywhere in `drivers/mmc/` on the dw_mmc path; the only firmware
  users are `drivers/mmc/host/vub300.c` and card quirks, and `CONFIG_MMC_VUB300` is not set
  (`config-nura-j7elte:2881`).
- `parity/firmware/MANIFEST.sha512` contains no mmc/SD/eMMC blob (entries are bcmdhd, HCD,
  nvram, fimc-is, mfc).
- eMMC/SD tuning data is computed at runtime (`dw_mci_execute_tuning`,
  `dw_mci_exynos_execute_tuning`), no blob file is opened. No
  `parity/firmware/files-storage.list` is therefore produced.

---

## P5 — Patch: none

P1–P4 pass; no patch generated, nothing to apply. `git -C <tree> diff` is empty for
`drivers/mmc/**` (no edits were made).

---

## Residual risks / tests

1. **No runtime verification** — static only; no aarch64 cross-toolchain installed, so the
   Nura image was not built and local-symbol/linker behaviour (the 7 generated symbols) is
   inferred from macros, not observed in a Nura binary.
2. **Version gap** — ROM is 3.10.61, Nura is 3.10.108. Symbol names and DT match, but
   behavioural deltas inside `dw_mmc.c` (tuning, DMA, runtime-PM) cannot be ruled out from
   symbols alone. The ROM's source is unavailable for a textual diff.
3. **eMMC tuning** is runtime-calibrated (`only_once_tune`, `use-fine-tuning`,
   `enable-ulp-mode`, `supports-ddr200-enhanced-strobe`); a bad sample point would show up
   only on hardware, not in static checks.
4. Suggested hardware tests: boot and confirm `dwmmc_exynos` probe for
   `13540000.dwmmc0`/`13560000.dwmmc2` in `dmesg`; mount the eMMC rootfs; insert/remove an SD
   card (GPIO card-detect on `gpa3-4`); suspend/resume with `keep-power-in-suspend` on
   dwmmc1; check `/sys/kernel/debug/mmc0/ios` reports HS200/DDR50 as on stock; run a
   write/read stress on both eMMC and SD.

---

## Appendix — ROM symbol → Nura location (113 symbols)

First match of each ROM symbol in the Nura tree (`drivers/mmc/host/`); paths are relative to
the kernel tree root.

| ROM symbol | Nura tree location |
|---|---|
| `dw_mci_biu_clk_dis` | drivers/mmc/host/dw_mmc.c:463 |
| `dw_mci_biu_clk_en` | drivers/mmc/host/dw_mmc.c:445 |
| `dw_mci_calc_timeout` | drivers/mmc/host/dw_mmc.c:1742 |
| `dw_mci_card_busy` | drivers/mmc/host/dw_mmc.c:2276 |
| `dw_mci_ciu_clk_dis` | drivers/mmc/host/dw_mmc.c:418 |
| `dw_mci_ciu_clk_en` | drivers/mmc/host/dw_mmc.c:394 |
| `dw_mci_ciu_reset` | drivers/mmc/host/dw_mmc.c:909 |
| `dw_mci_cmd_reg_summary` | drivers/mmc/host/dw_mmc.c:609 |
| `dw_mci_command_complete` | drivers/mmc/host/dw_mmc.c:2336 |
| `dw_mci_detect_interrupt` | drivers/mmc/host/dw_mmc.c:3943 |
| `dw_mci_direct_attrs` | drivers/mmc/host/dw_mmc.c:171 |
| `dw_mci_disable_interrupt` | drivers/mmc/host/dw_mmc.c:639 |
| `dw_mci_dma_cleanup` | drivers/mmc/host/dw_mmc.c:980 |
| `dw_mci_dto_timer` | drivers/mmc/host/dw_mmc.c:2415 |
| `dw_mci_early_resume` | drivers/mmc/host/dw_mmc.c:5137 |
| `dw_mci_enable_interrupt` | drivers/mmc/host/dw_mmc.c:654 |
| `dw_mci_enable_sdio_irq` | drivers/mmc/host/dw_mmc.c:2137 |
| `dw_mci_execute_tuning` | drivers/mmc/host/dw_mmc.c:2046 |
| `dw_mci_exit` | drivers/mmc/host/dw_mmc.c:5194 |
| `dw_mci_exynos_cfg_smu` | drivers/mmc/host/dw_mmc-exynos.c:157 |
| `dw_mci_exynos_execute_tuning` | drivers/mmc/host/dw_mmc-exynos.c:1144 |
| `dw_mci_exynos_ext_setpower` | drivers/mmc/host/dw_mmc-exynos.c:1443 |
| `dw_mci_exynos_get_host_id` | drivers/mmc/host/dw_mmc-exynos.c:53 |
| `dw_mci_exynos_match` | drivers/mmc/host/dw_mmc-exynos.c:1727 |
| `dw_mci_exynos_misc_control` | drivers/mmc/host/dw_mmc-exynos.c:1664 |
| `dw_mci_exynos_parse_dt` | drivers/mmc/host/dw_mmc-exynos.c:693 |
| `dw_mci_exynos_pltfm_driver` | drivers/mmc/host/dw_mmc-exynos.c:1759 |
| `dw_mci_exynos_pltfm_driver_exit` | macro/linker-generated (`include/linux/device.h:1151`) |
| `dw_mci_exynos_pltfm_driver_init` | macro/linker-generated (`include/linux/device.h:1146`) |
| `dw_mci_exynos_prepare_command` | drivers/mmc/host/dw_mmc-exynos.c:530 |
| `dw_mci_exynos_priv_init` | drivers/mmc/host/dw_mmc-exynos.c:64 |
| `dw_mci_exynos_probe` | drivers/mmc/host/dw_mmc-exynos.c:1748 |
| `dw_mci_exynos_register_dump` | drivers/mmc/host/dw_mmc-exynos.c:490 |
| `dw_mci_exynos_register_notifier` | drivers/mmc/host/dw_mmc-exynos.c:454 |
| `dw_mci_exynos_set_enable_shift.isra.1` | drivers/mmc/host/dw_mmc-exynos.c:880 |
| `dw_mci_exynos_set_ios` | drivers/mmc/host/dw_mmc-exynos.c:595 |
| `dw_mci_exynos_set_sample.constprop.11` | drivers/mmc/host/dw_mmc-exynos.c:927 |
| `dw_mci_exynos_setup_clock` | drivers/mmc/host/dw_mmc-exynos.c:475 |
| `dw_mci_exynos_shutdown` | drivers/mmc/host/dw_mmc-exynos.c:1701 |
| `dw_mci_exynos_unregister_notifier` | drivers/mmc/host/dw_mmc-exynos.c:464 |
| `dw_mci_fifo_reset` | drivers/mmc/host/dw_mmc.c:936 |
| `dw_mci_get_cd` | drivers/mmc/host/dw_mmc.c:2081 |
| `dw_mci_get_ro` | drivers/mmc/host/dw_mmc.c:2058 |
| `dw_mci_host_count` | drivers/mmc/host/dw_mmc-exynos.c:38 |
| `dw_mci_hw_reset` | drivers/mmc/host/dw_mmc.c:2264 |
| `dw_mci_idmac_complete_dma` | drivers/mmc/host/dw_mmc.c:1020 |
| `dw_mci_idmac_init` | drivers/mmc/host/dw_mmc.c:1341 |
| `dw_mci_idmac_ops` | drivers/mmc/host/dw_mmc.c:1381 |
| `dw_mci_idmac_start_dma` | drivers/mmc/host/dw_mmc.c:1295 |
| `dw_mci_idmac_stop_dma` | drivers/mmc/host/dw_mmc.c:992 |
| `dw_mci_idma_reset_dma` | drivers/mmc/host/dw_mmc.c:1010 |
| `dw_mci_init` | drivers/mmc/host/dw_mmc.c:5188 |
| `dw_mci_interrupt` | drivers/mmc/host/dw_mmc.c:3373 |
| `dw_mci_lpa_host` | drivers/mmc/host/dw_mmc-exynos.c:37 |
| `dw_mci_notify_change` | drivers/mmc/host/dw_mmc.c:3800 |
| `dw_mci_of_find_slot_node` | drivers/mmc/host/dw_mmc.c:3841 |
| `dw_mci_ops` | drivers/mmc/host/dw_mmc.c:2287 |
| `dw_mci_pltfm_driver` | drivers/mmc/host/dw_mmc-pltfm.c:138 |
| `dw_mci_pltfm_driver_exit` | macro/linker-generated (`include/linux/device.h:1151`) |
| `dw_mci_pltfm_driver_init` | macro/linker-generated (`include/linux/device.h:1146`) |
| `dw_mci_pltfm_early_resume` | drivers/mmc/host/dw_mmc-pltfm.c:109 |
| `dw_mci_pltfm_match` | drivers/mmc/host/dw_mmc-pltfm.c:132 |
| `dw_mci_pltfm_pmops` | drivers/mmc/host/dw_mmc-pltfm.c:125 |
| `dw_mci_pltfm_probe` | drivers/mmc/host/dw_mmc-pltfm.c:66 |
| `dw_mci_pltfm_register` | drivers/mmc/host/dw_mmc-pltfm.c:27 |
| `dw_mci_pltfm_remove` | drivers/mmc/host/dw_mmc-pltfm.c:71 |
| `dw_mci_pltfm_resume` | drivers/mmc/host/dw_mmc-pltfm.c:97 |
| `dw_mci_pltfm_suspend` | drivers/mmc/host/dw_mmc-pltfm.c:85 |
| `dw_mci_post_req` | drivers/mmc/host/dw_mmc.c:1510 |
| `dw_mci_pre_dma_transfer` | drivers/mmc/host/dw_mmc.c:1391 |
| `dw_mci_prepare_command` | drivers/mmc/host/dw_mmc.c:676 |
| `dw_mci_pre_req` | drivers/mmc/host/dw_mmc.c:1491 |
| `dw_mci_probe` | drivers/mmc/host/dw_mmc.c:4533 |
| `dw_mci_pull_data16` | drivers/mmc/host/dw_mmc.c:3024 |
| `dw_mci_pull_data32` | drivers/mmc/host/dw_mmc.c:3107 |
| `dw_mci_pull_data64` | drivers/mmc/host/dw_mmc.c:3191 |
| `dw_mci_push_data16` | drivers/mmc/host/dw_mmc.c:2973 |
| `dw_mci_push_data32` | drivers/mmc/host/dw_mmc.c:3056 |
| `dw_mci_push_data64` | drivers/mmc/host/dw_mmc.c:3139 |
| `dw_mci_read_data_pio` | drivers/mmc/host/dw_mmc.c:3238 |
| `dw_mci_reg_dump` | drivers/mmc/host/dw_mmc.c:2676 |
| `dw_mci_regs_fops` | drivers/mmc/host/dw_mmc.c:558 |
| `dw_mci_regs_open` | drivers/mmc/host/dw_mmc.c:553 |
| `dw_mci_regs_show` | drivers/mmc/host/dw_mmc.c:541 |
| `dw_mci_remove` | drivers/mmc/host/dw_mmc.c:4891 |
| `dw_mci_req_fops` | drivers/mmc/host/dw_mmc.c:533 |
| `dw_mci_req_open` | drivers/mmc/host/dw_mmc.c:528 |
| `dw_mci_req_show` | drivers/mmc/host/dw_mmc.c:488 |
| `dw_mci_request` | drivers/mmc/host/dw_mmc.c:1890 |
| `dw_mci_request_end` | drivers/mmc/host/dw_mmc.c:174 |
| `dw_mci_resume` | drivers/mmc/host/dw_mmc.c:5020 |
| `dw_mci_save_sfr` | drivers/mmc/host/dw_mmc-exynos.c:39 |
| `dw_mci_set_fine_tuning_bit` | drivers/mmc/host/dw_mmc-exynos.c:941 |
| `dw_mci_set_ios` | drivers/mmc/host/dw_mmc.c:1920 |
| `dw_mci_setup_bus` | drivers/mmc/host/dw_mmc.c:1667 |
| `dw_mci_shutdown` | drivers/mmc/host/dw_mmc.c:5156 |
| `dw_mci_start_command` | drivers/mmc/host/dw_mmc.c:764 |
| `__dw_mci_start_request` | drivers/mmc/host/dw_mmc.c:1774 |
| `dw_mci_start_request` | drivers/mmc/host/dw_mmc.c:1852 |
| `dw_mci_start_signal_voltage_switch` | drivers/mmc/host/dw_mmc.c:2251 |
| `dw_mci_status_reg_summary` | drivers/mmc/host/dw_mmc.c:621 |
| `dw_mci_stop_dma` | drivers/mmc/host/dw_mmc.c:823 |
| `dw_mci_suspend` | drivers/mmc/host/dw_mmc.c:4956 |
| `dw_mci_tasklet_func` | drivers/mmc/host/dw_mmc.c:2839 |
| `dw_mci_timeout_timer` | drivers/mmc/host/dw_mmc.c:3548 |
| `dw_mci_tp_mon` | drivers/mmc/host/dw_mmc.c:3601 |
| `dw_mci_tuning_sampling` | drivers/mmc/host/dw_mmc-exynos.c:901 |
| `dw_mci_update_clock` | drivers/mmc/host/dw_mmc.c:866 |
| `dw_mci_wait_reset` | drivers/mmc/host/dw_mmc.c:835 |
| `dw_mci_work_routine_card` | drivers/mmc/host/dw_mmc.c:3656 |
| `__initcall_dw_mci_exynos_pltfm_driver_init6` | macro/linker-generated (`include/linux/init.h:190`) |
| `__initcall_dw_mci_init6` | macro/linker-generated (`include/linux/init.h:190`) |
| `__initcall_dw_mci_pltfm_driver_init6` | macro/linker-generated (`include/linux/init.h:190`) |
