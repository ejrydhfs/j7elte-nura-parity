# gpu31 integration4 — backport 0001-0005 + Mode-B + panfrost7580 + M1/d4 (build record)

Date: 2026-11-19. Owner: G31_Integrator4. Phone stays OFF USB-net
(RNDIS unbound); all work is host-side. DO NOT FLASH — this report ends
in a flash REQUEST for Main; only Main (via G31_M1Verify) authorizes any
flash, and the fallback `/tmp/bindboot.img` stays untouched.

Prior fallback shas (untouched): bindboot
`fcec651e8af6f23e5b6d4ff552c2c6e4cf1337a7ab08d3dd36f0f038`,
candidate-1 kernel-boot
`cf25ed70948963ddbe4c364380b723bd988d2e5fce15cb68be726`.

## 1. Series applied (scratch-int4 only; shared kernel tree never edited)

Scratch: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch-int4`
(fresh `cp -a` of pristine
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`).
Applied with `git apply --check` + `git apply`, in order:

| # | Source file (repo, committed) | Target | Commit |
|---|---|---|---|
| 1 | `patches/drm-backport/0001-dma-fence-minimal-subset.patch` | `drivers/base/dma-fence.c`, `include/linux/dma-fence.h` | f698920 (G31_Sched3; review fixes) |
| 2 | `patches/drm-backport/0002-dma-resv-shim.patch` | `drivers/base/dma-resv.c`, `include/linux/dma-resv.h` | f698920 |
| 3 | `patches/drm-backport/0003-drm_syncobj-subset.patch` | `drivers/gpu/drm/drm_syncobj.[ch]` (local header) | f698920 |
| 4 | `patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch` | `drivers/gpu/drm/drm_sched.[ch]` (local header) | f698920 |
| 5 | `patches/drm-backport/0005-build-wiring-for-shims.patch` | `drivers/base/Makefile`, `drivers/gpu/drm/Makefile` | untouched by sched rework |
| 6 | `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch` | `drm_platform.c`, `drm_stub.c`, `drm_fops.c`, `drm_drv.c`, `include/drm/drmP.h` | pre-existing |
| 7 | `code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch` | `drivers/gpu/drm/drm_gem_cma_helper.c` (2 hunks) | pre-existing dmabuf slice; REQUIRED (see §4) |
| 8 | `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | `drivers/gpu/drm/panfrost/*` (17 new files) | 12bdaec (via 06f55df canonical job.c) |
| 9 | `code/gpu31/driver/0002-panfrost7580-build-wiring.patch` | `drivers/gpu/drm/{Kconfig,Makefile}`, `panfrost/{Kconfig,Makefile}` | 12bdaec (adds `-Idrivers/gpu/drm` to ccflags) |

`code/gpu31/series/0001-drm-exynos7580-decon-dsim-kms.patch` (display
swap) is DELIBERATELY NOT applied — render-only build, no double-drive.
NOT applied as a series item: `patches/drm/0002` DT patch (stock
`dt.img` rule, sha256 `352bda9b…8f937` unchanged).

Patch sha256 (built bytes; `code/gpu31/series/0001-0004` are byte-identical
mirrors of `patches/drm-backport/0001-0004` per G31_Sched3):

```
c109abac27251c77fe4c914c911a2f3d9ed8627cb6b03de3f5e2d0099c74d6e  backport/0001
9d0e58ee69facf0e22eddb2620abc4112ccca48a2196110e317a3d9089133c1  backport/0002
d0282eea302afba61719740d24f5dbc9c0b3abd9f91e1546b865a4896b2c5  backport/0003
7722a67005571c1a905c5d233f4a7d5951a42762f7ffee82e98b62f1db  backport/0004
68a6756f15d3668a0a9c56fc10fad67eddbf53a3781fc6adc3effb6adc557  backport/0005
438e4f095343893da74cc2dc01cd17002cd1713e8d985049ff946065bdf26  kms/Mode-B
75a287ea8308504f63af5626b7352657678e1ad7aa4866a1ca8f567590c  dmabuf/0001-CMA-port
99eef33f3511ffcd1634045fc05b45565e07ef9faef30d2ea06531804a552e37  driver/0001 (12bdaec)
5a9dcdc73aad2f029f51709bd26c991c82e797c43c8e736f6ad60b2438  driver/0002 (12bdaec)
```

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig closure)

Base: `code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`.
Fragments: `code/gpu31/config/fragments/d0-m1-minimal.fragment`
(`8ca1754c…0773ee4`), `code/gpu31/config/fragments/d4-panfrost7580.fragment`
(`7d8848cc…93928f38`, `CONFIG_DRM_PANFROST7580=y`).
Closure result (`grep .config`): `CONFIG_MODULES=y`, `CONFIG_DRM=y`,
`CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_GEM_CMA_HELPER=y` (auto-select of
the panfrost symbol), `CONFIG_DRM_EXYNOS=y`, `CONFIG_DRM_EXYNOS_DMABUF=y`,
`CONFIG_DRM_EXYNOS_G2D=y`, `CONFIG_DRM_PANFROST7580=y`,
`CONFIG_DMA_SHARED_BUFFER=y`.

Full `diff base .config`: 54 lines (`/tmp` copy at build time; DRM part):

```diff
< # CONFIG_MODULES is not set
> CONFIG_MODULES=y
< # CONFIG_DRM is not set
> CONFIG_DRM=y
> CONFIG_DRM_KMS_HELPER=y
> CONFIG_DRM_GEM_CMA_HELPER=y
> CONFIG_DRM_EXYNOS=y
> CONFIG_DRM_EXYNOS_DMABUF=y
> CONFIG_DRM_EXYNOS_G2D=y
> CONFIG_DRM_PANFROST7580=y
> CONFIG_HDMI=y            # auto-select of CONFIG_DRM=y, pre-existing stock dep
```

Remainder is default-n closure noise (`BT_HCIUART_*`, `I2C_STUB`,
`COMEDI`, `RTLLIB`, `VT6656`, `USB_ENESTORAGE`, `RBTREE_TEST`,
`INTERVAL_TREE_TEST`, `CRYPTO_TEST` — all `is not set`, no behaviour change).
No `DRM_EXYNOS7580`, no `DRM_KMS_CMA_HELPER`, no `FRAMEBUFFER_CONSOLE`.
Vendor fbdev intact: `EXYNOS_DECON_FB=y`, `EXYNOS_DECON_EXYNOS7580=y`,
`EXYNOS_MIPI_DSI=y`, `PANEL_EA8061_DYNAMIC=y`, `MALI_MIDGARD=y`
(kbase), `CMA_SIZE_MBYTES=0`.

## 3. Build

Script: `code/gpu31/series/build-int4.sh` (committed herewith; fresh
scratch copy, `-j4`, external TMPDIR, stock-dt.img pack).
Log: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/logs/gpu31-int4-build.log`
(5342 lines, tail `LD vmlinux / SYSMAP / OBJCOPY Image / RC=0`).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

Two scratch-side fixes were needed mid-build (both owned + folded by
peers; scratch carries equivalent bytes — see §4):
`panfrost7580_job.c` missing `*/`, and `-Idrivers/gpu/drm` ccflags.

## 4. Integration fixes found by this build (all folded upstream)

1. **CMA helper does not compile on arm64** (fatal, `drm_gem_cma_helper.o`):
   stock `drm_gem_cma_helper.c` calls `dma_alloc/free_writecombine`,
   which exist only in `arch/arm/include/asm/dma-mapping.h` (32-bit
   ARM). Fix: applied `code/gpu31/dmabuf/0001-*-writecombine.patch`
   (dma_alloc/free_attrs + `DMA_ATTR_WRITE_COMBINE`, the exynos-proven
   pattern) as series item 7. Driver keeps
   `select DRM_GEM_CMA_HELPER`; without this port the select is
   unbuildable. Integrator rule: any `CONFIG_DRM_PANFROST7580=y` build
   MUST also apply the dmabuf 0001 port.
2. **Shim headers unresolvable** (fatal, `panfrost7580_main.o`):
   0003/0004 headers are local to `drivers/gpu/drm/`; no `-I` covered
   them. Fix (G31_DriverAssemble2, driver 0002 in 12bdaec): ccflags-y
   gains `-Idrivers/gpu/drm`.
3. **job.c header comment unclosed** (fatal, `panfrost7580_job.o`
   `msleep` implicit): `/*` opened at line 2 never closed; all 9
   includes swallowed, `delay.h` lost. Fix (G31_MmuFix 06f55df,
   re-snapshotted into driver 0001 by 12bdaec): one ` */` line.
   Scratch-int4 carries the identical line (applied by hand before the
   peer commit landed; `diff` of the hunk region is empty).

## 5. Symbol proof (vmlinux, aarch64)

- Probe: `p7580_drv_probe` present (`T`-local `ffffffc0005774c8 t`;
  platform-driver `.probe`, name `panfrost7580`, OF match on stock
  `mali@0x11400000`). No `panfrost7580_probe` symbol exists by design —
  the driver names it `p7580_drv_probe` (acceptance criterion maps to
  this symbol; source `panfrost7580_main.c:386`).
- Core register path: `drm_get_platform_dev` `T ffffffc00055beec`
  (+ ksymtab). The driver attaches via `drm_platform_init` →
  `drm_get_platform_dev` (source `panfrost7580_main.c:480`), the only
  3.10 attach path; `p7580_drm_driver.num_ioctls` + `dev_private`
  published post-init.
- Render-minor allocation path (Mode-B): `drm_get_minor` /
  `drm_put_minor` both `T` (+ ksymtab); `drm_get_platform_dev`
  disassembly shows exactly 3 `bl drm_get_minor` calls with type args
  `#0x1` (LEGACY/primary), `#0x3` (RENDER, at `+0xf4`:
  `add x1,x19,#0x388; mov w2,#0x3; bl drm_get_minor`), and the
  control-minor call — matching `drm_platform.c:66-83`
  (control iff MODESET, primary, then render `dev->render`).
  Unwind verified in source: `drm_stub.c:482-483` (`drm_put_minor`),
  `:501-502` (`drm_unplug_minor`).
