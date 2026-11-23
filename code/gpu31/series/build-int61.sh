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
# plain non-comment fragments). Stock dt.img, reference ramdisk, Image-int61.
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int61}
LOG=${LOG:-$EXT/logs/int61-kms-build.log}
EVID=${EVID:-$EXT/logs/int61-kms-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int61 GATE FAIL: $1"; exit 1; }

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
addp "$REPO"/patches/drm-kms/0102-kms-module.patch
addp "$REPO"/patches/drm-kms/0103-kms-builtin-vendor-refuse.patch
addp "$REPO"/patches/drm-kms/0104-kms-sync-init-and-defer.patch
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
	EXC=""
	case "$(basename "$pf")" in
	0200-kms-main-bind.patch)
		EXC="--exclude=drivers/video/exynos/decon_7580/decon_core.c" ;;
	esac
	git apply $EXC --check "$pf" || gate_fail "git apply --check failed: $(basename "$pf")"
	git apply $EXC "$pf" || gate_fail "git apply failed: $(basename "$pf")"
done
note "0200 applied without drivers/video/exynos/decon_7580/decon_core.c (stock late_initcall kept)"

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
apply_frag "$REPO"/patches/drm-kms/diag/shim-only.fragment
# int61: keep the boot log (256 KB) for dmesg/pstore.
sed -i 's/^CONFIG_LOG_BUF_SHIFT=.*/CONFIG_LOG_BUF_SHIFT=18/' .config
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int61-kms-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed (see int61-kms-config.log)"
grep -qE 'recursive dependency|error:' "$EXT/logs/int61-kms-config.log" \
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
# CONFIG_FRAMEBUFFER_CONSOLE is not set
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_LOG_BUF_SHIFT=18
CONFIG_EXYNOS_SNAPSHOT_PSTORE=y
# CONFIG_MODULES is not set"
MISSF=$EXT/logs/int61-kms-config-missing.log
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
note "== int61 gate: layout and single wiring point =="
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
grep -q 'depends on DRM && OF && ARM64 && ARCH_EXYNOS && EXYNOS7580_DISPLAY_KMS && EXYNOS_IOVMM_V6' "$KMS/Kconfig" \
	|| gate_fail "Kconfig lost the EXYNOS_IOVMM_V6 dependency"
grep -A1 '^config DRM_EXYNOS7580$' "$KMS/Kconfig" | grep -q 'tristate "DRM/KMS support' \
	|| gate_fail "DRM_EXYNOS7580 is not tristate"
grep -q 'select DRM_KMS_CMA_HELPER' "$KMS/Kconfig" || gate_fail "Kconfig lost DRM_KMS_CMA_HELPER"
grep -q 'EXYNOS7580_DISPLAY_KMS' drivers/video/exynos/decon_7580/Kconfig \
	|| gate_fail "display-owner KMS choice missing from decon Kconfig"
grep -q 'EXYNOS7580_DISPLAY_KMS' drivers/gpu/drm/exynos/Kconfig \
	&& gate_fail "drm-kms-coexist/0001 was applied twice (choice in exynos/Kconfig)"
ls -l "$KMS" | tee -a "$EVID"

note "== int61 gate: atomic core + driver features =="
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
grep -q 'late_initcall_sync(exynos7580_drm_init)' "$KMS/exynos_drm7580_main.c" \
	|| gate_fail "bind point is not late_initcall_sync"
grep -q 'samsung,exynos5-decon_driver' "$KMS/exynos_drm7580_main.c" || gate_fail "DECON compatible missing"
grep -q 'drm_platform_init' "$KMS/exynos_drm7580_main.c" || gate_fail "3.10 drm_platform_init missing"
grep -q 'exynos7580_atomic_commit' "$KMS/exynos_drm7580_main.c" || gate_fail "mode_config atomic_commit missing"
grep -q 'DRIVER_RENDER' "$KMS/exynos_drm7580_main.c" && gate_fail "DRIVER_RENDER must not be set (card0)"
grep -q 'exynos7580_fbdev_init' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev slice init missing"
grep -q 'drm_fbdev_cma_restore_mode' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "initial modeset (restore_mode) missing"

note "== int61 gate: fixed shim ordering =="
CORE=drivers/video/exynos/decon_7580/decon_core.c
[ "$(grep -c 'register_framebuffer(fbinfo);' "$CORE")" = 1 ] || gate_fail "register_framebuffer(fbinfo) count != 1"
REG_L=$(grep -n 'register_framebuffer(fbinfo);' "$CORE" | cut -d: -f1 | head -1)
MUT_L=$(grep -n 'mutex_init(&decon->output_lock);' "$CORE" | cut -d: -f1 | head -1)
[ -n "$REG_L" ] && [ -n "$MUT_L" ] && [ "$REG_L" -lt "$MUT_L" ] \
	|| gate_fail "registration is not back before mutex_init/output_lock (reg=$REG_L mutex=$MUT_L)"
