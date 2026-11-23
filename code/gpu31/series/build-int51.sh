#!/bin/bash
# integration51 (decon-kms bisect pair): MODE=a -> the whole atomic/core/shim
# series is applied but the KMS driver is compiled out (vendor fbdev owns
# DECON, expected to boot exactly like int49); MODE=b -> KMS compiled in, but
# exynos7580_probe() refuses with -ENODEV right after matching, before
# decon_kms_get(), so the vendor late_initcall_sync fallback must bind.
#
# Both modes use the int50 assembly (single drivers/gpu/drm/exynos7580/
# layout, contract header, 18-patch series); MODE=b adds the diagnostic
# patches/drm-kms/diag/0202-int51b-probe-enodev.patch. Build only - Main flashes.
set -u
MODE=${MODE:-b}
case "$MODE" in a|b) ;; *) echo "MODE must be a or b"; exit 2 ;; esac
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

note "int51 mode=$MODE"
REPO_HEAD=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)

PATCHES=""
addp() { [ -f "$1" ] || { echo "missing patch: $1" | tee -a "$EVID"; exit 1; }; PATCHES="$PATCHES $1"; }
for n in 0001 0002 0003 0004 0005; do addp "$(echo "$REPO"/patches/drm-atomic/$n-*.patch)"; done
addp "$REPO"/patches/drm-kms/0001-core-drm-crtc-helper-atomic-aware-unused-function-teardown.patch
addp "$REPO"/patches/drm-kms/0002-core-drm-fb-helper-fix-id-drmfb.patch
addp "$REPO"/patches/drm-kms/0003-core-drm-gem-cma-helper-arm64-dma-attrs.patch
addp "$REPO"/patches/drm-kms/0005-core-drm-gem-cma-import-free.patch
addp "$REPO"/patches/drm-kms/0100-decon-export-shim.patch
addp "$REPO"/patches/drm-kms/0101-crtc-decon-atomic-crtc-and-link-enable.patch
addp "$REPO"/patches/drm-kms/0201-exynos7580-contract-header.patch
addp "$REPO"/patches/drm-kms/0001-dsi-encoder-connector.patch
addp "$REPO"/patches/drm-kms/0001-kmsfence-vblank-flip-events.patch
addp "$REPO"/patches/drm-kms/0001-plane-primary-overlays-and-formats.patch
addp "$REPO"/patches/drm-kms/0004-kmsfbdev-decon-kms-fbdev-emulation.patch
addp "$REPO"/patches/drm-kms/0001-kmsdmabuf-import-and-framebuffer.patch
addp "$REPO"/patches/drm-kms/0200-kms-main-bind.patch
if [ "$MODE" = b ]; then
	addp "$REPO"/patches/drm-kms/diag/0202-int51b-probe-enodev.patch
fi
note "series: $(echo $PATCHES | wc -w) patches"

