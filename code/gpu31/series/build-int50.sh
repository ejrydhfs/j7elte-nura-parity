#!/bin/bash
# integration50 (decon-kms assembly): M0 atomic core + core helper fixes +
# DECON export shim + CRTC/plane/DSI/fence/fbdev/dmabuf slices + main bind.
#
# Assembly rule (Main): all seven driver objects live in
# drivers/gpu/drm/exynos7580/; the slice patches were normalized from
# drivers/gpu/drm/exynos/ (fence/plane/crtc/fbdev) so 0200's Makefile is the
# single wiring point. The fbdev patch's exynos/Kconfig + exynos/Makefile
# hunks are dropped for the same reason (0200 carries the choice and the
# object list). 0201 creates the frozen contract header in the tree from
# code/gpu31/kms/exynos_drm7580.h (with the two atomic prototypes folded in).
#
# NOT applied: patches/drm-kms-coexist/0001-display-owner-kconfig.patch
# (subsumed by 0200) and patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch
# (superseded by this series).
#
# Order: M0 drm-atomic 0001..0005 -> core 0001/0002/0003/0005 -> shim 0100 ->
# 0101 crtc -> 0201 contract header -> dsi/fence/plane/fbdev/dmabuf -> 0200.
# Config: config-samsung-j7elte + patches/drm-kms-coexist/display-owner-kms.fragment
# (the fragment's "# CONFIG_X is not set" lines are applied as removals, unlike
# plain non-comment fragments). Stock dt.img, reference ramdisk, Image-int50.
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int50}
LOG=${LOG:-$EXT/logs/int50-kms-build.log}
EVID=${EVID:-$EXT/logs/int50-kms-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int50 GATE FAIL: $1"; exit 1; }

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR" || exit 1

REPO_HEAD=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)

# ---------------------------------------------------------------- patches
PATCHES=""
addp() { [ -f "$1" ] || { echo "missing patch: $1" | tee -a "$EVID"; exit 1; }; PATCHES="$PATCHES $1"; }
for n in 0001 0002 0003 0004 0005; do
	addp "$(echo "$REPO"/patches/drm-atomic/$n-*.patch)"
done
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
note "series: $(echo $PATCHES | wc -w) patches"

# new-file hunk integrity for every patch applied (int37/int38/int47 class)
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
note "all series patches applied cleanly (0 fuzz/rejects)"

# contract header must be exactly the repo source of truth
cmp -s drivers/gpu/drm/exynos7580/exynos_drm7580.h "$REPO/code/gpu31/kms/exynos_drm7580.h" \
	|| gate_fail "in-tree contract header differs from code/gpu31/kms/exynos_drm7580.h"
grep -q 'int exynos7580_atomic_check(struct drm_device \*drm,' drivers/gpu/drm/exynos7580/exynos_drm7580.h \
	|| gate_fail "contract header lost exynos7580_atomic_check prototype"
grep -q 'int exynos7580_atomic_commit(struct drm_device \*drm,' drivers/gpu/drm/exynos7580/exynos_drm7580.h \
	|| gate_fail "contract header lost exynos7580_atomic_commit prototype"
note "contract header folded (atomic_check/atomic_commit prototypes present, identical to repo source)"

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
apply_frag "$REPO"/patches/drm-kms-coexist/display-owner-kms.fragment
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int50-kms-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed (see int50-kms-config.log)"
grep -qE 'recursive dependency|error:' "$EXT/logs/int50-kms-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

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
MISSF=$EXT/logs/int50-kms-config-missing.log
: > "$MISSF"
printf '%s\n' "$CONFIG_EXPECT" | while IFS= read -r line; do
	[ -n "$line" ] || continue
	grep -qxF "$line" .config || printf 'missing: %s\n' "$line" >> "$MISSF"
done
if [ -s "$MISSF" ]; then
	cat "$MISSF" | tee -a "$EVID"
	gate_fail "config closure failed (missing expectations in $MISSF)"
