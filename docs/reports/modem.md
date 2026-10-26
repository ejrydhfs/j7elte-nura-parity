# Modem parity — Samsung SEC modem v1 (SS310AP) + Exynos7580 SHM/MCU IPC (j7elte)

**Verdict: parity (P1–P4 pass). No kernel patch required.**
The entire CP-modem kernel stack present in the ROM is present, under the same symbol
names, in the Nura tree (`drivers/misc/modem_v1/**`, `drivers/misc/mcu/{shm_ipc,mcu_ipc}.c`).
Remaining gap is **userspace only**: Nura has no RIL/CP-boot daemon, so calls/SMS/data
are untested (see *Userspace requirement*).

Scope of this report: ROM kallsyms namespace `modem* / shm* / shmem* / sipc*`
(165 names, 167 kallsyms entries; `boot_out/symbols.txt`), the DT nodes
`mif_pdata` (`sec_modem,modem_pdata`), `shmem@90000000` (`samsung,exynos7580-shm_ipc`),
`mcu_ipc@104B0000` (`samsung,exynos7580-mailbox`), the Nura config, and the CP firmware
package. Verification is static (objdump/kallsyms/source/DT comparison); no kernel build
was run.

## P1–P4 summary

| Parity | Result | Evidence |
|---|---|---|
| P1 symbols | **PASS (100%)** | All 62 selected ROM symbols map to Nura sources (table below): 14 in `drivers/misc/modem_v1`, 14 in `drivers/misc/mcu`, 11 in `drivers/usb/gadget/serial_acm.c`, 23 in generic `ipc/shm.c`. The 3 `shmem_driver{,_init,_exit}` and `modemctl_notify_event` names (outside the 62-name selection) are also present. No renamed/missing implementation. |
| P2 device tree | **PASS** | `mif_pdata`/`shmem`/`mcu_ipc` nodes byte-identical between ROM DTB `000_800.dts` and the Nura j7elte DTS build (318 sorted property lines diff-identical: `/tmp/rom_000_800_modemprops.txt` vs `/tmp/nura_j7_modemprops.txt`). Sources: ROM `boot_out/dtbs/000_800.dts:4251-4258, 4403-4630, 4631-4636`; Nura `arch/arm64/boot/dts/exynos7580-j7elte_rev00.dts:1489-1518` + `arch/arm64/boot/dts/modem-isla-pdata.dtsi:14-233`. |
| P3 config | **PASS** | `config-nura-j7elte:1095 CONFIG_SEC_MODEM_V1=y`, `:1096 CONFIG_SEC_MODEM_SS310AP=y`, `:1108 CONFIG_LINK_DEVICE_SHMEM=y`, `:1111 CONFIG_UMTS_MODEM_SS310AP=y`, `:1114 CONFIG_EXYNOS7580_MCU_IPC=y`, `:1115 CONFIG_EXYNOS7580_SHM_IPC=y`, `:1116 CONFIG_EXYNOS7580_PMU_CP=y`. (`SEC_MODEM_SS310AP` Kconfig `drivers/misc/modem_v1/Kconfig:10-17` selects `UMTS_MODEM_SS310AP`, `LINK_DEVICE_SHMEM`, `EXYNOS7580_MCU_IPC`, `EXYNOS7580_SHM_IPC`, `EXYNOS7580_PMU_CP` — all enabled.) |
| P4 firmware | **PASS (no kernel-opened blob)** | `request_firmware` appears nowhere in `drivers/misc/modem_v1/**` or `drivers/misc/mcu/**`. The CP image is pushed by userspace through `/dev/umts_boot0`; blob inventory and mapping in `parity/firmware/files-modem.list`. |

## P1 — symbol coverage and the "42 prior misses"

The earlier pass selected 62 names: 23 `modem_*` + 37 `shm_*` + 2 `sipc5_*`
(`modemctl_notify_event` is a 63rd modem-stack name if the `modemctl` prefix is included).
Disposition of the whole namespace (165 names = 24 `modem*` + 37 `shm_*` + 99 `shmem_*`
+ 2 `sipc5_*` + `shmlock_user_lock` + 2 `shmctl_*.constprop` clones):