grep -q 'fb0 is registered at the very end of probe' "$CORE" \
	&& gate_fail "shim still registers fb0 at the probe tail"
grep -q 'decon_kms_attach(decon)' "$CORE" && gate_fail "vendor handler still calls decon_kms_attach()"
grep -q 'if (!register_fb)' "$CORE" || gate_fail "no register_fb==false skip"
SKIP_L=$(grep -n 'if (!register_fb)' "$CORE" | cut -d: -f1 | head -1)
WIN_L=$(grep -n 'win_regs.vidosd_b = vidosd_b(0, 0, fbinfo->var.xres' "$CORE" | cut -d: -f1 | head -1)
[ -n "$SKIP_L" ] && [ -n "$WIN_L" ] && [ "$SKIP_L" -lt "$WIN_L" ] \
	|| gate_fail "register_fb skip is not before the fbinfo window branch"
grep -q 'struct fb_info \*fbinfo = NULL;' "$CORE" || gate_fail "fbinfo not initialised"
grep -q 'decon->fb_registered = true;' "$CORE" || gate_fail "fb_registered stamp missing"
note "shim: registration in place before output_lock/kthread, gated by register_fb; hand-off branch gone; KMS stand-up jumps past the fbinfo window branch"

note "== int61 gate: built-in KMS owns DECON, vendor probe refuses =="
grep -q 'the DRM/KMS driver owns the DECON node' "$CORE" || gate_fail "vendor refusal marker missing"
grep -q '#ifdef CONFIG_EXYNOS7580_DISPLAY_KMS' "$CORE" || gate_fail "vendor refusal not guarded by the KMS choice"
VPROBE=$(sed -n '/^static int decon_probe(struct platform_device \*pdev)/,/^}/p' "$CORE")
printf '%s' "$VPROBE" | grep -q 'return -ENODEV;' || gate_fail "vendor decon_probe does not refuse"
printf '%s' "$VPROBE" | grep -q 'decon_probe_common(pdev, true);' || gate_fail "vendor probe lost its normal path (compiled-out variant)"
grep -q '^late_initcall(exynos_decon_register);' "$CORE" || gate_fail "vendor DECON not at stock late_initcall"
grep -qE '^late_initcall_sync' "$CORE" && gate_fail "late_initcall_sync leaked into vendor DECON"
KMAIN="$KMS/exynos_drm7580_main.c"
grep -q '^late_initcall_sync(exynos7580_drm_init);' "$KMAIN" || gate_fail "KMS bind is not late_initcall_sync"
grep -q '^late_initcall(exynos7580_drm_init);' "$KMAIN" && gate_fail "KMS bind is still the plain late_initcall"
grep -q -- '-EPROBE_DEFER' "$KMAIN" || gate_fail "KMS probe does not defer"
grep -q 'deferring probe' "$KMAIN" || gate_fail "deferral is not logged"
for s in 'probe: calling decon_kms_get()' 'probe: decon_kms_get ok' \
	 'probe: calling drm_platform_init()' 'probe: success, card0 registered' \
	 'load: fbdev_init() (card0 fb0)' 'load: initial modeset'; do
	grep -qF "$s" "$KMAIN" || gate_fail "probe log missing: $s"
done
RM=$(sed -n '/^static int decon_remove(struct platform_device \*pdev)/,/^}/p' "$CORE")
printf '%s' "$RM" | grep -qE '^\s+decon_int_destroy_psr_thread' && gate_fail "int59 remove teardown still present"
printf '%s' "$RM" | grep -qE '^\s+decon_disable\(decon\);' && gate_fail "remove still calls decon_disable (the int59 hang)"
[ "$(grep -c 'kfree(decon' "$CORE")" = 0 ] || gate_fail "devm decon still kfree()d"
grep -q 'decon and decon->pdata are devm allocations' "$CORE" || gate_fail "probe fail_kfree double-free fix missing"
note "vendor: probe refuses under the KMS choice, stock late_initcall, minimal remove without decon_disable; KMS: built-in late_initcall_sync + -EPROBE_DEFER + step logs"

