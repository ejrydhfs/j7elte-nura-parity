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
# plain non-comment fragments). Stock dt.img, reference ramdisk, Image-int95.
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int95}
LOG=${LOG:-$EXT/logs/int95-kms-build.log}
EVID=${EVID:-$EXT/logs/int95-kms-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int95 GATE FAIL: $1"; exit 1; }

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
addp "$REPO"/patches/drm-kms/0125-kms-cma-mmap-offset-64bit.patch
addp "$REPO"/patches/drm-kms/0126-kms-lock-all-nesting.patch
addp "$REPO"/patches/drm-kms/0127-kms-gamma-size-and-frame-check.patch
addp "$REPO"/patches/drm-kms/0128-kms-lock-all-owner-depth.patch
addp "$REPO"/patches/drm-kms/0129-kms-page-flip-ebusy-and-prime-trace.patch
addp "$REPO"/patches/drm-kms/0130-kms-legacy-fb-sync.patch
addp "$REPO"/patches/drm-kms/0133-kms-lock-all-owner-death.patch
addp "$REPO"/patches/drm-kms/0134-kms-manual-page-flip-state.patch
addp "$REPO"/patches/drm-kms/0135-kms-blob-props-and-crtc-mode-id.patch
for p in "$REPO"/patches/drm-backport/000[1-4]-*.patch; do addp "$p"; done
addp "$REPO"/code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch
addp "$REPO"/code/gpu31/driver/0001-panfrost7580-driver-sources.patch
addp "$REPO"/code/gpu31/driver/0002-panfrost7580-build-wiring.patch
addp "$REPO"/code/gpu31/driver/0137-panfrost7580-mmap-instrument.patch
addp "$REPO"/code/gpu31/driver/0138-panfrost7580-mesa26-uapi.patch
addp "$REPO"/code/gpu31/driver/0139-panfrost7580-submit-deps.patch
addp "$REPO"/code/gpu31/driver/0140-panfrost7580-irq-masks-erratum.patch
addp "$REPO"/patches/drm-backport/0006-dma-buf-llseek-seek-end.patch
addp "$REPO"/patches/drm-backport/0007-dma-buf-export-fmode-lseek.patch
addp "$REPO"/patches/drm-kms/0136-kms-card0-prime-ioctl-hooks.patch
addp "$REPO"/patches/drm-kms/0137-kms-core-prime-allow-rdwr.patch
# int95: blob lifetime fix from BlobPropsBackport (new patch, after 0135).
BLOB=$(ls "$REPO"/patches/drm-kms/0138-*.patch 2>/dev/null | head -1)
[ -n "$BLOB" ] || gate_fail "0138 blob lifetime patch missing"
addp "$BLOB"
FB=$(ls "$REPO"/patches/drm-kms/0139-*.patch 2>/dev/null | head -1)
[ -n "$FB" ] || gate_fail "0139 fbdev teardown patch missing"
addp "$FB"
DIAG=$(ls "$REPO"/patches/drm-kms/0140-*.patch 2>/dev/null | head -1)
[ -n "$DIAG" ] || gate_fail "0140 atomic commit/check diagnostic patch missing"
addp "$DIAG"
LINKFIX=$(ls "$REPO"/patches/drm-kms/0141-*.patch 2>/dev/null | head -1)
[ -n "$LINKFIX" ] || gate_fail "0141 crtc link modeset gating patch missing"
addp "$LINKFIX"
COMMITFIX=$(ls "$REPO"/patches/drm-kms/0142-*.patch 2>/dev/null | head -1)
[ -n "$COMMITFIX" ] || gate_fail "0142 decon commit re-enable patch missing"
addp "$COMMITFIX"
WINDIAG=$(ls "$REPO"/patches/drm-kms/0143-*.patch 2>/dev/null | head -1)
[ -n "$WINDIAG" ] || gate_fail "0143 decon window dump/ion guard patch missing"
addp "$WINDIAG"
FRAMEDIAG=$(ls "$REPO"/patches/drm-kms/0144-*.patch 2>/dev/null | head -1)
[ -n "$FRAMEDIAG" ] || gate_fail "0144 frame accounting/buffer sample patch missing"
addp "$FRAMEDIAG"
# int95: the platform-driver rename (PanfrostName) is an in-place edit of
# driver 0001 (.driver.name + MODULE_ALIAS), so no extra patch here.
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
	0001-drm-3.10-render-node-mode-b.patch)
		# int95: drmP.h gets the DRIVER_RENDER_ONLY bit and struct member by
		# hand below (drm-atomic 0001 already took 0x8000 for DRIVER_ATOMIC).
		EXC="--exclude=include/drm/drmP.h" ;;
	esac
	git apply $EXC --check "$pf" || gate_fail "git apply --check failed: $(basename "$pf")"
	git apply $EXC "$pf" || gate_fail "git apply failed: $(basename "$pf")"
done
note "0200 applied without drivers/video/exynos/decon_7580/decon_core.c (stock late_initcall kept)"
# int95: drm-backport/0005 build wiring, hand-applied (its Makefile hunk no
# longer matches after the atomic core extended drm-y).
python3 - <<'PYEOF'
import re
p='drivers/gpu/drm/Makefile'
s=open(p).read()
if 'drm_syncobj.o' not in s:
    s=s.replace('drm-$(CONFIG_COMPAT)', 'drm-y += drm_syncobj.o drm_sched.o\ndrm-$(CONFIG_COMPAT)', 1)
    open(p,'w').write(s)
p='drivers/base/Makefile'
s=open(p).read()
if 'dma-fence.o' not in s:
    s=s.replace('obj-$(CONFIG_DMA_SHARED_BUFFER)', 'obj-$(CONFIG_DMA_SHARED_BUFFER) += dma-fence.o dma-resv.o\nobj-$(CONFIG_DMA_SHARED_BUFFER)', 1)
    open(p,'w').write(s)
# mode-b drmP.h by hand
p='include/drm/drmP.h'
s=open(p).read()
m=re.search(r'(?m)^#define DRIVER_ATOMIC.*$', s)
assert m, 'DRIVER_ATOMIC not found'
line=m.group(0)
if 'DRIVER_RENDER_ONLY' not in s:
    s=s.replace(line, line + '\n/* int95: render-only device (no primary/control minor, render node only). */\n#define DRIVER_RENDER_ONLY 0x10000', 1)
