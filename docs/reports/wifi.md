# Wi-Fi parity — Broadcom bcmdhd / BCM43438 (j7elte)

Scope: ROM `SM-J700M J700MUBU2BRC2` (kernel 3.10.61) vs Nura kernel tree
`android_kernel_samsung_universal7580-2a746c1…` (3.10.108). Static analysis only —
no aarch64 cross-toolchain is installed, so nothing was compiled and no runtime test
was performed.

**Verdict**

| Item | Result |
|---|---|
| P1 symbol coverage | **PASS** — 434/439 ROM symbols implemented in the *built* variant (98.86 %); 5 explained renames/removals |
| P2 device tree | **PASS** — `wlan` node and its two pinctrl groups are byte-identical (only `linux,phandle` emission differs) |
| P3 config | **PASS** — `CONFIG_BCM4343=y` selects `bcmdhd_1_77_nn/`; all dependencies present |
| P4 firmware | **FIXED BY PATCH** — original paths point into a non-existent `/system`; patch repoints them to `/lib/firmware/postmarketos/samsung-j7elte/`. Files themselves must be installed by a firmware package (see gap G1) |

The driver is at parity once the config patch is applied *and* the five `bcmdhd_*` blobs
are installed at the new paths.

---

## Variant selection (basis for P1/P3)

The ROM kernel was built from **`drivers/net/wireless/bcmdhd/`**:
`strings boot_out/kernel.bin` yields source paths such as
`drivers/net/wireless/bcmdhd/dhd_linux.c`, `…/dhd_custom_exynos.c`, `…/dhd_sdio.c`,
`…/wl_cfg80211.c` (no `bcmdhd_1_77_nn` string exists in the ROM image).

Nura builds **`bcmdhd_1_77_nn/`**:

- `drivers/net/wireless/Makefile:58-62` — `ifneq ($(CONFIG_BCM4343),)`,
  `obj-$(CONFIG_BCM4343) += bcmdhd_1_77_nn/`, plus
  `obj-$(CONFIG_MACH_UNIVERSAL7580) += bcmdhd_1_77_nn/dhd_custom_exynos.o` and
  `…/dhd_custom_memprealloc.o` (both active: `CONFIG_MACH_UNIVERSAL7580=y`,
  config line 308).
- `drivers/net/wireless/bcmdhd_1_77_nn/Makefile:934-958` — `CONFIG_BCM4343` block sets
  `-DBCM43430_CHIP -DHW_OOB`, and because `CONFIG_BCM4343=y` it sets
  `-DENABLE_INSMOD_NO_FW_LOAD -DUSE_LATE_INITCALL_SYNC` and `DRIVER_TYPE = y`
  (`dhd.o` is linked built-in).
- `SDIO` build, not PCIe: `CONFIG_BCMDHD_PCIE` is not set (config has no such line),
  so the Makefile's `BUS_IFACE_SDIO=y` branch applies (`-DBCMSDIO -DBCMLXSDMMC`,
  `-DOOB_INTR_ONLY` from `CONFIG_BCMDHD_OOB_HOST_WAKE=y`).

Note: `drivers/net/wireless/Kconfig:295-296` sources only `bcmdhd/Kconfig` and
`bcmdhd_1_77/Kconfig`; `bcmdhd_1_77_nn/Kconfig` is **not** sourced. Therefore the
effective declarations of `BCM4343`, `BCMDHD_FW_PATH`, `BCMDHD_NVRAM_PATH`,
`WIFI_BROADCOM_COB`, `WLAN_REGION_CODE` come from `bcmdhd/Kconfig` (first) and
`bcmdhd_1_77/Kconfig`; `BCMDHD_OOB_HOST_WAKE` and `BCMDHD_PREALLOC_MEMDUMP` are declared
only in `bcmdhd_1_77/Kconfig:212` / `:194`. All symbols used by the config are declared
by one of the sourced files, so the config is internally consistent, but the duplicate
`BCM4343`/`BROADCOM_WIFI` definitions (top-level `Kconfig:272`, `bcmdhd/Kconfig:8`,
`bcmdhd_1_77/Kconfig:1,15`) are a latent Kconfig hygiene issue (not a parity gap).

---

## P1 — symbol coverage

