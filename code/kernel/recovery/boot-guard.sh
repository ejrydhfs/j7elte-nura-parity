#!/bin/sh
# boot-guard.sh — userspace "boot OK" marker for the GPU-test single-slot fallback.
# Install ON THE PHONE (not in the repo rootfs) as an openrc service via
# code/kernel/recovery/boot-guard.initd. Every boot it waits for (a) fbdev X
# alive at 720x1280 and (b) the slot-specific smoke device, then resets the
# initramfs boot-attempt counter stored on CACHE (mmcblk0p21). A boot that
# never reaches this marker (kernel panic, display dead, uevent storm,
# milestone regression) counts toward BOOT_FAIL_MAX -> self-restore.
#
# Zero dependencies beyond /bin/sh + coreutils: runs before X if needed
# (X check just retries until UI_TIMEOUT).
#
# Device writes go to /data (root fs is near-full); the CACHE marker reset
# itself must go through rootshell because stock fstab does not mount p21.
set -eu
MARKER_MODE=${1:-}
UI_TIMEOUT=${UI_TIMEOUT:-300}
MILESTONE=${MILESTONE:-M1}

ok_x() {
  # X up at the panel-native geometry. xdpyinfo needs DISPLAY set.
  DISPLAY=:0 xdpyinfo 2>/dev/null | grep -q 'dimensions:[[:space:]]*720x1280'
}

ok_milestone() {
  case "$MILESTONE" in
    M1) [ -e /dev/dri/card0 ] && [ -e /dev/fb0 ] ;;
    M2) [ -e /dev/dri/renderD128 ] && [ -e /dev/fb0 ] ;;
    M3) # render node present AND Mesa reports a hardware (non-llvmpipe) renderer
        [ -e /dev/dri/renderD128 ] &&
          DISPLAY=:0 glxinfo -B 2>/dev/null | grep -qiE 'renderer.*(panfrost|mali|exynos)' ;;
  esac
}

deadline=$(( $(date +%s) + UI_TIMEOUT ))
while :; do
  if ok_x && ok_milestone; then
    break
  fi
  [ "$(date +%s)" -ge "$deadline" ] && {
    echo "boot-guard: TIMEOUT after ${UI_TIMEOUT}s (MILESTONE=$MILESTONE): boot counts as failed; counter left untouched" >&2
    exit 1
  }
  sleep 5
done

echo "boot-guard: boot OK (MILESTONE=$MILESTONE); resetting CACHE boot_count"
/usr/local/bin/rootshell sh -c '
  mkdir -p /fb-ok
  if mount -t ext4 -o rw /dev/block/mmcblk0p21 /fb-ok 2>/dev/null; then
    echo 0 > /fb-ok/boot_count
    sync
    umount /fb-ok
  else
    echo "boot-guard: CACHE mount failed; counter NOT reset" >&2
    exit 1
  fi
'
echo "boot-guard: DONE"
