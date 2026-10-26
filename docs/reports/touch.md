# Touchscreen parity — Melfas MMS438 (j7elte)

ROM: SM-J700M `J700MUBU2BRC2`, kernel `3.10.61-13213628`
Nura tree: `android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07` (3.10.108, baseline `b5e363a`)
Owned paths: `drivers/input/touchscreen/**`, `drivers/staging/samsung/sec_log.c`

## Verdict

**parity** — P1–P4 pass. No kernel patch produced. The ROM j7elte build drives a
Melfas MMS438 (I2C `0x48`). Two non-blocking deltas are recorded: the Nura config
leaves out the debug-only `CONFIG_SEC_DEBUG_TSP_LOG` facility (8 ROM symbols), and
the Nura tree ships a different (newer) MMS438 firmware build inside the kernel.
Neither affects touch input.

| Item | Result | Evidence |
|---|---|---|
| P1 symbol coverage | PASS | 77/77 `mms_*` ROM symbols defined in the Nura tree; the 8 `sec_tsp_log_*` misses are config-gated debug code present in source |
| P2 device tree | PASS | `melfas,mms_ts` node (rev04/rev06) and `imagis,ist30xx-ts` node (rev00) match ROM DTBs property-for-property, including GPIO/pinctrl/regulator values |
| P3 config | PASS | `CONFIG_TOUCHSCREEN_MELFAS_MMS438=y` + all deps; j7elte rev04/rev06 DTBs select the Melfas node |
| P4 firmware | PASS | Touch firmware is kernel built-in (`CONFIG_FIRMWARE_IN_KERNEL=y`), no file needed; present as `firmware/tsp_melfas/mms438_j7.fw(.ihex)` |
| P5 patch | n/a | no real gap found |

Verification is static only (no boot test, no kernel build): the 3.10.108 tree is not
built in this environment, and the contract mandates static verification.

## P1 — symbol coverage

Source of truth: `j7rom/boot_out/symbols.txt` (sorted kallsyms of the ROM `vmlinux.elf`).

- `grep -cE '^mms_' symbols.txt` → **77** symbols.
- `grep -cE 'ist30xx|ist3038|IST30' symbols.txt` → **0** (the ROM kernel contains no
  Imagis driver; see P2/P3).
- All 77 names resolve to a *definition* (function or data object) in
  `drivers/input/touchscreen/melfas_mms400/` in the Nura tree — 0 misses:

| Definition file | symbols |
|---|---|
| `melfas_mms400.c` | 28 |
| `melfas_mms400_test.c` | 30 |
| `melfas_mms400_cmd.c` | 9 |
| `melfas_mms400_mod.c` | 6 |
| `melfas_mms438_fw_update.c` | 4 |

Ten of the 77 are data objects and were checked as definitions as well:
`mms_attrs`/`mms_attr_group` (`melfas_mms400.c:838,846`),
`mms_cmd_attr`/`mms_cmd_attr_group`/`mms_commands` (`melfas_mms400_cmd.c:1290,1301,1024`),
`mms_test_attr`/`mms_test_attr_group` (`melfas_mms400_test.c:1403,1429`),
`mms_driver`/`mms_id`/`mms_match_table` (`melfas_mms400.c:1208,1200,1190`).
The `constprop`/`isra` suffixes in the ROM (`mms_isc_program_page.constprop.3`,
`mms_isc_read_status.isra.1`, `mms_proc_table_data.isra.0`) are compiler clones of
`melfas_mms438_fw_update.c:168,32` and `melfas_mms400_test.c:130`.

ROM coverage of the driver is therefore **77/77 (100 %)**.

### The 8 "misses"

They are not Melfas symbols; they belong to the Samsung TSP logging facility in
`drivers/staging/samsung/sec_log.c`:

```
sec_tsp_log_buf        sec_log.c:213
sec_tsp_log_ptr        sec_log.c:212
sec_tsp_log_size       sec_log.c:214
sec_tsp_log_setup      sec_log.c:216
sec_tsp_log_read       sec_log.c:355
sec_tsp_log_write      sec_log.c:318
sec_tsp_log_late_init  sec_log.c:381
__initcall_sec_tsp_log_late_init7   (late_initcall at sec_log.c:399)
```

All of them are inside `#ifdef CONFIG_SEC_DEBUG_TSP_LOG` (`sec_log.c:211`); the same
guard also removes `sec_debug_tsp_log` (`sec_log.c:286`, a 9th ROM symbol exported by
this block). Nura's config has `# CONFIG_SEC_DEBUG_TSP_LOG is not set`
(`config-nura-j7elte:3216`), while the ROM kallsyms contains the block, i.e. the ROM
had it enabled.