m=re.search(r'(?m)^\tstruct drm_minor \*primary;.*$', s)
assert m, 'primary minor member not found'
if 'struct drm_minor *render;' not in s:
    s=s.replace(m.group(0), m.group(0) + '\n\tstruct drm_minor *render;\t/* int95: render node (Mode B) */', 1)
open(p,'w').write(s)
print('int95 hand wiring done')
PYEOF
grep -q 'DRIVER_RENDER_ONLY' include/drm/drmP.h || gate_fail "DRIVER_RENDER_ONLY missing"
grep -q 'struct drm_minor \*render;' include/drm/drmP.h || gate_fail "render minor member missing"

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
apply_frag "$REPO"/patches/drm-kms-coexist/gpu-pairing.fragment
# int95: module build.  The shipping config has CONFIG_MODULES unset; the
# fragment pins DRM_EXYNOS7580=y, so both need overriding after the fragments.
grep -v '^CONFIG_MODULES=\|^CONFIG_MODULE_UNLOAD=\|^# CONFIG_MODULES is not set\|^# CONFIG_MODULE_UNLOAD is not set' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_MODULES=y\nCONFIG_MODULE_UNLOAD=y\n' >> .config
grep -v '^CONFIG_DRM_EXYNOS7580=' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_DRM_EXYNOS7580=m\n' >> .config
# int95: keep the boot log (256 KB) for dmesg/pstore.
sed -i 's/^CONFIG_LOG_BUF_SHIFT=.*/CONFIG_LOG_BUF_SHIFT=18/' .config
# int95: lockup hardening - a deadlock must panic and auto-reboot into the
# headless SSH state instead of hanging until a battery pull.
grep -v '^CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=\|^CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=\|^# CONFIG_BOOTPARAM_HARDLOCKUP_PANIC is not set\|^# CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC is not set' .config > .config.tmp && mv .config.tmp .config
printf 'CONFIG_BOOTPARAM_HARDLOCKUP_PANIC=y\nCONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y\n' >> .config
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int95-kms-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed (see int95-kms-config.log)"
grep -qE 'recursive dependency|error:' "$EXT/logs/int95-kms-config.log" \
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
MISSF=$EXT/logs/int95-kms-config-missing.log
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
note "== int95 gate: layout and single wiring point =="
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

note "== int95 gate: atomic core + driver features =="
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

note "== int95 gate: fixed shim ordering =="
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

note "== int95 gate: built-in KMS owns DECON, vendor probe refuses =="
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

