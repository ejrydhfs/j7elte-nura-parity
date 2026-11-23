#!/bin/bash
# integration51d/e (strict-minimal bisect after int51a ALSO bootlooped):
#   MODE=d: drm-atomic/0001..0005 only, stock vendor tree otherwise. Proves
#           whether the 3.10 atomic core backport alone boots.
#   MODE=e: 0100 DECON export shim only, stock vendor tree otherwise, KMS
#           driver absent. Proves whether the vendor DECON probe split /
#           register_framebuffer-at-tail change breaks boot.
# Both configs are the stock j7elte config plus a diagnostic fragment that
# differs from stock only by PSTORE_CONSOLE (ramoops is registered by the
# vendor exynos-ss driver on the bootloader "log_pstore" region, so no DT
# edit is needed) - and, for d, the minimum DRM symbols the atomic objects
# need to build.  Build only; Main flashes.
set -u
MODE=${MODE:-d}
case "$MODE" in d|e) ;; *) echo "MODE must be d or e"; exit 2 ;; esac
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int51$MODE}
LOG=${LOG:-$EXT/logs/int51$MODE-build.log}
EVID=${EVID:-$EXT/logs/int51$MODE-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int51$MODE GATE FAIL: $1"; exit 1; }

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR" || exit 1
note "int51$MODE: strict-minimal variant"

PATCHES=""
if [ "$MODE" = d ]; then
	for n in 0001 0002 0003 0004 0005; do
		PATCHES="$PATCHES $(echo "$REPO"/patches/drm-atomic/$n-*.patch)"
	done
else
	PATCHES="$REPO/patches/drm-kms/0100-decon-export-shim.patch"
fi
for pf in $PATCHES; do
	[ -f "$pf" ] || gate_fail "missing patch $pf"
done
note "series: $(echo $PATCHES | wc -w) patch(es)"
HUNK_BAD=""
for pf in $PATCHES; do
	one=$(awk -v name="$(basename "$pf")" '
/^diff --git / { if (hdr && cnt != hdr) printf "%s: declared %d, plus %d (%s)\n", name, hdr, cnt, file; hdr=0; cnt=0; file=""; next }
/^\+\+\+ / { file=$0; next }
/^@@ -0,0 \+1,([0-9]+) @@/ { hdr=substr($0, index($0,"+1,")+3); sub(/ @@.*/,"",hdr); cnt=0; next }
hdr && /^\+/ { cnt++ }
END { if (hdr && cnt != hdr) printf "%s: declared %d, plus %d (%s)\n", name, hdr, cnt, file }
' "$pf")
	[ -n "$one" ] && HUNK_BAD="$HUNK_BAD$one"
done
[ -z "$HUNK_BAD" ] || gate_fail "new-file hunk count mismatch: $HUNK_BAD"
for pf in $PATCHES; do
	echo "== $(basename "$pf")" >> "$EVID"
	sha256sum "$pf" >> "$EVID"
	git apply --check "$pf" || gate_fail "git apply --check failed: $(basename "$pf")"
	git apply "$pf" || gate_fail "git apply failed: $(basename "$pf")"
done
note "patch(es) applied cleanly"

# ---------------------------------------------------------------- config
cp "$REPO"/code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
apply_frag() {
	local f=$1 line sym
	while IFS= read -r line; do
		case "$line" in
		'# CONFIG_'*' is not set')
			sym=$(printf '%s' "$line" | sed -E 's/^# (CONFIG_[A-Za-z0-9_]+) is not set$/\1/')
			[ "$sym" = "$line" ] && continue
			grep -v "^${sym}=\|^# ${sym} is not set" .config > .config.tmp && mv .config.tmp .config
			printf '%s\n' "$line" >> .config ;;
		CONFIG_*=*)
			sym=${line%%=*}
			grep -v "^${sym}=\|^# ${sym} is not set" .config > .config.tmp && mv .config.tmp .config
			printf '%s\n' "$line" >> .config ;;
		esac
	done < "$f"
}
if [ "$MODE" = d ]; then
	FRAG=$REPO/patches/drm-kms/diag/m0-only.fragment
else
	FRAG=$REPO/patches/drm-kms/diag/shim-only.fragment
fi
apply_frag "$FRAG"
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int51$MODE-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed"
grep -qE 'recursive dependency|error:' "$EXT/logs/int51$MODE-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

CONFIG_EXPECT="
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_EXYNOS_SNAPSHOT_PSTORE=y"
[ "$MODE" = d ] && CONFIG_EXPECT="$CONFIG_EXPECT
CONFIG_DRM=y"
MISSF=$EXT/logs/int51$MODE-config-missing.log
: > "$MISSF"
printf '%s\n' "$CONFIG_EXPECT" | while IFS= read -r line; do
	[ -n "$line" ] || continue
	grep -qxF "$line" .config || printf 'missing: %s\n' "$line" >> "$MISSF"
