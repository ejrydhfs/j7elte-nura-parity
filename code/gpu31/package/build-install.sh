#!/bin/sh
# Reproducible build + install of the j7elte GPU userspace stack.
# Winning stack (2026-11-19, kernel #35 live): software GL via
# j7elte-gpu-soft + stock Mesa/Xorg. Hardware GL needs a kernel change
# (panfrost/DRM) and is NOT built here.
#
# Host build of the device package (needs abuild + aports tree):
#   1. cp -r code/pmaports/device/archived/j7elte-gpu-soft <aports>/device/testing/
#   2. cd <aports>/device/testing/j7elte-gpu-soft && abuild checksum && abuild -r
# On-device build (root fs 94% full — use /data):
#   1. cp -r code/pmaports/device/archived/j7elte-gpu-soft /data/gpusoft
#   2. cd /data/gpusoft && REPODEST=/data/pkgs abuild -r
#   3. apk add --allow-untrusted /data/pkgs/*/j7elte-gpu-soft-1-r0.apk
# Install + activate:
#   apk add j7elte-gpu-soft
#   # provides /etc/X11/xorg.conf.d/10-fbdev.conf and
#   # /etc/profile.d/j7elte-softgl.sh (LIBGL_ALWAYS_SOFTWARE=1)
# Verify (no reboot needed):
#   code/userspace/gpu/check-3d.sh   # expect: no /dev/dri, kbase OK, llvmpipe
set -eu
echo "see header comments for build/install steps"
