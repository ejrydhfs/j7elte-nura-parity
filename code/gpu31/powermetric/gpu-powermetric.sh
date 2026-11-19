#!/system/bin/sh
# gpu-powermetric.sh — read-only GPU power-proxy sampler for j7elte.
# Runs ON THE DEVICE as root (needs /dev/mali0 + mali sysfs).
# Takes no destructive action: never writes clock/lock/dvfs nodes,
# never resets time_in_state (pass --reset-tis to do that explicitly).
# Usage: sh gpu-powermetric.sh [--samples N] [--interval S] [--reset-tis]
# Output: one timestamped block per sample, suitable for pasting into
# docs/reports/gpu31-powermetric.md.
SAMPLES=3
INTERVAL=5
RESET_TIS=0
for a in "$@"; do
  case "$a" in
    --samples=*) SAMPLES="${a#--samples=}" ;;
    --interval=*) INTERVAL="${a#--interval=}" ;;
    --reset-tis) RESET_TIS=1 ;;
  esac
done
R=""
for d in /sys/class/regulator/regulator.*; do
  if [ "$(cat $d/name 2>/dev/null)" = "vdd_g3d" ]; then R=$d; fi
done
if [ "$RESET_TIS" = "1" ]; then
  echo 1 > /sys/devices/11400000.mali/time_in_state 2>/dev/null && echo "# time_in_state reset"
fi
i=0
while [ "$i" -lt "$SAMPLES" ]; do
  i=$((i+1))
  echo "=== sample $i $(date -u +%FT%TZ) ==="
  echo "mali_clock_MHz=$(cat /sys/devices/11400000.mali/clock 2>/dev/null)"
  echo "mali_vol_uV=$(cat /sys/devices/11400000.mali/vol 2>/dev/null)"
  echo "mali_util_pct=$(cat /sys/devices/11400000.mali/utilization 2>/dev/null)"
  echo "mali_perf=$(cat /sys/devices/11400000.mali/perf 2>/dev/null)"
  echo "mali_power_state=$(cat /sys/devices/11400000.mali/power_state 2>/dev/null)"
  echo "mali_runtime_status=$(cat /sys/devices/11400000.mali/power/runtime_status 2>/dev/null)"
  echo "mali_runtime_active_ms=$(cat /sys/devices/11400000.mali/power/runtime_active_time 2>/dev/null)"
  echo "mali_runtime_suspended_ms=$(cat /sys/devices/11400000.mali/power/runtime_suspended_time 2>/dev/null)"
  echo "mali_time_in_state=$(cat /sys/devices/11400000.mali/time_in_state 2>/dev/null | tr '\n' ';')"
  echo "mali_dvfs=$(cat /sys/devices/11400000.mali/dvfs 2>/dev/null)"
  echo "vdd_g3d_node=$R"
  echo "vdd_g3d_uV=$(cat $R/microvolts 2>/dev/null)"
  echo "vdd_g3d_state=$(cat $R/state 2>/dev/null)"
  echo "vdd_g3d_num_users=$(cat $R/num_users 2>/dev/null)"
  echo "vdd_g3d_min_uV=$(cat $R/min_microvolts 2>/dev/null)"
  echo "vdd_g3d_max_uV=$(cat $R/max_microvolts 2>/dev/null)"
  echo "batt_current_now_mA=$(cat /sys/class/power_supply/battery/current_now 2>/dev/null)"
  echo "batt_current_avg_mA=$(cat /sys/class/power_supply/battery/current_avg 2>/dev/null)"
  echo "batt_voltage_now_uV=$(cat /sys/class/power_supply/battery/voltage_now 2>/dev/null)"
  echo "batt_status=$(cat /sys/class/power_supply/battery/status 2>/dev/null)"
  echo "batt_capacity_pct=$(cat /sys/class/power_supply/battery/capacity 2>/dev/null)"
  echo "batt_temp_deciC=$(cat /sys/class/power_supply/battery/temp 2>/dev/null)"
  echo "fg_current_now_mA=$(grep POWER_SUPPLY_CURRENT_NOW /sys/class/power_supply/sm5703-fuelgauge/uevent 2>/dev/null)"
  echo "cool_gpufreq=$(cat /sys/class/thermal/cooling_device2/cur_state 2>/dev/null)/$(cat /sys/class/thermal/cooling_device2/max_state 2>/dev/null)"
  echo "tz0_mC=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
  echo "tz1_mC=$(cat /sys/class/thermal/thermal_zone1/temp 2>/dev/null)"
  if [ "$i" -lt "$SAMPLES" ]; then sleep "$INTERVAL"; fi
done