ROM symbol universe (`boot_out/symbols.txt`, 55 634 symbols) filtered on
`^(dhd_|wifi_platform_|bcmsdh)`: **439** symbols
(366 `dhd_*`, 64 `bcmsdh*`, 9 `wifi_platform_*`).

Method: every ROM name was checked against the built variant's sources (`.c`/`.h`);
compiler clones (`.constprop.*`, `.isra.*`, `.part.*` — 5 entries) were mapped to their
base name and the base was verified present/defined
(`dhd_bcmsdh_recv_buf`, `dhd_bcmsdh_send_buf` in `dhd_sdio.c`;
`dhd_get_concurrent_capabilites` in `dhd_common.c`/`dhd_linux.c`;
`dhd_get_hostreorder_pkts` in `dhd_cdc.c`; `dhd_sysfs_exit` in `dhd_linux.c`).

**434 of 439 symbols are implemented in `bcmdhd_1_77_nn` = 98.86 %** (≥98 % threshold).
All 439 names do exist somewhere in the Nura tree — the five below exist only in the
older `bcmdhd/` directory, which is not built.

| ROM symbol | ROM/old implementation | Built-variant equivalent | Why it is not a gap |
|---|---|---|---|
| `dhd_read_macaddr` | `bcmdhd/dhd_custom_sec.c:382` | `dhd_set_macaddr_from_file`, `bcmdhd_1_77_nn/dhd_custom_cis.c:292` (called `dhd_linux.c:3098` via `dhd_process_cid_mac`) | API renamed/reworked in the newer Broadcom release; same EFS-backed MAC file logic (`MACINFO_EFS`, `dhd_custom_cis.c:56`) |
| `dhd_wakelock_waive` | `bcmdhd/dhd_linux.c:8331` | `dhd_os_wake_lock_waive`, `bcmdhd_1_77_nn/dhd_linux.c:16167` | renamed |
| `dhd_wakelock_restore` | `bcmdhd/dhd_linux.c:8358` | `dhd_os_wake_lock_restore`, `bcmdhd_1_77_nn/dhd_linux.c:16193` | renamed |
| `dhd_os_sdtxlock` | `bcmdhd/dhd_linux.c:6895` | — | SDIO TX lock dropped in the newer release; no caller in `bcmdhd_1_77_nn` (build-internal API, not an external contract) |
| `dhd_os_sdtxunlock` | `bcmdhd/dhd_linux.c:6901` | — | same |

`wifi_platform_*` and `bcmsdh*` symbols are complete in the built variant, e.g.
`wifi_platform_set_power`/`_bus_enumerate`/`_get_irq_number` in
`dhd_linux_platdev.c`, `bcmsdh_sdmmc_probe`/`_suspend`/`_resume` in
`bcmsdh_sdmmc_linux.c`, `bcmsdh_oob_intr_register` in `bcmsdh.c`.

**Conclusion:** P1 passes. The only functional difference worth noting is that the ROM
used an older Broadcom driver release than the one Nura builds (see residual risk R3);
no port of the five symbols is required for coverage.

---

## P2 — device tree

Effective node in the Nura tree: `arch/arm64/boot/dts/exynos7580.dtsi:778-786`
(included by `exynos7580-j7elte_rev00.dts:13`):

```
wlan {
        compatible = "samsung,brcm-wlan";
        gpios = <&gpd1 0 0x1 /* WLAN_EN */
                 &gpa2 3 0x1 /* WLAN HOST WAKE GPIO_0 */>;
        pinctrl-names = "default";
        pinctrl-0 = <&cfg_wlanen &cfg_wlanhostwake>;
        status = "okay";
};
```

Compiled-DT comparison (dtc -s, sorted):

- Nura `/tmp/nura_j7.sorted.dts:5245-5251` vs ROM `/tmp/rom_000_800.sorted.dts:5367-5373`
  — identical property set and values (`gpios = <0x41 0x00 0x01 0x42 0x03 0x01>`).
- `cfg-wlanen` (gpd1-0: function 1, drv 3, pud 0, pud-pdn 0, con-pdn 3, val 0):
  Nura `:3618-3627` vs ROM `:3693-3703` — identical.
- `cfg-wlanhostwake` (gpa2-3: function 0, pud 1):
  Nura `:3332-3337` vs ROM `:3388-3394` — identical.