fi
note "config closure: 14 fragment expectations exact (KMS choice, DRM_EXYNOS off, fbcon off, CMA 32, IOVMM v6, vendor display on)"
grep -E 'CONFIG_(EXYNOS7580_DISPLAY_KMS|DRM_EXYNOS7580|DRM_KMS_CMA_HELPER|CMA_SIZE_MBYTES|EXYNOS_IOVMM_V6)=' .config | tee -a "$EVID"

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
note "== int50 gate: layout and single wiring point =="
KMS=drivers/gpu/drm/exynos7580
for f in Kconfig Makefile exynos_drm7580.h exynos_drm7580_main.c exynos_drm7580_crtc.c \
	 exynos_drm7580_plane.c exynos_drm7580_dsi.c exynos_drm7580_dmabuf.c \
	 exynos_drm7580_fence.c exynos_drm7580_fbdev.c; do
	[ -f "$KMS/$f" ] || gate_fail "$KMS/$f missing"
done
[ "$(ls "$KMS"/*.c | wc -l)" = 7 ] || gate_fail "expected exactly 7 driver sources in $KMS"
[ "$(ls drivers/gpu/drm/exynos/exynos_drm7580_*.c 2>/dev/null | wc -l)" = 0 ] \
	|| gate_fail "slice sources still exist under drivers/gpu/drm/exynos/ (path normalization incomplete)"
grep -q '^obj-$(CONFIG_DRM_EXYNOS7580) += exynos-drm7580.o$' "$KMS/Makefile" \
	|| gate_fail "Makefile does not wire exynos-drm7580.o"
for o in main crtc plane dsi dmabuf fence fbdev; do
	grep -q "exynos_drm7580_$o.o" "$KMS/Makefile" || gate_fail "Makefile lost $o object"
done
[ "$(grep -c 'exynos_drm7580_.*\.o' "$KMS/Makefile")" = 7 ] || gate_fail "Makefile must list exactly 7 objects"
grep -q 'depends on DRM=y && OF && ARM64 && ARCH_EXYNOS && EXYNOS7580_DISPLAY_KMS && EXYNOS_IOVMM_V6' "$KMS/Kconfig" \
	|| gate_fail "Kconfig lost the EXYNOS_IOVMM_V6 dependency"
grep -q 'select DRM_KMS_CMA_HELPER' "$KMS/Kconfig" || gate_fail "Kconfig lost DRM_KMS_CMA_HELPER"
grep -q 'EXYNOS7580_DISPLAY_KMS' drivers/video/exynos/decon_7580/Kconfig \
	|| gate_fail "display-owner KMS choice missing from decon Kconfig"
grep -q 'EXYNOS7580_DISPLAY_KMS' drivers/gpu/drm/exynos/Kconfig \
	&& gate_fail "drm-kms-coexist/0001 was applied twice (choice in exynos/Kconfig)"
ls -l "$KMS" | tee -a "$EVID"

note "== int50 gate: atomic core + driver features =="
[ -f include/drm/drm_atomic.h ] || gate_fail "M0 drm_atomic.h missing"
[ -f include/drm/drm_atomic_helper.h ] || gate_fail "M0 drm_atomic_helper.h missing"
[ -f drivers/gpu/drm/drm_atomic.c ] || gate_fail "M0 drm_atomic.c missing"
grep -q 'DRM_IOCTL_MODE_ATOMIC' drivers/gpu/drm/drm_drv.c || gate_fail "DRM_IOCTL_MODE_ATOMIC not wired into the ioctl table"
grep -q 'drm_mode_atomic_ioctl' drivers/gpu/drm/drm_atomic.c || gate_fail "drm_mode_atomic_ioctl not implemented"
grep -q 'DRIVER_ATOMIC' "$KMS/exynos_drm7580_main.c" || gate_fail "our driver features lost DRIVER_ATOMIC"
[ "$(grep -rn 'driver_features.*DRIVER_ATOMIC' drivers/gpu/drm --include=*.c | wc -l)" = 1 ] \
	|| gate_fail "DRIVER_ATOMIC must be set by our driver only, got $(grep -rn 'driver_features.*DRIVER_ATOMIC' drivers/gpu/drm --include=*.c | wc -l) registrations"
grep -rn 'driver_features.*DRIVER_ATOMIC' drivers/gpu/drm --include=*.c | grep -q "$KMS/" \
	|| gate_fail "DRIVER_ATOMIC registration is not ours"
grep -q 'late_initcall(exynos7580_drm_init)' "$KMS/exynos_drm7580_main.c" \
	|| gate_fail "bind point is not late_initcall"