- No fbdev/kbase regression: kbase symbols linked (`560` matches for
  `mali_kbase|kbase_`; `CONFIG_MALI_MIDGARD=y`), decon/fbdev symbols
  linked (`179` matches, `CONFIG_EXYNOS_DECON_FB=y` et al intact),
  `FRAMEBUFFER_CONSOLE` unset, DT untouched.

## 6. Image + sha256

- Image: `…/scratch-int4/arch/arm64/boot/Image`, 18144376 B,
  `c31bb8f2227e2daa839d508fbb59f9b8f7035ed43ce50934bb7ff57665b58541`.
- Boot image: `/tmp/kernel-boot-int4.img` (27283456 B = kernel
  18144376 + ramdisk 8822392 + dt 313344; ramdisk `ac0b6f8b…`,
  dt `352bda9b…` — both stock reference),
  `5678dca4b32b17df63ba377d5c1e908053ec07a425652dbd58b4ead2348d84ab`.
  Copy: `…/gpu31/candidate4-kernel-boot.img` (same sha).
- Pack rule honoured: `dt.img` NEVER rebuilt (stock bytes reused).

## 7. Flash request (QUEUED — DO NOT FLASH from this ticket)

Request for Main: flash `/tmp/kernel-boot-int4.img`
(`5678dca4…84ab`) at Main's discretion via the normal
G31_M1Verify-gated path, with `/tmp/bindboot.img`
(`fcec651e…f038`) as fallback. Expected on device: `/dev/dri/card0`
(exynos/G2D, primary) PLUS `/dev/dri/renderD128` (panfrost7580, no
DRIVER_MODESET), vendor `/dev/fb0` + fbdev X untouched, kbase
`/dev/mali0` present. This ticket performs no flash, no reboot, no
device contact.
