#!/bin/sh
# gpu31 closure checker — extends code/kernel/m1/m1-kconfig-check.sh to the
# full config matrix. Read-only vs the shared tree (private $OUT under the
# scratch ELarmstrong, host /tmp is tmpfs-small so OUT must NOT be /tmp).
# Usage: sh code/gpu31/config/check-matrix.sh [fragment ...]
# Default: all fragments in code/gpu31/config/fragments/*.fragment.
# Exit 0 iff every variant closes with the expected symbols; prints the
# resolved-symbol block per variant (paste into docs/reports/gpu31-config.md).
set -e
KDIR=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07
STOCK=code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64
OUT=${OUT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/mconfig}
FRAGDIR=code/gpu31/config/fragments
WANT='^CONFIG_MODULES=|^# CONFIG_MODULES is not set|^CONFIG_MALI_MIDGARD=|^# CONFIG_MALI_MIDGARD is not set|^CONFIG_MALI_T7XX=|^# CONFIG_MALI_T7XX is not set|^CONFIG_MALI_R15P0=|^# CONFIG_MALI_R15P0 is not set|^CONFIG_DRM=|^# CONFIG_DRM is not set|^CONFIG_DRM_KMS_HELPER=|^CONFIG_DRM_KMS_CMA_HELPER=|^# CONFIG_DRM_KMS_CMA_HELPER|^CONFIG_DRM_EXYNOS=|^CONFIG_DRM_EXYNOS_G2D=|^# CONFIG_DRM_EXYNOS_G2D|^CONFIG_DRM_EXYNOS_DMABUF=|^# CONFIG_DRM_EXYNOS_DMABUF|^CONFIG_DRM_EXYNOS_VIDI=|^# CONFIG_DRM_EXYNOS_VIDI is not set|^CONFIG_DRM_EXYNOS_FIMD=|^# CONFIG_DRM_EXYNOS_FIMD is not set|^CONFIG_DRM_EXYNOS_HDMI=|^# CONFIG_DRM_EXYNOS_HDMI is not set|^CONFIG_DRM_EXYNOS_IPP=|^# CONFIG_DRM_EXYNOS_IPP is not set|^CONFIG_DRM_EXYNOS_ROTATOR=|^# CONFIG_DRM_EXYNOS_ROTATOR is not set|^CONFIG_DRM_EXYNOS_FIMC=|^# CONFIG_DRM_EXYNOS_FIMC is not set|^CONFIG_DRM_EXYNOS_GSC=|^# CONFIG_DRM_EXYNOS_GSC is not set|^CONFIG_FB_SYS_FILLRECT=|^# CONFIG_FB_SYS_FILLRECT|^CONFIG_HDMI=|^CONFIG_MFD_SYSCON=|^# CONFIG_MFD_SYSCON is not set|^CONFIG_FB_MODE_HELPERS=|^# CONFIG_FB_MODE_HELPERS is not set|^CONFIG_VIDEOMODE_HELPERS=|^# CONFIG_VIDEOMODE_HELPERS is not set|^CONFIG_EXYNOS_DECON_FB=|^CONFIG_FRAMEBUFFER_CONSOLE=|^# CONFIG_FRAMEBUFFER_CONSOLE is not set|^CONFIG_CMA_SIZE_MBYTES='
[ -d "$KDIR" ] || { echo "FAIL: shared tree not mounted: $KDIR"; exit 1; }
[ -f "$STOCK" ] || { echo "FAIL: stock config missing: $STOCK"; exit 1; }
if [ $# -gt 0 ]; then FRAGS="$*"; else FRAGS="$FRAGDIR"/*.fragment; fi
fail=0
for frag in $FRAGS; do
  name=$(basename "$frag")
  rm -rf "$OUT"; mkdir -p "$OUT"
  cp "$STOCK" "$OUT/.config"
  # Merge BOTH enable lines (CONFIG_X=y) and disable lines
  # ("# CONFIG_X is not set") so fragments can deselect symbols.
  python3 - "$OUT/.config" "$frag" <<'EOF'
import re
import sys
cfg_path, frag_path = sys.argv[1], sys.argv[2]
req = []
for line in open(frag_path):
    s = line.strip()
    if re.match(r'# CONFIG_\w+ is not set', s):
        req.append(s)
        continue
    if not s or s.startswith('#'):
        continue
    if re.match(r'CONFIG_\w+=', s):
        req.append(s)
cfg = open(cfg_path).read().splitlines()
out = list(cfg)
for f in req:
    sym = f.split()[1] if f.startswith('#') else f.split('=')[0]
    dis = '# %s is not set' % sym
    have = {l.split('=')[0]: i for i, l in enumerate(out) if l.startswith('CONFIG_')}
    if f.startswith('#'):
        if sym in have:
            out[have[sym]] = f
        elif dis not in out:
            out.append(f)
    elif sym in have:
        out[have[sym]] = f
    elif dis in out:
        out[out.index(dis)] = f
    else:
        out.append(f)
open(cfg_path, 'w').write('\n'.join(out) + '\n')
print('merged %d: %s' % (len(req), ' '.join(x.split()[1] if x.startswith('#') else x.split('=')[0] for x in req)))
EOF
  make -C "$KDIR" O="$OUT" ARCH=arm64 olddefconfig >/dev/null 2>&1
  echo "=== $name ==="
  grep -E "$WANT" "$OUT/.config" || { echo "WARN: no symbols matched"; fail=1; }
  case "$name" in
    c0-*) grep -q '^CONFIG_MODULES=y' "$OUT/.config" || { echo "FAIL $name: MODULES not y"; fail=1; }
          grep -q '^# CONFIG_DRM is not set' "$OUT/.config" || { echo "FAIL $name: DRM should stay off"; fail=1; };;
    d0-*|m1-*) grep -q '^CONFIG_DRM_EXYNOS_G2D=y' "$OUT/.config" || { echo "FAIL $name: G2D not y"; fail=1; }
          grep -q '^# CONFIG_FRAMEBUFFER_CONSOLE is not set' "$OUT/.config" || { echo "FAIL $name: fbcon drifted on"; fail=1; };;
    d2-*) grep -q '^CONFIG_DRM_EXYNOS_FIMD=y' "$OUT/.config" || { echo "FAIL $name: FIMD did not close"; fail=1; }
          # GSC needs ARCH_EXYNOS5 (absent on this arm64 tree): olddefconfig
          # silently DROPS the request — the symbol vanishes entirely (not even
          # an "is not set" line). Assert absence: presence would be wrong.
          grep -q '^CONFIG_DRM_EXYNOS_GSC=' "$OUT/.config" && { echo "FAIL $name: GSC should drop (no ARCH_EXYNOS5)"; fail=1; };;
    d3-*) grep -q '^# CONFIG_MODULES is not set' "$OUT/.config" || { echo "FAIL $name: MODULES should stay off"; fail=1; };;
    m0-*) grep -q '^# CONFIG_MALI_MIDGARD is not set' "$OUT/.config" || { echo "FAIL $name: MALI still on"; fail=1; }
          grep -q '^# CONFIG_MALI_T7XX is not set' "$OUT/.config" || { echo "FAIL $name: T7XX still on"; fail=1; };;
  esac
done
[ "$fail" -eq 0 ] && echo ALLPASS || { echo MATRIX-FAIL; exit 1; }
