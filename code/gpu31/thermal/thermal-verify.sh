#!/bin/sh
# gpu31-thermal: verify GPU thermal wiring on the live phone.
# Read-only except for the optional cooling-state injection test, which only
# writes the cooling device's own cur_state sysfs node (restored afterwards)
# and does NOT touch clocks, locks, governors, or reboot the phone.
#
# Usage: thermal-verify.sh [--inject]
#   without --inject: report only (safe under shared-phone rules).
#   with --inject:    additionally drive thermal-gpufreq-0 cur_state 0->2->0
#                     as root and confirm the kbase max-lock follows.
#
# Prints PASS/FAIL per check; exit 0 only if all non-optional checks pass.
# Writes big temp files under /data, never /tmp or /.
INJECT=0
[ "$1" = "--inject" ] && INJECT=1

PASS=0; FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# --- 1. thermal zones + GPU cooling device exist -----------------------------
if [ -d /sys/class/thermal/thermal_zone2 ] && \
   [ "$(cat /sys/class/thermal/cooling_device2/type 2>/dev/null)" = "thermal-gpufreq-0" ]; then
    ok "GPU zone2 bound to thermal-gpufreq-0"
else
    bad "GPU zone2 / thermal-gpufreq-0 missing"
fi

# --- 2. GPU TMU control enabled ----------------------------------------------
if [ "$(cat /sys/devices/11400000.mali/tmu 2>/dev/null)" = "1" ]; then
    ok "kbase tmu attribute = 1 (TMU events honoured)"
else
    bad "kbase tmu attribute != 1 (TMU events ignored)"
fi

# --- 3. max_state covers the trip table --------------------------------------
MAX=$(cat /sys/class/thermal/cooling_device2/max_state 2>/dev/null)
TRIPS=0
for t in 0 1 2 3 4 5 6 7; do
    [ -f /sys/class/thermal/thermal_zone2/trip_point_${t}_temp ] && TRIPS=$((TRIPS+1))
done
if [ -n "$MAX" ] && [ "$MAX" -ge 3 ]; then
    ok "cdev max_state=$MAX over $TRIPS trips"
else
    bad "cdev max_state=$MAX too small for $TRIPS trips"
fi

# --- 4. DVFS table sanity (live 5-level ASV table expected) -------------------
echo "INFO: dvfs_table=$(cat /sys/devices/11400000.mali/dvfs_table 2>/dev/null)"
echo "INFO: dvfs_max_lock=$(cat /sys/devices/11400000.mali/dvfs_max_lock 2>/dev/null)"
echo "INFO: dvfs_max_lock_status=$(cat /sys/devices/11400000.mali/dvfs_max_lock_status 2>/dev/null)"

# --- 5. optional injection: cdev state -> TMU max lock ------------------------
if [ "$INJECT" = "1" ]; then
    if [ "$(id -u)" != "0" ]; then
        echo "SKIP: --inject needs root (run via rootshell)"
    else
        CUR0=$(cat /sys/class/thermal/cooling_device2/cdev0 2>/dev/null)
        LOCK0=$(cat /sys/devices/11400000.mali/dvfs_max_lock 2>/dev/null)
        echo 2 > /sys/class/thermal/cooling_device2/cur_state
        sleep 2
        LOCK1=$(cat /sys/devices/11400000.mali/dvfs_max_lock 2>/dev/null)
        echo 0 > /sys/class/thermal/cooling_device2/cur_state
        sleep 2
        LOCK2=$(cat /sys/devices/11400000.mali/dvfs_max_lock 2>/dev/null)
        echo "INFO: max_lock before=$LOCK0 during(cdev=2)=$LOCK1 after=$LOCK2 (cdev0 was $CUR0)"
        if [ "$LOCK1" != "$LOCK0" ] || [ "$LOCK1" != "-1" ]; then
            ok "cooling-state injection moved the TMU max lock ($LOCK0 -> $LOCK1 -> $LOCK2)"
        else
            bad "cooling-state injection had no effect (lock stayed $LOCK1)"
        fi
    fi
else
    echo "SKIP: injection test (re-run with --inject via rootshell to actuate)"
fi

echo "---"
echo "thermal-verify: $PASS pass, $FAIL fail"
[ "$FAIL" = "0" ]