note "== int61 gate: no core gem mmap/offset in our paths =="
grep -q 'drm_gem_mmap(' "$KMS"/*.c && gate_fail "exynos7580 sources still call core drm_gem_mmap()"
grep -q 'drm_gem_create_mmap_offset' "$KMS"/*.c && gate_fail "exynos7580 sources still call drm_gem_create_mmap_offset()"
grep -q 'drm_gem_cma_dumb_create' "$KMS/exynos_drm7580_main.c" || gate_fail "CMA dumb_create not wired"
grep -q 'exynos7580_gem_prime_import' "$KMS/exynos_drm7580_main.c" || gate_fail "PRIME import not wired"

note "== int61 gate: linked driver symbols =="
VML=vmlinux
for s in exynos7580_atomic_check exynos7580_atomic_commit exynos7580_commit_tail \
	 exynos7580_vblank_irq exynos7580_crtc_init exynos7580_crtc_enable_link \
	 exynos7580_planes_init exynos7580_dsi_init exynos7580_dsi_get_modes \
	 exynos7580_fb_create exynos7580_gem_prime_import exynos7580_fb_scanout \
	 exynos7580_fbdev_init; do
	[ "$(aarch64-linux-gnu-nm "$VML" | grep -c " [tT] $s\$")" = 1 ] \
		|| gate_fail "vmlinux must define $s exactly once"
done
aarch64-linux-gnu-nm "$VML" | grep -q ' __initcall_exynos7580_drm_init7s$' \
	|| gate_fail "late_initcall_sync bind symbol (…init7s) missing from vmlinux"
aarch64-linux-gnu-nm "$VML" | grep -q ' __initcall_exynos7580_drm_init7$' \
	&& gate_fail "KMS still has a plain late_initcall bind symbol"
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

note "== int61 gate: built-in KMS in vmlinux, no modules =="
for s in exynos7580_atomic_check exynos7580_atomic_commit exynos7580_commit_tail \
	 exynos7580_vblank_irq exynos7580_crtc_init exynos7580_crtc_enable_link \
	 exynos7580_planes_init exynos7580_dsi_init exynos7580_dsi_get_modes \
	 exynos7580_fb_create exynos7580_gem_prime_import exynos7580_fb_scanout \
	 exynos7580_fbdev_init; do
	[ "$(aarch64-linux-gnu-nm vmlinux | grep -c " [tT] $s\$")" = 1 ] || gate_fail "$s missing from vmlinux"
done
aarch64-linux-gnu-nm vmlinux | grep -q ' __initcall_exynos7580_drm_init7s$' || gate_fail "KMS late_initcall_sync symbol missing"
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] decon_kms_get$' || gate_fail "decon_kms_get missing from vmlinux"
[ "$(find drivers -name '*.ko' | wc -l)" = 0 ] || gate_fail "unexpected .ko built"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'card0 ready (atomic)' || gate_fail "KMS bind string missing from Image"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'DECON handed to KMS' || gate_fail "KMS hand-off string missing from Image"
for s in 'probe: calling decon_kms_get()' 'probe: success, card0 registered' \
	 'load: fbdev_init() (card0 fb0)' 'load: initial modeset' \
	 'DECON/DSIM not ready, deferring probe'; do
	aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF "$s" \
		|| gate_fail "Image lost probe log: $s"
done
grep -q '^CONFIG_LOG_BUF_SHIFT=18$' .config || gate_fail "log buffer is not 256 KB"
note "KMS linked built-in (13 symbols + initcall + decon_kms_get), zero .ko, bind strings in Image"

note "== int61 gate: pstore capture available =="
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] ess_pstore_init$' || gate_fail "ess_pstore_init not linked"
grep -q '^CONFIG_PSTORE_CONSOLE=y' .config || gate_fail "pstore console off"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'log_pstore' || gate_fail "ess ramoops region name missing"
note "pstore: ess_pstore_init + PSTORE_CONSOLE + log_pstore region; no DT edit"

note "== int61 gate: KMS objects clean =="
[ -f drivers/gpu/drm/exynos7580/built-in.o ] || gate_fail "exynos7580/built-in.o missing"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "vmlinux predates this build (stale prebuilt vmlinux)"
W=$(grep -E 'warning:' "$LOG" | grep -c 'drivers/gpu/drm/exynos7580/')
[ "$W" = 0 ] || gate_fail "exynos7580 sources produced $W warnings"
grep -E 'CC +drivers/gpu/drm/exynos7580/' "$LOG" | tee -a "$EVID"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/exynos7580" 2>&1 | tee -a "$EVID"

STAGE=${STAGE:-$EXT/stage-int61}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
# Single line: the kernel splits its command line on spaces only, a newline
# inside the stored string would corrupt the preceding token.
CL=$(tr '\n' ' ' < "$STAGE/cmdline" | sed 's/  */ /g; s/ $//')
case " $CL " in
*" ignore_loglevel "*) ;;
*) CL="$CL ignore_loglevel" ;;
esac
printf '%s' "$CL" > "$STAGE/cmdline"
grep -q 'ignore_loglevel' "$STAGE/cmdline" || gate_fail "ignore_loglevel not in the kernel command line"
[ "$(wc -l < "$STAGE/cmdline")" = 0 ] || gate_fail "kernel command line is not a single line"
note "cmdline: $(cat "$STAGE/cmdline")"
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int61.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int61.img" | tee -a "$EVID"
sha256sum "$EXT/kernel-boot-int61.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int61"
sha256sum "$EXT/Image-int61" | tee -a "$EVID"

note "== int61 gate: PASS (built-in KMS at late_initcall_sync with -EPROBE_DEFER + step logs, 21 patches, vendor probe refuses under the KMS choice, vendor at stock late_initcall, minimal remove, LOG_BUF_SHIFT=18 + ignore_loglevel, pstore console, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