HUNK_BAD=""
for pf in $PATCHES; do
	one=$(awk -v name="$(basename "$pf")" '
/^diff --git / { if (hdr && cnt != hdr) printf "%s: declared %d, plus-lines %d (%s)\n", name, hdr, cnt, file; hdr=0; cnt=0; file=""; next }
/^\+\+\+ / { file=$0; next }
/^@@ -0,0 \+1,([0-9]+) @@/ { hdr=substr($0, index($0,"+1,")+3); sub(/ @@.*/,"",hdr); cnt=0; next }
hdr && /^\+/ { cnt++ }
END { if (hdr && cnt != hdr) printf "%s: declared %d, plus-lines %d (%s)\n", name, hdr, cnt, file }
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
note "all series patches applied cleanly"

cmp -s drivers/gpu/drm/exynos7580/exynos_drm7580.h "$REPO/code/gpu31/kms/exynos_drm7580.h" \
	|| gate_fail "in-tree contract header differs from the repo source"

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
if [ "$MODE" = a ]; then
	apply_frag "$REPO"/patches/drm-kms/diag/display-owner-fbdev-diag.fragment
else
	apply_frag "$REPO"/patches/drm-kms-coexist/display-owner-kms.fragment
fi
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int51$MODE-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed"
grep -qE 'recursive dependency|error:' "$EXT/logs/int51$MODE-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

if [ "$MODE" = a ]; then
CONFIG_EXPECT="
CONFIG_EXYNOS7580_DISPLAY_FBDEV=y
# CONFIG_EXYNOS7580_DISPLAY_KMS is not set
CONFIG_DRM=y
CONFIG_CMA_SIZE_MBYTES=32
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_EXYNOS_MIPI_DSI=y
CONFIG_EXYNOS_DECON_LCD=y
CONFIG_PANEL_EA8061_DYNAMIC=y
CONFIG_EXYNOS_IOVMM_V6=y
# CONFIG_DRM_EXYNOS is not set
# CONFIG_FRAMEBUFFER_CONSOLE is not set"
# The KMS driver symbol has an unmet dependency in this mode, so Kconfig
# drops it entirely (no "# ... is not set" line): require absence.
else
CONFIG_EXPECT="
CONFIG_EXYNOS7580_DISPLAY_KMS=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_KMS_CMA_HELPER=y
CONFIG_DRM_EXYNOS7580=y
CONFIG_CMA_SIZE_MBYTES=32
CONFIG_EXYNOS_IOVMM_V6=y
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_EXYNOS_MIPI_DSI=y
CONFIG_EXYNOS_DECON_LCD=y
CONFIG_PANEL_EA8061_DYNAMIC=y
# CONFIG_DRM_EXYNOS is not set
# CONFIG_EXYNOS7580_DISPLAY_FBDEV is not set
# CONFIG_FRAMEBUFFER_CONSOLE is not set"
fi
MISSF=$EXT/logs/int51$MODE-config-missing.log
: > "$MISSF"
printf '%s\n' "$CONFIG_EXPECT" | while IFS= read -r line; do
	[ -n "$line" ] || continue
	grep -qxF "$line" .config || printf 'missing: %s\n' "$line" >> "$MISSF"
done
if [ -s "$MISSF" ]; then cat "$MISSF" | tee -a "$EVID"; gate_fail "config closure failed"; fi
note "config closure: $(printf '%s\n' "$CONFIG_EXPECT" | grep -c .) expectations exact"

# ---------------------------------------------------------------- build
BUILD_START=$(date +%s)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
BUILD_RC=$?
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ "$BUILD_RC" = 0 ] || gate_fail "make -j4 Image failed (rc=$BUILD_RC)"
[ "$(stat -c %Y arch/arm64/boot/Image)" -ge "$BUILD_START" ] || gate_fail "stale Image"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "stale vmlinux (prebuilt artifact)"
[ "$(grep -c 'error:' "$LOG")" = 0 ] || gate_fail "build log contains errors"
W=$(grep -E 'warning:' "$LOG" | grep -c 'drivers/gpu/drm/exynos7580/')
[ "$W" = 0 ] || gate_fail "exynos7580 sources produced $W warnings"
note "build rc=0, Image mtime $(stat -c %y arch/arm64/boot/Image), log $(wc -l < "$LOG") lines, $(grep -c 'warning:' "$LOG") warnings"

# ---------------------------------------------------------------- gates
note "== int51$MODE gate: layout =="
KMS=drivers/gpu/drm/exynos7580
for f in Kconfig Makefile exynos_drm7580.h exynos_drm7580_main.c exynos_drm7580_crtc.c \
	 exynos_drm7580_plane.c exynos_drm7580_dsi.c exynos_drm7580_dmabuf.c \
	 exynos_drm7580_fence.c exynos_drm7580_fbdev.c; do
	[ -f "$KMS/$f" ] || gate_fail "$KMS/$f missing"
done
[ "$(ls "$KMS"/*.c | wc -l)" = 7 ] || gate_fail "expected 7 driver sources"
[ "$(ls drivers/gpu/drm/exynos/exynos_drm7580_*.c 2>/dev/null | wc -l)" = 0 ] || gate_fail "slice sources left under exynos/"

