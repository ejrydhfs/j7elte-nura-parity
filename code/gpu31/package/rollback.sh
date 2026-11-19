#!/bin/sh
# Rollback for the j7elte GPU userspace stack.
# The stack is userspace-only: removing the package restores the prior state.
# No kernel flash, no DT change, no reboot required.
#
#   apk del j7elte-gpu-soft
#   rm -f /etc/X11/xorg.conf.d/10-fbdev.conf /etc/profile.d/j7elte-softgl.sh
#
# Effect: Xorg falls back to its previous fbdev config
# (/etc/X11/xorg.conf.d/99-fbdev.conf, equivalent stanza) and Mesa
# auto-selects the renderer again (llvmpipe on this kernel, since there
# is no /dev/dri render node). Verify with:
#   ls /dev/dri            # expect: No such file or directory
#   DISPLAY=:0 glxinfo -B  # expect: llvmpipe, Accelerated: no
#
# Kernel-side rollback (only if a test kernel was flashed by the
# integrator — NOT part of this package): reflash the known-good image:
#   dd if=<known-good-boot.img> of=/dev/block/mmcblk0p10 bs=512 conv=fsync
#   sync; reboot
# See code/kernel/recovery/RECOVERY.md for the full no-boot procedure.
set -eu
if [ "$(id -u)" -eq 0 ]; then
  apk del j7elte-gpu-soft || true
  rm -f /etc/X11/xorg.conf.d/10-fbdev.conf /etc/profile.d/j7elte-softgl.sh
  echo "rolled back: j7elte-gpu-soft removed"
else
  echo "run as root on device"
fi