**A. Modem stack — 55 names, all implemented in Nura at identical names.**

| ROM symbol(s) | Nura source |
|---|---|
| `modem_probe` `modem_suspend` `modem_resume` `modem_shutdown` `modem_pm_ops` `modem_driver` | `drivers/misc/modem_v1/modem_main.c:740,867,892,856,924,929` |
| `modem_driver_init` `modem_driver_exit` | `module_platform_driver(modem_driver)` `modem_main.c:943` |
| `modemctl_notify_event` | `drivers/misc/modem_v1/modem_utils.c:1168` |
| `modem_state_string` (ROM has 3 copies) | `static const char *modem_state_string[]` in `drivers/misc/modem_v1/modem_utils.h:215-231` — a header-static array, hence one copy per including CU (3 ROM copies disassemble as pointer tables, e.g. `0xffffffc0009c35d0`); same name, same role |
| `modem_init_func` | `modem_init_call modem_init_func[MAX_MODEM_TYPE]` `drivers/misc/modem_v1/modem_variation.c:124` (ROM copy at `0xffffffc0009c3480` disassembles as a function-pointer table — consistent) |
| `modem_notify_event` `modem_event_notifier` | `drivers/misc/modem_v1/modem_notifier.c:35,25` |
| `sipc5_init_io_device` `sipc5_build_config` (ROM: `.isra.6` clone) | `drivers/misc/modem_v1/modem_io_device.c:1343,30` |
| `shmem_handle_cp_crash` `shmem_forced_cp_crash` `shmem_cmd_handler` `shmem_oom_handler_work` `shmem_rx_task` `shmem_init_comm` `shmem_terminate_comm` `shmem_send` `shmem_boot_on` `shmem_xmit_boot` `shmem_security_request` `shmem_start_download` `shmem_update_firm_info` `shmem_force_dump` `shmem_start_upload` `shmem_full_dump` `shmem_close_tx` `shmem_ioctl` `shmem_tx_state_handler` `shmem_irq_handler` `shmem_qos_work` `shmem_qos_req_handler` `shmem_create_link_device` (23) | `drivers/misc/modem_v1/link_device_shmem.c:171,213,416,1379,1445,1463,1524,1534,1581,1683,1733,1762,1792,1808,1817,1847,1897,1968,2020,2051,2083,2114,2229` |
| `shm_probe` `shm_remove` `shm_get_phys_base` `shm_get_phys_size` `shm_get_boot_size` `shm_get_ipc_rgn_offset` `shm_get_ipc_rgn_size` `shm_get_security_param3` `shm_get_security_param2` `shm_request_region` `shm_get_boot_region` `shm_get_boot_phy_address` `shm_get_ipc_region` `shm_release_region` (14) | `drivers/misc/mcu/shm_ipc.c:147,190,30,35,40,45,50,55,81,101,126,131,136,142` |
| `shmem_driver` `shmem_driver_init` `shmem_driver_exit` | `module_platform_driver(shmem_driver)` `drivers/misc/mcu/shm_ipc.c:201-210` |

ROM addresses cluster exactly per these CUs (nm of `boot_out/vmlinux.elf`):
`shm_*` + `shmem_driver*` at `0xffffffc00051cccc-0x51cfb8`/`0x95be40`/`0xe714ec`,
modem_main at `0x51cfcc-0x51d5ac`, io_device/sipc5 at `0x51ffcc-0x520a2c`,
link_device_shmem at `0x522a3c-0x526028` — matching the source mapping.

**B. USB gadget ACM "modem" — 11 names, also present in Nura, but NOT the CP modem.**
`modem_open` `modem_close` `modem_read` `modem_poll` `modem_ioctl` `modem_fops`
`modem_device` `modem_register` `modem_misc_register` `modem_unregister` `modem_wait_q`
→ `drivers/usb/gadget/serial_acm.c:37,44,49,69,94,121,131,137,152,166,30`.
The prior hypothesis `modem_close ∈ serial_acm.c` is **confirmed**: ROM
`modem_open..modem_ioctl` occupy one CU at `0xffffffc00060e79c-0x60f380`, and
`modem_close` disassembles to `mov w0,#0; ret` (`kernel.bin` @ `0x60e7ac`), i.e. the
trivial `serial_acm.c:44` release op. These back the f_acm USB gadget (`f_acm.c:912`
calls `modem_misc_register()`), unrelated to the baseband.

