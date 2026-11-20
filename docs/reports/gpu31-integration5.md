# gpu31 integration5 — full candidate with the fixed series (Mode-B v2 render-only)

Date: 2026-11-19/20. Owner: G31_Integrator5. Phone: M1 image (#37, UI
healthy, no /dev/dri) — Main flashes. DO NOT FLASH from this ticket; this
report ends in a flash REQUEST for Main.

Fixed series on top of 20ea233: R1 scheduler init (b199c8f/20ea233),
SUBMIT deadlock + timeouts (c553b8e + repair e4edb3a), render-only
registration (02bd790, Mode-B v2 with DRIVER_RENDER_ONLY).

Prior fallback shas (untouched): bindboot
`fcec651e8af6f23e5b6d4ff552c2c6e4cf1337a7ab08d3dd36f0f038`,
candidate-1 kernel-boot
`cf25ed70948963ddbe4c364380b723bd988d2e5fce15cb68be726`.
int4 candidate (superseded — card0 + renderD128, X wedges):
Image `c31bb8f2…765b58541`, boot
`/tmp/kernel-boot-int4.img` `5678dca4…84ab`.

## 1. Series applied (scratch-int5 only; shared kernel tree never edited)

Scratch:
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch-int5`
(fresh `cp -a` of pristine
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`).
Applied with `git apply --check` + `git apply`, in order
(all `--check` clean; full apply re-verified on a throwaway copy first):