Only delta: the ROM's older dtc emits a redundant `linux,phandle` next to `phandle`
(ignored per contract); property values are equal. The driver consumes this node in
`dhd_custom_exynos.c` (`of_find_compatible_node(NULL, NULL, "samsung,brcm-wlan")`,
lines 208 and 231), requesting GPIO index 0 (`WLAN_REG_ON`, output) and, with
`CONFIG_BCMDHD_OOB_HOST_WAKE=y`, index 1 (`WLAN_HOST_WAKE`, input → `gpio_to_irq`).

**Conclusion:** P2 passes.

---

## P3 — config

`parity/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`:

| line | value | purpose |
|---|---|---|
| 1360 | `CONFIG_WLAN=y` | gate for the wireless subtree |
| 1363 | `CONFIG_WIFI_CONTROL_FUNC=y` | platform abstraction (declared `drivers/net/wireless/Kconfig:267`) |
| 1364 | `CONFIG_BROADCOM_WIFI=y` | `bcmdhd*/Kconfig` parent |
| 1372 | `CONFIG_BCM4343=y` | selects `bcmdhd_1_77_nn/` (Makefile:58-62) and sets `DRIVER_TYPE=y`, `-DOOB_INTR_ONLY` |
| 1373-1380 | `BCM4334/4335/4339/4354/43241/43455/4334W/43454` unset | only the 4343 variant is built |
| 1381 | `CONFIG_BCMDHD_FW_PATH="/system/etc/wifi/bcmdhd_sta.bin"` | **P4 gap** |
| 1382 | `CONFIG_BCMDHD_NVRAM_PATH="/system/etc/wifi/nvram_net.txt"` | **P4 gap** |
| 1383 | `CONFIG_BROADCOM_WIFI_RESERVED_MEM=y` | `-DCONFIG_DHD_USE_STATIC_BUF -DENHANCED_STATIC_BUF` (Makefile) |
| 1384 | `CONFIG_WIFI_BROADCOM_COB=y` | `dhd_sec_feature.h:69-72`: `#undef USE_CID_CHECK`, `#define READ_MACADDR` |
| 1385 | `CONFIG_WLAN_REGION_CODE=100` | EUR open (same as ROM build) |
| 1396 | `CONFIG_BCMDHD_OOB_HOST_WAKE=y` | `-DOOB_INTR_ONLY` + host-wake GPIO/IRQ |
| 1393 | `# CONFIG_BCMDHD_PREALLOC_MEMDUMP is not set` | matches ROM (not required) |

Dependencies also satisfied: `CONFIG_CFG80211=y` (931), `CONFIG_CFG80211_WEXT=y` (939),
`CONFIG_WEXT_CORE=y` (929), `CONFIG_RFKILL=y` (945), `CONFIG_MACH_UNIVERSAL7580=y` (308,
needed for `dhd_custom_exynos.o`/`dhd_custom_memprealloc.o` built by the parent Makefile).

**Conclusion:** P3 passes. Caveat from the variant section: `bcmdhd_1_77_nn/Kconfig` is
not sourced, so its (inactive) `BCMDHD_FW_PATH` default `/system/etc/firmware/fw_bcmdhd.bin`
never applies; the values in the config file are authoritative.

---

## P4 — firmware

### Why the current paths cannot work

`/system` does not exist on a Nura/Alpine rootfs, and `CONFIG_BCMDHD_FW_PATH`/`_NVRAM_PATH`
are the effective paths: with `CONFIG_BCM4343=y` the Makefile sets
`-DENABLE_INSMOD_NO_FW_LOAD`, `wl_android_init()` sets
`dhd_download_fw_on_driverload = FALSE` (`wl_android.c:6639`; default is `TRUE`,
`dhd_linux.c:893`), so `dhd_update_fw_nv_path()` takes the
`#ifdef CONFIG_BCMDHD_FW_PATH` / `#ifdef CONFIG_BCMDHD_NVRAM_PATH` branch
(`dhd_linux.c:9265-9271`) and copies those strings into `dhdinfo->fw_path` / `nv_path`.
Downloads go through `dhd_os_open_image()` → `filp_open()` (`dhd_linux.c:12974-12979`)
and `kernel_read()`, i.e. **absolute-path VFS open, not request_firmware** — so any
absolute path works and no driver code change is needed.

### Patch

`parity/patches/wifi/0001-j7elte-wifi-firmware-paths.patch` (pmaports tree, applies to
baseline `ba682bd`):

