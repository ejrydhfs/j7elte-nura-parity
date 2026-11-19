#!/bin/sh
# integration4 reproducible rebuild: M1 + backport shims + Mode-B render
# node + panfrost7580 render-only driver + d4 config.
# Fresh scratch copy of the pristine kernel tree; host-side only (-j4
# allowed, 18 GB host RAM); external-disk TMPDIR (host /tmp is tmpfs).
# Packs boot.img with the STOCK dt.img + reference ramdisk (never rebuilt).
set -eu
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch-int4}
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
LOG=${LOG:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/logs/gpu31-int4-build.log}
export TMPDIR=${TMPDIR:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/tmp}
export TMP=$TMPDIR TEMP=$TMPDIR
mkdir -p "$TMPDIR"

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR"

# 1. backport shims 0001-0005 (patches/drm-backport, committed f698920)
for p in "$REPO"/patches/drm-backport/000[1-5]-*.patch; do
	git apply --check "$p" && git apply "$p"
done
# 2. Mode-B render node (no modeset takeover)
git apply --check "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch \
&& git apply "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch
# 3. panfrost7580 render-only driver (committed f76e4d1)
for p in "$REPO"/code/gpu31/driver/000[12]-*.patch; do
	git apply --check "$p" && git apply "$p"
done

# 4. config: pmaports base + d0-m1-minimal + d4-panfrost7580, then closure
cp "$REPO"/code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
for f in "$REPO"/code/gpu31/config/fragments/d0-m1-minimal.fragment \
	 "$REPO"/code/gpu31/config/fragments/d4-panfrost7580.fragment; do
	grep -v '^#' "$f" | grep -v '^$' | cut -d= -f1 | while read -r sym; do
		grep -v "^${sym}=\|^# ${sym} is not set" .config > .config.tmp && mv .config.tmp .config
	done
	grep -v '^#' "$f" | grep -v '^$' >> .config
done
yes "" | make ARCH=arm64 olddefconfig >/dev/null

# 5. build
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
ls -l arch/arm64/boot/Image
sha256sum arch/arm64/boot/Image

# 6. pack boot.img (stock dt.img rule: dt.img is NEVER rebuilt)
if [ -f /tmp/bimg/dt.img ]; then
	python3 /tmp/mkboot.py arch/arm64/boot/Image /tmp/bimg/ramdisk-new.gz \
		/tmp/bimg/dt.img /tmp/kernel-boot-int4.img "$(cat /tmp/bimg/cmdline)"
	ls -l /tmp/kernel-boot-int4.img
	sha256sum /tmp/kernel-boot-int4.img
fi
