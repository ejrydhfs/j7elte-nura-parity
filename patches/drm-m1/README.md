# M1 — DRM core + render/card node WITHOUT the display-driver swap

Goal: `CONFIG_MODULES` + `CONFIG_DRM` fragment so that `/dev/dri/card0`
(and `controlD64`) exist while the vendor `EXYNOS_DECON_FB` fbdev stack keeps
driving the panel and fbdev X keeps working at 720x1280. This is the M1
milestone: presence of the DRM device node, not modesetting.

## What the fragment does (`m1-kconfig.fragment`, 6 symbols)

| Symbol | Why |
|---|---|
| `CONFIG_MODULES=y` | Staging only. No `.ko` built in M1; groundwork for the M2 kbase-shim module path. |
| `CONFIG_DRM=y` | DRM core. Auto-selects `HDMI/I2C/I2C_ALGOBIT/DMA_SHARED_BUFFER` (all already `=y`). Deps (`AGP=n`, `MMU=y`) satisfied by stock config. |
| `CONFIG_DRM_KMS_HELPER=y` | KMS helpers. Selects `FB` (already `=y`); `FRAMEBUFFER_CONSOLE` select is gated on `!EXPERT` and stock has `EXPERT=y`, so no fbcon churn. |
| `CONFIG_DRM_EXYNOS=y` | In-tree exynos DRM core (`exynos_drm_drv.c`) + GEM ioctls. Binds only subdrivers whose platform devices exist. |
| `CONFIG_DRM_EXYNOS_G2D=y` | Standalone 2D blitter. OF match (`samsung,exynos5250-g2d`) matches nothing in the stock j7elte DT, so its probe never fires — it only guarantees `fine_cnt>=1` in `exynos_drm_device_register()` (manager-less subdriver path) so `load()` returns 0 and the already-created primary node survives. |
| `CONFIG_DRM_EXYNOS_DMABUF=y` | Zero-cost ioctl surface; M2 prime groundwork. |

Deliberately omitted: `DRM_KMS_CMA_HELPER` (nothing in this exynos snapshot
selects or references it — `grep DRM_*CMA_HELPER drivers/gpu/drm/exynos/`
is empty; exynos carries its own `dma_alloc_attrs` GEM backend in
`exynos_drm_gem.c`/`exynos_drm_buf.c`; the only selectors are
tilcdc/shmobile/imx, all with unsatisfied deps on arm64 — so CMA-vs-TTM is
moot on M1), all display subdrivers (`FIMD/HDMI/IPP/FIMC/GSC/ROTATOR/VIDI`),
`FRAMEBUFFER_CONSOLE`, any vendor-fbdev off-switch, any CMA-size bump.

## Why `/dev/dri` appears with no modeset driver

`platform_device_register_simple("exynos-drm")` →
`exynos_drm_platform_probe` → `drm_platform_init` → `drm_get_platform_dev`
(`drivers/gpu/drm/drm_platform.c`) runs `drm_fill_in_dev` +
`drm_get_minor(PRIMARY)` **first**, then calls `load()` →
`exynos_drm_device_register()` (`drivers/gpu/drm/exynos/exynos_drm_core.c`).
With M1 the subdriver list holds only G2D (`manager==NULL` → `fine_cnt++`,
no encoder/connector), `load()` returns 0, and `exynos_drm_fbdev_init()`
early-returns 0 when `num_connector==0`. Expected M1 state: `card0` +
`controlD64` present, zero connectors/CRTCs, `/dev/fb0` + X untouched.

## DT / display-driver answer: NO (default)

No in-repo `exynos7580` DRM display driver and no DT compatible change is
needed for M1. The `patches/drm/0001+0002` display-driver swap (new
`samsung,exynos7580-decon-drm`/`-dsim` compatibles + DT edits) is M2 scope.
M1 compiles only pre-existing in-tree exynos objects and modifies no
`.dts/.dtsi`, so the stock `dt.img` keeps booting (no S-Boot hang risk).

## Kconfig closure proof (no Image build, no tree writes)

Run: `sh code/kernel/m1/m1-kconfig-check.sh` (private `/tmp/m1proof` out-dir
against the read-only tree; merges the fragment, then
`make O=… ARCH=arm64 olddefconfig`). Resolved values (verified 2026-11-18):

```text
CONFIG_MODULES=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_EXYNOS=y
CONFIG_DRM_EXYNOS_G2D=y
CONFIG_DRM_EXYNOS_DMABUF=y
CONFIG_HDMI=y
CONFIG_FB=y + CFB_FILLRECT/COPYAREA/IMAGEBLIT=y   (vendor fbdev unchanged)
CONFIG_EXYNOS_DECON_FB=y / EXYNOS7580=y / MIPI_DSI=y / DECON_LCD=y /
  PANEL_EA8061_DYNAMIC=y                          (vendor display ON)
CONFIG_CMA_SIZE_MBYTES=0                          (tuned at M2)
# CONFIG_DRM_KMS_CMA_HELPER is not set            (unselected, see above)
# CONFIG_DRM_EXYNOS_FIMD/HDMI/VIDI/IPP is not set (display swap deferred)
# CONFIG_FRAMEBUFFER_CONSOLE is not set           (X keeps /dev/fb0)
```

The only diff hunks vs the pmaports config are the expected DRM block plus
`CONFIG_MODULES` + auto-selects (`HDMI`) and trailing new-symbol defaults
(`BT_HCIUART_*`, `I2C_STUB`, `COMEDI`, `RTLLIB`, …) that `olddefconfig`
materializes; no vendor display symbol changes.

## Integrator: the one serialized build + flash + verify

> Device/flashing is owned by the integrator. Do NOT run these in parallel
> with other kernel builds — one `Image` build at a time (~7 min).

```sh
# 1. From a clean tree state, merge the fragment onto the pmaports config:
cp code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
python3 - <<'EOF'
frag = [l.strip() for l in open('patches/drm-m1/m1-kconfig.fragment')
        if l.strip().startswith('CONFIG_') and l.strip().endswith('=y')]
cfg = open('.config').read().splitlines()
have = {l.split('=')[0]: i for i, l in enumerate(cfg) if l.startswith('CONFIG_')}
out = list(cfg)
for f in frag:
    sym = f.split('=')[0]; dis = '# %s is not set' % sym
    if sym in have: out[have[sym]] = f
    elif dis in cfg: out[cfg.index(dis)] = f
    else: out.append(f)
open('.config', 'w').write('\n'.join(out) + '\n')
EOF
# 2. Serialized build (integrator's flow):
sh code/kernel/build.sh
# 3. Repack with the STOCK dt.img (M1 makes no DT change) and flash p10:
#    python3 mkboot.py --kernel Image --dt stock-dt.img --out boot-m1.img
#    dd if=boot-m1.img of=/dev/mmcblk0p10 conv=fsync && sync
#    echo b > /proc/sysrq-trigger   # sysrq-b reboot
# 4. Post-boot checks (or: sh code/kernel/m1/m1-verify.sh):
ls /dev/dri            # expect card0 + controlD64
dmesg | grep -i drm    # expect "Initialized exynos … on minor 0"
xdpyinfo | grep dimensions   # expect 720x1280, X on fbdev still up
```

## Rollback

Known-good image: the pre-M1 boot image the integrator flashed last
(`boot-stock.img` / the backup taken before step 3). Roll back with:

```sh
# dd if=boot-stock.img of=/dev/mmcblk0p10 conv=fsync && sync
# echo b > /proc/sysrq-trigger
```

No kernel-tree or DT change ships in M1 beyond the six Kconfig symbols, so
reflashing the known-good image restores the exact prior state.
