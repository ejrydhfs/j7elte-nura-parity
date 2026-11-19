# gpu31 integration2 — Mode-B render node + M1 + shim foundation (build record)

Date: 2026-11-19. Owner: G31_Integrator3. Phone UP kernel #37 (M1 flashed, DRM
core up, `/dev/mali0` present, NO `/dev/dri` — no driver bound). Host-side
build + host symbol proof only; no device contact this round.
Fallback `/tmp/bindboot.img` sha256
`fcec651e8af6f23e5b6d4ff552c2c6e4cf1336d8fc37a7c7ab08d3dd36f0f038`,
untouched. `/tmp/kernel-boot.img` (candidate 1, M1-only)
`cf25ed70948963ddbe8c3643800d723bd988d2e5fce16895b02cc51cb68be726`
untouched — superseded as flash candidate only on Main's go.
Flash gate: ONLY G31_M1Verify flashes, only on Main's explicit go. No reboot
by integrator.

## 1. Series applied (scratch tree only; shared kernel tree never edited)

Scratch: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch`
(copy of `android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`).

| # | Source file (repo) | Target | State |
|---|---|---|---|
| 1 | `code/gpu31/series/0001-dma-fence-minimal-subset.patch` | `drivers/base/dma-fence.c`, `include/linux/dma-fence.h` | applied (pre-existing) |
| 2 | `code/gpu31/series/0002-dma-resv-shim.patch` | `drivers/base/dma-resv.c`, `include/linux/dma-resv.h` | applied (pre-existing) |
| 3 | `code/gpu31/series/0003-drm_syncobj-subset.patch` | `drivers/gpu/drm/drm_syncobj.[ch]` (local header, not `include/drm/`) | applied (pre-existing) |
| 4 | `code/gpu31/series/0004-drm_sched-panfrost-job-only-shim.patch` | `drivers/gpu/drm/drm_sched.[ch]` (local header) | applied (pre-existing) |
| 5 | `code/gpu31/series/0005-build-wiring-for-shims.patch` | `drivers/base/Makefile`, `drivers/gpu/drm/Makefile` | applied (pre-existing; `dma-fence.o dma-resv.o` under `CONFIG_DMA_SHARED_BUFFER`, `drm_syncobj.o drm_sched.o` in `drm-y`) |
| 6 | `code/gpu31/series/0001-drm-exynos7580-decon-dsim-kms.patch` | `drivers/gpu/drm/exynos7580/*` + CMA helper, Kconfig/Makefile | applied (pre-existing), compiled, NOT enabled (`# CONFIG_DRM_EXYNOS7580 is not set` — no double-drive) |
| 7 | `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch` | `drm_platform.c`, `drm_stub.c`, `drm_fops.c`, `drm_drv.c`, `include/drm/drmP.h` | applied (pre-existing; hunks verified in tree: `dev->render` alloc/unwind, put/unplug release, render-auth open, MASTER-on-render deny) |
| C | `code/gpu31/config/fragments/d0-m1-minimal.fragment` (= M1 6-symbol set; sha differs from `series/m1-kconfig.fragment` in comments only) | `.config` via `olddefconfig` closure | resolved |

`code/gpu31/series/SHA256SUMS` covers items 1–6 + `m1-kconfig.fragment`.
Mode-B patch sha256
`438e4f095343893c54774da35cc1202cd17002c33e18d985049ff946065bdf26`.
`code/gpu31/series/build-int2.sh` is the reproducible rebuild script
(serial `-j1`, external TMPDIR; Main 2026-11-19 raised headroom to `-j4`,
adopted for the NEXT build — this one was already mid-flight and was kept
serial to avoid restarting over truncated objects).

NOT applied: `patches/drm/0002` DT patch (stock `dt.img` rule);
`patches/drm/config-drm-exynos7580.fragment` (display-swap, M2-gated);
panfrost7580 driver series (G31_DriverAssemble2 in flight — folds into
integration3 on delivery; `CONFIG_DRM_PANFROST7580` therefore absent here).

## 2. Config (closure of d0-m1-minimal on pmaports base)

Base: `code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`.
`diff base .config` DRM-relevant closure:

```diff
< # CONFIG_MODULES is not set
> CONFIG_MODULES=y
< # CONFIG_DRM is not set
> CONFIG_DRM=y
> CONFIG_DRM_KMS_HELPER=y
> # CONFIG_DRM_LOAD_EDID_FIRMWARE is not set
> # I2C encoder or helper chips: CH7006/SIL164/NXP_TDA998X all not set
> CONFIG_DRM_EXYNOS=y
> CONFIG_DRM_EXYNOS_DMABUF=y
> # CONFIG_DRM_EXYNOS_FIMD/HDMI/VIDI/IPP not set; CONFIG_DRM_EXYNOS_G2D=y
> # CONFIG_DRM_EXYNOS7580 is not set
> # CONFIG_DRM_UDL is not set
> CONFIG_HDMI=y            # auto-select of CONFIG_DRM=y, pre-existing stock dep
```

Non-DRM closure noise (from `olddefconfig` resolving newer Kconfig symbols,
all default-n/off): `BT_HCIUART_{BCSP,ATH3K,LL,3WIRE}`, `I2C_STUB`, `COMEDI`,
`RTLLIB`, `VT6656`, `USB_ENESTORAGE`, `RBTREE_TEST`, `INTERVAL_TREE_TEST`,
`CRYPTO_TEST`. No behavioural change from these (verified: all `is not set`).

Vendor fbdev intact (`.config` grep): `EXYNOS_DECON_FB=y`,
`EXYNOS_DECON_EXYNOS7580=y`, `EXYNOS_MIPI_DSI=y`, `PANEL_EA8061_DYNAMIC=y`,
kbase `MALI_MIDGARD/T7XX/R15P0=y`; `FRAMEBUFFER_CONSOLE` unset;
`CMA_SIZE_MBYTES=0`.
`d4-panfrost7580.fragment` (`CONFIG_DRM_PANFROST7580=y`) is staged untracked
in-tree but NOT resolved into this build — takes effect in integration3.

## 3. Build

Command (`code/gpu31/series/build-int2.sh`):
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j1 Image`
with `TMPDIR=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/tmp`.
Log: `gpu31/logs/gpu31-int2-serial-build.log` (external drive; 496
`warning:` lines, all pre-existing stock GCC-16 noise — same families as the
M1 baseline: `array-bounds`, `dangling-pointer`, `address`, `format-truncation`;
zero `error:` lines).

Result: RC=0. `arch/arm64/boot/Image` 18 110 712 B,
sha256 `93ef21ab1d1046e4a253f7ea9b2c4273ab7da42207047ba34faca6d330726fab`.
(Note: byte-identical size to candidate 1 with different hash — expected:
same M1 config, Mode-B adds the render path.)

Boot image: `python3 /tmp/mkboot.py Image /tmp/bimg/ramdisk-new.gz
/tmp/bimg/dt.img <out> "console=tty0 loglevel=7"` →
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/kernel-boot-int2.img`
27 250 688 B,
sha256 `6708ba40a480ce9145d67b499ba69e61ddadc8bed17695a799f391c898edb9f6`
(stock `dt.img` + `ramdisk-new.gz` + cmdline reused; pack script printed
`kernel 18110712, ramdisk 8822392, dt 313344`).

Prior art: an earlier `-jN` attempt in this window died at
`kernel/irq/resend.o` with `Broken pipe` under `scripts/Makefile.build` —
parallel-make artifact on the constrained host, not a source error; the
serial rerun above is clean (RC=0). Kept serial per the "one build at a
time, external TMPDIR" rule then in force.

## 4. Symbol proof (host, from scratch objects/vmlinux — NOT device dmesg)

`nm vmlinux` (all `T`, i.e. global text):

```text
dma_fence_add_callback  drm_sched_init      drm_syncobj_create
dma_fence_context_alloc drm_sched_job_alloc exynos_drm_platform_probe (t, static)
drm_get_minor           drm_ioctl           drm_platform_init
```

Render-minor path proof (this 3.10 tree has no `drm_dev_register` —
post-3.10 API, correctly absent: `grep -c drm_dev_register vmlinux` = 0;
acceptance maps to the platform path that exists here):

- `drm_get_minor` exported (`T` in `built-in.o` + `__ksymtab`) and called
  THREE times in `drm_platform.o:dis(drm_get_platform_dev)` with
  `w2 = #0x2` (CONTROL), `#0x1` (LEGACY/primary), `#0x3` (`DRM_MINOR_RENDER`
  = 3, `include/drm/drmP.h:986`).
- `dev->render` released/unplugged in `drm_stub.c:482-502`
  (`drm_put_minor`/`drm_unplug_minor`, NULL-guarded).
- `renderD%d` sysfs naming present (`strings drm_sysfs.o`, `drm_sysfs.c:513`).
- Per-minor MASTER-on-render deny in `drm_drv.c:442-446`; render-authenticated
  open in `drm_fops.c:283-285`.
- `dma_resv_*` + `drm_sched_init`/`drm_syncobj_create` present in
  `drivers/base/built-in.o` / `drivers/gpu/drm` objects.
- `panfrost7580_probe`: absent (`grep -c panfrost7580 vmlinux` = 0) —
  EXPECTED, driver series pending (integration3).
- `drivers/gpu/drm/exynos7580/*.o`: none built — EXPECTED
  (`CONFIG_DRM_EXYNOS7580 is not set`, code-only).

Expected on device (M1Verify-owned, post-flash): `/dev/dri/card0`,
`/dev/dri/controlD64` (DRIVER_MODESET), `/dev/dri/renderD128`; zero
connectors; `/dev/fb0` + fbdev X intact. `panfrost7580_probe` appears only
from integration3.

## 5. Verdict + flash request

PASS (build + host symbol proof). Candidate:
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/kernel-boot-int2.img`
(`6708ba40…`). Request: Main to gate G31_M1Verify flash at will
(`dd` + `m1-verify.sh` + render-node probe: card0/controlD64/renderD128,
unprivileged PRIME HANDLE_TO_FD, SETCRTC-from-render → `-EACCES`).
Rollback: `/tmp/bindboot.img`. Next: integration3 folds the panfrost7580
driver series (G31_DriverAssemble2 delivery) + `d4-panfrost7580.fragment`
(`CONFIG_DRM_PANFROST7580=y`) on top of this exact baseline.
