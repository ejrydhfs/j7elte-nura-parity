#!/bin/sh
# Restore the SM5703 fuelgauge capacity_max calibration that the stock ROM
# keeps in /efs/Battery/batt_capacity_max.
#
# Stock behaviour: /system/bin/lpm ("LPMBattery", the binary the ROM runs as
# its battery daemon) reads that file on boot and writes the value to
# /sys/class/power_supply/battery/batt_capacity_max, then stores the value the
# gauge learns back into the file.  This script does the boot half.  The
# write-back half is opt-in in j7elte-power.conf (persist_capacity_max=1).
#
# postmarketOS does not mount /efs, so the script mounts the EFS partition
# read-only for the duration of the restore and unmounts it again.  It never
# fails the unit: a missing or unreadable EFS partition is reported and
# ignored, leaving the device-tree default (990) in place.
#
# Overridable from the environment:
#   J7ELTE_POWER_CONF  config file (default /etc/j7elte-power.conf)
#   J7ELTE_EFS_MOUNT   mount point (default /efs)
#   J7ELTE_EFS_DEVICE  block device to mount (default: resolve by label)
#   J7ELTE_FORCE_RW    1 = mount read-write (only useful with persist enabled)

set -u

CONF=${J7ELTE_POWER_CONF:-/etc/j7elte-power.conf}
efs_mount=${J7ELTE_EFS_MOUNT:-/efs}
efs_device=${J7ELTE_EFS_DEVICE:-}
force_rw=${J7ELTE_FORCE_RW:-0}

# Defaults matching a pristine j7elte-power.conf; the file may override the
# two keys this script needs.
battery_dir=/sys/class/power_supply/battery
efs_capacity_max_file=/efs/Battery/batt_capacity_max

if [ -r "$CONF" ]; then
	# shellcheck disable=SC1090
	. "$CONF"
fi

log() {
	echo "j7elte-power-restore: $*" >&2
}

bat_attr="$battery_dir/batt_capacity_max"

if [ ! -e "$bat_attr" ]; then
	log "$bat_attr not present (fuelgauge not probed?); nothing to restore"
	exit 0
fi

mounted_by_us=0
if ! grep -qs "[[:space:]]$efs_mount[[:space:]]" /proc/mounts; then
	if [ -z "$efs_device" ]; then
		# The Exynos eMMC partition table is a GPT whose names match the
		# Samsung PIT (EFS is a real partition there); udev creates the
		# by-partlabel symlink.  The Android-style by-name path is kept as
		# a fallback in case the device is booted with an Android-style
		# init that creates it.
		for cand in \
			/dev/disk/by-partlabel/EFS \
			/dev/block/platform/13540000.dwmmc0/by-name/EFS
		do
			if [ -e "$cand" ]; then
				efs_device=$cand
				break
			fi
		done
	fi

	if [ -z "$efs_device" ]; then
		log "EFS partition not found; skipping capacity_max restore"
		exit 0
	fi

	if [ ! -d "$efs_mount" ]; then
		mkdir -p "$efs_mount" || {
			log "cannot create $efs_mount"
			exit 0
		}
	fi

	mount_opts="ro,nosuid,nodev,noatime"
	[ "$force_rw" = "1" ] && mount_opts="rw,nosuid,nodev,noatime"

	if ! mount -t ext4 -o "$mount_opts" "$efs_device" "$efs_mount"; then
		log "cannot mount $efs_device on $efs_mount; skipping restore"
		exit 0
	fi
	mounted_by_us=1
fi

value=""
if [ -r "$efs_capacity_max_file" ]; then
	# The ROM writes a plain decimal, optionally with a newline.
	value=$(tr -cd '0-9' < "$efs_capacity_max_file" 2>/dev/null)
elif [ -e "$efs_capacity_max_file" ]; then
	log "cannot read $efs_capacity_max_file; leaving device-tree default in place"
fi

if [ -z "$value" ]; then
	log "no value in $efs_capacity_max_file; leaving device-tree default in place"
else
	# Same window the sec_battery store handler accepts (>800 and <1200).
	if [ "$value" -le 800 ] || [ "$value" -ge 1200 ]; then
		log "refusing out-of-range capacity_max=$value (kernel accepts 801..1199)"
	else
		if printf '%s\n' "$value" > "$bat_attr" 2>/dev/null; then
			effective=$(cat "$bat_attr" 2>/dev/null)
			log "restored capacity_max=$value (effective $effective)"
		else
			log "failed to write $bat_attr"
		fi
	fi
fi

if [ "$mounted_by_us" = "1" ]; then
	umount "$efs_mount" || log "failed to unmount $efs_mount"
fi

exit 0