note "== int51$MODE gate: atomic core still present =="
[ -f include/drm/drm_atomic.h ] || gate_fail "drm_atomic.h missing"
[ -f drivers/gpu/drm/drm_atomic.c ] || gate_fail "drm_atomic.c missing"
[ -f drivers/gpu/drm/drm_atomic.o ] || gate_fail "drm_atomic.o not built"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] drm_mode_atomic_ioctl$')" = 1 ] \
	|| gate_fail "drm_mode_atomic_ioctl not linked once"
grep -q 'DRM_IOCTL_MODE_ATOMIC' drivers/gpu/drm/drm_drv.c || gate_fail "atomic ioctl not wired"
note "M0 atomic core linked (drm_atomic.o, drm_mode_atomic_ioctl, DRM_IOCTL_MODE_ATOMIC)"

if [ "$MODE" = a ]; then
	note "== int51a gate: KMS compiled out, vendor fbdev owns DECON =="
	grep -qxF 'CONFIG_EXYNOS7580_DISPLAY_FBDEV=y' .config || gate_fail "FBDEV choice not on"
	grep -qxF '# CONFIG_EXYNOS7580_DISPLAY_KMS is not set' .config || gate_fail "KMS choice still on"
	grep -q '^CONFIG_DRM_EXYNOS7580=' .config && gate_fail "KMS driver symbol still on"
	[ ! -f "$KMS/exynos-drm7580.o" ] || gate_fail "KMS object was built with KMS off"
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] exynos7580_atomic_commit$')" = 0 ] \
		|| gate_fail "KMS symbols in vmlinux with KMS off"
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' __initcall_exynos7580_drm_init7$')" = 0 ] \
		|| gate_fail "KMS initcall in vmlinux with KMS off"
	[ -f drivers/video/exynos/decon_7580/built-in.o ] || gate_fail "vendor DECON not built"
	grep -qxF 'CONFIG_EXYNOS_DECON_EXYNOS7580=y' .config || gate_fail "vendor DECON config off"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'samsung,exynos5-decon_driver' \
		|| gate_fail "DECON compatible missing from Image"
	note "int51a proof: series in, KMS driver not in the image, vendor DECON/fbdev config on"
else
	note "== int51b gate: probe refuses before decon_kms_get =="
	grep -qxF 'CONFIG_DRM_EXYNOS7580=y' .config || gate_fail "KMS driver symbol not on"
	[ -f "$KMS/exynos-drm7580.o" ] || gate_fail "KMS object missing"
	DIAG=$(awk '/^static int exynos7580_probe/,/^}/' "$KMS/exynos_drm7580_main.c")
	printf '%s\n' "$DIAG" | grep -q 'int51b: probe refused before decon_kms_get (diagnostic)' \
		|| gate_fail "int51b diagnostic line missing from probe"
	DIAG_RET=$(printf '%s\n' "$DIAG" | grep -n 'int51b: probe refused' | cut -d: -f1 | head -1)
	DIAG_GET=$(printf '%s\n' "$DIAG" | grep -n 'ex->decon = decon_kms_get(dev);' | cut -d: -f1 | head -1)
	[ -n "$DIAG_RET" ] && [ -n "$DIAG_GET" ] && [ "$DIAG_RET" -lt "$DIAG_GET" ] \
		|| gate_fail "diagnostic return is not before decon_kms_get (ret=$DIAG_RET get=$DIAG_GET)"
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'int51b: probe refused before decon_kms_get (diagnostic)' \
		|| gate_fail "diagnostic string missing from Image"
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] exynos7580_atomic_commit$')" = 1 ] \
		|| gate_fail "KMS atomic_commit not linked once"
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' __initcall_exynos7580_drm_init7$')" = 1 ] \
		|| gate_fail "KMS late_initcall not linked"
	note "int51b proof: KMS compiled in and linked, probe returns -ENODEV before DECON stand-up"
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
note "== int51$MODE gate: PASS (series applied with hunk integrity, config closure, fresh Image+vmlinux, layout single-point, atomic core linked, mode-specific KMS/vendor evidence, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
