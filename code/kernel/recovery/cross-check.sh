#!/bin/sh
# cross-check.sh — host-side consistency checks for the recovery slice.
# No device, no kernel build, no tree writes. Exit non-zero on any failure.
cd "$(dirname "$0")/../../.."

fails=0
say() { echo "$1"; }
fail() { say "FAIL: $1"; fails=$((fails + 1)); }
pass() { say "pass: $1"; }

# 1. decide.py runs and the golden traces hold.
TRACE1=$(python3 code/kernel/recovery/decide.py sim OK FAIL FAIL FAIL FAIL 2>&1 | tail -1)
[ "$TRACE1" = "final: boot_count=0 restores=1" ] \
  && pass "3 strikes then restore (FAIL_MAX=3)" \
  || fail "strike-restore trace: got '$TRACE1'"
TRACE2=$(python3 code/kernel/recovery/decide.py sim FAIL FAIL FAIL --fail-max=3 --no-copy 2>&1 | tail -1)
[ "$TRACE2" = "final: boot_count=3 restores=0" ] \
  && pass "no-copy path never restores (fail-open boot)" \
  || fail "no-copy trace: got '$TRACE2'"
TRACE3=$(python3 code/kernel/recovery/decide.py sim OK OK --fail-max=3 2>&1 | tail -1)
[ "$TRACE3" = "final: boot_count=0 restores=0" ] \
  && pass "healthy boots keep counter at 0" \
  || fail "healthy trace: got '$TRACE3'"

# 2. Initramfs patch is well-formed and references the real hardware map.
P=code/kernel/recovery/initramfs-0003-bootcount-fallback.patch
grep -q 'mmcblk0p10' "$P" && grep -q 'mmcblk0p21' "$P" \
  && pass "patch names BOOT p10 + CACHE p21" \
  || fail "patch partition nodes"
grep -q 'ANDROID!' "$P" && grep -q '33554432' "$P" \
  && pass "patch validates cache copy (magic + 32MiB cap)" \
  || fail "patch copy validation"
grep -q 'init_2nd.sh' "$P" \
  && pass "patch targets init_2nd.sh" \
  || fail "patch target file"
grep -q 'reboot -f' "$P" && grep -q 'sysrq-trigger' "$P" \
  && pass "patch reboots via reboot -f, sysrq b as fallback" \
  || fail "patch reboot path"
grep -qi 'twrp\|RECOVERY.md' "$P" \
  && pass "patch documents TWRP as out-of-scope class" \
  || fail "patch TWRP note"

# 3. boot-guard checks per-milestone devices + 720x1280 X.
G=code/kernel/recovery/boot-guard.sh
grep -q 'renderD128' "$G" && grep -q '720x1280' "$G" \
  && pass "guard checks render node + panel geometry" \
  || fail "guard checks"
grep -q 'mmcblk0p21' "$G" && grep -q 'boot_count' "$G" \
  && pass "guard resets CACHE counter" \
  || fail "guard counter reset"
grep -q 'rootshell' "$G" \
  && pass "guard uses rootshell (sudo config is broken)" \
  || fail "guard privilege path"

# 4. Milestone names used here match the M1 verify script.
grep -q 'MILESTONE' "$G" && grep -q '/dev/dri/card0' "$G" \
  && pass "guard milestone switch includes M1 card0" \
  || fail "guard M1 parity with m1-verify.sh"

# 5. Slice hygiene: shared kernel tree untouched, our files only.
if git -C /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07 status --short 2>/dev/null | grep -q .; then
  fail "shared kernel tree dirty"
else
  pass "shared kernel tree untouched"
fi

[ "$fails" -eq 0 ] && { say "CROSS-CHECK: ALL PASS"; exit 0; }
say "CROSS-CHECK: $fails FAILURES"; exit 1