```
-CONFIG_BCMDHD_FW_PATH="/system/etc/wifi/bcmdhd_sta.bin"
-CONFIG_BCMDHD_NVRAM_PATH="/system/etc/wifi/nvram_net.txt"
+CONFIG_BCMDHD_FW_PATH="/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin"
+CONFIG_BCMDHD_NVRAM_PATH="/lib/firmware/postmarketos/samsung-j7elte/nvram_net.txt"
```

Verified with `git -C parity/pmaports apply --check` → `APPLY_OK`.

Path assumption (explicitly requested): `/lib/firmware` is the kernel firmware-loader
base and the Alpine/postmarketOS location for kernel-installed firmware/modules. Because
`dhd_os_open_image()` resolves the path through the VFS, it also works if `/lib` is a
symlink to `/usr/lib`; the directory must exist verbatim in the rootfs. No Nura rootfs
image is available in this workspace to verify the directory, so this is stated as an
assumption, not an observation.

### Blob mapping — `parity/firmware/files-wifi.list`

| ROM source (image path / runtime) | install path | sha512 |
|---|---|---|
| `/etc/wifi/bcmdhd_sta.bin` (`/system/etc/wifi/…`) | `/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin` | `45f068ac52fad1ccaab265fedf1e96ee7cd691b8944be2e044a22e1905c6b7bb39a12913b57b22ea3950664efbe4ce5d2fe2dc82f260ab612265b673913d4000` |
| `/etc/wifi/nvram_net.txt` | `…/nvram_net.txt` | `2e291ea1b2f20b749e6167d3814a24ecf26f819e468584cc44a106f840145b88c3dde4208b7bb708137e308c9c4051330bb56d113715b6853a25497c8705f23c` |
| `/etc/wifi/bcmdhd_apsta.bin` | `…/bcmdhd_apsta.bin` | `6113f39c018140d15cc0623006fea7c1ea62bdf65fcad2281905574d82befaea6246abf21a67231d60124cc7271626200647d3869340dcc70e0d7f73be45f0f6` |
| `/etc/wifi/bcmdhd_mfg.bin` | `…/bcmdhd_mfg.bin` | `28b91e82ff6ca5bdbc6bd59a85283cb6b109ed45ae257da332f5bdeec6f92f3766f68fd3a7031ae21b90aa074b46ba1634238f1d7c5afe4e1cb896103644787c` |
| `/etc/wifi/nvram_mfg.txt` | `…/nvram_mfg.txt` | `18dd44fedc0f1ec2e54f0c645a7b60949f1ad49a69343ad3e2aece894b1344c5a86abedc16dc2774cf4b232c68ae7de455769f701004aec31374dbb13179ee52` |

The ROM files were re-dumped from `system.raw` with `debugfs` during this analysis;
`bcmdhd_sta.bin` and `nvram_net.txt` are byte-identical to `parity/firmware/`
(`cmp` clean) and match `MANIFEST.sha512`. The manifest records `/etc/wifi/…` because
debugfs paths are relative to the system-image root; at runtime Android mounts that image
at `/system`, which is why the old config said `/system/etc/wifi/…`.

Only `bcmdhd_sta.bin` and `nvram_net.txt` are opened by this configuration. SoftAP uses
the same STA firmware plus the `apsta` iovar (`dhd_linux.c:9229,10438`); `bcmdhd_apsta.bin`
and `bcmdhd_mfg.bin` are not selected by `CONFIG_BCMDHD_FW_PATH`. They are mapped anyway
because the ROM ships them and manufacturing/`mfg` mode uses them.

### CLM blob — no extra file needed

With `-DDHD_BLOB_EXISTENCE_CHECK` and `-DCONCATE_BLOB`
(`bcmdhd_1_77_nn/Makefile:283,782`), the driver probes the hard-coded default
`CONFIG_BCMDHD_CLM_PATH` = `/system/etc/wifi/bcmdhd_clm.blob` (`dhd.h:303-309`) via
`dhd_set_blob_support()` (`dhd_linux.c:18559-18575`). That file does not exist in the ROM
system image either (`debugfs ls /etc/wifi` shows only the five files above plus
supplicant configs). Result: `is_blob = FALSE`, the firmware path is not suffixed with
`_blob`, and `dhd_apply_default_clm()` (`dhd_common.c:4792`) falls back to validating the
CLM data embedded in the firmware (`dhd_check_current_clm_data`). Behaviour is the same on
ROM and Nura, so no CLM blob or code change is required. (The `/system` path fails
harmlessly here.)