note "== int95 gate: no core gem mmap/offset in our paths =="
grep -q 'drm_gem_mmap(' "$KMS"/*.c && gate_fail "exynos7580 sources still call core drm_gem_mmap()"
grep -q 'drm_gem_create_mmap_offset' "$KMS"/*.c && gate_fail "exynos7580 sources still call drm_gem_create_mmap_offset()"
grep -q 'drm_gem_cma_dumb_create' "$KMS/exynos_drm7580_main.c" || gate_fail "CMA dumb_create not wired"
grep -q 'exynos7580_gem_prime_import' "$KMS/exynos_drm7580_main.c" || gate_fail "PRIME import not wired"

note "== int95 gate: module built, symbols, metadata =="
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
echo "$MODS" > "$EXT/logs/int95-modules.txt"
[ "$(echo "$MODS" | wc -l)" = 2 ] || gate_fail "loadable set is not exactly 2 modules: $(echo $MODS)"
MODI=$(command -v modinfo || echo aarch64-linux-gnu-modinfo)
"$MODI" "$KO" | tee -a "$EVID"
"$MODI" "$KO" | grep -q '^license:.*GPL' || gate_fail "module license is not GPL"
"$MODI" "$KO" | grep -q 'exynos-drm7580\.ko$' || gate_fail "module filename unexpected"
"$MODI" "$KO" | grep -q '^depends:.*drm_kms_helper' || gate_fail "helper dependency missing"
"$MODI" "$KO" | grep -q '^vermagic:.*aarch64' || gate_fail "vermagic is not aarch64"
aarch64-linux-gnu-nm "$KO" | grep -E ' (T|D) exynos7580_' | tee -a "$EVID"

note "== int95 gate: every imported symbol resolves =="
UND=$(aarch64-linux-gnu-nm -u "$KO" | awk '{print $NF}' | sort -u)
echo "$UND" > "$EXT/logs/int95-imports.txt"
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
echo "$UNDH" > "$EXT/logs/int95-imports-helper.txt"
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

note "== int95 gate: loadable set has no out-of-range unwind relocations =="
# Supported by arch/arm64/kernel/module.c (3.10): 256..260,262, 263..269,
# 270..272, 273..286, 299.  261 (PREL32) is the failing one; readelf truncates
# long type names, so decode the type from r_info (low 32 bits, last 3 hex).
ALLOWED_NUM="256 257 258 259 260 262 263 264 265 266 267 268 269 270 271 272 273 274 275 276 277 278 279 280 282 283 284 285 286 299"
for k in "$KO" "$KO_HELPER"; do
	n=$(aarch64-linux-gnu-readelf -r "$k" | grep -c 'R_AARCH64_PREL32')
	[ "$n" = 0 ] || gate_fail "$(basename "$k") still has $n R_AARCH64_PREL32 relocations (int95 regression)"
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

note "== int95 gate: DSI encoder mode_set hook =="
DSI="$KMS/exynos_drm7580_dsi.c"
grep -q 'exynos7580_dsi_encoder_mode_set' "$DSI" || gate_fail "encoder mode_set implementation missing"
grep -q '\.mode_set' "$DSI" || gate_fail "encoder .mode_set not wired"
aarch64-linux-gnu-strings "$KO" | grep -qF 'DSI encoder: mode_set' || gate_fail "mode_set log missing from the module"
# module-only change: the patch must not touch any built-in source
BAD=$(awk '/^--- a\//{print substr($0,7)}' "$REPO/patches/drm-kms/0109-kms-dsi-encoder-mode-set.patch" \
	| grep -v '^drivers/gpu/drm/exynos7580/' | head -1)
[ -z "$BAD" ] || gate_fail "0109 touches a built-in file: $BAD"
note "encoder: mode_set no-op wired, log in the .ko; 0109 touches only drivers/gpu/drm/exynos7580/ (image code unchanged)"

note "== int95 gate: helper guards (optional callbacks no longer unguarded) =="
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

note "== int95 gate: render driver in the image =="
grep -q '^CONFIG_DRM_PANFROST7580=y$' .config || gate_fail "panfrost render driver not enabled"
grep -q 'DRIVER_RENDER_ONLY 0x10000' include/drm/drmP.h || gate_fail "DRIVER_RENDER_ONLY bit wrong"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] p7580_drv_probe$')" = 1 ] || gate_fail "panfrost probe not linked"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [dD] p7580_drm_driver$')" = 1 ] || gate_fail "panfrost drm_driver missing"
grep -q '\.name = "panfrost"' drivers/gpu/drm/panfrost/panfrost7580_main.c || gate_fail "render driver not named panfrost"
aarch64-linux-gnu-nm vmlinux | grep -q 'drm_get_platform_dev' || gate_fail "mode-b render node helper missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'render-only' || gate_fail "render-only init print missing"
grep -q 'drm_syncobj.o' drivers/gpu/drm/Makefile || gate_fail "0005 drm-y wiring missing"
grep -q 'dma-fence.o' drivers/base/Makefile || gate_fail "0005 dma-fence wiring missing"
for sym in drm_property_create_blob drm_property_lookup_blob drm_atomic_set_mode_for_crtc drm_atomic_set_mode_prop_for_crtc drm_mode_convert_umode; do
	aarch64-linux-gnu-nm vmlinux | grep -q " [tT] $sym\$" || gate_fail "blob-props symbol missing: $sym"
done
grep -q '"MODE_ID"' drivers/gpu/drm/drm_crtc.c || gate_fail "MODE_ID property missing"
grep -q 'prop_mode_id' drivers/gpu/drm/drm_crtc.c || gate_fail "MODE_ID not attached to CRTCs"
grep -q 'DRM_IOCTL_MODE_CREATEPROPBLOB' drivers/gpu/drm/drm_drv.c || gate_fail "CREATEPROPBLOB ioctl missing"
grep -q 'DRM_IOCTL_MODE_DESTROYPROPBLOB' drivers/gpu/drm/drm_drv.c || gate_fail "DESTROYPROPBLOB ioctl missing"
grep -q 'drm_atomic_set_mode_for_crtc' drivers/gpu/drm/drm_atomic.c || gate_fail "MODE_ID setter missing"
grep -q 'drm_property_destroy_user_blobs(dev, file_priv)' drivers/gpu/drm/drm_fops.c || gate_fail "blob destroy_user_blobs hook missing"
KMAIN="$KMS/exynos_drm7580_main.c"
grep -q 'prime_handle_to_fd\s*=\s*drm_gem_prime_handle_to_fd' "$KMAIN" || gate_fail "card0 prime_handle_to_fd missing"
grep -q 'prime_fd_to_handle\s*=\s*drm_gem_prime_fd_to_handle' "$KMAIN" || gate_fail "card0 prime_fd_to_handle missing"
for sym in drm_gem_prime_handle_to_fd drm_gem_prime_fd_to_handle; do
	aarch64-linux-gnu-nm vmlinux | grep -q " [tT] $sym\$" || gate_fail "prime helper missing: $sym"
done
[ "$(find drivers -name '*.ko' | wc -l)" = 2 ] || gate_fail "panfrost must be built in (=y)"
note "panfrost built-in render driver, render-only mode-b, blob/MODE_ID present"

note "== int95 gate: panfrost mmap instrumentation =="
GG=drivers/gpu/drm/panfrost/panfrost7580_gem.c
GI=drivers/gpu/drm/panfrost/panfrost7580_ioctl.c
grep -qF 'p7580_gem_mmap: enter' "$GG" || gate_fail "mmap entry log missing"
grep -qF 'p7580_gem_mmap: offset_lookup MISS' "$GG" || gate_fail "mmap lookup-miss log missing"
grep -qF 'p7580_gem_mmap: hit obj_size=' "$GG" || gate_fail "mmap hit log missing"
grep -qF 'p7580_gem_mmap: FAIL branch rc=' "$GG" || gate_fail "mmap FAIL log missing"
grep -qF 'p7580_gem_fault:' "$GG" || gate_fail "fault log missing"
grep -qF 'p7580_offset_alloc:' "$GG" || gate_fail "offset alloc log missing"
grep -qF 'p7580_create_bo:' "$GI" || gate_fail "create_bo log missing"
grep -qF 'p7580_mmap_bo: handle=' "$GI" || gate_fail "mmap_bo log missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'p7580_gem_mmap: enter' || gate_fail "mmap instrumentation not in vmlinux"
aarch64-linux-gnu-strings vmlinux | grep -qF 'p7580_mmap_bo: handle=' || gate_fail "mmap_bo instrumentation not in vmlinux"
note "mmap instrumentation present (kernel + vmlinux strings)"

note "== int95 gate: dma-buf llseek SEEK_END =="
grep -q 'static loff_t dma_buf_llseek' drivers/base/dma-buf.c || gate_fail "dma_buf_llseek missing"
grep -q 'whence == SEEK_END' drivers/base/dma-buf.c || gate_fail "SEEK_END branch missing"
awk '/static const struct file_operations dma_buf_fops = \{/{f=1} f && /\.llseek[ \t]*= dma_buf_llseek/{print "ok"; exit} f && /\};/{exit}' drivers/base/dma-buf.c | grep -q ok || gate_fail "dma_buf_fops .llseek not wired"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] dma_buf_llseek$')" = 1 ] || gate_fail "dma_buf_llseek not linked"
note "dma-buf llseek present (SEEK_END = dmabuf->size, wired in fops, linked)"

note "== int95 gate: dma-buf file is seekable, fops slot points at llseek =="
grep -q 'file->f_mode |= FMODE_LSEEK' drivers/base/dma-buf.c || gate_fail "dma-buf file FMODE_LSEEK missing"
grep -q 'if (IS_ERR(file))' drivers/base/dma-buf.c || gate_fail "dma_buf_export anon file error check missing"
read FOPSA LOFFA <<EOF_NM
$(aarch64-linux-gnu-nm vmlinux | awk '$3=="dma_buf_fops"{f=$1} $3=="dma_buf_llseek"{l=$1} END{print f, l}')
EOF_NM
[ -n "$FOPSA" ] && [ -n "$LOFFA" ] || gate_fail "dma_buf_fops/dma_buf_llseek symbol missing"
python3 - "$FOPSA" "$LOFFA" vmlinux <<'EOF_ELF' || gate_fail "dma_buf_fops.llseek slot does not point at dma_buf_llseek"
import sys, struct, subprocess, re
fops, loff, elf = int(sys.argv[1], 16), int(sys.argv[2], 16), sys.argv[3]
segs = []
for line in subprocess.check_output(["aarch64-linux-gnu-readelf", "-lW", elf], text=True).splitlines():
    m = re.match(r"\s*LOAD\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", line)
    if m:
        segs.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(4), 16)))
def rd(addr, n):
    for off, vaddr, filesz in segs:
        if vaddr <= addr and addr + n <= vaddr + filesz:
            with open(elf, "rb") as f:
                f.seek(off + (addr - vaddr))
                return f.read(n)
    raise SystemExit("address 0x%x not in a LOAD segment" % addr)
# struct file_operations: owner at 0, llseek at 8 (arm64, 3.10)
ptr = struct.unpack("<Q", rd(fops + 8, 8))[0]
print("dma_buf_fops=0x%x llseek_slot=0x%x dma_buf_llseek=0x%x" % (fops, ptr, loff))
sys.exit(0 if ptr == loff else 1)
EOF_ELF
note "dma-buf file seekable (FMODE_LSEEK) and dma_buf_fops.llseek slot proven in vmlinux"

note "== int95 gate: PRIME_HANDLE_TO_FD accepts DRM_RDWR =="
grep -q '^#define DRM_RDWR O_RDWR' include/uapi/drm/drm.h || gate_fail "DRM_RDWR define missing"
grep -q '~(DRM_CLOEXEC | DRM_RDWR)' drivers/gpu/drm/drm_prime.c || gate_fail "drm_prime flags mask not widened"
grep -q 'args->flags & (DRM_CLOEXEC | DRM_RDWR)' drivers/gpu/drm/drm_prime.c || gate_fail "DRM_RDWR not passed to export"
note "DRM_RDWR accepted and passed to drm_gem_prime_export/dma_buf_export"

note "== int95 gate: platform driver named panfrost (uevent DRIVER=panfrost) =="
PM=drivers/gpu/drm/panfrost/panfrost7580_main.c
awk '/static struct platform_driver p7580_platform_driver = \{/{f=1} f && /^\};/{exit} f' "$PM" | grep -q '\.name = "panfrost"' \
	|| gate_fail "platform_driver .name is not panfrost (uevent DRIVER would stay panfrost7580)"
grep -q '{ .compatible = "arm,mali-t720" }' "$PM" || gate_fail "DT match lost arm,mali-t720"
grep -q '{ .compatible = "arm,mali" }' "$PM" || gate_fail "DT match lost arm,mali"
grep -q 'MODULE_ALIAS("platform:panfrost")' "$PM" || gate_fail "module alias not renamed"
note "platform driver .name=panfrost, DT compatibles intact, module alias panfrost"

note "== int95 gate: panfrost DRM 1.1 + Mesa 26 GET_PARAM/flags uapi =="
DM=drivers/gpu/drm/panfrost/panfrost7580_main.c
DI=drivers/gpu/drm/panfrost/panfrost7580_ioctl.c
DU=drivers/gpu/drm/panfrost/panfrost_drm.h
awk '/static struct drm_driver p7580_drm_driver = \{/{f=1} f && /^\};/{exit} f' "$DM" | grep -q '\.minor = 1,' \
	|| gate_fail "drm_driver minor is not 1 (Mesa 26 refuses < 1.1)"
grep -q 'DRM_PANFROST_PARAM_SELECTED_COHERENCY' "$DU" || gate_fail "modern param enum missing"
grep -q 'DRM_PANFROST_PARAM_AFBC_FEATURES' "$DU" || gate_fail "AFBC param missing"
grep -q '#define PANFROST_BO_NOEXEC\s*1' "$DU" || gate_fail "PANFROST_BO_NOEXEC missing"
grep -q '#define PANFROST_BO_HEAP\s*2' "$DU" || gate_fail "PANFROST_BO_HEAP missing"
grep -q '#define PANFROST_BO_WB_MMAP\s*4' "$DU" || gate_fail "PANFROST_BO_WB_MMAP missing"
grep -q 'p7580_capture_features' "$DM" || gate_fail "feature capture not wired at probe"
grep -q 'P7580_GPU_AFBC_FEATURES\s*0x04Cu' "$DM" || gate_fail "AFBC feature register offset wrong"
grep -q 'P7580_GPU_L2_FEATURES\s*0x004u' "$DM" || gate_fail "L2 feature register offset wrong"
grep -q 'P7580_GPU_MMU_FEATURES\s*0x014u' "$DM" || gate_fail "MMU feature register offset wrong"
grep -q 'p7580_drv_get_param(dev, args->param' "$DI" || gate_fail "GET_PARAM does not use the probe snapshot"
grep -q 'unknown flags' "$DI" || gate_fail "CREATE_BO still rejects unknown flags"
grep -q 'P7580_SUBMIT_BO_MAX' "$DI" || gate_fail "ioctl.c lost its submit caps"
aarch64-linux-gnu-strings vmlinux | grep -qF 'p7580_get_param: param=' || gate_fail "GET_PARAM log not in vmlinux"
aarch64-linux-gnu-strings vmlinux | grep -qF 'p7580_features:' || gate_fail "feature snapshot log not in vmlinux"
note "DRM 1.1, modern param enum/flags, GET_PARAM snapshot + logging, CREATE_BO unknown flags warn-ignored"

note "== int95 gate: blob lifetime fix present (list owns a reference) =="
grep -q '0138' "$EVID" || gate_fail "blob lifetime patch 0138 not in the series"
DC=drivers/gpu/drm/drm_crtc.c
grep -q 'static void drm_property_unlist_blob_locked' "$DC" || gate_fail "blob unlist helper missing"
[ "$(grep -c 'drm_property_unlist_blob_locked' "$DC")" -ge 6 ] || gate_fail "blob unlist not used by every teardown path"
grep -q 'kref_get(&blob->refcount)' "$DC" || gate_fail "create_blob does not take the second (caller) reference"
grep -q 'list_del_init(&blob->head_global)' "$DC" || gate_fail "free_blob does not re-init head_global"
grep -q 'list_del_init(&blob->head_file)' "$DC" || gate_fail "free_blob does not re-init head_file"
awk '/list_for_each_entry_safe\(blob, bt, &dev->mode_config.property_blob_list,/{f=1;next} f && /head_global\) \{/{getline; if ($0 ~ /drm_property_unlist_blob_locked/) ok=1; f=0} END{exit !ok}' "$DC" \
	|| gate_fail "mode_config_cleanup does not unlist (still unrefs the shared reference)"
note "0138 blob lifetime: list owns its reference, every teardown unlists (rmmod clean is the runtime gate)"

note "== int95 gate: fbdev torn down before mode_config_cleanup =="
grep -q '0139' "$EVID" || gate_fail "fbdev teardown patch 0139 not in the series"
KF=drivers/gpu/drm/exynos7580/exynos_drm7580_fbdev.c
KM=drivers/gpu/drm/exynos7580/exynos_drm7580_main.c
grep -q '^void exynos7580_fbdev_fini(void)' "$KF" || gate_fail "exynos7580_fbdev_fini not implemented"
grep -q 'void exynos7580_fbdev_fini(void);' drivers/gpu/drm/exynos7580/exynos_drm7580.h || gate_fail "fbdev_fini missing from contract header"
grep -q 'exynos7580_fbdev_fini();' "$KF" || gate_fail "devm release does not delegate to fbdev_fini (not idempotent)"
awk '/static void exynos7580_fbdev_devm_release/{f=1} f && /^}/{exit} f && /exynos7580_fbdev != data/{guard=1} f && /exynos7580_fbdev_fini\(\);/{call=1} END{exit !(guard && call)}' "$KF" \
	|| gate_fail "devm release is not idempotent (no handle guard / no delegating call)"
awk '/static int exynos7580_unload/{f=1} f && /exynos7580_fbdev_fini\(\);/{fin=NR} f && /drm_mode_config_cleanup\(drm\);/{clean=NR} END{exit !(fin && clean && fin < clean)}' "$KM" \
	|| gate_fail "fbdev_fini does not precede drm_mode_config_cleanup in unload"
KO=$(find drivers -name '*exynos*drm*.ko' | head -1)
[ -n "$KO" ] || gate_fail "KMS module .ko not found for the fbdev_fini symbol gate"
aarch64-linux-gnu-nm "$KO" | grep -q ' T exynos7580_fbdev_fini$' \
	|| gate_fail "fbdev_fini missing from the KMS module ($KO)"
note "fb0 fini precedes vblank/mode_config cleanup; devm release idempotent; symbol in the .ko"

note "== int95 gate: atomic commit/check diagnostics =="
DCORE=drivers/gpu/drm/drm_atomic.c
DCRTC=drivers/gpu/drm/exynos7580/exynos_drm7580_crtc.c
DPL=drivers/gpu/drm/exynos7580/exynos_drm7580_plane.c
DMB=drivers/gpu/drm/exynos7580/exynos_drm7580_dmabuf.c
grep -q 'drm_atomic_plane_check: plane=.*src=' "$DCORE" || gate_fail "core plane-check state log missing"
grep -q 'drm_atomic_crtc_check: crtc=' "$DCORE" || gate_fail "core crtc-check state log missing"
grep -q 'requires full modeset' "$DCORE" || gate_fail "allow_modeset diagnosis missing"
grep -q 'driver atomic_check rc=' "$DCORE" || gate_fail "driver atomic_check rc log missing"
grep -q 'exynos7580_dump_atomic_state' "$DCRTC" || gate_fail "state dump helper missing"
grep -q 'prepare_planes rc=' "$DCRTC" || gate_fail "commit stage logs missing"
grep -q 'exynos7580_check_rect: FAIL' "$DPL" || gate_fail "check_rect rule logs missing"
grep -q 'fractional src' "$DPL" || gate_fail "16.16 src log missing"
grep -q 'exynos7580_fb_scanout: fb=' "$DMB" || gate_fail "fb_scanout log missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'drm_atomic_plane_check: plane=' || gate_fail "core plane diagnostic not in vmlinux"
aarch64-linux-gnu-strings vmlinux | grep -qF 'requires full modeset' || gate_fail "allow_modeset diagnostic not in vmlinux"
KO=$(find drivers -name '*exynos*drm*.ko' | head -1)
aarch64-linux-gnu-strings "$KO" | grep -qF 'exynos7580_check_rect: FAIL' || gate_fail "KMS module diagnostic strings missing"
aarch64-linux-gnu-strings "$KO" | grep -qF 'exynos7580_atomic_commit: prepare_planes rc=' || gate_fail "commit stage strings missing from the .ko"
note "atomic core + driver commit/check/plane/scanout diagnostics present (Image + KMS .ko)"

note "== int95 gate: stale link flag cannot force modesets =="
DCRTC=drivers/gpu/drm/exynos7580/exynos_drm7580_crtc.c
grep -q 'atomic_check: needs_full_modeset=' "$DCRTC" || gate_fail "link/modeset decision log missing"
grep -q 'link on, needs_full_modeset cleared' "$DCRTC" || gate_fail "enable_link does not clear the flag"
grep -q 'clearing stale needs_full_modeset' "$DCRTC" || gate_fail "already-on path does not clear the stale flag"
grep -q 'link_off plane-only commit without ALLOW_MODESET' "$DCRTC" || gate_fail "link_off rejection log missing"
if grep -q 'crtc_link.needs_full_modeset && !state->mode_changed' "$DCRTC"; then
	gate_fail "old unconditional needs_full_modeset forcing still present"
fi
grep -q 'activating' "$DCRTC" || gate_fail "activation path not considered"
KO=$(find drivers -name '*exynos*drm*.ko' | head -1)
aarch64-linux-gnu-strings "$KO" | grep -qF 'atomic_check: needs_full_modeset=' || gate_fail "decision log missing from the KMS .ko"
aarch64-linux-gnu-strings "$KO" | grep -qF 'needs_full_modeset cleared' || gate_fail "clear log missing from the KMS .ko"
note "link gating fixed: flag cleared on enable, activation derived, link_off named; module strings present"

note "== int95 gate: commit re-enables a non-ON DECON, disables are logged =="
SHIM=drivers/video/exynos/decon_7580/decon_kms_api.c
grep -q 'decon_kms_commit: state=%d -> enable -> retry' "$SHIM" || gate_fail "commit re-enable log missing"
grep -q 'enable rc=%d state=%d' "$SHIM" || gate_fail "commit enable result log missing"
grep -q 'decon_kms_disable: state=%d by %s' "$SHIM" || gate_fail "disable state/comm log missing"
if awk '/int decon_kms_commit\(struct decon_kms_dev/,/^}/' "$SHIM" | grep -A1 'state != DECON_STATE_ON' | grep -q 'return -EAGAIN'; then
	gate_fail "decon_kms_commit still returns -EAGAIN without enabling"
fi
grep -q 'crtc_disable by %s' drivers/gpu/drm/exynos7580/exynos_drm7580_crtc.c || gate_fail "crtc_disable comm log missing"
grep -q 'dpms mode=%d by %s' drivers/gpu/drm/exynos7580/exynos_drm7580_crtc.c || gate_fail "dpms comm log missing"
grep -q 'disable_link: disabling link state' drivers/gpu/drm/exynos7580/exynos_drm7580_crtc.c || gate_fail "disable_link comm log missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'decon_kms_commit: state=%d -> enable -> retry' || gate_fail "shim re-enable log not in vmlinux"
KO=$(find drivers -name '*exynos*drm*.ko' | head -1)
aarch64-linux-gnu-strings "$KO" | grep -qF 'crtc_disable by %s' || gate_fail "crtc disable log missing from the KMS .ko"
note "commit enable+retry (single), disable/comm logs in shim and module"

note "== int95 gate: DECON window/register dump + ion-free guard =="
SHIM=drivers/video/exynos/decon_7580/decon_kms_api.c
CORE=drivers/video/exynos/decon_7580/decon_core.c
grep -q 'decon_kms_program: nwin=' "$SHIM" || gate_fail "program summary log missing"
grep -q 'decon_kms_program: win\[%u\] kms_state=' "$SHIM" || gate_fail "per-window program log missing"
grep -q 'decon_kms_regs\[%s\]: VIDCON0=' "$SHIM" || gate_fail "register dump missing"
grep -q 'decon_kms_regs\[%s\]: win\[%u\] WINCON=' "$SHIM" || gate_fail "per-window register dump missing"
grep -q 'dump_regs(decon, "after update_regs")' "$SHIM" || gate_fail "register dump not called after update_regs"
grep -q 'dump_regs(decon, "after vsync")' "$SHIM" || gate_fail "register dump not called after vsync"
grep -q 'kms-owned addr' "$CORE" || gate_fail "ion-free skip log missing"
grep -q 'if (!dma->attachment)' "$CORE" || gate_fail "attachment guard missing in decon_free_dma_buf"
aarch64-linux-gnu-strings vmlinux | grep -qF 'decon_kms_regs[%s]: win[%u] WINCON=' || gate_fail "register dump not in vmlinux"
aarch64-linux-gnu-strings vmlinux | grep -qF 'kms-owned addr' || gate_fail "ion-free guard not in vmlinux"
note "window/register dumps and KMS dma-buf free guard present (built-in)"

note "== int95 gate: per-commit frame accounting + buffer sample =="
DCRTC=drivers/gpu/drm/exynos7580/exynos_drm7580_crtc.c
DPL=drivers/gpu/drm/exynos7580/exynos_drm7580_plane.c
DMB=drivers/gpu/drm/exynos7580/exynos_drm7580_dmabuf.c
SHIM=drivers/video/exynos/decon_7580/decon_kms_api.c
grep -q '^unsigned int exynos7580_commit_seq;' "$DCRTC" || gate_fail "commit sequence not defined"
grep -q 'seq=%u start by %s' "$DCRTC" || gate_fail "commit start log missing"
grep -q 'commit_tail done, frames=%u' "$DCRTC" || gate_fail "commit tail frame count log missing"
grep -q 'exynos7580_frames_in_commit++' "$DPL" || gate_fail "per-frame counter not incremented"
grep -q 'exynos7580_plane_commit_frame: seq=%u frame=%u astate=' "$DPL" || gate_fail "per-frame fb/addr log missing"
grep -q 'sample import fb=' "$DMB" || gate_fail "import buffer sample missing"
grep -q 'sample cma fb=' "$DMB" || gate_fail "cma buffer sample missing"
grep -q 'decon_kms_commit: call=%u' "$SHIM" || gate_fail "shim commit call counter missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'decon_kms_commit: call=%u' || gate_fail "shim call log not in vmlinux"
KO=$(find drivers -name '*exynos*drm*.ko' | head -1)
aarch64-linux-gnu-strings "$KO" | grep -qF 'exynos7580_plane_commit_frame: seq=%u frame=%u astate=' || gate_fail "frame log missing from the KMS .ko"
aarch64-linux-gnu-strings "$KO" | grep -qF 'sample import fb=' || gate_fail "sample log missing from the KMS .ko"
note "commit seq/frame counters, fb/addr logs, buffer sampling and shim call counter present"

note "== int95 gate: GPU submit dependency logging =="
SUBMIT=drivers/gpu/drm/panfrost/panfrost7580_submit.c
grep -q 'p7580_submit: jc=0x%llx slot=' "$SUBMIT" || gate_fail "submit summary log missing"
grep -q 'p7580_submit: bo\[%u\] handle=' "$SUBMIT" || gate_fail "per-BO resv log missing"
grep -q 'p7580_submit: inf\[%u\]=' "$SUBMIT" || gate_fail "in-fence log missing"
grep -q 'p7580_deps: seq=%u slot=' "$SUBMIT" || gate_fail "dependency pre-dump missing"
grep -q 'deps_queued++' "$SUBMIT" || gate_fail "dependency counter missing"
grep -q 'deps_queued=%u' "$SUBMIT" || gate_fail "dependency summary log missing"
grep -q 'p7580_resv: submit_no=' "$SUBMIT" || gate_fail "resv publication log missing"
grep -q 'p7580_serial: submit_no=' "$SUBMIT" || gate_fail "serial_prev log missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'p7580_deps: seq=%u slot=' || gate_fail "dependency log not in vmlinux"
aarch64-linux-gnu-strings vmlinux | grep -qF 'p7580_resv: submit_no=' || gate_fail "resv log not in vmlinux"
note "submit/dependency/resv/serial logs present (driver built-in)"

note "== int95 gate: irq masks, erratum 10797, timeout fault diagnostics =="
JOB=drivers/gpu/drm/panfrost/panfrost7580_job.c
JMH=drivers/gpu/drm/panfrost/panfrost7580_jm_hw.h
grep -q 'P7580_GPU_IRQ_MASK_ALL\s*0x10581u' "$JMH" || gate_fail "kbase GPU_IRQ_MASK value missing"
grep -q 'P7580_MMU_INT_MASK\s*0x2008u' "$JMH" || gate_fail "MMU_INT_MASK offset missing"
grep -q 'P7580_MMU_AS_FAULTSTATUS\s*0x1Cu' "$JMH" || gate_fail "AS FAULTSTATUS offset missing"
grep -q 'P7580_GPU_SHADER_CONFIG_T72X\s*0x02000000u' "$JMH" || gate_fail "erratum 10797 value missing"
grep -q 'NOT_STARTED/OK' "$JMH" || gate_fail "JS state labels missing"
grep -q 'static void p7580_job_irq_program' "$JOB" || gate_fail "irq program helper missing"
grep -q 'irq-mask gpu_irq_mask=' "$JOB" || gate_fail "irq mask log missing"
[ "$(grep -c 'p7580_job_irq_program(jd' "$JOB")" -ge 2 ] || gate_fail "irq program not used in init+rearm"
grep -q 'post-reset SHADER_CONFIG=' "$JOB" || gate_fail "post-reset SHADER_CONFIG log missing"
[ "$(grep -c 'p7580_job_post_reset(jd)' "$JOB")" -ge 2 ] || gate_fail "post_reset not used on reset and cycle paths"
grep -q 'TIMEOUT-DIAG' "$JOB" || gate_fail "timeout fault diagnostics missing"
aarch64-linux-gnu-strings vmlinux | grep -qF 'TIMEOUT-DIAG slot=%d js=%s' || gate_fail "timeout diagnostics not in vmlinux"
aarch64-linux-gnu-strings vmlinux | grep -qF 'irq-mask gpu_irq_mask=' || gate_fail "irq mask log not in vmlinux"
note "MMU/GPU masks, 10797 SHADER_CONFIG (reset+cycle) and TIMEOUT fault dump present"


note "== int95 gate: explicit flip state + dead-owner abandon =="
grep -q 'drm_lock_all_force_release' drivers/gpu/drm/drm_crtc.c || gate_fail "force-release helper missing"
grep -q 'pid_alive(owner)' drivers/gpu/drm/drm_crtc.c || gate_fail "owner liveness check missing"
grep -q 'drm_modeset_lock_all_force_unlock(dev, "client file release")' drivers/gpu/drm/drm_fops.c || gate_fail "drm_release hook missing"
grep -q 'get_task_struct(current)' drivers/gpu/drm/drm_crtc.c || gate_fail "owner task ref missing"
grep -q 'put_task_struct' drivers/gpu/drm/drm_crtc.c || gate_fail "owner task put missing"
grep -q 'exynos7580_page_flip: state=' "$KMS/exynos_drm7580_crtc.c" || gate_fail "flip state log missing"
grep -q 'drm_atomic_set_fb_for_plane(plane_state, fb)' "$KMS/exynos_drm7580_crtc.c" || gate_fail "flip fb attach missing"
grep -q 'plane_state->src_w = mode->hdisplay << 16' "$KMS/exynos_drm7580_crtc.c" || gate_fail "flip src/dst missing"
for s in 'drm_modeset_lock_all_force_release: owner' 'drm_modeset_lock_all_force_release: forcing unlock' 'drm_modeset_lock_all_force_release: released'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "abandon log missing from image: $s"
done
for s in 'exynos7580_page_flip: state=%p total_planes='; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$s" || gate_fail "flip log missing from module: $s"
done
note "flip state explicit + fb attached; dead-owner abandon path present"

note "== int95 gate: legacy fb sync + page-flip trace =="
grep -q 'legacy fb NULL -> -EBUSY' drivers/gpu/drm/drm_crtc.c || gate_fail "page-flip EBUSY trace missing"
grep -q 'drm_gem_prime_fd_to_handle: dev=' drivers/gpu/drm/drm_prime.c || gate_fail "prime fd_to_handle trace missing"
grep -q 'crtc->fb = fb' "$KMS/exynos_drm7580_crtc.c" || gate_fail "page_flip legacy fb assignment missing"
grep -q 'c->fb = p->state->fb' "$KMS/exynos_drm7580_crtc.c" || gate_fail "commit legacy fb sync missing"
for s in 'legacy fb NULL -> -EBUSY' 'drm_gem_prime_fd_to_handle: dev='; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "built-in flip trace missing: $s"
done
for s in 'crtc->fb=%p primary->state->fb=%p'; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$s" || gate_fail "module fb-sync log missing: $s"
done
note "legacy fb synced after commits and in page_flip; EBUSY/prime traces present"

note "== int95 gate: owner/depth lock_all =="
grep -q 'lock_all_owner' include/drm/drm_crtc.h || gate_fail "lock_all_owner field missing"
grep -q 'same-task nest by' drivers/gpu/drm/drm_crtc.c || gate_fail "same-task nest log missing"
grep -q 'is not the owner' drivers/gpu/drm/drm_crtc.c || gate_fail "non-owner guard missing"
grep -q 'first nest backtrace follows' drivers/gpu/drm/drm_crtc.c || gate_fail "nest backtrace missing"
for s in 'same-task nest by' 'is not the owner' 'first nest backtrace follows'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "owner lock log missing: $s"
done
note "lock_all owner/depth + backtrace present"

note "== int95 gate: lock_all nesting + gamma + frame check =="
grep -q 'lock_all_depth' include/drm/drm_crtc.h || gate_fail "lock_all_depth field missing"
grep -q 'same-task nest by %s depth=' drivers/gpu/drm/drm_crtc.c || gate_fail "nested lock_all log missing"
grep -q 'nested release by %s depth=' drivers/gpu/drm/drm_crtc.c || gate_fail "nested unlock log missing"
grep -q 'gamma_size = 256' "$KMS/exynos_drm7580_crtc.c" || gate_fail "gamma_size not set"
grep -q 'commits again' "$KMS/exynos_drm7580_plane.c" || gate_fail "duplicate frame check missing"
for s in 'drm_modeset_lock_all: same-task nest by' 'drm_modeset_unlock_all: nested release by'; do
	aarch64-linux-gnu-strings vmlinux | grep -qF "$s" || gate_fail "nesting log missing from image: $s"
done
for s in 'commits again'; do
	aarch64-linux-gnu-strings "$KO" | grep -qF "$s" || gate_fail "module check missing: $s"
done
note "legacy lock_all nests; gamma_size 256; duplicate-frame check present"

note "== int95 gate: 64-bit CMA mmap offset =="
CMA=drivers/gpu/drm/drm_gem_cma_helper.c
grep -q 'static unsigned long get_gem_mmap_offset' "$CMA" || gate_fail "get_gem_mmap_offset still 32-bit"
grep -q 'unsigned long offset = key << PAGE_SHIFT' "$CMA" || gate_fail "offset shift not 64-bit"
grep -q 'unsigned int key' "$CMA" && gate_fail "32-bit key still present"
grep -q 'offset=%lu' "$CMA" || gate_fail "offset log not 64-bit"
aarch64-linux-gnu-strings vmlinux | grep -qF 'get_gem_mmap_offset obj=%p key=' || gate_fail "offset trace missing from image"
note "mmap offset returned as unsigned long (no 32-bit truncation)"

note "== int95 gate: mmap / PRIME traces =="
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

note "== int95 gate: modeset lock_all simplified + logged =="
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

note "== int95 gate: initial modeset uses the atomic state + explicit commit =="
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

note "== int95 gate: bounded vsync thread stop =="
API=drivers/video/exynos/decon_7580/decon_kms_api.c
IDRV=drivers/video/exynos/decon_7580/decon-int_drv.c
VT=$(sed -n '/^static int decon_wait_for_vsync_thread/,/^}/p' "$IDRV")
printf '%s' "$VT" | grep -q 'wait_event_interruptible_timeout' || gate_fail "vsync thread still waits without timeout"
printf '%s' "$VT" | grep -q 'HZ / 10' || gate_fail "vsync poll interval missing"
printf '%s' "$VT" | grep -q 'kthread_should_stop() || decon->kms_teardown' || gate_fail "vsync thread does not break on teardown"
grep -q 'wake_up_interruptible_all(&decon->vsync_info.wait)' "$API" || gate_fail "teardown does not wake vsync waiters"
grep -q 'wake_up_interruptible_all(&decon->wait_frmdone)' "$API" || gate_fail "teardown does not wake frmdone waiters"
note "vsync thread: 100 ms timed wait + stop/teardown break; teardown wakes both queues"

note "== int95 gate: fb postcondition + pan guard =="
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

note "== int95 gate: VIDINTCON0 trace + non-blocking teardown =="
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

note "== int95 gate: unconditional FRAME force-arm =="
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

note "== int95 gate: bounded vsync + per-frame arming + lockup panic =="
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

note "== int95 gate: FRAME irq re-arm, ESD idempotency, diagnostics =="
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

note "== int95 gate: fbdev LUT + destroy hooks =="
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

note "== int95 gate: DECON powered before the first register read =="
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

note "== int95 gate: pstore capture available =="
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] ess_pstore_init$' || gate_fail "ess_pstore_init not linked"
grep -q '^CONFIG_PSTORE_CONSOLE=y' .config || gate_fail "pstore console off"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'log_pstore' || gate_fail "ess ramoops region name missing"
note "pstore: ess_pstore_init + PSTORE_CONSOLE + log_pstore region; no DT edit"

note "== int95 gate: KMS objects clean =="
[ -f drivers/gpu/drm/exynos7580/built-in.o ] || gate_fail "exynos7580/built-in.o missing"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "vmlinux predates this build (stale prebuilt vmlinux)"
W=$(grep -E 'warning:' "$LOG" | grep -c 'drivers/gpu/drm/exynos7580/')
[ "$W" = 0 ] || gate_fail "exynos7580 sources produced $W warnings"
grep -E 'CC +drivers/gpu/drm/exynos7580/' "$LOG" | tee -a "$EVID"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
ls "$PRISTINE/drivers/gpu/drm/exynos7580" 2>&1 | tee -a "$EVID"

STAGE=${STAGE:-$EXT/stage-int95}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
# Single line: the kernel splits its command line on spaces only, a newline
# inside the stored string would corrupt the preceding token.
CL=$(tr '\n' ' ' < "$STAGE/cmdline" | sed 's/  */ /g; s/ $//')
# int95: all log/lockup options on one line.
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
	"$STAGE/dt.img" "$EXT/kernel-boot-int95.img" "$(cat "$STAGE/cmdline")"
ls -l "$EXT/kernel-boot-int95.img" | tee -a "$EVID"
sha256sum "$EXT/kernel-boot-int95.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int95"
sha256sum "$EXT/Image-int95" | tee -a "$EVID"
cp drivers/gpu/drm/drm_kms_helper.ko "$EXT/drm_kms_helper-int95.ko"
cp drivers/gpu/drm/exynos7580/exynos-drm7580.ko "$EXT/exynos-drm7580-int95.ko"
sha256sum "$EXT/drm_kms_helper-int95.ko" "$EXT/exynos-drm7580-int95.ko" | tee -a "$EVID"

note "== int95 gate: PASS (KMS as module, ... + 0128 owner/depth lock_all + 0129 flip/prime trace + 0130 legacy fb sync + 0133 lock force-release + 0134 explicit flip state; no KMS symbols in vmlinux; 2 .ko, imports resolved; LOG_BUF_SHIFT=18 + ignore_loglevel; pstore; stock-dt boot) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
