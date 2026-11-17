#!/bin/sh
# Build linux-samsung-j7elte (3.10.108) for j7elte and produce an Android boot.img
# that S-Boot accepts.  Verified on 2026-11-17: Image builds with GCC 16,
# boots on the phone; do NOT touch the dt.img (see docs section 16).
set -eu
WORK=${WORK:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k}
SHA=2a746c1bc1a41eae05988c153f527c8a5dc72d07
SRC=$WORK/android_kernel_samsung_universal7580-$SHA
PMD=$(cd "$(dirname "$0")/../.." && pwd)/code/pmaports/device/archived/linux-samsung-j7elte

[ -d "$SRC" ] || {
	mkdir -p "$WORK" && cd "$WORK"
	curl -sL -o k.tar.gz "https://codeload.github.com/LineageOS/android_kernel_samsung_universal7580/tar.gz/$SHA"
	tar xzf k.tar.gz
}

cd "$SRC"
cp "$PMD/config-samsung-j7elte.aarch64" .config
for p in "$PMD"/*.patch; do
	patch -p1 -N -r - < "$p" >/dev/null 2>&1 || true
done
yes "" | make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- oldconfig >/dev/null

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j2 Image
ls -l arch/arm64/boot/Image

# Boot image: reuse the reference ramdisk/cmdline and the STOCK dt.img.
# Replacing the DTB payloads inside the DTBH container makes S-Boot hang, and
# a rebuilt DTB is not needed (same source content), so keep /tmp/bimg/dt.img.
if [ -f /tmp/bimg/dt.img ]; then
	python3 /tmp/mkboot.py arch/arm64/boot/Image /tmp/bimg/ramdisk-new.gz \
		/tmp/bimg/dt.img /tmp/kernel-boot.img "$(cat /tmp/bimg/cmdline)"
	echo "boot image: /tmp/kernel-boot.img"
fi
