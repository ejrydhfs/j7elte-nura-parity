#!/bin/bash
# int54-diag: rescue kernel.  Only job: boot headless with MODULES=y and expose
# the previous boot's crash log through pstore over USB.
#
# No source patches at all (no 0100/0101/0102, no KMS series).  Config is the
# stock config-samsung-j7elte (which already has PSTORE=y, PSTORE_RAM=y,
# PSTORE_PMSG=y, EXYNOS_SNAPSHOT_PSTORE=y, EXYNOS_SNAPSHOT_PANIC_REBOOT=y and
# the vendor exynos-ss ramoops registration) plus:
#   CONFIG_PSTORE_CONSOLE=y     record the console into ramoops
#   CONFIG_MODULES=y            the int53 delta under test
#   CONFIG_MODULE_UNLOAD=y
#   EXYNOS_DECON_EXYNOS7580=n   all display drivers off: drivers/video/exynos/
#   EXYNOS_MIPI_DSI=n           Makefile drops decon_7580/, so DECON, DSIM and
#                               the panels are not compiled in and nothing
#                               probes the display at boot.
#
# First boot after flashing reads int53's panic from the retained ramoops
# region at /sys/fs/pstore (dmesg-ramoops-*), no DT change.
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int54}
LOG=${LOG:-$EXT/logs/int54-diag-build.log}
EVID=${EVID:-$EXT/logs/int54-diag-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int54 GATE FAIL: $1"; exit 1; }

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR" || exit 1

note "series: 0 patches (no KMS, no shim: stock vendor tree)"

# ---------------------------------------------------------------- config
cp "$REPO"/code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
force_cfg() {
	grep -v "^$1=\|^# $1 is not set$" .config > .config.tmp && mv .config.tmp .config
	if [ "$2" = n ]; then printf '# %s is not set\n' "$1" >> .config
	else printf '%s=%s\n' "$1" "$2" >> .config; fi
}
force_cfg CONFIG_PSTORE_CONSOLE y
force_cfg CONFIG_MODULES y
force_cfg CONFIG_MODULE_UNLOAD y
force_cfg CONFIG_EXYNOS_DECON_EXYNOS7580 n
force_cfg CONFIG_EXYNOS_MIPI_DSI n
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int54-diag-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed (see int54-diag-config.log)"
grep -qE 'recursive dependency|error:' "$EXT/logs/int54-diag-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

CONFIG_EXPECT="
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_EXYNOS_SNAPSHOT_PSTORE=y
CONFIG_EXYNOS_SNAPSHOT_PANIC_REBOOT=y
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
# CONFIG_EXYNOS_DECON_EXYNOS7580 is not set
# CONFIG_EXYNOS_MIPI_DSI is not set
# CONFIG_DRM is not set"
MISSF=$EXT/logs/int54-diag-config-missing.log
: > "$MISSF"
printf '%s\n' "$CONFIG_EXPECT" | while IFS= read -r line; do
	[ -n "$line" ] || continue
	grep -qxF "$line" .config || printf 'missing: %s\n' "$line" >> "$MISSF"
done
if [ -s "$MISSF" ]; then
	cat "$MISSF" | tee -a "$EVID"
	gate_fail "config closure failed (missing expectations in $MISSF)"
fi
note "config closure: pstore console + ramoops + modules on; DECON/DSIM/DRM off"
grep -E 'CONFIG_(PSTORE|PSTORE_RAM|PSTORE_CONSOLE|MODULES|MODULE_UNLOAD|EXYNOS_DECON_EXYNOS7580|EXYNOS_MIPI_DSI|DRM)=' .config | tee -a "$EVID" || true

# ---------------------------------------------------------------- build
BUILD_START=$(date +%s)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
BUILD_RC=$?
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ "$BUILD_RC" = 0 ] || gate_fail "make -j4 Image failed (rc=$BUILD_RC; see $LOG)"
[ "$(stat -c %Y arch/arm64/boot/Image)" -ge "$BUILD_START" ] \
	|| gate_fail "Image predates this build (stale artifact from the pristine copy)"
note "build rc=0, Image mtime $(stat -c %y arch/arm64/boot/Image)"
[ "$(grep -c 'error:' "$LOG")" = 0 ] || gate_fail "build log contains errors"
note "build log: $(wc -l < "$LOG") lines, $(grep -c 'warning:' "$LOG") warnings"

# ---------------------------------------------------------------- gates
note "== int54 gate: no display driver in the image =="
for s in decon_probe decon_probe_common decon_register_entity decon_int_drvdata \
	 exynos_decon_register dsim_probe exynos7580_drm_driver; do
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c " [tTdD] $s\$")" = 0 ] \
		|| gate_fail "$s is linked (display stack not compiled out)"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'decon registered successfully' \
	&& gate_fail "vendor DECON driver string in Image"
for s in exynos7580_atomic_check exynos7580_atomic_commit exynos7580_crtc_init \
	 exynos7580_fbdev_init exynos7580_probe exynos7580_dsi_init; do
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c " [tTdD] $s\$")" = 0 ] \
		|| gate_fail "$s in Image (KMS series leaked in)"
done
note "display: DECON/DSIM/panels/KMS all absent from vmlinux and Image"

note "== int54 gate: pstore + ramoops + module loader =="
for s in ess_pstore_init pstore_register pstore_console_write ramoops_init; do
	aarch64-linux-gnu-nm vmlinux | grep -q " [tT] $s\$" || gate_fail "$s missing from vmlinux"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'log_pstore' \
	|| gate_fail "ess ramoops item name (log_pstore) not in Image"
for s in load_module SyS_finit_module; do
	aarch64-linux-gnu-nm vmlinux | grep -q " [tT] $s\$" || gate_fail "$s missing: module loader not built"
done
[ "$(find drivers -name '*.ko' | wc -l)" = 0 ] || gate_fail "unexpected .ko built (no driver should be modular)"
aarch64-linux-gnu-nm vmlinux | grep -q ' __ksymtab_' || gate_fail "no ksymtab in vmlinux"
note "pstore: pstore_register + ramoops + ess_pstore_init linked, log_pstore region name present; module loader present"

note "== int54 gate: build hygiene =="
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "vmlinux predates this build"
grep -E 'CC +drivers/trace/exynos-ss|CC +kernel/module' "$LOG" | head -4 | tee -a "$EVID"
note "fresh vmlinux; pstore and module code compiled in this build"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"

STAGE=${STAGE:-$EXT/stage-int54}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int54-diag.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int54-diag.img" | tee -a "$EVID"
sha256sum "$EXT/kernel-boot-int54-diag.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int54-diag"
sha256sum "$EXT/Image-int54-diag" | tee -a "$EVID"

note "== int54-diag gate: PASS (0 patches, display stack compiled out, pstore console + vendor ramoops + module loader in, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
