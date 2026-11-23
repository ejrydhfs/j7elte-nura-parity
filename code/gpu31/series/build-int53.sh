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
# int53: same 18-patch series plus 0102-kms-module.patch; the KMS driver is
# built as a module (CONFIG_DRM_EXYNOS7580=m) while the vendor DECON fbdev
# stack stays built in and owns the display at boot.  The module is never
# auto-loaded; the device test unbinds the vendor driver and insmods the .ko.
# Config: config-samsung-j7elte + patches/drm-kms-coexist/display-owner-kms.fragment
# (the fragment's "# CONFIG_X is not set" lines are applied as removals, unlike
# plain non-comment fragments). Stock dt.img, reference ramdisk, Image-int53.
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int53}
LOG=${LOG:-$EXT/logs/int53-kms-build.log}
EVID=${EVID:-$EXT/logs/int53-kms-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int53 GATE FAIL: $1"; exit 1; }

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
# int53: the shipping config has CONFIG_MODULES unset, so no tristate can be
# =m.  The module build needs the loader and unload support (rmmod); this is
# the only config delta against int52 besides the KMS symbol itself.
grep -v '^CONFIG_MODULES=\|^CONFIG_MODULE_UNLOAD=\|^# CONFIG_MODULES is not set\|^# CONFIG_MODULE_UNLOAD is not set' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_MODULES=y\nCONFIG_MODULE_UNLOAD=y\n' >> .config
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
# int53: the KMS fragment pins CONFIG_DRM_EXYNOS7580=y; this is the module
# build, so override it before olddefconfig resolves the tree.
# int53: pin the values the module build needs after the fragment.  The KMS
# helper library stays built in (=y) so the only loadable object is
# exynos-drm7580.ko; a =m helper would add a second insmod to the test.
force_cfg() {
	grep -v "^$1=" .config > .config.tmp && mv .config.tmp .config
	printf '%s=%s\n' "$1" "$2" >> .config
}
force_cfg CONFIG_DRM_EXYNOS7580 m
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int53-kms-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed (see int53-kms-config.log)"
grep -qE 'recursive dependency|error:' "$EXT/logs/int53-kms-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

CONFIG_EXPECT="
CONFIG_EXYNOS7580_DISPLAY_KMS=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=m
CONFIG_DRM_KMS_CMA_HELPER=y
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y
CONFIG_DRM_EXYNOS7580=m
CONFIG_CMA_SIZE_MBYTES=32
CONFIG_EXYNOS_IOVMM_V6=y
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_EXYNOS_MIPI_DSI=y
CONFIG_EXYNOS_DECON_LCD=y
CONFIG_PANEL_EA8061_DYNAMIC=y
# CONFIG_DRM_EXYNOS is not set
# CONFIG_EXYNOS7580_DISPLAY_FBDEV is not set
# CONFIG_FRAMEBUFFER_CONSOLE is not set"
MISSF=$EXT/logs/int53-kms-config-missing.log
: > "$MISSF"
printf '%s\n' "$CONFIG_EXPECT" | while IFS= read -r line; do
	[ -n "$line" ] || continue
	grep -qxF "$line" .config || printf 'missing: %s\n' "$line" >> "$MISSF"
done
if [ -s "$MISSF" ]; then
	cat "$MISSF" | tee -a "$EVID"
	gate_fail "config closure failed (missing expectations in $MISSF)"
fi
note "config closure: 14 fragment expectations exact (KMS choice, KMS driver =m, DRM_EXYNOS off, fbcon off, CMA 32, IOVMM v6, vendor display on)"
grep -E 'CONFIG_(EXYNOS7580_DISPLAY_KMS|DRM_EXYNOS7580|DRM_KMS_CMA_HELPER|CMA_SIZE_MBYTES|EXYNOS_IOVMM_V6)=' .config | tee -a "$EVID"

# ---------------------------------------------------------------- build
BUILD_START=$(date +%s)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image modules > "$LOG" 2>&1
BUILD_RC=$?
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ "$BUILD_RC" = 0 ] || gate_fail "make -j4 Image failed (rc=$BUILD_RC; see $LOG)"
[ "$(stat -c %Y arch/arm64/boot/Image)" -ge "$BUILD_START" ] \
	|| gate_fail "Image predates this build (stale artifact from the pristine copy)"
