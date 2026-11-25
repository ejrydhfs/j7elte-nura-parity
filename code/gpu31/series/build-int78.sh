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
# plain non-comment fragments). Stock dt.img, reference ramdisk, Image-int78.
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int78}
LOG=${LOG:-$EXT/logs/int78-kms-build.log}
EVID=${EVID:-$EXT/logs/int78-kms-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int78 GATE FAIL: $1"; exit 1; }

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
addp "$REPO"/patches/drm-kms/0105-kms-probe-drvdata-order.patch
addp "$REPO"/patches/drm-kms/0106-kms-module-no-ehframe.patch
addp "$REPO"/patches/drm-kms/0107-kms-decon-power-before-regs.patch
addp "$REPO"/patches/drm-kms/0108-kms-clocks-on-before-regs.patch
addp "$REPO"/patches/drm-kms/0109-kms-dsi-encoder-mode-set.patch
addp "$REPO"/patches/drm-kms/0110-kms-atomic-helper-guard-optional-ops.patch
addp "$REPO"/patches/drm-kms/0111-kms-fbdev-lut-and-destroy-hooks.patch
addp "$REPO"/patches/drm-kms/0112-kms-frame-irq-rearm-and-irq-diag.patch
addp "$REPO"/patches/drm-kms/0113-kms-fbdev-scanout-state-log.patch
addp "$REPO"/patches/drm-kms/0114-kms-bounded-vsync-and-frame-arm.patch
addp "$REPO"/patches/drm-kms/0115-kms-force-arm-frame-irq.patch
addp "$REPO"/patches/drm-kms/0116-kms-intcon-trace-and-teardown.patch
addp "$REPO"/patches/drm-kms/0117-kms-fb-postcondition-and-pan-guard.patch
addp "$REPO"/patches/drm-kms/0118-kms-vsync-thread-bounded-stop.patch
addp "$REPO"/patches/drm-kms/0119-kms-fbdev-postcondition-pr-err.patch
addp "$REPO"/patches/drm-kms/0120-kms-initial-modeset-atomic-state.patch
addp "$REPO"/patches/drm-kms/0121-kms-modeset-lock-all-simplify.patch
addp "$REPO"/patches/drm-kms/0122-kms-fbdev-restore-lock-log.patch
addp "$REPO"/patches/drm-kms/0123-kms-cma-mmap-trace.patch
addp "$REPO"/patches/drm-kms/0124-kms-prime-import-trace.patch
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
# int78: module build.  The shipping config has CONFIG_MODULES unset; the
# fragment pins DRM_EXYNOS7580=y, so both need overriding after the fragments.
grep -v '^CONFIG_MODULES=\|^CONFIG_MODULE_UNLOAD=\|^# CONFIG_MODULES is not set\|^# CONFIG_MODULE_UNLOAD is not set' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_MODULES=y\nCONFIG_MODULE_UNLOAD=y\n' >> .config
grep -v '^CONFIG_DRM_EXYNOS7580=' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_DRM_EXYNOS7580=m\n' >> .config
# int78: keep the boot log (256 KB) for dmesg/pstore.
sed -i 's/^CONFIG_LOG_BUF_SHIFT=.*/CONFIG_LOG_BUF_SHIFT=18/' .config
# int78: lockup hardening - a deadlock must panic and auto-reboot into the
# headless SSH state instead of hanging until a battery pull.
grep -v '^CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=\|^CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=\|^# CONFIG_BOOTPARAM_HARDLOCKUP_PANIC is not set\|^# CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC is not set' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=y\nCONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y\n' >> .config
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int78-kms-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed (see int78-kms-config.log)"
grep -qE 'recursive dependency|error:' "$EXT/logs/int78-kms-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

CONFIG_EXPECT="
CONFIG_EXYNOS7580_DISPLAY_KMS=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=m
CONFIG_DRM_KMS_CMA_HELPER=y
CONFIG_DRM_EXYNOS7580=m
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
CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=y
CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y
CONFIG_EXYNOS_SNAPSHOT_PSTORE=y
CONFIG_MODULES=y
CONFIG_MODULE_UNLOAD=y"
MISSF=$EXT/logs/int78-kms-config-missing.log
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
note "== int78 gate: layout and single wiring point =="
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

note "== int78 gate: atomic core + driver features =="
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
	|| gate_fail "module_exit missing"
grep -q '#ifdef MODULE' "$KMS/exynos_drm7580_main.c" || gate_fail "MODULE guard missing"
grep -q 'samsung,exynos5-decon_driver' "$KMS/exynos_drm7580_main.c" || gate_fail "DECON compatible missing"
grep -q 'drm_platform_init' "$KMS/exynos_drm7580_main.c" || gate_fail "3.10 drm_platform_init missing"
grep -q 'exynos7580_atomic_commit' "$KMS/exynos_drm7580_main.c" || gate_fail "mode_config atomic_commit missing"
grep -q 'DRIVER_RENDER' "$KMS/exynos_drm7580_main.c" && gate_fail "DRIVER_RENDER must not be set (card0)"
grep -q 'exynos7580_fbdev_init' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev slice init missing"
grep -q 'drm_fbdev_cma_restore_mode' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "initial modeset (restore_mode) missing"

note "== int78 gate: fixed shim ordering =="
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

note "== int78 gate: built-in KMS owns DECON, vendor probe refuses =="
grep -q 'the DRM/KMS driver owns the DECON node' "$CORE" || gate_fail "vendor refusal marker missing"
grep -q '#ifdef CONFIG_EXYNOS7580_DISPLAY_KMS' "$CORE" || gate_fail "vendor refusal not guarded by the KMS choice"
VPROBE=$(sed -n '/^static int decon_probe(struct platform_device \*pdev)/,/^}/p' "$CORE")
printf '%s' "$VPROBE" | grep -q 'return -ENODEV;' || gate_fail "vendor decon_probe does not refuse"
printf '%s' "$VPROBE" | grep -q 'decon_probe_common(pdev, true);' || gate_fail "vendor probe lost its normal path (compiled-out variant)"
# object-level proof that the guard is active in the compiled kernel
aarch64-linux-gnu-objdump -d vmlinux --disassemble=decon_probe \
	| grep -q 'mov.*#0xffffffed' || gate_fail "compiled decon_probe is not the early -ENODEV return"
grep -q '^late_initcall(exynos_decon_register);' "$CORE" || gate_fail "vendor DECON not at stock late_initcall"
grep -qE '^late_initcall_sync' "$CORE" && gate_fail "late_initcall_sync leaked into vendor DECON"
KMAIN="$KMS/exynos_drm7580_main.c"
grep -q 'module_init(exynos7580_drm_init);' "$KMAIN" || gate_fail "module path has no module_init"
grep -q 'module_exit(exynos7580_drm_exit);' "$KMAIN" || gate_fail "module path has no module_exit"
# the late_initcall_sync line may only exist inside the built-in #else branch
awk '/^#ifdef MODULE/{m=1} /^#else/{if(m){b=1}} /^#endif/{if(b){e=1}} b&&/^late_initcall_sync\(exynos7580_drm_init\)/{found=1} END{exit !found}' "$KMAIN" \
	|| gate_fail "built-in sync branch lost or sync moved outside #else"
grep -q -- '-EPROBE_DEFER' "$KMAIN" || gate_fail "KMS probe does not defer"
grep -q 'deferring probe' "$KMAIN" || gate_fail "deferral is not logged"
for s in 'probe: calling decon_kms_get()' 'probe: decon_kms_get ok' \
	 'probe: calling drm_platform_init()' 'probe: success, card0 registered' \
	 'load: fbdev_init() (card0 fb0)' 'load: initial modeset' \
	 'probe: decon ok, drvdata published'; do
	grep -qF "$s" "$KMAIN" || gate_fail "probe log missing: $s"
done
# regression guard for the int61 bug: drvdata must be published only after
# decon_kms_get() (which expects the vendor decon_device or NULL).
PGET_L=$(grep -n 'ex->decon = decon_kms_get(dev);' "$KMAIN" | cut -d: -f1 | head -1)
PSET_L=$(grep -n 'platform_set_drvdata(pdev, ex);' "$KMAIN" | cut -d: -f1 | head -1)
[ -n "$PGET_L" ] && [ -n "$PSET_L" ] && [ "$PGET_L" -lt "$PSET_L" ] \
	|| gate_fail "probe publishes drvdata before decon_kms_get (get=$PGET_L set=$PSET_L)"
[ "$(grep -c 'platform_set_drvdata(pdev, ex);' "$KMAIN")" = 1 ] \
	|| gate_fail "probe must publish its drvdata exactly once"
RM=$(sed -n '/^static int decon_remove(struct platform_device \*pdev)/,/^}/p' "$CORE")
printf '%s' "$RM" | grep -qE '^\s+decon_int_destroy_psr_thread' && gate_fail "int59 remove teardown still present"
printf '%s' "$RM" | grep -qE '^\s+decon_disable\(decon\);' && gate_fail "remove still calls decon_disable (the int59 hang)"
[ "$(grep -c 'kfree(decon' "$CORE")" = 0 ] || gate_fail "devm decon still kfree()d"
grep -q 'decon and decon->pdata are devm allocations' "$CORE" || gate_fail "probe fail_kfree double-free fix missing"
note "vendor: probe refuses under the KMS choice, stock late_initcall, minimal remove without decon_disable; KMS: built-in late_initcall_sync + -EPROBE_DEFER + step logs"

note "== int78 gate: no core gem mmap/offset in our paths =="
grep -q 'drm_gem_mmap(' "$KMS"/*.c && gate_fail "exynos7580 sources still call core drm_gem_mmap()"
grep -q 'drm_gem_create_mmap_offset' "$KMS"/*.c && gate_fail "exynos7580 sources still call drm_gem_create_mmap_offset()"
grep -q 'drm_gem_cma_dumb_create' "$KMS/exynos_drm7580_main.c" || gate_fail "CMA dumb_create not wired"
grep -q 'exynos7580_gem_prime_import' "$KMS/exynos_drm7580_main.c" || gate_fail "PRIME import not wired"

note "== int78 gate: module built, symbols, metadata =="
KO=drivers/gpu/drm/exynos7580/exynos-drm7580.ko
KO_HELPER=drivers/gpu/drm/drm_kms_helper.ko
[ -f "$KO" ] || gate_fail "$KO missing"
[ -f "$KO_HELPER" ] || gate_fail "$KO_HELPER missing"
grep -qE 'LD \[M\] +drivers/gpu/drm/exynos7580/exynos-drm7580\.ko' "$LOG" \
	|| gate_fail "build did not link exynos-drm7580.ko"
for sym in exynos7580_atomic_check exynos7580_atomic_commit exynos7580_commit_tail \
	 exynos7580_vblank_irq exynos7580_crtc_init exynos7580_crtc_enable_link \
	 exynos7580_planes_init exynos7580_dsi_init exynos7580_dsi_get_modes \
	 exynos7580_fb_create exynos7580_gem_prime_import exynos7580_fb_scanout \
	 exynos7580_fbdev_init; do
	[ "$(aarch64-linux-gnu-nm "$KO" | grep -c " [tT] $sym\$")" = 1 ] \
		|| gate_fail "$sym is not defined exactly once in the module"
done
aarch64-linux-gnu-nm vmlinux | grep -qE ' [tT] exynos7580_(atomic_commit|atomic_check)$' \
	&& gate_fail "KMS symbols leaked into vmlinux (driver not modular)"
aarch64-linux-gnu-nm vmlinux | grep -q '__initcall_exynos7580_drm_init' \
	&& gate_fail "KMS initcall present in vmlinux"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] exynos_drm_driver$')" = 0 ] \
	|| gate_fail "in-tree exynos DRM driver is linked (DRM_EXYNOS must be off)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'card0 ready (atomic)' \
	&& gate_fail "KMS bind string still in Image (driver not modular)"
for sym in 'card0 ready (atomic)' 'samsung,exynos5-decon_driver' \
	 'probe: calling decon_kms_get()' 'probe: decon_kms_get ok' \
	 'probe: decon ok, drvdata published' \
	 'probe: calling drm_platform_init()' 'probe: success, card0 registered' \
	 'load: dsi_init()' 'load: register' 'load: fbdev_init() (card0 fb0)' \
	 'load: initial modeset' 'DECON/DSIM not ready, deferring probe'; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$sym" \
		|| gate_fail "module lost string: $sym"
done
MODS=$(find drivers -name '*.ko' | sort)
echo "$MODS" > "$EXT/logs/int78-modules.txt"
[ "$(echo "$MODS" | wc -l)" = 2 ] || gate_fail "loadable set is not exactly 2 modules: $(echo $MODS)"
MODI=$(command -v modinfo || echo aarch64-linux-gnu-modinfo)
"$MODI" "$KO" | tee -a "$EVID"
"$MODI" "$KO" | grep -q '^license:.*GPL' || gate_fail "module license is not GPL"
"$MODI" "$KO" | grep -q 'exynos-drm7580\.ko$' || gate_fail "module filename unexpected"
"$MODI" "$KO" | grep -q '^depends:.*drm_kms_helper' || gate_fail "helper dependency missing"
"$MODI" "$KO" | grep -q '^vermagic:.*aarch64' || gate_fail "vermagic is not aarch64"
aarch64-linux-gnu-nm "$KO" | grep -E ' (T|D) exynos7580_' | tee -a "$EVID"

note "== int78 gate: every imported symbol resolves =="
UND=$(aarch64-linux-gnu-nm -u "$KO" | awk '{print $NF}' | sort -u)
echo "$UND" > "$EXT/logs/int78-imports.txt"
UNRES=""
while read -r sym; do
	[ -n "$sym" ] || continue
	case "$sym" in __this_module|_GLOBAL_OFFSET_TABLE_) continue ;; esac
	grep -q "	$sym	" Module.symvers || UNRES="$UNRES $sym"
done <<EOF2
$UND
EOF2
[ -z "$UNRES" ] || gate_fail "module imports symbols nothing exports:$UNRES"
UNDH=$(aarch64-linux-gnu-nm -u "$KO_HELPER" | awk '{print $NF}' | sort -u)
echo "$UNDH" > "$EXT/logs/int78-imports-helper.txt"
UNRESH=""
while read -r sym; do
	[ -n "$sym" ] || continue
	case "$sym" in __this_module|_GLOBAL_OFFSET_TABLE_) continue ;; esac
	grep -q "	$sym	" Module.symvers || UNRESH="$UNRESH $sym"
done <<EOF2
$UNDH
EOF2
[ -z "$UNRESH" ] || gate_fail "helper module imports unresolved:$UNRESH"
grep -q 'undefined!' "$LOG" && gate_fail "modpost reported undefined symbols"
grep -q 'WARNING: modpost' "$LOG" && gate_fail "modpost warnings present"
for sym in decon_kms_get decon_kms_put decon_kms_enable decon_kms_disable \
	 decon_kms_commit decon_kms_commit_async decon_kms_caps decon_kms_map_dmabuf \
	 decon_kms_unmap_dmabuf iovmm_map iovmm_unmap; do
	grep -q "	$sym	" Module.symvers || gate_fail "$sym not exported by the built-in tree"
done
aarch64-linux-gnu-nm vmlinux | grep -q ' __ksymtab_decon_kms_get$' || gate_fail "decon_kms_get not in vmlinux ksymtab"
note "imports: KMS $(echo "$UND" | grep -c .) symbols, helper $(echo "$UNDH" | grep -c .); all in Module.symvers, modpost clean"

note "== int78 gate: loadable set has no out-of-range unwind relocations =="
# Supported by arch/arm64/kernel/module.c (3.10): 256..260,262, 263..269,
# 270..272, 273..286, 299.  261 (PREL32) is the failing one; readelf truncates
# long type names, so decode the type from r_info (low 32 bits, last 3 hex).
ALLOWED_NUM="256 257 258 259 260 262 263 264 265 266 267 268 269 270 271 272 273 274 275 276 277 278 279 280 282 283 284 285 286 299"
for k in "$KO" "$KO_HELPER"; do
	n=$(aarch64-linux-gnu-readelf -r "$k" | grep -c 'R_AARCH64_PREL32')
	[ "$n" = 0 ] || gate_fail "$(basename "$k") still has $n R_AARCH64_PREL32 relocations (int78 regression)"
	hx=$(aarch64-linux-gnu-readelf -r "$k" \
		| awk '$2 ~ /^[0-9a-f]+$/ && $3 ~ /^R_(AARCH64|ARM)_/ {print substr($2,length($2)-2)}' \
		| sort -u)
	[ -n "$hx" ] || gate_fail "$(basename "$k"): could not read relocation types"
	types=""
	for h in $hx; do
		t=$((16#$h))
		case " $ALLOWED_NUM " in
		*" $t "*) ;;
		*) gate_fail "$(basename "$k"): relocation type $t is not handled by the 3.10 arm64 loader" ;;
		esac
		types="$types $t"
	done
	aarch64-linux-gnu-readelf -S "$k" | grep -q '\.rela\.eh_frame' \
		&& gate_fail "$(basename "$k") still has .rela.eh_frame"
	note "$(basename "$k"): relocation types$types"
done
note "no PREL32/.rela.eh_frame in the loadable set; all remaining relocation types are in the loader's supported list"

note "== int78 gate: DSI encoder mode_set hook =="
DSI="$KMS/exynos_drm7580_dsi.c"
grep -q 'exynos7580_dsi_encoder_mode_set' "$DSI" || gate_fail "encoder mode_set implementation missing"
grep -q '\.mode_set' "$DSI" || gate_fail "encoder .mode_set not wired"
aarch64-linux-gnu-strings "$KO" | grep -qF 'DSI encoder: mode_set' || gate_fail "mode_set log missing from the module"
# module-only change: the patch must not touch any built-in source
BAD=$(awk '/^--- a\//{print substr($0,7)}' "$REPO/patches/drm-kms/0109-kms-dsi-encoder-mode-set.patch" \
	| grep -v '^drivers/gpu/drm/exynos7580/' | head -1)
[ -z "$BAD" ] || gate_fail "0109 touches a built-in file: $BAD"
note "encoder: mode_set no-op wired, log in the .ko; 0109 touches only drivers/gpu/drm/exynos7580/ (image code unchanged)"

note "== int78 gate: helper guards (optional callbacks no longer unguarded) =="
HA=drivers/gpu/drm/drm_atomic_helper.c
[ "$(grep -c 'else if (funcs->commit)' "$HA")" = 2 ] || gate_fail "commit else-branches not guarded"
[ "$(grep -c 'else if (funcs->dpms)' "$HA")" = 2 ] || gate_fail "dpms else-branches not guarded"
[ "$(grep -c '^[[:space:]]*if (funcs->mode_set)$' "$HA")" = 1 ] || gate_fail "encoder mode_set not guarded"
[ "$(grep -c 'else if (funcs->atomic_update)' "$HA")" = 1 ] || gate_fail "plane atomic_update not guarded"
awk '/^[[:space:]]*else$/{getline; if ($0 ~ /funcs->(commit|dpms|mode_set|atomic_update)[[:space:]]*\(/) {print "unguarded: " $0; bad=1}} END{exit bad}' "$HA" \
	|| gate_fail "an unguarded optional callback remains"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c 'drm_atomic_helper_commit_post_planes')" = 0 ] \
	|| gate_fail "drm_atomic_helper is in vmlinux (helper is not a module?)"
aarch64-linux-gnu-nm "$KO_HELPER" | grep -q ' [tT] drm_atomic_helper_commit_post_planes$' \
	|| gate_fail "commit_post_planes missing from drm_kms_helper.ko"
BAD=$(awk '/^--- a\//{print substr($0,7)}' "$REPO/patches/drm-kms/0110-kms-atomic-helper-guard-optional-ops.patch" \
	| grep -v '^drivers/gpu/drm/drm_atomic_helper.c$' | head -1)
[ -z "$BAD" ] || gate_fail "0110 touches an unexpected file: $BAD"
note "helpers: commit/dpms/mode_set/atomic_update guarded; commit_post_planes in the .ko, not in vmlinux"

note "== int78 gate: mmap / PRIME traces =="
grep -q 'get_gem_mmap_offset obj=' drivers/gpu/drm/drm_gem_cma_helper.c || gate_fail "offset trace missing"
grep -q 'drm_gem_cma_mmap: enter pgoff=' drivers/gpu/drm/drm_gem_cma_helper.c || gate_fail "mmap enter trace missing"
grep -q 'drm_gem_cma_mmap: paddr=' drivers/gpu/drm/drm_gem_cma_helper.c || gate_fail "mmap remap trace missing"
grep -q 'exynos7580_gem_prime_import: enter' "$KMS/exynos_drm7580_dmabuf.c" || gate_fail "prime import trace missing"
grep -q 'exynos7580_fb_create: %ux%u fmt=' "$KMS/exynos_drm7580_dmabuf.c" || gate_fail "addfb2 trace missing"
for s in 'get_gem_mmap_offset obj=' 'drm_gem_cma_mmap: enter pgoff=' 'drm_gem_cma_mmap: paddr='; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "built-in mmap trace missing: $s"
done
for s in 'exynos7580_gem_prime_import: enter' 'exynos7580_gem_prime_import: ok' \
	 'exynos7580_fb_create: %ux%u fmt='; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$s" || gate_fail "module prime trace missing: $s"
done
note "mmap offset/pgoff/remap and PRIME import/ADDFB2 traces present"

note "== int78 gate: modeset lock_all simplified + logged =="
grep -q 'bring-up simplification' drivers/gpu/drm/drm_crtc.c || gate_fail "lock_all simplification marker missing"
grep -q 'drm_modeset_lock_all: acquired by' drivers/gpu/drm/drm_crtc.c || gate_fail "lock_all log missing"
grep -q 'drm_modeset_unlock_all: enter by' drivers/gpu/drm/drm_crtc.c || gate_fail "unlock_all log missing"
grep -q 'restore_mode enter (mutex_locked=' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev restore enter log missing"
grep -q 'restore_mode exit (mutex_locked=' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev restore exit log missing"
for s in 'drm_modeset_lock_all: acquired by' 'drm_modeset_unlock_all: enter by'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "lock log missing from image: $s"
done
for s in 'restore_mode enter (mutex_locked=' 'restore_mode exit (mutex_locked='; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$s" || gate_fail "fbdev lock log missing from module: $s"
done
note "lock_all simplified (no connection/ww locks) and both bracket logs present"

note "== int78 gate: initial modeset uses the atomic state + explicit commit =="
KMAIN="$KMS/exynos_drm7580_main.c"
grep -q 'initial modeset state: crtc id=' "$KMAIN" || gate_fail "atomic state log missing"
grep -q 'drm_atomic_set_fb_for_plane' "$KMAIN" || gate_fail "explicit fb attach missing"
grep -q 'drm_atomic_commit(state)' "$KMAIN" || gate_fail "explicit commit missing"
grep -q 'pstate->fb' "$KMAIN" || gate_fail "atomic postcondition missing"
grep -q 'primary plane has no fb0 after the initial modeset' "$KMAIN" && gate_fail "legacy postcondition still present"
for s in 'initial modeset state: crtc id=' 'explicit commit of fb' \
	 'explicit commit ret=' 'still inconsistent after the explicit commit'; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$s" || gate_fail "module log missing: $s"
done
note "initial modeset: atomic-state postcondition, explicit fb commit fallback"

note "== int78 gate: bounded vsync thread stop =="
API=drivers/video/exynos/decon_7580/decon_kms_api.c
IDRV=drivers/video/exynos/decon_7580/decon-int_drv.c
VT=$(sed -n '/^static int decon_wait_for_vsync_thread/,/^}/p' "$IDRV")
printf '%s' "$VT" | grep -q 'wait_event_interruptible_timeout' || gate_fail "vsync thread still waits without timeout"
printf '%s' "$VT" | grep -q 'HZ / 10' || gate_fail "vsync poll interval missing"
printf '%s' "$VT" | grep -q 'kthread_should_stop() || decon->kms_teardown' || gate_fail "vsync thread does not break on teardown"
grep -q 'wake_up_interruptible_all(&decon->vsync_info.wait)' "$API" || gate_fail "teardown does not wake vsync waiters"
grep -q 'wake_up_interruptible_all(&decon->wait_frmdone)' "$API" || gate_fail "teardown does not wake frmdone waiters"
note "vsync thread: 100 ms timed wait + stop/teardown break; teardown wakes both queues"

note "== int78 gate: fb postcondition + pan guard =="
grep -q 'crtc.state->active' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "atomic postcondition missing"
grep -q 'primary->state->fb != fb' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "primary state fb check missing"
grep -q 'legacy primary->fb' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "postcondition log missing"
grep -q 'pan_display: crtc' drivers/gpu/drm/drm_fb_helper.c || gate_fail "pan_display guard missing"
grep -q 'fbdev scanout failed: crtc id=' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "pr_err postcondition log missing"
grep -q 'list_for_each_entry(tmp, &dev->mode_config.crtc_list' drivers/gpu/drm/drm_fb_helper.c \
	|| gate_fail "pan_display membership check missing"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c 'drm_fb_helper_pan_display')" = 0 ] \
	|| gate_fail "drm_fb_helper is built-in (patch not module-only)"
for s in 'legacy primary->fb' 'pan_display: crtc %p is not registered' 'fbdev scanout failed: crtc id='; do
	aarch64-linux-gnu-strings "$KO_HELPER" | grep -qF "$s" \
		|| aarch64-linux-gnu-strings "$KO" | grep -qF "$s" \
		|| gate_fail "module string missing: $s"
done
note "fb predicate on atomic state; pan_display guarded; changes module-only"

note "== int78 gate: VIDINTCON0 trace + non-blocking teardown =="
API=drivers/video/exynos/decon_7580/decon_kms_api.c
REG=drivers/video/exynos/decon_7580/decon_reg_7580.c
grep -q 'decon_reg_set_int: writing VIDINTCON0 val=' "$REG" || gate_fail "set_int write trace missing"
grep -q 'decon_reg_set_int: VIDINTCON0 readback=' "$REG" || gate_fail "set_int readback trace missing"
grep -q 'scratch VIDOUTCON0' "$API" || gate_fail "scratch write/read missing"
grep -q 'drvdata_ok=' "$API" || gate_fail "arm context log missing"
grep -q 'kms_teardown' drivers/video/exynos/decon_7580/decon.h || gate_fail "kms_teardown flag missing"
[ "$(grep -c 'kms_teardown' drivers/video/exynos/decon_7580/decon_core.c)" -ge 3 ] || gate_fail "core teardown bail-outs missing"
grep -q 'wake_up_interruptible_all(&decon->vsync_info.wait)' "$API" || gate_fail "vsync wake missing in teardown"
grep -q 'wake_up_interruptible_all(&decon->wait_frmdone)' "$API" || gate_fail "frmdone wake missing in teardown"
grep -q 'skipping kthread_stop' "$API" || gate_fail "bounded worker stop missing"
grep -q 'TASK_UNINTERRUPTIBLE' "$API" || gate_fail "D-state check missing"
for s in 'decon_reg_set_int: writing VIDINTCON0 val=' 'decon_reg_set_int: VIDINTCON0 readback=' \
	 'scratch VIDOUTCON0' 'skipping kthread_stop' 'update handler: teardown'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "trace missing from image: $s"
done
note "intcon write trace + teardown guards/wakes bounded-stop present"

note "== int78 gate: unconditional FRAME force-arm =="
API=drivers/video/exynos/decon_7580/decon_kms_api.c
grep -q 'decon_reg_set_int(DECON_INT, &psr, DSI_MODE_SINGLE, 1);' "$API" || gate_fail "force set_int missing"
grep -q 'DSIM_IOC_VSYNC' "$API" || gate_fail "DSIM vsync call missing"
grep -q 'VIDINTCON0 still zero after arm' "$API" || gate_fail "zero-readback log missing"
grep -q 'decon_helper.h' "$API" || gate_fail "decon_helper.h include missing"
[ "$(grep -c 'decon_activate_vsync(decon);' "$API")" = 0 ] || gate_fail "refcounted arm still used in the shim"
[ "$(grep -c 'decon_kms_arm_frame(decon,' "$API")" = 3 ] || gate_fail "expected 3 force-arm call sites"
grep -q 'DSIM_IOC_VSYNC ret=' "$API" || gate_fail "DSIM vsync return not logged"
for s in 'VIDINTCON0 after set_int=' 'DSIM_IOC_VSYNC ret=' 'VIDINTCON0 still zero after arm'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "force-arm log missing: $s"
done
note "force-arm: set_int+DSIM vsync unconditionally, 3 call sites, readbacks logged"

note "== int78 gate: bounded vsync + per-frame arming + lockup panic =="
API=drivers/video/exynos/decon_7580/decon_kms_api.c
CORE=drivers/video/exynos/decon_7580/decon_core.c
grep -q '^CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=y$' .config || gate_fail "hardlockup panic off"
grep -q '^CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y$' .config || gate_fail "softlockup panic off"
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] watchdog_enable$' || gate_fail "hardlockup watchdog code missing"
grep -q 'vsync wait <=100ms' "$CORE" || gate_fail "bounded KMS vsync wait missing in update_regs"
grep -q 'decon_kms_arm_frame' "$API" || gate_fail "arm helper missing"
[ "$(grep -c 'decon_kms_arm_frame(decon,' "$API")" = 3 ] || gate_fail "arming call sites changed"
grep -q 'bounded vsync wait failed' "$API" || gate_fail "commit timeout error missing"
grep -q -- '-ETIMEDOUT' "$API" || gate_fail "commit does not return -ETIMEDOUT"
[ "$(grep -c 'decon_reg_start(' drivers/video/exynos/decon_7580/decon_core.c)" -ge 1 ] || gate_fail "reg_start missing"
for s in 'decon_kms_arm_frame' 'vsync wait <=100ms' 'bounded vsync wait failed' \
	 'VIDINTCON0 before=' 'VIDINTCON0 after set_int='; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "built-in log missing: $s"
done
note "lockup panic config + bounded vsync + arming after enable/program present"

note "== int78 gate: FRAME irq re-arm, ESD idempotency, diagnostics =="
API="$KMS/../video/exynos/decon_7580/decon_kms_api.c"
API=drivers/video/exynos/decon_7580/decon_kms_api.c
CORE=drivers/video/exynos/decon_7580/decon_core.c
IDRV=drivers/video/exynos/decon_7580/decon-int_drv.c
grep -q 'decon_kms_arm_frame' "$API" || gate_fail "FRAME force-arm helper missing"
grep -q 'FRAME irq armed' "$API" || gate_fail "arm log missing"
grep -q 'decon_kms_enable: decon_enable ret=' "$API" || gate_fail "enable logging missing"
grep -q 'decon_kms_log_state' include/linux/exynos_decon_kms.h || gate_fail "log_state not declared"
grep -q 'esd_irq_on' drivers/video/exynos/decon_7580/decon.h || gate_fail "ESD idempotency flag missing"
grep -q 'esd_irq_on' drivers/video/exynos/decon_7580/decon_core.c || gate_fail "ESD guards missing"
grep -q 'decon IRQ' "$IDRV" || gate_fail "IRQ number logging missing"
grep -q 'decon_int_irq_log_count' "$IDRV" || gate_fail "FRAME irq status logging missing"
for s in 'decon_kms_enable: decon_enable ret=' 'FRAME irq armed' \
	 'decon irq: VIDINTCON1=' 'decon IRQ'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "built-in log missing: $s"
done
grep -q 'fbdev scanout failed' "$KMS/exynos_drm7580_fbdev.c" || gate_fail "fbdev state log missing"
aarch64-linux-gnu-strings "$KO" | grep -qF 'fbdev scanout failed' || gate_fail "fbdev state log missing from the module"
note "FRAME irq re-armed after enable, ESD idempotent, IRQ/reg diagnostics present"

note "== int78 gate: fbdev LUT + destroy hooks =="
CRTC="$KMS/exynos_drm7580_crtc.c"
DSI="$KMS/exynos_drm7580_dsi.c"
grep -q '\.load_lut' "$CRTC" || gate_fail "CRTC .load_lut missing"
grep -q '\.gamma_set' "$CRTC" || gate_fail "CRTC .gamma_set missing"
grep -q '\.destroy.*drm_encoder_cleanup' "$DSI" || gate_fail "encoder .destroy missing"
grep -q '\.destroy.*drm_connector_cleanup' "$DSI" || gate_fail "connector .destroy missing"
aarch64-linux-gnu-nm -u "$KO" | awk '{print $NF}' | grep -E '^(drm_encoder_cleanup|drm_connector_cleanup)$' | sort -u | tee -a "$EVID"
BAD=$(awk '/^--- a\//{print substr($0,7)}' "$REPO/patches/drm-kms/0111-kms-fbdev-lut-and-destroy-hooks.patch" \
	| grep -v '^drivers/gpu/drm/exynos7580/' | head -1)
[ -z "$BAD" ] || gate_fail "0111 touches an unexpected file: $BAD"
note "LUT no-ops + encoder/connector destroy wired; 0111 is module-only"

note "== int78 gate: DECON powered before the first register read =="
grep -q 'regs powered (pd attached, clocks on), reading VIDCON0' "$CORE" || gate_fail "power log missing in vendor core"
grep -q 'DECON has no power domain attached (pd_disp)' "$CORE" || gate_fail "no explicit pm_domain check"
grep -q 'ret = decon_int_get_clocks(decon);' "$CORE" || gate_fail "clock get result not checked"
grep -q 'failed to get DECON clocks' "$CORE" || gate_fail "clock failure not logged"
grep -q 'pm_runtime was enabled and the device powered above' "$CORE" \
	|| gate_fail "late duplicate pm_runtime block not removed"
[ "$(grep -c 'pm_runtime_enable(dev);' "$CORE")" = 1 ] || gate_fail "pm_runtime_enable not exactly once in the vendor core"
[ "$(grep -c 'pm_runtime_get_sync(dev);' "$CORE")" = 1 ] || gate_fail "early pm_runtime_get_sync not exactly once"
[ "$(grep -c 'platform_set_drvdata(pdev, decon);' "$CORE")" = 1 ] \
	|| gate_fail "vendor drvdata must be published exactly once (runtime PM needs it)"
DR_L=$(grep -n 'platform_set_drvdata(pdev, decon);' "$CORE" | cut -d: -f1 | head -1)
EN_L=$(grep -n 'pm_runtime_enable(dev);' "$CORE" | cut -d: -f1 | head -1)
[ -n "$DR_L" ] && [ -n "$EN_L" ] && [ "$DR_L" -lt "$EN_L" ] \
	|| gate_fail "drvdata not published before pm_runtime_enable (dr=$DR_L en=$EN_L)"
SS_L=$(grep -n 'decon_reg_get_stop_status(DECON_INT) &&' "$CORE" | cut -d: -f1 | head -1)
[ -n "$EN_L" ] && [ -n "$SS_L" ] && [ "$EN_L" -lt "$SS_L" ] \
	|| gate_fail "pm_runtime_enable is not before the first register read (en=$EN_L read=$SS_L)"
for s in 'regs powered (pd attached, clocks on), reading VIDCON0' 'has no power domain attached' \
	 'failed to power DECON power domain' 'failed to get DECON clocks'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "built-in DECON lost log: $s"
done
for s in 'decon: pm_runtime suspended=%d' 'decon clocks: pclk=' 'clocks enabled (pclk=' \
	 'DECON clock %s missing'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "built-in DECON lost log: $s"
done
grep -q 'pm_runtime_set_suspended(dev);' "$CORE" || gate_fail "runtime PM not forced suspended"
grep -q 'decon_kms_clocks_on(decon)' "$CORE" || gate_fail "explicit clock enable missing"
grep -q 'decon_kms_clocks_off(decon);' "$CORE" || gate_fail "explicit clock disable missing in remove"
grep -q 'balance the explicit clock enables' drivers/video/exynos/decon_7580/decon_kms_api.c \
	|| gate_fail "teardown clock balance missing in decon_kms_teardown"
grep -q 'struct clk \*cks\[5\]' drivers/video/exynos/decon_7580/decon_kms_api.c \
	|| gate_fail "teardown does not disable the five clocks"
[ "$(grep -c 'clk_prepare_enable(cks\[i\])' "$CORE")" = 1 ] || gate_fail "clock enable loop missing"
SUS_L=$(grep -n 'pm_runtime_set_suspended(dev);' "$CORE" | cut -d: -f1 | head -1)
CLK_L=$(grep -n 'decon_kms_clocks_on(decon)' "$CORE" | cut -d: -f1 | head -1)
[ -n "$SUS_L" ] && [ -n "$CLK_L" ] && [ "$CLK_L" -gt "$SUS_L" ] \
	|| gate_fail "clock enable is not after pm_runtime_set_suspended (sus=$SUS_L clk=$CLK_L)"
note "vendor core: drvdata line $DR_L, pm_runtime enabled line $EN_L, set_suspended line $SUS_L, explicit clocks line $CLK_L, first VIDCON0 read line $SS_L"

note "== int78 gate: pstore capture available =="
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] ess_pstore_init$' || gate_fail "ess_pstore_init not linked"
grep -q '^CONFIG_PSTORE_CONSOLE=y' .config || gate_fail "pstore console off"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'log_pstore' || gate_fail "ess ramoops region name missing"
note "pstore: ess_pstore_init + PSTORE_CONSOLE + log_pstore region; no DT edit"

note "== int78 gate: KMS objects clean =="
[ -f drivers/gpu/drm/exynos7580/built-in.o ] || gate_fail "exynos7580/built-in.o missing"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "vmlinux predates this build (stale prebuilt vmlinux)"
W=$(grep -E 'warning:' "$LOG" | grep -c 'drivers/gpu/drm/exynos7580/')
[ "$W" = 0 ] || gate_fail "exynos7580 sources produced $W warnings"
grep -E 'CC +drivers/gpu/drm/exynos7580/' "$LOG" | tee -a "$EVID"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/exynos7580" 2>&1 | tee -a "$EVID"

STAGE=${STAGE:-$EXT/stage-int78}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
# Single line: the kernel splits its command line on spaces only, a newline
# inside the stored string would corrupt the preceding token.
CL=$(tr '\n' ' ' < "$STAGE/cmdline" | sed 's/  */ /g; s/ $//')
# int78: all log/lockup options on one line.
for opt in ignore_loglevel hardlockup_panic=1 softlockup_panic=1 panic=5; do
	case " $CL " in *" $opt "*) ;; *) CL="$CL $opt" ;; esac
