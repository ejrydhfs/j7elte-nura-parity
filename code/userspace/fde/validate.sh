#!/bin/sh
# Static validation for the j7elte FDE userspace package.
#
# No hardware and no pmOS chroot are available here, so this only proves:
#  - the shipped shell scripts parse in POSIX/busybox sh,
#  - the APKBUILD checksums and install paths are consistent,
#  - the kernel config in this tree really has the dm-crypt/fbdev/input
#    options the unlock flow needs,
#  - the BOOT partition size hard-coded in j7elte-fde-check matches the PIT,
#  - the unl0kr drop-in only uses keys unl0kr's parser understands.
#
# Usage: ./validate.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
PKG="$ROOT/parity/pmaports/device/archived/j7elte-fde"
CONFIG="$ROOT/config-nura-j7elte"
PIT="$ROOT/session/tmp-artifacts/modemcp/j7elte.pit"

fail=0
pass=0

ok() {
	pass=$((pass + 1))
	printf '  [ ok ] %s\n' "$*"
}
bad() {
	fail=$((fail + 1))
	printf '  [FAIL] %s\n' "$*"
}
section() { printf '\n== %s ==\n' "$*"; }

[ -d "$PKG" ] || {
	echo "package dir not found: $PKG" >&2
	exit 1
}

section "Shell syntax (sh -n)"
for f in "$PKG/j7elte-fde-check" "$PKG/50-j7elte-fde-fb.sh"; do
	if sh -n "$f" 2>/tmp/j7elte-fde-sh-n.err; then
		ok "$(basename "$f") parses"
	else
		bad "$(basename "$f"): $(cat /tmp/j7elte-fde-sh-n.err)"
	fi
done

if command -v shellcheck >/dev/null 2>&1; then
	for f in "$PKG/j7elte-fde-check" "$PKG/50-j7elte-fde-fb.sh"; do
		if shellcheck -s sh -S warning "$f"; then
			ok "shellcheck: $(basename "$f")"
		else
			bad "shellcheck: $(basename "$f")"
		fi
	done
else
	echo "  [skip] shellcheck not installed"
fi

section "APKBUILD sources and checksums"
listed=$(awk '/^source="/{f=1;next} f&&/^[[:space:]]*"$/{f=0} f' "$PKG/APKBUILD" |
	sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | grep -v '^$')
for f in $listed; do
	[ -f "$PKG/$f" ] && ok "source exists: $f" || bad "source missing: $f"