note "build rc=0, Image mtime $(stat -c %y arch/arm64/boot/Image)"
[ "$(grep -c 'error:' "$LOG")" = 0 ] || gate_fail "build log contains errors"
note "build log: $(wc -l < "$LOG") lines, $(grep -c 'warning:' "$LOG") warnings"

# ---------------------------------------------------------------- gates
note "== int53 gate: layout and single wiring point =="
KMS=drivers/gpu/drm/exynos7580
for f in Kconfig Makefile exynos_drm7580.h exynos_drm7580_main.c exynos_drm7580_crtc.c \
	 exynos_drm7580_plane.c exynos_drm7580_dsi.c exynos_drm7580_dmabuf.c \
	 exynos_drm7580_fence.c exynos_drm7580_fbdev.c; do
	[ -f "$KMS/$f" ] || gate_fail "$KMS/$f missing"
done
[ "$(ls "$KMS"/*.c | grep -vc '\.mod\.c$')" = 7 ] || gate_fail "expected exactly 7 driver sources in $KMS (excluding generated .mod.c)"
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

note "== int53 gate: atomic core + driver features =="
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
grep -q 'module_init(exynos7580_drm_init);' "$KMS/exynos_drm7580_main.c" \
	|| gate_fail "module_init missing"
grep -q 'module_exit(exynos7580_drm_exit);' "$KMS/exynos_drm7580_main.c" \
	|| gate_fail "module_exit missing (rmmod could not unregister)"
grep -q 'late_initcall(exynos7580_drm_init);' "$KMS/exynos_drm7580_main.c" \
	|| gate_fail "built-in late_initcall path lost"
grep -q '#ifdef MODULE' "$KMS/exynos_drm7580_main.c" || gate_fail "MODULE guard missing"
grep -q 'samsung,exynos5-decon_driver' "$KMS/exynos_drm7580_main.c" || gate_fail "DECON compatible missing"
grep -q 'drm_platform_init' "$KMS/exynos_drm7580_main.c" || gate_fail "3.10 drm_platform_init missing"
grep -q 'exynos7580_atomic_commit' "$KMS/exynos_drm7580_main.c" || gate_fail "mode_config atomic_commit missing"
grep -q 'DRIVER_RENDER' "$KMS/exynos_drm7580_main.c" && gate_fail "DRIVER_RENDER must not be set (card0)"
grep -q 'exynos7580_fbdev_init' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev slice init missing"
grep -q 'drm_fbdev_cma_restore_mode' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "initial modeset (restore_mode) missing"

note "== int53 gate: fixed shim ordering =="
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

note "== int53 gate: unbind leaves the DECON re-bindable =="
grep -q 'decon_unregister_entity(decon);' "$CORE" || gate_fail "vendor decon_remove() does not unregister the media entity"
grep -q 'platform_set_drvdata(pdev, NULL);' "$CORE" || gate_fail "vendor decon_remove() leaves a dangling platform drvdata"
grep -q 'if (decon_int_drvdata == decon)' "$CORE" || gate_fail "vendor decon_remove() leaves the exported decon_int_drvdata dangling"
grep -q 'EXPORT_SYMBOL_GPL(iovmm_map);' drivers/iommu/exynos-iovmm-v6.c \
	|| gate_fail "iovmm_map is not exported (module load would fail)"
grep -q 'EXPORT_SYMBOL_GPL(iovmm_unmap);' drivers/iommu/exynos-iovmm-v6.c \
	|| gate_fail "iovmm_unmap is not exported (module load would fail)"
grep -q '^#ifdef MODULE' "$KMS/exynos_drm7580_main.c" || gate_fail "module_init guard missing"
note "unbind/rmmod plumbing: remove clears drvdata + decon_int_drvdata and unregisters the subdev; iovmm_map/unmap exported GPL"

