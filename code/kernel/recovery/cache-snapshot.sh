#!/bin/sh
# cache-snapshot.sh — snapshot the CURRENT p10 boot.img into the CACHE
# fallback slot, WITHOUT rebooting or flashing. Run over ssh on the live
# device (read-only against p10) after a build has proven good on hardware.
# The initramfs fallback only restores from this copy; until it exists (or
# after it goes stale) the fallback reports "no valid cache copy" and the
# TWRP path (RECOVERY.md section 3) remains the only recovery.
#
# Usage (from the host; short flock hold, no reboot):
#   flock /tmp/gpsdev.lock -c "SSH_ASKPASS=/tmp/ap.sh ... ssh ... \
#     'sh -s M1' < code/kernel/recovery/cache-snapshot.sh"
# Writes land on p21 (CACHE, 200 MiB), never on the near-full root fs.
set -eu
MILESTONE=${1:-M1}

echo "--- cache-snapshot ($MILESTONE): reading p10 (read-only) ---"
/usr/local/bin/rootshell sh -c '
  set -eu
  mkdir -p /fb-snap
  mount -t ext4 -o rw /dev/block/mmcblk0p21 /fb-snap
  dd if=/dev/block/mmcblk0p10 of=/fb-snap/known-good-boot.img bs=512 conv=fsync
  sync
  SZ=$(stat -c%s /fb-snap/known-good-boot.img)
  echo "snapshot bytes: $SZ"
  head -c8 /fb-snap/known-good-boot.img | grep -q ANDROID! || { echo "FAIL: no ANDROID! magic"; exit 1; }
  sha256sum /fb-snap/known-good-boot.img
  echo 0 > /fb-snap/boot_count
  sync
  umount /fb-snap
'
echo "--- snapshot done; counter reset to 0 ---"
echo "Record on host too: HOST_MILESTONE=$MILESTONE $(date -u +%FT%TZ)"