**C. Generic kernel false positives — not modem at all.**
- 23 `shm_*` names = SysV shared memory `ipc/shm.c`: `shm_open` `shm_close` `shm_destroy`
  `shm_destroy_orphaned` `shm_try_destroy_current` `shm_try_destroy_orphaned` `shm_init`
  `shm_init_ns` `shm_exit_ns` `shm_fault` `shm_mmap` `shm_release` `shm_fsync`
  `shm_fallocate` `shm_get_unmapped_area` `shm_security` `shm_more_checks` `shm_vm_ops`
  `shm_file_operations` `shm_file_operations_huge` `shm_mnt` `shm_rcu_free`
  `shm_may_destroy` (+`shmlock_user_lock`, `shmctl_down/nolock.constprop` clones).
  Verified by name in `ipc/shm.c:68-429,587-598` and by ROM address cluster
  `0x402e4c-0x403abc`. Note `shm_security`/`shm_more_checks` are the SysV permission
  checks, not the SS310AP security params (`shm_get_security_param2/3` are the latter).
- 73 `shmem_*` names = tmpfs `mm/shmem.c` (`shmem_fill_super`, `shmem_getpage_gfp`,
  `shmem_fault`, `shmem_ops`, `shmem_aops`, …), ROM cluster `0x2c2f08-0x2c6a6c` +
  `0x981b00-0x982100`. Includes the misleadingly named `shmem_match` `shmem_link`
  `shmem_create` `shmem_set_file` `shmem_destroy_callback` — all tmpfs.

**The 42 "misses" are therefore explained, and none is a real gap.** Of the 62 selected
names only 14 are literally defined in `drivers/misc/modem_v1/**`; the other 48 are
11 gadget (`serial_acm.c`) + 14 (`drivers/misc/mcu/shm_ipc.c`) + 23 SysV (`ipc/shm.c`),
and the prior run's 42 misses are drawn from those 48. A name-based match against
`drivers/misc/modem_v1/**` alone cannot see `drivers/misc/mcu/shm_ipc.c` and
`drivers/usb/gadget/serial_acm.c`, and the SysV/tmpfs `shm_*`/`shmem_*` names were
false positives to begin with. The second prior hypothesis is confirmed with a
correction: the `shm_*`/`shmem_*` modem symbols are the SHM link/shm driver functions,
but they are **not renamed** — they exist under identical names in
`drivers/misc/mcu/shm_ipc.c` and `drivers/misc/modem_v1/link_device_shmem.c`.
(The interrupted run left no notes; `/tmp/modemcp/` contains only `j7elte.pit` +
`modem.bin`.)

## P2 — device tree

ROM `boot_out/dtbs/000_800.dts` vs Nura `exynos7580-j7elte_rev00.dts` +
`modem-isla-pdata.dtsi` (j7elte_rev00 includes `modem-isla-pdata.dtsi`, line 14):

