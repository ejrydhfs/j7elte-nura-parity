#!/bin/sh
# Rebuild firmware.tar.gz for firmware-samsung-j7elte from the stock SM-J700M ROM.
#
# Usage: ./extract-firmware.sh <AP_J700MUBU2BRC2_*.tar.md5>
#
# Requires: tar, simg2img, debugfs, gzip, sha512sum.
# The tarball is deterministic (sorted, epoch mtimes), so the sha512 in the
# APKBUILD is reproducible.
set -eu

AP=${1:?usage: $0 <AP_J700MUBU2BRC2_*.tar.md5>}
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "extracting system.img from $AP ..."
tar -xf "$AP" -C "$work" system.img
simg2img "$work/system.img" "$work/system.raw"

# ROM path (inside system.img) -> local name
EXTRACT="
/etc/wifi/bcmdhd_sta.bin
/etc/wifi/bcmdhd_apsta.bin
/etc/wifi/bcmdhd_mfg.bin
/etc/wifi/nvram_net.txt
/etc/wifi/nvram_mfg.txt
/vendor/firmware/bcm43438A1_V0051.0230.hcd
/vendor/firmware/fimc_is_fd.bin
/vendor/firmware/fimc_is_fw2_3l2.bin
/vendor/firmware/mfc_fw.bin
/vendor/firmware/setfile_3l2.bin
/vendor/firmware/setfile_5e3.bin
"

rm -rf files
mkdir files
for src in $EXTRACT; do
	base=$(basename "$src")
	debugfs -R "dump $src files/$base" "$work/system.raw" >/dev/null 2>&1
	if [ ! -s "files/$base" ]; then
		echo "ERROR: $src not found in system.img" >&2
		exit 1
	fi
done

echo "verifying checksums ..."
(cd files && sha512sum -c ../files.sha512)

echo "building deterministic firmware.tar.gz ..."
(cd files && tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - *) |
	gzip -n > firmware.tar.gz
sha512sum firmware.tar.gz