---

## Gaps outside the allowed edit set

**G1 — no firmware package exists.** The pmaports baseline contains only
`device/archived/device-samsung-j7elte` and `device/archived/linux-samsung-j7elte`; there
is no firmware package and no blob in the tree. The five files must be installed at the
paths above (e.g. by a new `firmware-samsung-j7elte` package or by the kernel package's
`package()`, both of which need the actual blobs, which are not in this workspace's
pmaports tree). Until then P4 is satisfied only at the config level.

**G2 — config source hash.** `linux-samsung-j7elte/APKBUILD` lists
`config-samsung-j7elte.aarch64` in `sha512sums` (baseline hash
`23e49e4993a98684717beaca7ea44f2cd4e0079460e573acc93237aa8b1d55115c72a22c0f16f3b8638c5b82ab57ea71ee8c043bf61d89f7cb190711e87d0156`).
Applying the config patch invalidates it. Since my ownership is limited to the config
file, the APKBUILD line is not part of the patch; it must be regenerated at integration
time (after all agents' config changes). With this patch alone the new hash is:

```
34466397a05cc2deef8f898890234da3288ee2e27202f6b8a2d5ff78a2811edb49f1c3137ced64d3a3888457a005c9cf87c26286d3fbb6ec17eecd0a5577e8b5  config-samsung-j7elte.aarch64
```

---

## Residual risks / tests

- **R1 (firmware presence):** `/lib/firmware/postmarketos/samsung-j7elte/` must exist and
  contain `bcmdhd_sta.bin` + `nvram_net.txt` before the built-in driver registers wlan0;
  otherwise `dhd_update_fw_nv_path()` fails with “firmware path not found”. No rootfs was
  available to verify the directory (assumption stated above).
- **R2 (MAC address):** `CONFIG_WIFI_BROADCOM_COB=y` forces `READ_MACADDR`
  (`dhd_sec_feature.h:69-72`); `dhd_set_macaddr_from_file()` reads
  `/efs/wifi/.mac.info` (`dhd_custom_cis.c:56,292`) and, if absent, generates a random
  MAC and writes it back to the same path. Nura does not mount `/efs` (deviceinfo has no
  EFS mount), so the write fails and the fallback cannot persist; the MAC then falls back
  to the NVRAM value `macaddr=00:90:4c:c5:12:38` in `nvram_net.txt`, i.e. the same address
  on every device. `dhd_process_cid_mac()` ignores the error (`dhd_linux.c:3092-3100`).
  Needs a runtime check (`ip link show wlan0`) and, if duplicates matter, an EFS mount or
  a pre-seeded per-device MAC file.
- **R3 (variant delta):** ROM shipped the older `bcmdhd/` release, Nura builds
  `bcmdhd_1_77_nn/` (LineageOS choice for this device). Five internal symbols differ
  (P1 table); externally visible behaviour was not runtime-tested. Static review found the
  firmware/NVRAM/OOB/COB paths equivalent.
- **R4 (build limitation):** no aarch64 toolchain is installed. Compilation of
  `bcmdhd_1_77_nn` with this 3.10.108 config (e.g. the `-DUSE_CID_CHECK` vs
  `#undef USE_CID_CHECK` combination, `CONFIG_64BIT` dropping `BCMDHDMA32`) was not
  verified; only source/config/DT/symbol analysis was possible.
- **R5 (OOB IRQ):** host wake is DT-driven (gpa2-3, `cfg-wlanhostwake`, active-high IRQ)
  and `-DOOB_INTR_ONLY` is set; DT matches ROM exactly, so no risk identified beyond
  `gpio_to_irq` availability on this SoC, which the DT already uses.
- **R6 (regulatory):** NVRAM ships `ccode=GB` and region code 100 (EUR open), identical to
  ROM; no change made.

Suggested runtime tests once an image boots: `dmesg | grep -i bcmdhd` for the
`firmware path`/`nvram path` lines and OOB IRQ registration, `ip link show wlan0` (MAC),
`iw dev wlan0 scan`, a WPA2 association, suspend/resume (host-wake IRQ), and SoftAP mode.