Why this is not a driver gap:

- The Melfas driver only uses the facility through `tsp_debug_{err,info,dbg}` macros;
  with the config off, `sec_debug.h:63` makes `sec_debug_tsp_log()` a no-op and the
  macros fall back to `dev_*`/`printk`. Input event handling, power, IRQ, firmware
  update and sysfs are unconditional.
- The only functional code under the same guard is the ghost-touch intensity dump
  (`melfas_mms400.c:270-317`): `tsp_dump()` becomes a `"not support"` printk
  (`melfas_mms400.c:319`). `tsp_dump()` is only called from the debug layer
  (`include/linux/sec_debug.h:60`), never from the touch path; the
  `p_ghost_check == NULL` guard at `melfas_mms400.c:312` keeps the config-off build safe.
- `sec_log.c` itself is still built: `drivers/staging/samsung/Makefile:18`
  (`obj-$(CONFIG_SEC_EXT) += sec_log.o`) and `CONFIG_SEC_EXT=y` (config:3194).

If the debug interface is wanted later, the delta is a one-line config change; it is
reported as fenced text below because `wifi-parity` owns the pmaports config.

## P2 — device tree

ROM DTBs in `boot_out/dtbs/` (three appended j7elte DTBs, `d00dfeed` each). The
revision labels below are [INFERENCE] from node content plus the Nura DTB list order
(`rev00 rev04 rev06`); all three ROM DTBs carry the same model string
`"Samsung J7ELTE revision 00 board based on EXYNOS7580"`, so the ROM model property
itself does not distinguish revisions.

| ROM DTB | touch node | Nura DTS |
|---|---|---|
| `000_800.dtb` (rev00) | `touchscreen@50`, `imagis,ist30xx-ts` | `exynos7580-j7elte_rev00.dts:351` |
| `001_19800.dtb` (rev04) | `mms_ts@48`, `melfas,mms_ts` | `exynos7580-j7elte_rev04.dts:357` |
| `002_32800.dtb` (rev06) | `mms_ts@48`, `melfas,mms_ts` | `exynos7580-j7elte_rev06.dts:357` |

### Melfas node (ROM `001_19800.dts:3005-3018`, `002_32800.dts:3071-3084`)

| property | ROM value | Nura value | match |
|---|---|---|---|
| node | `mms_ts@48` | `mms_ts@48` | yes |
| `compatible` | `"melfas,mms_ts"` | same | yes |
| `reg` | `<0x48>` | `<0x48>` | yes |
| `pinctrl-names` | `"on_state","off_state"` | same | yes |
| `pinctrl-0` | attn-irq: `gpa3-2`, fn `0xf`, pud `0`, drv `0` | `&attn_irq`, identical (`rev04:250`) | yes |
| `pinctrl-1` | attn-input: `gpa3-2`, fn `0`, pud `0`, drv `0`; i2c-pdn_off: `gpc4-1`,`gpc4-0`, con-pdn `2`, pud-pdn `1` | `&attn_input &i2c_pdn_off`, identical (`rev04:193,244`) | yes |
| `melfas,vdd_en` | `"tsp_avdd"` | `"tsp_avdd"` | yes |
| `melfas,irq-gpio` | `<&gpa3 2 0>` | `<&gpa3 2 0>` | yes |
| `melfas,scl-gpio` | `<&gpc4 1 0>` | `<&gpc4 1 0>` | yes |
| `melfas,sda-gpio` | `<&gpc4 0 0>` | `<&gpc4 0 0>` | yes |
| `melfas,max_x` | `<0x438>` (1080) | `<1080>` | yes |
| `melfas,max_y` | `<0x780>` (1920) | `<1920>` | yes |
| `melfas,fw_name` | `"tsp_melfas/mms438_j7.fw"` | same | yes |

ROM phandles resolved from the decompiled DTB: `0x29` = `gpa3` GPIO controller
(`001_19800.dts:122-128`), `0x2a` = `gpc4` (`:2276-2282`), `0x26/0x27` =
attn-irq/attn-input (`:364-378`), `0x28` = i2c-pdn_off (`:2331`).

Power: `tsp_avdd` is LDO33, 3000000 µV, `regulator-initial-mode = <1>` in both
(ROM `001_19800.dts:3419-3424`, `002_32800.dts:3485-3490`; Nura `rev04:681-687`,
`rev06:679-685`).