done
printf '%s' "$CL" > "$STAGE/cmdline"
for opt in ignore_loglevel hardlockup_panic=1 softlockup_panic=1 panic=5; do
	grep -q -- "$opt" "$STAGE/cmdline" || gate_fail "$opt not in the kernel command line"
done
[ "$(wc -l < "$STAGE/cmdline")" = 0 ] || gate_fail "kernel command line is not a single line"
note "cmdline: $(cat "$STAGE/cmdline")"
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int78.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int78.img" | tee -a "$EVID"
sha256sum "$EXT/kernel-boot-int78.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int78"
sha256sum "$EXT/Image-int78" | tee -a "$EVID"
cp drivers/gpu/drm/drm_kms_helper.ko "$EXT/drm_kms_helper-int78.ko"
cp drivers/gpu/drm/exynos7580/exynos-drm7580.ko "$EXT/exynos-drm7580-int78.ko"
sha256sum "$EXT/drm_kms_helper-int78.ko" "$EXT/exynos-drm7580-int78.ko" | tee -a "$EVID"

note "== int78 gate: PASS (KMS as module, 0103 refusal + 0105 drvdata order + 0106 unwind-free modules + 0107 power + 0108 clocks + 0109 encoder mode_set + 0110 helper guards + 0111 LUT/destroy + 0112 FRAME-irq + 0113 fbdev log + 0114 bounded vsync + 0115 force-arm + 0116 intcon trace/teardown + 0117 fb postcondition/pan guard + 0118 bounded vsync-thread stop + 0119 fb pr_err + 0120 atomic initial modeset + 0121 lock_all simplify/log + 0122 fbdev restore lock log + 0123 mmap trace + 0124 prime trace; no KMS symbols in vmlinux; 2 .ko, imports resolved; LOG_BUF_SHIFT=18 + ignore_loglevel; pstore; stock-dt boot) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
