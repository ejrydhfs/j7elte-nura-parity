#!/bin/sh
# M1 Kconfig closure proof -- NO Image build, NO tree writes.
# Reproduces the resolved .config quoted below. Safe to re-run anytime;
# uses a private /tmp out-dir against the READ-ONLY kernel tree.
#
#   sh code/kernel/m1/m1-kconfig-check.sh
#
# Expected resolved M1 values (verified 2026-11-18, olddefconfig):
#   CONFIG_MODULES=y
#   CONFIG_DRM=y
#   CONFIG_DRM_KMS_HELPER=y
#   CONFIG_DRM_EXYNOS=y
#   CONFIG_DRM_EXYNOS_G2D=y
#   CONFIG_DRM_EXYNOS_DMABUF=y
#   CONFIG_HDMI=y                        (auto-select of CONFIG_DRM)
#   CONFIG_FB=y + CFB_FILLRECT/COPYAREA/IMAGEBLIT=y   (unchanged, vendor fbdev)
#   CONFIG_EXYNOS_DECON_FB=y / EXYNOS7580=y / MIPI_DSI=y / DECON_LCD=y /
#     PANEL_EA8061_DYNAMIC=y             (unchanged, vendor display ON)
#   CONFIG_CMA_SIZE_MBYTES=0             (unchanged, tuned at M2)
#   # CONFIG_DRM_KMS_CMA_HELPER is not set  (dropped: nothing in this exynos
#     snapshot selects or references it; exynos carries its own GEM backend)
#   # CONFIG_DRM_EXYNOS_FIMD/HDMI/VIDI/IPP "is not set"  (display swap deferred)
#   # CONFIG_FRAMEBUFFER_CONSOLE is not set (fbdev X keeps /dev/fb0)
set -e
KDIR=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07
OUT=/tmp/m1proof
rm -rf "$OUT"; mkdir -p "$OUT"
cp code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 "$OUT/.config"
python3 - "$OUT/.config" patches/drm-m1/m1-kconfig.fragment <<'EOF'
import sys
cfg_path, frag_path = sys.argv[1], sys.argv[2]
frag = [l.strip() for l in open(frag_path) if l.strip().startswith('CONFIG_') and l.strip().endswith('=y')]
cfg = open(cfg_path).read().splitlines()
have = {l.split('=')[0]: i for i, l in enumerate(cfg) if l.startswith('CONFIG_')}
out = list(cfg)
for f in frag:
    sym = f.split('=')[0]
    dis = '# %s is not set' % sym
    if sym in have:
        out[have[sym]] = f
    elif dis in cfg:
        out[cfg.index(dis)] = f
    else:
        out.append(f)
open(cfg_path, 'w').write('\n'.join(out) + '\n')
print('merged: %s' % ' '.join(frag))
EOF
make -C "$KDIR" O="$OUT" ARCH=arm64 olddefconfig 2>&1 | tail -2
echo '--- resolved M1/DRI/FB/DECON symbols ---'
grep -nE '^CONFIG_MODULES=|^CONFIG_DRM=|^CONFIG_DRM_KMS_HELPER|^CONFIG_DRM_EXYNOS=|^CONFIG_DRM_EXYNOS_G2D|^CONFIG_DRM_EXYNOS_DMABUF|^CONFIG_HDMI=|^CONFIG_FB=|^CONFIG_FB_CFB_FILLRECT|^CONFIG_EXYNOS_DECON_FB|^CONFIG_EXYNOS_DECON_EXYNOS7580|^CONFIG_EXYNOS_DECON_LCD=|^CONFIG_EXYNOS_MIPI_DSI|^CONFIG_PANEL_EA8061_DYNAMIC|^CONFIG_CMA_SIZE_MBYTES|^CONFIG_DMA_CMA|^# CONFIG_DRM_EXYNOS_FIMD|^# CONFIG_DRM_KMS_CMA_HELPER|^# CONFIG_FRAMEBUFFER_CONSOLE' "$OUT/.config"