note "== int53 gate: no core gem mmap/offset in our paths =="
grep -q 'drm_gem_mmap(' "$KMS"/*.c && gate_fail "exynos7580 sources still call core drm_gem_mmap()"
grep -q 'drm_gem_create_mmap_offset' "$KMS"/*.c && gate_fail "exynos7580 sources still call drm_gem_create_mmap_offset()"
grep -q 'drm_gem_cma_dumb_create' "$KMS/exynos_drm7580_main.c" || gate_fail "CMA dumb_create not wired"
grep -q 'exynos7580_gem_prime_import' "$KMS/exynos_drm7580_main.c" || gate_fail "PRIME import not wired"

note "== int53 gate: module built, symbols, metadata =="
KO=drivers/gpu/drm/exynos7580/exynos-drm7580.ko
[ -f "$KO" ] || gate_fail "$KO missing"
grep -qE 'LD \[M\] +drivers/gpu/drm/exynos7580/exynos-drm7580\.ko' "$LOG" || gate_fail "build did not link the module (LD [M] line missing)"
for s in exynos7580_atomic_check exynos7580_atomic_commit exynos7580_commit_tail \
	 exynos7580_vblank_irq exynos7580_crtc_init exynos7580_crtc_enable_link \
	 exynos7580_planes_init exynos7580_dsi_init exynos7580_dsi_get_modes \
	 exynos7580_fb_create exynos7580_gem_prime_import exynos7580_fb_scanout \
	 exynos7580_fbdev_init; do
	[ "$(aarch64-linux-gnu-nm "$KO" | grep -c " [tT] $s\$")" = 1 ] \
		|| gate_fail "$s is not defined exactly once in $KO"
done
[ "$(aarch64-linux-gnu-nm "$KO" | grep -c ' [tT] exynos_drm_driver$')" = 0 ] \
	|| gate_fail "in-tree exynos DRM driver is in the module (DRM_EXYNOS must be off)"
# the built-in kernel must not contain the KMS driver at all
aarch64-linux-gnu-nm vmlinux | grep -qE ' [tT] exynos7580_(atomic_commit|atomic_check)$' \
	&& gate_fail "KMS symbols leaked into vmlinux (driver not modular)"
aarch64-linux-gnu-nm vmlinux | grep -q '__initcall_exynos7580_drm_init' \
	&& gate_fail "built-in KMS initcall present in vmlinux"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'card0 ready (atomic)' \
	&& gate_fail "KMS bind string still in Image (driver not modular)"
aarch64-linux-gnu-strings "$KO" | grep -qF 'card0 ready (atomic)' \
	|| gate_fail "$KO lost the bind string"
aarch64-linux-gnu-strings "$KO" | grep -qF 'samsung,exynos5-decon_driver' \
	|| gate_fail "$KO lost the DECON compatible"
MODI=$(command -v modinfo || echo aarch64-linux-gnu-modinfo)
"$MODI" "$KO" | tee -a "$EVID"
"$MODI" "$KO" | grep -q '^license:.*GPL' || gate_fail "module license is not GPL (GPL-only exports would fail to load)"
"$MODI" "$KO" | grep -q 'exynos-drm7580\.ko$' || gate_fail "module filename unexpected"
"$MODI" "$KO" | grep -q '^vermagic:.*aarch64' || gate_fail "vermagic is not aarch64"
grep -q 'depends:' "$EVID" || true
aarch64-linux-gnu-nm "$KO" | grep -E ' (T|D) exynos7580_' | tee -a "$EVID"
sha256sum "$KO" | tee -a "$EVID"

note "== int53 gate: module chain and every imported symbol =="
# The KMS driver selects DRM_KMS_HELPER; with CONFIG_MODULES=y that hidden
# tristate resolves to m, so the loadable set is exactly these two objects.
KO_HELPER=drivers/gpu/drm/drm_kms_helper.ko
[ -f "$KO_HELPER" ] || gate_fail "$KO_HELPER missing (DRM_KMS_HELPER did not build as a module)"
[ "$(grep -c '=m$' .config | cat)" -ge 2 ] || gate_fail "expected at least two =m symbols"
MODS=$(find drivers -name '*.ko' | sort)
echo "$MODS" > "$EXT/logs/int53-modules.txt"
echo "$MODS" | tee -a "$EVID"
[ "$(echo "$MODS" | wc -l)" = 2 ] || gate_fail "loadable set is not exactly the KMS driver + DRM_KMS_HELPER: $(echo $MODS)"
"$MODI" "$KO" | grep -q '^depends:.*drm_kms_helper' || gate_fail "module does not declare its drm_kms_helper dependency"
"$MODI" "$KO_HELPER" | grep -q 'drm_kms_helper\.ko$' || gate_fail "helper module filename unexpected"
sha256sum "$KO_HELPER" | tee -a "$EVID"
UND=$(aarch64-linux-gnu-nm -u "$KO" | awk '{print $NF}' | sort -u)
echo "$UND" > "$EXT/logs/int53-imports.txt"
UNRES=""
while read -r sym; do
	[ -n "$sym" ] || continue
	case "$sym" in __this_module|_GLOBAL_OFFSET_TABLE_) continue ;; esac
	grep -q "	$sym	" Module.symvers || UNRES="$UNRES $sym"
done <<EOF2
$UND
EOF2
if [ -n "$UNRES" ]; then
	note "unresolved imports:$UNRES"
	gate_fail "module imports symbols nothing in the build exports:$UNRES"
fi
UNDH=$(aarch64-linux-gnu-nm -u "$KO_HELPER" | awk '{print $NF}' | sort -u)
echo "$UNDH" > "$EXT/logs/int53-imports-helper.txt"
UNRESH=""
while read -r sym; do
	[ -n "$sym" ] || continue
	case "$sym" in __this_module|_GLOBAL_OFFSET_TABLE_) continue ;; esac
	grep -q "	$sym	" Module.symvers || UNRESH="$UNRESH $sym"
done <<EOF2
$UNDH
EOF2
[ -z "$UNRESH" ] || gate_fail "drm_kms_helper imports unresolved:$UNRESH"
grep -q 'undefined!' "$LOG" && gate_fail "modpost reported undefined symbols (see $LOG)"
grep -q 'WARNING: modpost' "$LOG" && gate_fail "modpost warnings present (see $LOG)"
note "imports: KMS $(echo "$UND" | grep -c .) symbols (logs/int53-imports.txt), helper $(echo "$UNDH" | grep -c .) (logs/int53-imports-helper.txt); all resolved in Module.symvers, modpost clean"
grep -E '\b(decon_kms_|decon_export_|decon_int_drvdata|dsim[01]_for_decon)' "$EXT/logs/int53-imports.txt" | tee -a "$EVID"
# Built-in kernel side of the contract: the shim symbols the module calls are
# really exported GPL from vmlinux.
for s in decon_kms_get decon_kms_put decon_kms_enable decon_kms_disable 	 decon_kms_commit decon_kms_commit_async decon_kms_caps decon_kms_map_dmabuf 	 decon_kms_unmap_dmabuf; do
	grep -q "	$s	" Module.symvers || gate_fail "$s not exported by the built-in tree"
done
aarch64-linux-gnu-nm vmlinux | grep -q ' __ksymtab_decon_kms_get$' || gate_fail "decon_kms_get is not in vmlinux ksymtab"

note "== int53 gate: KMS objects clean =="
[ -f "$KO" ] || gate_fail "exynos7580 module missing"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "vmlinux predates this build (stale prebuilt vmlinux)"
W=$(grep -E 'warning:' "$LOG" | grep -c 'drivers/gpu/drm/exynos7580/')
[ "$W" = 0 ] || gate_fail "exynos7580 sources produced $W warnings"
grep -E 'CC +drivers/gpu/drm/exynos7580/' "$LOG" | tee -a "$EVID"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/exynos7580" 2>&1 | tee -a "$EVID"

STAGE=${STAGE:-$EXT/stage-int53}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int53.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int53.img" | tee -a "$EVID"
sha256sum "$EXT/kernel-boot-int53.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int53"
sha256sum "$EXT/Image-int53" | tee -a "$EVID"
# the loadable set is shipped next to the boot image (no modules dir on the
# device; the test insmods these two files directly).
cp drivers/gpu/drm/drm_kms_helper.ko "$EXT/drm_kms_helper-int53.ko"
cp drivers/gpu/drm/exynos7580/exynos-drm7580.ko "$EXT/exynos-drm7580-int53.ko"
sha256sum "$EXT/drm_kms_helper-int53.ko" "$EXT/exynos-drm7580-int53.ko" | tee -a "$EVID"
ls -l "$EXT/drm_kms_helper-int53.ko" "$EXT/exynos-drm7580-int53.ko" | tee -a "$EVID"

note "== int53 module gate: PASS (18-patch series + 0102 module patch, DRM_EXYNOS7580=m, vendor fbdev built in, KMS symbols only in exynos-drm7580.ko, zero unresolved imports against vmlinux ksymtab, GPL metadata, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
