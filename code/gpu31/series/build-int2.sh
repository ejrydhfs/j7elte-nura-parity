#!/bin/sh
# integration2 reproducible rebuild: M1 + backport shims + KMS display-code
# (compiled, not enabled) + Mode-B render node. Prereq: scratch tree already
# carries the applied series (Mode-B hunks verified). Serial build only
# (prior -jN run died in kernel/irq on a Broken-pipe artifact, not a source
# error); TMPDIR on the external drive (host /tmp is a ~10G tmpfs at 99%).
set -eu
SCR=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch
LOG=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/logs/gpu31-int2-serial-build.log
export TMPDIR=/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/tmp TMP=$TMPDIR TEMP=$TMPDIR
mkdir -p "$TMPDIR"
cd "$SCR"
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j1 Image > "$LOG" 2>&1
ls -l arch/arm64/boot/Image
sha256sum arch/arm64/boot/Image
python3 /tmp/mkboot.py arch/arm64/boot/Image /tmp/bimg/ramdisk-new.gz /tmp/bimg/dt.img /tmp/kernel-boot-int2.img "$(cat /tmp/bimg/cmdline)"
ls -l /tmp/kernel-boot-int2.img
sha256sum /tmp/kernel-boot-int2.img