Bus: ROM `i2c@13860000` = `samsung,s3c2440-i2c`, `samsung,i2c-sda-delay = <100>`,
`samsung,i2c-max-bus-freq = <400000>`, `status = "okay"` (`001_19800.dts:2991-3003`);
Nura `exynos7580.dtsi:691-702` + board `rev04:351-356`/`rev06:351-356` — identical
values.

### Imagis node (rev00) (ROM `000_800.dts:2975-2986`, Nura `rev00:351-365`)

`touchscreen@50`, `"imagis,ist30xx-ts"`, `reg <0x50>`, same `pinctrl-0/1` groups,
`imagis,regulator_avdd = "tsp_avdd"`, `imagis,irq-gpio = <&gpa3 2 0>`,
`imagis,fw-bin = <1>`, `imagis,ic-version = "ist3038c"`,
`imagis,project-name = "j7"` — all equal. No deltas.

### Which controller is enabled for j7elte

- The **DTB decides by board revision**: rev00 → `imagis,ist30xx-ts`; rev04/rev06 →
  `melfas,mms_ts`.
- The **ROM kernel only ever compiled Melfas**: 77 `mms_*` symbols, zero
  `ist30xx*` symbols, no kernel modules at all (`boot_out/rd/` contains no `.ko`,
  and the ROM system image has no `/lib/modules` directory). So the shipping J700M
  build is the Melfas/rev04-or-rev06 configuration; the ist30xx DTB exists but the
  ROM kernel could not bind it.
- Nura builds all three DTBs (`arch/arm64/boot/dts/Makefile:13`, gated by
  `CONFIG_CAMERA_J7=y`, config:2176) and both drivers, i.e. a superset. Whichever
  DTB the bootloader selects, the matching driver is present.

## P3 — config

Nura config `config-nura-j7elte` (byte-identical to
`parity/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`,
`diff` → same):

| symbol | line | value |
|---|---|---|
| `CONFIG_INPUT_TOUCHSCREEN` | 1468 | y |
| `CONFIG_TOUCHSCREEN_MELFAS_MMS438` | 1510 | y |
| `CONFIG_TOUCHSCREEN_IST30XXC` | 1514 | y |
| `CONFIG_I2C` | 1632 | y |
| `CONFIG_OF` | 1001 | y |
| `CONFIG_PINCTRL` | 1726 | y |
| `CONFIG_REGULATOR` | 2054 | y |
| `CONFIG_SEC_EXT` (builds `sec_log.o`) | 3194 | y |
| `CONFIG_SEC_SYSFS` | 3195 | y |
| `CONFIG_BATTERY_SAMSUNG` | 3384 | y |
| `CONFIG_VBUS_NOTIFIER` | 1128 | y |
| `CONFIG_FW_LOADER` | 967 | y |
| `CONFIG_FIRMWARE_IN_KERNEL` | 968 | y |
| `CONFIG_CAMERA_J7` (builds j7elte DTBs) | 2176 | y |
| `CONFIG_SEC_DEBUG_TSP_LOG` | 3216 | **not set** (ROM: enabled) |

`CONFIG_TOUCHSCREEN_MELFAS_MMS438` pulls in
`drivers/input/touchscreen/Makefile:79` → `melfas_mms400/`; the subdir Makefile
builds all five objects (`melfas_mms400.o, _mod.o, _test.o, _cmd.o,
melfas_mms438_fw_update.o`) — matching the 77-symbol set and its per-file split (P1).

Only delta (debug-only, non-blocking; pmaports config is owned by `wifi-parity`):

```diff
--- a/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64
+++ b/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64
@@ -3215,3 +3215,3 @@
 # CONFIG_SEC_AVC_LOG is not set
-# CONFIG_SEC_DEBUG_TSP_LOG is not set
+CONFIG_SEC_DEBUG_TSP_LOG=y
 CONFIG_SEC_DEBUG_LAST_KMSG=y
```

## P4 — firmware

### ROM

Touch firmware is **built into the ROM kernel**, not a filesystem blob:

- ROM symbols `_fw_tsp_melfas_mms438_j7_fw_bin` (vaddr `0xffffffc000a10888`) and
  `_fw_tsp_melfas_mms438_j7_fw_name` (vaddr `0xffffffc000c11ec8`), consistent with the
  3.10 `firmware/Makefile` `cmd_fwbin` scheme (`.incbin` + `.builtin_fw` entry; the
  `__start_builtin_fw`/`__end_builtin_fw` symbols are present at
  `0xffffffc000c3e190`/`0xffffffc000c3e1d8`).
- Extent to the next `_fw_end` (`0xffffffc000a1c8e8`) = `0xc060` = **49248 bytes**.
  In `kernel.bin` (Image text_offset `0x205000`) the blob is at file offset
  `0x80b888`, first bytes `4d424e014d3448305201040001000000` (`MBN.M4H0S`, ver 0x52).
