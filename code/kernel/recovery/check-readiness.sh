#!/bin/sh
# check-readiness.sh — validate GPU-test flash readiness WITHOUT touching the device.
# Read-only: checksums local files, prints exact flash/recovery commands. No ssh/adb/dd.
# Usage: check-readiness.sh [--record] [new-boot.img]
set -eu
KNOWN=${KNOWN:-/tmp/bindboot.img}
RECORD_FILE=${RECORD_FILE:-code/kernel/recovery/known-good.sha256}
NEW=${1:-/tmp/kernel-boot.img}
[ "${1:-}" = "--record" ] && { NEW=${2:-/tmp/kernel-boot.img}; RECORD=1; } || RECORD=0

fail=0
say() { echo "$1"; }
[ -f "$KNOWN" ] || { say "MISSING known-good image: $KNOWN"; fail=1; }
if [ -f "$KNOWN" ]; then
  sum=$(sha256sum "$KNOWN" | cut -d' ' -f1)
  size=$(stat -c%s "$KNOWN")
  say "known-good: $KNOWN size=$size sha256=$sum"
  [ "$size" -gt 33554432 ] && { say "WARN: known-good > 32MiB (BOOT p10 size)"; fail=1; }
  if [ -f "$RECORD_FILE" ]; then
    want=$(cut -d' ' -f1 < "$RECORD_FILE")
    [ "$sum" = "$want" ] || { say "MISMATCH vs $RECORD_FILE (recorded $want)"; fail=1; }
  else
    say "NOTE: no $RECORD_FILE to compare against"
  fi
  if [ "${RECORD:-0}" = 1 ]; then
    echo "$sum  $KNOWN" > "$RECORD_FILE"
    say "recorded -> $RECORD_FILE"
  fi
fi
if [ -f "$NEW" ]; then
  nsize=$(stat -c%s "$NEW")
  nsum=$(sha256sum "$NEW" | cut -d' ' -f1)
  say "new image: $NEW size=$nsize sha256=$nsum"
  [ "$nsize" -gt 33554432 ] && { say "FAIL: new image > 32MiB, will not fit p10"; fail=1; }
  head -c8 "$NEW" | grep -q ANDROID! || { say "FAIL: new image lacks ANDROID! magic"; fail=1; }
else
  say "NOTE: new image $NEW not present (build first via code/kernel/build.sh + mkboot.py)"
fi
for f in /tmp/bimg/dt.img /tmp/bimg/ramdisk-new.gz /tmp/bimg/cmdline; do
  [ -f "$f" ] || { say "MISSING $f (needed for mkboot.py assembly)"; fail=1; }
done
command -v adb >/dev/null 2>&1 || say "NOTE: adb not on PATH (needed only for TWRP recovery)"
echo "--- flash (running system, needs device) ---"
echo "scp $NEW user@172.16.42.1:/tmp/new-boot.img"
echo "ssh user@172.16.42.1 \"sudo -n sh -c 'dd if=/tmp/new-boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync'\""
echo "ssh user@172.16.42.1 \"sudo -n sh -c 'sync; reboot'\"  # sysrq-b does NOT sync"
echo "--- recovery (TWRP, needs device) ---"
echo "adb push $KNOWN /data/bindboot.img"
echo "adb shell 'dd if=/data/bindboot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync; reboot'"
[ "$fail" -eq 0 ] && echo "READY" || { echo "NOT READY"; exit 1; }