| Item | ROM DTB | Nura DTS | Match |
|---|---|---|---|
| `mif_pdata` compatible | `sec_modem,modem_pdata` (000_800.dts:4404) | `modem-isla-pdata.dtsi:15` | ✔ |
| interrupts | `<0x00 0x12 0x00 0x00 0x1df 0x00>` (:4406) | `<0 18 0>, <0 479 0>` (:17) | ✔ (0x12=18, 0x1df=479) |
| `mif,int_ap2cp_*`, `mif,irq_cp2ap_*`, `mbx_*` | :4407-4428 | :19-40 | ✔ (incl. `mbx_ap2cp_info_value=33`, `mbx_ap2cp_lock_value=39`) |
| `mif,name/type/net/use_handover/ipc_version/link_types/link_name/link_attrs/num_iodevs` | `ss310ap`, `0x0a`, 0, 0, `0x32`(50), `0x200`, `"shmem"`, `0x7c8`, `0x14`(20) (:4430-4438) | `modem-isla-pdata.dtsi:41-49` (same values) | ✔ |
| 20 `iodevs` (`umts_ipc0/1`, `umts_rfs0`, `umts_csd`, `umts_router`, `umts_dm0`, `ipc_loopback0`, `rmnet0-7`, `multipdp`, `umts_boot0`, `umts_ramdump0`, `smd4`, `umts_ciq0`) | :4440-4629 | :50-211 | ✔ (ids/attrs identical; compiled dumps identical) |
| `pinctrl-0`, `gpios`, `modem_ctrl,gpio_cp_rst_n`, `mif,gpio_ds_det` | :4440-4444 (`<0x6c 0x6d>`, gpd0 0-3, `<0x6e 6 1>`, gpd0 4) | `exynos7580-j7elte_rev00.dts:1489-1499` (`<&cp_rst_n &ds_det>`, `&gpd0 0-3`, `<&gpe0 6 0x1>`, `<&gpd0 4 0x0>`) | ✔ |
| `mif,ap_clk_table` (14 entries 1600000→300000) / `mif,mif_clk_table` (8 entries 825000→200000) | :4445-4446 | `exynos7580-j7elte_rev00.dts:1500-1517` | ✔ (hex values equal) |
| `shmem@90000000` (`samsung,exynos7580-shm_ipc`) | `reg = <0x00 0x90000000 0x00 0x5c00000>`, `shmem,ipc_offset = <0x5800000>`, `shmem,ipc_size = <0x400000>` (:4631-4636) | `modem-isla-pdata.dtsi:224-231` (same) | ✔ |
| `mcu_ipc@104B0000` (`samsung,exynos7580-mailbox`) | `reg=<0x0 0x104b0000 0x0 0x17c>`, `interrupts=<0 0x17f 0>`, `mcu,name="mcu_ipc"`, `mcu,id=1`, `mcu,irq_affinity_mask=3` (:4251-4258) | `modem-isla-pdata.dtsi:214-222` (same) | ✔ |

Compiled-DTB cross-check: property lines of these nodes sorted from the decompiled
DTBs (`/tmp/rom_000_800.sorted.dts` vs `/tmp/nura_j7.sorted.dts`, the main agent's
diff basis) are identical — 318 vs 318 lines, empty diff, including all `iod,*`,
`mif,*`, `mbx_*`, `shmem,*`, `mcu,*`, `modem_ctrl,*`, `gpios`, `pinctrl*` lines.

Driver parsing matches the node: `shm_probe` reads `shmem,ipc_offset` and
`shmem,ipc_size` and `platform_get_resource(…, IORESOURCE_MEM, 0)` for base/size
(`drivers/misc/mcu/shm_ipc.c:147-180`) — all present in both DTBs.

## P3 — config

All required symbols `=y` in `config-nura-j7elte` (lines 1095, 1096, 1114, 1115) plus
the dependency set `CONFIG_LINK_DEVICE_SHMEM=y` (1108), `CONFIG_UMTS_MODEM_SS310AP=y`
(1111), `CONFIG_EXYNOS7580_PMU_CP=y` (1116). No config change needed. (`EXYNOS7580_MCU_IPC`
also gates the PMU wakeup mask `arch/arm64/mach-exynos/pmu.c:120-123`, enabled.)

## P4 — firmware / CP inventory

Kernel side: no firmware file is ever opened (no `request_firmware`, no file path in
`drivers/misc/modem_v1/**` or `drivers/misc/mcu/**`). The CP image is streamed from
userspace: `shmem_xmit_boot` (`link_device_shmem.c:1683`) implements `IPC_BOOT` writes
to `/dev/umts_boot0` (registered from the `iodevs` DT list; `modem_io_device.c:570,1343`),
driven by ioctls `IOCTL_MODEM_BOOT_ON/OFF/DONE` and `IOCTL_MODEM_XMIT_BOOT`
(`drivers/misc/modem_v1/modem_prj.h:74-82`) — exactly the interface stock `/sbin/cbd`
uses (strings: `IOCTL_MODEM_XMIT_BOOT`, `Send BOOT`, `Send MAIN`, `STEP NV`).