- sha512 of the ROM blob:
  `edb7de5d11760b632d16b8342094ff5c7b26545b58197fac5f838ee961e73272a4a56b216b4ea75769f635de414ed1019bd271c7321f727836fa746f8d9c09a2`
- No file-based copy anywhere in the ROM: `/vendor/firmware` contains only
  bcm43438A1, fimc_is_*, mfc_fw, setfile_*; `/etc/firmware` does not exist; the
  string `tsp_melfas` appears 0 times in `system.raw`; nothing in the ramdisk.

### Nura

- `firmware/Makefile:169`:
  `fw-shipped-$(CONFIG_TOUCHSCREEN_MELFAS_MMS438) += tsp_melfas/mms438_j7.fw tsp_melfas/mms438_j7x.fw`
- Source is stored as Intel HEX: `firmware/tsp_melfas/mms438_j7.fw.ihex` (3079 lines).
  The `$(obj)/%: $(obj)/%.ihex` rule converts it with `objcopy -Iihex -Obinary`, and
  `cmd_fwbin` wraps it into `.gen.o`; `obj-$(CONFIG_FIRMWARE_IN_KERNEL) += ...gen.o`
  makes it a built-in firmware entry.
- `CONFIG_FIRMWARE_IN_KERNEL=y` (config:968), `CONFIG_FW_LOADER=y` (config:967), so
  `request_firmware("tsp_melfas/mms438_j7.fw", ...)` at `melfas_mms400.c:626`
  resolves from the built-in list — no `/lib/firmware` file is required.
- DTB and driver agree on the name: `melfas,fw_name =
  "tsp_melfas/mms438_j7.fw"` (rev04/rev06 DTBs) → `melfas_mms400_mod.c:290`.
- Converted size = 49248 bytes, same as the ROM blob, but a **different firmware
  build**: byte 0x8 is `0x53` (Nura) vs `0x52` (ROM), 12042 bytes differ overall.
  sha512 (Nura): `a834a2eed880316e2a3c438a3c764f518941c3c2849d4484c96bc5aea8c4f934518155042eba368deddf1113809eeb4f8aecdd0b17812817f909175876ace68b`.
  This is the firmware shipped by the 3.10.108 downstream drop, not the ROM's 0x52.
- Probe auto-updates firmware from this built-in image
  (`MMS_USE_AUTO_FW_UPDATE 1`, `melfas_mms400.h:169`;
  `melfas_mms400.c:1028-1032`), so a device coming from the ROM will be flashed to
  the 0x53 build on first Nura boot.
- `FFU_FW_PATH "ffu_tsp.bin"` (`melfas_mms400.c:748`) is only used by the manual
  FFU sysfs command; it is absent on the ROM as well.
- The ist30xx driver firmware is a C header (`imagis_30xxc/ist30xxc_fw.h`), not a
  file, and is irrelevant to the Melfas configuration.
- No `parity/firmware/files-touch.list` is produced: touch firmware is not
  file-based on either side (per contract, the list only applies to file blobs).

## Residual risks / tests

1. Untested at runtime here (static analysis only; no boot). Suggested smoke test on
   j7elte rev04/06: `dmesg | grep -i melfas` must show
   `MELFAS MMS438 Touchscreen is initialized successfully` (string also present in
   the ROM kernel) and `input: sec_touchscreen`; then verify multitouch in an
   evtest/`libinput debug-events` session.
2. First boot on a device previously running the ROM will flash the built-in 0x53
   firmware (P4 delta). Watch for `mms_flash_fw` failure; the driver retries 3 times
   and then continues with the old firmware.
3. `CONFIG_SEC_DEBUG_TSP_LOG` is off, so `/proc/tsp_msg` and the `tsp_dump` ghost
   intensity trace do not exist. Enable the config line shown in P3 if those debug
   interfaces are needed.
4. The ist30xx/rev00 path remains unexercised (ROM had no Imagis driver); it is
   extra capability in Nura, not a parity requirement for J700M.
5. Regulator `tsp_avdd` (LDO33, 3.0 V) has no `regulator-boot-on`; if the bootloader
   leaves it off, the driver's `mms_power_control()` enables it at probe (same code
   as ROM, `melfas_mms400_mod.c:20`).

## Appendix — 77 `mms_*` ROM symbols → Nura definition

