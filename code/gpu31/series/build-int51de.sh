#!/bin/bash
# integration51d/e (strict-minimal bisect after int51a ALSO bootlooped):
#   MODE=d: drm-atomic/0001..0005 only, stock vendor tree otherwise. Proves
#           whether the 3.10 atomic core backport alone boots.
#   MODE=e: 0100 as originally committed (register_framebuffer at probe
#           tail) - the int51e bootloop reproduction.
#   MODE=f: 0100 fixed (int51f): registration back at its original
#           position, gated by register_fb, the option-A hand-off branch
#           removed, and register_fb==false skipping the whole
#           fb/window-programming section.
# Both configs are the stock j7elte config plus a diagnostic fragment that
# differs from stock only by PSTORE_CONSOLE (ramoops is registered by the
# vendor exynos-ss driver on the bootloader "log_pstore" region, so no DT
# edit is needed) - and, for d, the minimum DRM symbols the atomic objects
# need to build.  Build only; Main flashes.
set -u
MODE=${MODE:-d}
case "$MODE" in d|e|f) ;; *) echo "MODE must be d, e or f"; exit 2 ;; esac
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
[ "$MODE" != d ] && grep -q '^CONFIG_DRM=' .config && gate_fail "DRM enabled in shim-only variant (not minimal)"

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
	note "== int51$MODE gate: shim only, no DRM/KMS =="
	CORE=drivers/video/exynos/decon_7580/decon_core.c
	[ -f drivers/video/exynos/decon_7580/decon_kms_api.c ] || gate_fail "shim source missing"
	[ -f include/linux/exynos_decon_kms.h ] || gate_fail "shim header missing"
	aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] decon_probe_common$' \
		|| gate_fail "decon_probe_common not linked"
	aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] decon_kms_get$' \
		|| gate_fail "decon_kms_get not linked"
	[ "$(grep -c 'register_framebuffer(fbinfo);' "$CORE")" = 1 ] || gate_fail "register_framebuffer(fbinfo) count != 1"
	REG_L=$(grep -n 'register_framebuffer(fbinfo);' "$CORE" | cut -d: -f1 | head -1)
	MUT_L=$(grep -n 'mutex_init(&decon->output_lock);' "$CORE" | cut -d: -f1 | head -1)
	grep -q 'decon->fb_registered = true;' "$CORE" || gate_fail "fb_registered stamp missing"
	[ ! -f drivers/gpu/drm/drm_atomic.c ] || gate_fail "atomic core present (not shim-only)"
	[ ! -d drivers/gpu/drm/exynos7580 ] || gate_fail "KMS driver present (not shim-only)"
	if [ "$MODE" = e ]; then
		# the committed-int51e defect: registration moved to probe tail
		grep -q 'fb0 is registered at the very end of probe' "$CORE" 			|| gate_fail "int51e reproduction lost the tail-registration block"
		[ -n "$REG_L" ] && [ -n "$MUT_L" ] && [ "$REG_L" -gt "$MUT_L" ] 			|| gate_fail "int51e reproduction: registration is not after output_lock"
		note "int51e proof: original 0100 shape (register_framebuffer after mutex_init/output_lock and the kthread start)"
	else
		# int51f: registration restored in place, hand-off gone, KMS stand-up skips fb
		grep -q 'fb0 is registered at the very end of probe' "$CORE" 			&& gate_fail "fixed shim still registers fb0 at the probe tail"
		grep -q 'decon_kms_attach(decon)' "$CORE" 			&& gate_fail "fixed shim still calls the option-A hand-off"
		[ -n "$REG_L" ] && [ -n "$MUT_L" ] && [ "$REG_L" -lt "$MUT_L" ] 			|| gate_fail "fixed shim registration is not before mutex_init/output_lock"
		grep -q 'if (!register_fb)' "$CORE" 			|| gate_fail "fixed shim has no register_fb==false skip"
		SKIP_L=$(grep -n 'if (!register_fb)' "$CORE" | cut -d: -f1 | head -1)
		WIN_L=$(grep -n 'win_regs.vidosd_b = vidosd_b(0, 0, fbinfo->var.xres' "$CORE" | cut -d: -f1 | head -1)
		[ -n "$SKIP_L" ] && [ -n "$WIN_L" ] && [ "$SKIP_L" -lt "$WIN_L" ] \
			|| gate_fail "register_fb skip is not before the fbinfo window branch (skip=$SKIP_L win=$WIN_L)"
		grep -q 'goto decon_init_done;' "$CORE" || gate_fail "no decon_init_done target for the skip"
		grep -q 'struct fb_info \*fbinfo = NULL;' "$CORE" || gate_fail "fbinfo not initialised"
		note "int51f proof: register_framebuffer back before output_lock and the kthread, gated by register_fb; hand-off branch removed; KMS stand-up jumps past the fbinfo window branch"
	fi
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
