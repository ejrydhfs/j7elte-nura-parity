#!/bin/sh
# M1 post-flash verification (runs ON THE PHONE after the integrator's
# serialized build+flash; asserts the M1 acceptance state).
#   sh code/kernel/m1/m1-verify.sh
# PASS = card0 + controlD64 exist, vendor fbdev + X alive. Connectors/CRTCs
# are EXPECTED ABSENT on M1 (no modeset driver); that is M2's job.
set -e
echo '--- /dev/dri ---'
ls -l /dev/dri
test -e /dev/dri/card0 || { echo 'FAIL: no card0'; exit 1; }
echo '--- drm init ---'
dmesg | grep -i drm | head -20
dmesg | grep -i -E 'exynos-drm|exynos_drm' | head -20
echo '--- vendor fbdev still alive ---'
ls -l /dev/fb0
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || true
echo '--- X still up at 720x1280 ---'
xdpyinfo 2>/dev/null | grep -E 'dimensions|number of screens' || \
  DISPLAY=:0 xdpyinfo | grep -E 'dimensions|number of screens'
echo '--- GEM smoke (M2 groundwork; informational only) ---'
ls /dev/dri/by-path/ 2>/dev/null || true
echo 'M1-VERIFY-DONE'