done
[ -s "$MISSF" ] && { cat "$MISSF" | tee -a "$EVID"; gate_fail "config closure failed"; }
note "config closure: $(printf '%s\n' "$CONFIG_EXPECT" | grep -c .) expectations exact (stock + diagnostic only)"
# The stock config must stay otherwise untouched: these must not appear.
[ "$MODE" = d ] && grep -q '^CONFIG_DRM_EXYNOS7580=' .config && gate_fail "KMS driver symbol present (not minimal)"
[ "$MODE" = e ] && grep -q '^CONFIG_DRM=' .config && gate_fail "DRM enabled in shim-only variant (not minimal)"

# ---------------------------------------------------------------- build
BUILD_START=$(date +%s)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
BUILD_RC=$?
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ "$BUILD_RC" = 0 ] || gate_fail "make -j4 Image failed (rc=$BUILD_RC)"
[ "$(stat -c %Y arch/arm64/boot/Image)" -ge "$BUILD_START" ] || gate_fail "stale Image"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "stale vmlinux (prebuilt artifact)"
[ "$(grep -c 'error:' "$LOG")" = 0 ] || gate_fail "build log contains errors"
note "build rc=0, Image mtime $(stat -c %y arch/arm64/boot/Image), log $(wc -l < "$LOG") lines, $(grep -c 'warning:' "$LOG") warnings"

# ---------------------------------------------------------------- gates
note "== int51$MODE gate: pstore capture available =="
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] ess_pstore_init$' \
	|| gate_fail "vendor ess_pstore_init not linked (ramoops source missing)"
grep -q '^CONFIG_PSTORE_CONSOLE=y' .config || gate_fail "pstore console capture off"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'ramoops' || gate_fail "ramoops string missing from Image"
note "ramoops: vendor ess_pstore_init (exynos-ss, bootloader log_pstore region) + PSTORE_CONSOLE; readable from /sys/fs/pstore next boot, no DT edit"

if [ "$MODE" = d ]; then
	note "== int51d gate: atomic core only, vendor tree untouched =="
	[ -f drivers/gpu/drm/drm_atomic.c ] || gate_fail "drm_atomic.c missing"
	[ -f drivers/gpu/drm/drm_atomic.o ] || gate_fail "drm_atomic.o not built"
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] drm_mode_atomic_ioctl$')" = 1 ] \
		|| gate_fail "drm_mode_atomic_ioctl not linked"
	grep -q 'DRM_IOCTL_MODE_ATOMIC' drivers/gpu/drm/drm_drv.c || gate_fail "atomic ioctl not wired"
	[ ! -d drivers/gpu/drm/exynos7580 ] || gate_fail "exynos7580 driver present (not M0-only)"
	grep -q 'decon_probe_common' drivers/video/exynos/decon_7580/*.c 2>/dev/null \
		&& gate_fail "DECON shim present (0100 applied; not M0-only)"
	[ ! -f drivers/video/exynos/decon_7580/decon_kms_api.c ] || gate_fail "decon_kms_api.c present (not M0-only)"
	note "int51d proof: only drm-atomic/0001..0005 are in the tree; no shim, no KMS, vendor DECON logs unchanged"
else
	note "== int51e gate: shim only, no DRM/KMS =="
	[ -f drivers/video/exynos/decon_7580/decon_kms_api.c ] || gate_fail "shim source missing"
	[ -f include/linux/exynos_decon_kms.h ] || gate_fail "shim header missing"
	aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] decon_probe_common$' \
		|| gate_fail "decon_probe_common not linked"
	aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] decon_kms_get$' \
		|| gate_fail "decon_kms_get not linked"
	grep -q 'register_framebuffer' drivers/video/exynos/decon_7580/decon_core.c || gate_fail "register_framebuffer lost"
	grep -q 'decon->fb_registered = true;' drivers/video/exynos/decon_7580/decon_core.c \
		|| gate_fail "fb_registered stamp missing (shim not applied)"
	[ ! -f drivers/gpu/drm/drm_atomic.c ] || gate_fail "atomic core present (not shim-only)"
	[ ! -d drivers/gpu/drm/exynos7580 ] || gate_fail "KMS driver present (not shim-only)"
	note "int51e proof: pristine vendor tree + 0100 only; DECON probe split + register_framebuffer at probe tail are the only deltas"
fi

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
STAGE=${STAGE:-$EXT/stage-int51$MODE}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int51$MODE.img" "$(cat "$STAGE/cmdline")"
sha256sum "$EXT/kernel-boot-int51$MODE.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int51$MODE"
sha256sum "$EXT/Image-int51$MODE" | tee -a "$EVID"
note "== int51$MODE gate: PASS (minimal series applied with hunk integrity, stock config + pstore-console only, fresh Image+vmlinux, mode-specific tree isolation, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