```
melfas_mms400.c:19    mms_reboot
melfas_mms400.c:40    mms_i2c_read
melfas_mms400.c:92    mms_i2c_read_next
melfas_mms400.c:132   mms_i2c_write
melfas_mms400.c:168   mms_enable
melfas_mms400.c:220   mms_disable
melfas_mms400.c:247   mms_input_open
melfas_mms400.c:260   mms_input_close
melfas_mms400.c:277   mms_ghost_touch_check
melfas_mms400.c:329   mms_get_ready_status
melfas_mms400.c:376   mms_get_fw_version
melfas_mms400.c:412   mms_get_fw_version_u16
melfas_mms400.c:435   mms_disable_esd_alert
melfas_mms400.c:509   mms_charger_attached
melfas_mms400.c:535   mms_interrupt
melfas_mms400.c:611   mms_fw_update_from_kernel
melfas_mms400.c:667   mms_fw_update_from_storage
melfas_mms400.c:734   mms_fw_update_from_ffu
melfas_mms400.c:788   mms_sys_fw_update
melfas_mms400.c:838   mms_attrs
melfas_mms400.c:846   mms_attr_group
melfas_mms400.c:930   mms_probe
melfas_mms400.c:1154  mms_remove
melfas_mms400.c:1190  mms_match_table
melfas_mms400.c:1200  mms_id
melfas_mms400.c:1208  mms_driver
melfas_mms400.c:1224  mms_init
melfas_mms400.c:1239  mms_exit
melfas_mms400_mod.c:20    mms_power_control
melfas_mms400_mod.c:109   mms_clear_input
melfas_mms400_mod.c:131   mms_input_event_handler
melfas_mms400_mod.c:253   mms_parse_devicetree
melfas_mms400_mod.c:307   mms_config_input
melfas_mms400_mod.c:347   mms_vbus_notification
melfas_mms400_test.c:130  mms_proc_table_data
melfas_mms400_test.c:381  mms_run_test
melfas_mms400_test.c:586  mms_get_image
melfas_mms400_test.c:740  mms_sys_fw_version
melfas_mms400_test.c:774  mms_sys_info
melfas_mms400_test.c:818  mms_sys_device_enable
melfas_mms400_test.c:843  mms_sys_device_disable
melfas_mms400_test.c:868  mms_sys_irq_enable
melfas_mms400_test.c:893  mms_sys_irq_disable
melfas_mms400_test.c:919  mms_sys_power_on
melfas_mms400_test.c:944  mms_sys_power_off
melfas_mms400_test.c:969  mms_sys_reboot
melfas_mms400_test.c:997  mms_sys_glove_mode_store
melfas_mms400_test.c:1025 mms_sys_glove_mode_show
melfas_mms400_test.c:1059 mms_sys_charger_mode_store
melfas_mms400_test.c:1087 mms_sys_charger_mode_show
melfas_mms400_test.c:1121 mms_sys_window_mode_store
melfas_mms400_test.c:1149 mms_sys_window_mode_show
melfas_mms400_test.c:1183 mms_sys_palm_rejection_mode_store
melfas_mms400_test.c:1211 mms_sys_palm_rejection_mode_show
melfas_mms400_test.c:1245 mms_sys_intensity
melfas_mms400_test.c:1267 mms_sys_rawdata
melfas_mms400_test.c:1289 mms_sys_test_cm_delta
melfas_mms400_test.c:1311 mms_sys_test_cm_abs
melfas_mms400_test.c:1333 mms_sys_test_cm_jitter
melfas_mms400_test.c:1355 mms_sys_test_short
melfas_mms400_test.c:1403 mms_test_attr
melfas_mms400_test.c:1429 mms_test_attr_group
melfas_mms400_test.c:1436 mms_sysfs_create
melfas_mms400_test.c:1462 mms_sysfs_remove
melfas_mms400_cmd.c:1024  mms_commands
melfas_mms400_cmd.c:1068  mms_sys_cmd
melfas_mms400_cmd.c:1193  mms_sys_cmd_status
melfas_mms400_cmd.c:1228  mms_sys_cmd_result
melfas_mms400_cmd.c:1257  mms_sys_cmd_list
melfas_mms400_cmd.c:1290  mms_cmd_attr
melfas_mms400_cmd.c:1301  mms_cmd_attr_group
melfas_mms400_cmd.c:1308  mms_sysfs_cmd_create
melfas_mms400_cmd.c:1354  mms_sysfs_cmd_remove
melfas_mms438_fw_update.c:32  mms_isc_read_status
melfas_mms438_fw_update.c:93  mms_isc_erase_page
melfas_mms438_fw_update.c:168 mms_isc_program_page
melfas_mms438_fw_update.c:228 mms_flash_fw
```