**CP package inventory** — `SAMFW.COM_SM-J700M_PCW_J700MUBU2BRC2_fac.zip`:

| Archive member | Size | Content |
|---|---|---|
| `AP_J700MUBU2BRC2_CL13213628_QB17312148_REV00_user_low_ship.tar.md5` | 3,299,727,540 | system (already mirrored in `system.raw`) |
| `BL_J700MUBU2BRC2_CL13213628_QB17312148_REV00_user_low_ship.tar.md5` | 4,556,977 | bootloader/sboot |
| `CP_J700MUBU2BRC1_CL13166835_QB17211199_REV00_user_low_ship.tar.md5` | 34,898,017 | single member `modem.bin` (34,887,024 B) — the SS310AP CP firmware |
| `CSC_UUB_J700MUUB2BRC3_CL12262419_QB17432125_REV00_user_low_ship.tar.md5` | 94,126,263 | CSC |

`modem.bin` sha512 `4a4e459eb18b396b87e09935f79b63ca6e6c770c8490eff2624f3a10ea5405cb2fd03d885657dfb693179ae0da2dbb295320a29ddc67f4ecad5a28292237dd97`
— matches the extracted `parity/firmware/modem.bin` byte-for-byte. (Note: this "fac"
combo ships CP build **BRC1** with AP build **BRC2**; that is the vendor pairing, not
an extraction artifact.) PIT (`/tmp/modemcp/j7elte.pit`): `RADIO (id 14) ← modem.bin`;
`CDMA-RADIO (id 13) ← modem_cdma.bin` not shipped (GSM device). Install mapping:
`parity/firmware/files-modem.list` (canonical stock target: the RADIO partition =
`/dev/block/mmcblk0p5` = `by-name/RADIO`, `init.baseband.rc:3,29`,
`ueventd.samsungexynos7580.rc:105`; filesystem copy `/lib/firmware/postmarketos/samsung-j7elte/modem.bin`
for a future userspace loader).

## Userspace requirement (why calls/SMS/data are still untested)

The kernel exposes SIPC5 io devices only. Nura currently has no userspace driving them;
stock needs:
1. a CP boot daemon (`/sbin/cbd -d -tss310 -bm -mm -P …/by-name/RADIO`): loads
   BOOT/MAIN images from the CP firmware and NV records to `/dev/umts_boot0`, toggles
   CP power/reset via `IOCTL_MODEM_ON/RESET/BOOT_*` on the modem control device;
2. a Samsung SIPC5 RIL (`rild` + `libsec-ril.so`, `default.prop:4-5`) speaking
   `/dev/umts_ipc0` (control), `/dev/umts_rfs0` (EFS/NV), `/dev/smd0`, and using the
   `rmnet0-7` netdevs created by `modem_io_device.c` for data;
3. NV state dirs `/efs` (EFS partition) and `/cpefs` (CPEFS partition).
All stock binaries are Android-bionic and not installable into a musl rootfs (same
caveat as `files-gps.list`); an open implementation (e.g. a cbd clone + ofono/rild
shim) is required before telephony can be tested. This is a userspace gap only —
no kernel change is warranted.

## Patches

None. P1–P4 pass; `parity/patches/modem/` intentionally not created.

## Residual risks / tests

- Static verification only (kallsyms/objdump/DT/config/source); no build or hardware
  test was run (per assignment). A `j7rom/parity/build.sh` build of `drivers/misc/modem_v1`
  + `drivers/misc/mcu` would be the next check.
- The SS310AP boot handshake (security params via `shm_get_security_param2/3`,
  `CONFIG`-independent `CP_NONSECURE_BOOT` branch in `shm_ipc.c:63-67`) is exercised
  only by stock `cbd`; behavior under a reimplementation is untested.
- CP firmware version skew (CP BRC1 vs AP BRC2) is as-shipped; use this exact
  `modem.bin` for any future bring-up.