| # | Source file (repo, committed) | Target | Commit |
|---|---|---|---|
| 1 | `patches/drm-backport/0001-dma-fence-minimal-subset.patch` | `drivers/base/dma-fence.c`, `include/linux/dma-fence.h` | f698920 (G31_Sched3; review fixes) |
| 2 | `patches/drm-backport/0002-dma-resv-shim.patch` | `drivers/base/dma-resv.c`, `include/linux/dma-resv.h` | f698920 |
| 3 | `patches/drm-backport/0003-drm_syncobj-subset.patch` | `drivers/gpu/drm/drm_syncobj.[ch]` (local header) | f698920 |
| 4 | `patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch` | `drivers/gpu/drm/drm_sched.[ch]` (local header) | f698920 |
| 5 | `patches/drm-backport/0005-build-wiring-for-shims.patch` | `drivers/base/Makefile`, `drivers/gpu/drm/Makefile` | untouched by sched rework |
| 6 | `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch` | `drm_platform.c`, `drm_stub.c`, `drm_fops.c`, `drm_drv.c`, `include/drm/drmP.h` | 02bd790 Mode-B **v2** (DRIVER_RENDER_ONLY, render-ONLY) |
| 7 | `code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch` | `drivers/gpu/drm/drm_gem_cma_helper.c` (2 hunks) | pre-existing dmabuf slice; REQUIRED (int4 §4 item 1: arm64 has no `dma_alloc_writecombine`) |
| 8 | `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | `drivers/gpu/drm/panfrost/*` (17 new files) | 02bd790 (on 20ea233: submit c553b8e+e4edb3a, open b199c8f, render-only companion) |
| 9 | `code/gpu31/driver/0002-panfrost7580-build-wiring.patch` | `drivers/gpu/drm/{Kconfig,Makefile}`, `panfrost/{Kconfig,Makefile}` | 12bdaec (`-Idrivers/gpu/drm` ccflags) |

`code/gpu31/series/0001-drm-exynos7580-decon-dsim-kms.patch` (display
swap) is DELIBERATELY NOT applied — render-only build, no double-drive.
NOT applied as a series item: `patches/drm/0002` DT patch (stock
`dt.img` rule, sha256 `352bda9b…8f937` unchanged).

Patch sha256 (built bytes):

```
c109abac27251c77fe4c914c911a2f3d9ed8627cb6cb03de3f5e2d0099c74d6e  backport/0001
9d0e58ee69facf0c22eddbf2620abc4112ccca48a2196110e317a3d9089133c1  backport/0002
d0282eea302afba61719740d24f5dbc9c0b3abd9f91e99b1546b865a4896b2c5  backport/0003
7722a6700557f1c12a905c5d233f4a7d5951a42762f7ffee82e98b62f1db  backport/0004
68a6756f15d3668a0a9c56fc10fad67eddbf53a3781fc6adc3035beafd6ce557  backport/0005
f815228c458f9ccfbec672aec2a2815fa43dc2add74ac606619bf631ac816a1f  kms/Mode-B-v2 (02bd790)
75a287ea8308504f63af04356b2a373bd2657678e1ad7aa4866a1ca8f567590c  dmabuf/0001-CMA-port
dbeae09d7498a4b782d879cc22b90fdee0a63255167138e9ed50c103463e7473  driver/0001 (02bd790 on 20ea233)
a4d9c2ae623d0fd4b087a6c42270bc61c49f5d02ea511e0a9f72dcea18565d54  driver/0002 (12bdaec)
```

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig closure)

Base: `code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`.
Fragments: `code/gpu31/config/fragments/d0-m1-minimal.fragment`,
`code/gpu31/config/fragments/d4-panfrost7580.fragment`
(`CONFIG_DRM_PANFROST7580=y`).
Closure result (`grep .config`): `CONFIG_MODULES=y`, `CONFIG_DRM=y`,
`CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_GEM_CMA_HELPER=y` (auto-select),
`CONFIG_DRM_EXYNOS=y`, `CONFIG_DRM_EXYNOS_DMABUF=y`,
`CONFIG_DRM_EXYNOS_G2D=y`, `CONFIG_DRM_PANFROST7580=y`,
`CONFIG_DMA_SHARED_BUFFER=y`.

Full `diff base .config` (DRM part; remainder is default-n closure noise —
`BT_HCIUART_*`, `I2C_STUB`, `COMEDI`, `RTLLIB`, `VT6656`,
`USB_ENESTORAGE`, `RBTREE_TEST`, `INTERVAL_TREE_TEST`, `CRYPTO_TEST` —
all `is not set`, no behaviour change):

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

No `DRM_EXYNOS7580`, no `DRM_KMS_CMA_HELPER`, no `FRAMEBUFFER_CONSOLE`.
Vendor fbdev intact: `EXYNOS_DECON_FB=y`, `EXYNOS_DECON_EXYNOS7580=y`,
`EXYNOS_MIPI_DSI=y`, `MALI_MIDGARD=y` (kbase), `CMA_SIZE_MBYTES=0`.

## 3. Build

Script: `code/gpu31/series/build-int5.sh` (committed herewith; fresh
scratch copy, `-j4`, external TMPDIR, stock-dt.img pack).
Log: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/logs/gpu31-int5-build.log`
(5392 lines, tail `LD vmlinux / SYSMAP / OBJCOPY Image`, RC=0).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
No scratch-side fixes were needed — all int4 findings are folded upstream
(CMA port is series item 7; ccflags in driver 0002; job.c comment closed
in driver 0001). The `error` grep hits (6) are benign: `KERN_ERR` macro
definitions and `scsi_error.o` compile lines, no build errors.

## 4. Symbol proof (vmlinux, aarch64)

- Probe: `p7580_drv_probe` present (`t ffffffc000577548`;
  platform-driver `.probe`, name `panfrost7580`, OF match on stock
  `mali@0x11400000`). No `panfrost7580_probe` symbol exists by design
  (count 0) — the driver names it `p7580_drv_probe` (acceptance
  criterion maps to this symbol).
- Core register path: `drm_get_platform_dev` `T ffffffc00055bf0c`
  (+ ksymtab). The driver attaches via `drm_platform_init` →
  `drm_get_platform_dev` (source `panfrost7580_main.c`), the only
  3.10 attach path.
- Render-only branch: `drm_get_minor` / `drm_put_minor` both `T`
  (+ ksymtab). `drm_get_platform_dev` disassembly:
  `ldr w0,[x0,#416]` (driver_features) → `tbnz w0,#15,<render>` —
  bit 15 = `DRIVER_RENDER_ONLY 0x8000` (`include/drm/drmP.h:163`).
  Render path (`+0x1c4`): `add x1,x19,#0x388; mov w2,#0x3;
  bl drm_get_minor` — type `#0x3` = `DRM_MINOR_RENDER`, target
  `dev->render` (`#0x388`), BEFORE `->load()`; legacy `DRM_INFO` on
  `dev->primary->index` skipped (render-index `DRM_INFO` instead);
  unwind `err_g3` releases render vs primary respectively.
  `drm_platform.o` (relocatable, pre-link) shows the same shape:
  `tbnz w0,#15` → render `bl drm_get_minor` with `w2=#0x3`, versus
  the legacy fallthrough (`tbz #13` MODESET → control `w2=#0x2`,
  then primary `w2=#0x1` at `#0x380`).
- No card0 allocation path in the compiled `drm_platform.o` evidence:
  the ONLY `DRM_MINOR_LEGACY` (`w2=#0x1`) call sits in the
  non-render-only fallthrough after the `tbnz #15` branch; a
  `DRIVER_RENDER_ONLY` device never reaches it — no primary idr slot,
  no `card%d` node from panfrost7580. (`dev->render` holder is new in
  v2; 3.10 has no `dev->render` — keeps unwind symmetric, no exynos
  patch.)
- No fbdev/kbase regression: kbase symbols linked (`560` matches for
  `mali_kbase|kbase_`; `CONFIG_MALI_MIDGARD=y`, initcall + `T`
  symbols), decon/fbdev symbols linked (`302` matches incl.
  `decon_fb_*`, `decon_fb_ops`, `exynos_drm_g2d`; `CONFIG_EXYNOS_DECON_FB=y`
  et al intact), `FRAMEBUFFER_CONSOLE` unset, DT untouched.

## 5. Image + sha256

- Image: `…/scratch-int5/arch/arm64/boot/Image`, 18144376 B,
  `a783e6cb0ccf27535e19ba7bf5f281f1cc9d18a2990e38068f292b94d3efd89e`.
- Boot image: `/tmp/kernel-boot-int5.img` (27283456 B = kernel
  18144376 + ramdisk 8822392 + dt 313344; ramdisk `ac0b6f8b…`,
  dt `352bda9b…` — both stock reference),
  `dc8928749e6ab2963f5d64058095efa897bf5515bc5537c124393c3938f8af4d`.
- Pack rule honoured: `dt.img` NEVER rebuilt (stock bytes reused;
  sha `352bda9bcf02bce50c9fc6ad6ca845727715fe1942fe25604d67f94e69d8f937`).

## 6. Flash request (QUEUED — DO NOT FLASH from this ticket)

Request for Main: flash `/tmp/kernel-boot-int5.img`
(`dc892874…f8af4d`) at Main's discretion via the normal
G31_M1Verify-gated path, with `/tmp/bindboot.img`
(`fcec651e…f038`) as fallback. Expected on device:
`/dev/dri/renderD128` ONLY from panfrost7580 (NO panfrost card0 —
v2 render-only; exynos/G2D card0 may be present per M1 shape),
vendor `/dev/fb0` + fbdev X untouched, kbase `/dev/mali0` present.
This ticket performs no flash, no reboot, no device contact.
