#!/bin/sh
# M1 post-flash verification — runs ON THE PHONE, after the M1 flash only.
# Flash owner: G31_M1Verify, and only after Main's explicit go. No other
# slice flashes. This script never reboots (cold boot is a scarce resource).
#   sh code/kernel/m1/m1-verify.sh
#
# EXPECTED M1 STATE (pre-flash #35 baseline: docs/reports/gpu31-m1-boot.md):
#   /dev/dri/card0 + /dev/dri/controlD64 PRESENT (new in M1; absent on #35)
#   dmesg carries exynos-drm init ("on minor 0"); zero connectors/CRTCs
#     (no modeset driver on M1 — that is M2's job, absence is EXPECTED)
#   /dev/fb0 PRESENT, virtual_size "720,1280" (unchanged from #35)
#   Xorg up, root window 720x1280 (unchanged from #35)
#   USB-net back: a default route exists (proves the RNDIS path is up)
#   /dev/ttySAC0 char node opens (GPS stock parity)
#   /dev/mali0 PRESENT (kbase intact)
# Exit 0 only if every item passes; every item prints PASS/FAIL/INFO.
set -u
FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }
info() { echo "INFO: $1"; }

echo '--- 1. /dev/dri nodes ---'
ls -l /dev/dri 2>&1 || fail '/dev/dri missing'
test -c /dev/dri/card0 2>/dev/null && pass 'card0 present' \
  || fail 'card0 missing'
test -c /dev/dri/controlD64 2>/dev/null && pass 'controlD64 present' \
  || fail 'controlD64 missing'
if test -e /dev/dri/renderD128 2>/dev/null; then
  info 'renderD128 present (M2 scope; unexpected on M1 but not a failure)'
else
  info 'renderD128 absent (expected on M1; M2 scope)'
fi
ls /dev/dri/by-path/ 2>/dev/null || info 'no by-path (expected on M1)'

echo '--- 2. dmesg: exynos-drm probe ---'
DMESG="$(dmesg 2>/dev/null)"
if [ -z "$DMESG" ] && command -v /usr/local/bin/rootshell >/dev/null 2>&1; then
  DMESG="$(/usr/local/bin/rootshell sh -c 'dmesg' 2>/dev/null)"
fi
if [ -z "$DMESG" ]; then
  fail 'dmesg unreadable (tried user + rootshell)'
else
  echo "$DMESG" | grep -i drm | head -20
  echo "$DMESG" | grep -qiE 'exynos-drm|exynos_drm' \
    && pass 'exynos-drm probe line in dmesg' \
    || fail 'no exynos-drm line in dmesg'
fi

echo '--- 3. vendor fbdev still alive ---'
test -c /dev/fb0 && pass '/dev/fb0 present' || fail '/dev/fb0 missing'
VSZ="$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
echo "virtual_size=$VSZ"
[ "$VSZ" = "720,1280" ] && pass 'virtual_size 720,1280' \
  || fail "virtual_size $VSZ (want 720,1280)"

echo '--- 4. X still up at 720x1280 ---'
pgrep -a Xorg 2>/dev/null | head -3 || info 'pgrep found no Xorg line'
XDPI="$(xdpyinfo 2>/dev/null | grep -E 'dimensions' || DISPLAY=:0 xdpyinfo 2>/dev/null | grep -E 'dimensions')"
echo "$XDPI"
echo "$XDPI" | grep -q '720x1280' && pass 'X root 720x1280' \
  || fail 'X dimensions not 720x1280'

echo '--- 5. USB-net returned ---'
ip -o link 2>/dev/null | head -12 || info 'ip link unavailable'
if ip route show default 2>/dev/null | grep -q .; then
  ip route show default | head -3
  pass 'default route present'
else
  fail 'no default route (USB-net path suspect)'
fi

echo '--- 6. GPS tty still opens (stock parity) ---'
test -c /dev/ttySAC0 && pass '/dev/ttySAC0 char node present' \
  || fail '/dev/ttySAC0 missing'
# NOTE (baseline #35): 'user' is not in dialout, so a user-space open gets
# EACCES. The node opening as root is the stock-parity signal.
if head -c 0 /dev/ttySAC0 2>/dev/null; then
  pass '/dev/ttySAC0 opens as user'
elif command -v /usr/local/bin/rootshell >/dev/null 2>&1 \
  && /usr/local/bin/rootshell sh -c 'head -c 0 /dev/ttySAC0' 2>/dev/null; then
  pass '/dev/ttySAC0 opens as root (EACCES as user is stock group perms)'
else
  fail '/dev/ttySAC0 does not open (even as root)'
fi
test -r /sys/class/sec/gps/GPS_PWR_EN/value \
  && pass 'GPS_PWR_EN readable' \
  || fail 'GPS_PWR_EN/value missing'

echo '--- 7. kbase intact ---'
test -c /dev/mali0 && pass '/dev/mali0 present' || fail '/dev/mali0 missing'

if [ "$FAIL" -eq 0 ]; then
  echo 'M1-VERIFY-PASS (7/7 groups)'
else
  echo "M1-VERIFY-FAIL ($FAIL failing check(s))"
  exit 1
fi