done
on_disk=$(for f in "$PKG"/*; do
	basename "$f"
done | grep -v '^APKBUILD$' | sort)
if [ "$(printf '%s\n' "$on_disk" | sort)" = "$(printf '%s\n' "$listed" | sort)" ]; then
	ok "source list matches package directory"
else
	bad "source list does not match package directory"
fi

sum_report=$(mktemp)
awk '/^sha512sums="/{f=1;next} f&&/^[[:space:]]*"$/{f=0} f' "$PKG/APKBUILD" |
while read -r sum file; do
	[ -n "$sum" ] || continue
	actual=$(sha512sum "$PKG/$file" 2>/dev/null | cut -d' ' -f1)
	if [ "$sum" = "$actual" ]; then
		echo "ok $file"
	else
		echo "FAIL $file"
	fi
done >"$sum_report"
while read -r status file; do
	if [ "$status" = ok ]; then
		ok "sha512 $file"
	else
		bad "sha512 mismatch or missing file: $file"
	fi
done <"$sum_report"
rm -f "$sum_report"

section "APKBUILD content"
grep -q '^pkgname=j7elte-fde$' "$PKG/APKBUILD" &&
	ok "pkgname=j7elte-fde" || bad "unexpected pkgname"
if grep -qE '^[[:space:]]*unl0kr$' "$PKG/APKBUILD"; then
	ok "depends on unl0kr"
else
	bad "unl0kr missing from depends"
fi
grep -q 'usr/share/mkinitfs-triggers' "$PKG/APKBUILD" &&
	ok "installs mkinitfs trigger" || bad "no mkinitfs trigger"
grep -q 'usr/share/mkinitfs/hooks/' "$PKG/APKBUILD" &&
	ok "installs boot-time hook into /hooks" || bad "hook not installed"

section "initramfs file list"
while read -r path; do
	case "$path" in '' | '#'*) continue ;; esac
	src="$PKG/$(basename "$path")"
	[ -f "$src" ] && ok "listed file exists in package: $path" ||
		bad "listed file has no package source: $path"
done <"$PKG/40-j7elte-fde.files"

section "unl0kr drop-in"
# Keys accepted by unl0kr/config.c:parsing_handler().
allowed="animations backend timeout autohide layout popovers obscured bullet default alternate keyboard pointer touchscreen fbdev_force_refresh terminal_prevent_graphics_mode terminal_allow_keyboard_input"
while IFS= read -r line; do
	line=$(printf '%s' "$line" | sed 's/[[:space:]]*$//')
	case "$line" in
	'' | '#'*) continue ;;
	'['*']') continue ;;
	*=*)
		key=$(printf '%s' "$line" | cut -d= -f1)
		case " $allowed " in
		*" $key "*) ok "known unl0kr key: $key" ;;
		*) bad "unl0kr does not parse key '$key'" ;;
		esac
		;;
	*) bad "unparseable line in 50-j7elte.conf: $line" ;;
	esac
done <"$PKG/50-j7elte.conf"

section "Kernel config ($(basename "$CONFIG"))"
for sym in CONFIG_BLK_DEV_DM CONFIG_DM_CRYPT CONFIG_CRYPTO_AES CONFIG_CRYPTO_XTS \
	CONFIG_CRYPTO_CBC CONFIG_CRYPTO_SHA256 CONFIG_BLK_DEV_INITRD \
	CONFIG_RD_GZIP CONFIG_DEVTMPFS CONFIG_FB CONFIG_EXYNOS_DECON_FB \
	CONFIG_INPUT_EVDEV CONFIG_VT CONFIG_LCD_CLASS_DEVICE \
	CONFIG_BACKLIGHT_CLASS_DEVICE; do
	if grep -qx "$sym=y" "$CONFIG"; then
		ok "$sym=y"
	else
		bad "$sym is not built in"
	fi
done
if grep -qx 'CONFIG_TOUCHSCREEN_MELFAS_MMS438=y' "$CONFIG" ||
	grep -qx 'CONFIG_TOUCHSCREEN_IST30XXC=y' "$CONFIG"; then
	ok "built-in touchscreen driver"
else
	bad "no built-in touchscreen driver"
fi
if grep -q '^[^#]*=m$' "$CONFIG"; then
	bad "kernel has loadable modules; initramfs uses a static kernel"
else
	ok "kernel is fully static (=y only), nothing to modprobe before unlock"
fi

section "PIT BOOT partition vs j7elte-fde-check constant"
if [ -f "$PIT" ]; then
	# PIT entry: 9 x u32, then partitionName[32]; entry size 132 bytes,
	# entries start at 0x1c. Name offset 36, blockCount offset 24.
	pit_boot=$(od -An -tu4 -j $((0x1c + 13 * 132 + 24)) -N 4 "$PIT" |
		awk '{ print $1 * 512 }')
	pit_boot=$((pit_boot))
	if [ "$pit_boot" -eq 33554432 ]; then
		ok "PIT BOOT = $pit_boot B (32 MiB)"
	else
		bad "PIT BOOT = $pit_boot B, expected 33554432"
	fi
	script_boot=$(sed -n 's/^BOOT_PART_BYTES=\$((\([0-9]*\) \* 1024 \* 1024))$/\1/p' \
		"$PKG/j7elte-fde-check")
	[ "$script_boot" = 32 ] &&
		ok "j7elte-fde-check assumes 32 MiB BOOT" ||
		bad "j7elte-fde-check BOOT size constant is '$script_boot', expected 32"
else
	echo "  [skip] PIT not found: $PIT"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