grep -q 'samsung,exynos5-decon_driver' "$KMS/exynos_drm7580_main.c" || gate_fail "DECON compatible missing"
grep -q 'drm_platform_init' "$KMS/exynos_drm7580_main.c" || gate_fail "3.10 drm_platform_init missing"
grep -q 'exynos7580_atomic_commit' "$KMS/exynos_drm7580_main.c" || gate_fail "mode_config atomic_commit missing"
grep -q 'DRIVER_RENDER' "$KMS/exynos_drm7580_main.c" && gate_fail "DRIVER_RENDER must not be set (card0)"
grep -q 'exynos7580_fbdev_init' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev slice init missing"
grep -q 'drm_fbdev_cma_restore_mode' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "initial modeset (restore_mode) missing"

note "== int50 gate: no core gem mmap/offset in our paths =="
grep -q 'drm_gem_mmap(' "$KMS"/*.c && gate_fail "exynos7580 sources still call core drm_gem_mmap()"
grep -q 'drm_gem_create_mmap_offset' "$KMS"/*.c && gate_fail "exynos7580 sources still call drm_gem_create_mmap_offset()"
grep -q 'drm_gem_cma_dumb_create' "$KMS/exynos_drm7580_main.c" || gate_fail "CMA dumb_create not wired"
grep -q 'exynos7580_gem_prime_import' "$KMS/exynos_drm7580_main.c" || gate_fail "PRIME import not wired"

note "== int50 gate: linked driver symbols =="
VML=vmlinux
for s in exynos7580_atomic_check exynos7580_atomic_commit exynos7580_commit_tail \
	 exynos7580_vblank_irq exynos7580_crtc_init exynos7580_crtc_enable_link \
	 exynos7580_planes_init exynos7580_dsi_init exynos7580_dsi_get_modes \
	 exynos7580_fb_create exynos7580_gem_prime_import exynos7580_fb_scanout \
	 exynos7580_fbdev_init; do
	[ "$(aarch64-linux-gnu-nm "$VML" | grep -c " [tT] $s\$")" = 1 ] \
		|| gate_fail "vmlinux must define $s exactly once"
done
aarch64-linux-gnu-nm "$VML" | grep -q ' __initcall_exynos7580_drm_init7$' \
	|| gate_fail "late_initcall bind symbol missing from vmlinux"
aarch64-linux-gnu-nm "$VML" | grep -q ' [dD] exynos7580_drm_driver$' || gate_fail "drm_driver missing from vmlinux"
[ "$(aarch64-linux-gnu-nm "$VML" | grep -c ' [tT] exynos7580_atomic_commit$')" = 1 ] \
	|| gate_fail "atomic_commit symbol is not single"
[ "$(aarch64-linux-gnu-nm "$VML" | grep -c ' [tT] exynos_drm_driver$')" = 0 ] \
	|| gate_fail "in-tree exynos DRM driver is linked (DRM_EXYNOS must be off)"
aarch64-linux-gnu-nm drivers/gpu/drm/exynos7580/exynos-drm7580.o | grep -E ' (T|D) exynos7580_' | tee -a "$EVID"
for s in 'exynos7580: %s bound, card0 ready (atomic)' 'samsung,exynos5-decon_driver' 'exynos7580-decon-drm'; do
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" || gate_fail "Image lost '$s'"
done
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -E 'exynos7580|card0 ready' | head -8 | tee -a "$EVID"

note "== int50 gate: KMS objects clean =="
[ -f drivers/gpu/drm/exynos7580/built-in.o ] || gate_fail "exynos7580/built-in.o missing"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "vmlinux predates this build (stale prebuilt vmlinux)"
W=$(grep -E 'warning:' "$LOG" | grep -c 'drivers/gpu/drm/exynos7580/')
[ "$W" = 0 ] || gate_fail "exynos7580 sources produced $W warnings"
grep -E 'CC +drivers/gpu/drm/exynos7580/' "$LOG" | tee -a "$EVID"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/exynos7580" 2>&1 | tee -a "$EVID"

STAGE=${STAGE:-$EXT/stage-int50}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int50.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int50.img" | tee -a "$EVID"
sha256sum "$EXT/kernel-boot-int50.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int50"
sha256sum "$EXT/Image-int50" | tee -a "$EVID"

note "== int50 assembly gate: PASS (7 sources + header in exynos7580/, 18-patch series applied with hunk integrity, KMS choice/DRM_EXYNOS/IOVMM closure, atomic core + single atomic_commit, no core gem-mmap in our paths, 15 slice symbols once each in vmlinux, callback strings in Image, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
