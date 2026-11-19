#!/bin/sh
# get-gpu-state.sh - read-only GPU state dump for the verdict harness.
# Runs ON THE DEVICE as root. Takes no arguments, writes no files.
# Prints key=value lines consumed by run-harness.sh gate 5.
#
# Sysfs sources (drivers/gpu/arm/t7xx/r15p0, Exynos7580 backend):
#  - platform/exynos/gpu_utilization.c: DEVICE_ATTR(utilization) -> env utilization 0-100
#  - platform/exynos/gpu_custom_interface.c: DEVICE_ATTR(time_in_state) "MHz cycles" rows
#  - mali_kbase_core_linux.c: gpuinfo, core_mask
#  - kbase pm: power/runtime_status (genpd), pm/runtime_status fallback
# POSIX sh only.

DEV=/sys/devices/11400000.mali
: ${DBG:=/sys/kernel/debug}

kv() { printf '%s=%s\n' "$1" "$2"; }

# gpuinfo: "Mali-T72x MP2 r1p0 0x0720"
if [ -r "$DEV/gpuinfo" ]; then
	kv gpuinfo "$(head -n1 "$DEV/gpuinfo" 2>/dev/null | tr '\n' ' ' | sed 's/  */ /g; s/^ //; s/ $//')"
else
	kv gpuinfo MISSING
fi

# core_mask: "Current core mask (JS0) : 0x3" + Available line
if [ -r "$DEV/core_mask" ]; then
	kv core_mask_js0 "$(grep -m1 'JS0' "$DEV/core_mask" 2>/dev/null | grep -o '0x[0-9a-fA-F]*')"
	kv core_mask_avail "$(grep -m1 'Available' "$DEV/core_mask" 2>/dev/null | grep -o '0x[0-9a-fA-F]*')"
else
	kv core_mask_js0 MISSING
	kv core_mask_avail MISSING
fi

# runtime status: genpd path first, then pm core fallback
found_rs=0
for f in "$DEV/power/runtime_status" /sys/devices/platform/11400000.mali/power/runtime_status; do
	if [ -r "$f" ]; then
		kv runtime_status "$(cat "$f" 2>/dev/null)"
		found_rs=1
		break
	fi
done
if [ "$found_rs" -eq 0 ]; then
	kv runtime_status UNKNOWN
fi

# utilisation: exynos backend attr, percent when awake else 0
if [ -r "$DEV/utilization" ]; then
	kv utilization "$(cat "$DEV/utilization" 2>/dev/null | tr -d ' \n')"
elif [ -r /sys/devices/platform/11400000.mali/utilization ]; then
	kv utilization "$(cat /sys/devices/platform/11400000.mali/utilization 2>/dev/null | tr -d ' \n')"
else
	kv utilization MISSING
fi

# time_in_state: sum all cycle counters ("MHz cycles" rows)
if [ -r "$DEV/time_in_state" ]; then
	kv tis_total "$(awk '{s+=$2} END{print s+0}' "$DEV/time_in_state" 2>/dev/null)"
	kv tis_rows "$(grep -c . "$DEV/time_in_state" 2>/dev/null)"
elif [ -r /sys/devices/platform/11400000.mali/time_in_state ]; then
	kv tis_total "$(awk '{s+=$2} END{print s+0}' /sys/devices/platform/11400000.mali/time_in_state 2>/dev/null)"
	kv tis_rows "$(grep -c . /sys/devices/platform/11400000.mali/time_in_state 2>/dev/null)"
else
	kv tis_total MISSING
	kv tis_rows MISSING
fi

# debugfs utilization_gp* (mali400 utgard only; absent on midgard is normal)
if [ -r "$DBG/mali0/utilization_gp_pp" ]; then
	kv dbg_util_gp_pp "$(cat "$DBG/mali0/utilization_gp_pp" 2>/dev/null | tr -d ' \n')"
else
	kv dbg_util_gp_pp ABSENT
fi

# current clock / policy context (informational, never a gate on its own)
if [ -r "$DEV/clock" ]; then
	kv clock "$(cat "$DEV/clock" 2>/dev/null | tr -d ' \n')"
else
	kv clock UNKNOWN
fi
if [ -r "$DEV/power_state" ]; then
	kv power_state "$(cat "$DEV/power_state" 2>/dev/null | tr -d ' \n' | head -c 64)"
else
	kv power_state UNKNOWN
fi
