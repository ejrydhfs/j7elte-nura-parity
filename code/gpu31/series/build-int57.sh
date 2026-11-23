#!/bin/bash
# int57: int56 base (M0 atomic + core fixes, stock display, modules off) plus
# the KMS fragment config only.  EXYNOS7580_DISPLAY_KMS is created by 0200's
# vendor-Kconfig hunk, extracted and applied alone (no initcall move, no KMS
# sources).  CONFIG_DRM_EXYNOS7580 has no symbol in this tree (no-op): no KMS
# driver exists.  Purpose: is the KMS config / ownership choice what breaks
# boot?
set -u
EXT=${EXT:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31}
REPO=${REPO:-$(cd "$(dirname "$0")/../../.." && pwd)}
PRISTINE=${PRISTINE:-/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
SCR=${SCR:-$EXT/scratch-int57}
LOG=${LOG:-$EXT/logs/int57-build.log}
EVID=${EVID:-$EXT/logs/int57-evidence.log}
export TMPDIR=${TMPDIR:-$EXT/tmp}
mkdir -p "$TMPDIR" "$(dirname "$LOG")"
: > "$EVID"

note() { echo "$1"; echo "$1" >> "$EVID"; }
gate_fail() { note "int57 GATE FAIL: $1"; exit 1; }

rm -rf "$SCR"
cp -a "$PRISTINE" "$SCR"
cd "$SCR" || exit 1

PATCHES=""
for n in 0001 0002 0003 0004 0005; do
	PATCHES="$PATCHES $(echo "$REPO"/patches/drm-atomic/$n-*.patch)"
done
PATCHES="$PATCHES $REPO/patches/drm-kms/0001-core-drm-crtc-helper-atomic-aware-unused-function-teardown.patch"
PATCHES="$PATCHES $REPO/patches/drm-kms/0002-core-drm-fb-helper-fix-id-drmfb.patch"
PATCHES="$PATCHES $REPO/patches/drm-kms/0003-core-drm-gem-cma-helper-arm64-dma-attrs.patch"
PATCHES="$PATCHES $REPO/patches/drm-kms/0005-core-drm-gem-cma-import-free.patch"
for pf in $PATCHES; do [ -f "$pf" ] || gate_fail "missing patch $pf"; done
note "series: $(echo $PATCHES | wc -w) patches (M0 atomic + core helper fixes, no shim/KMS)"
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
note "all series patches applied cleanly (0 fuzz/rejects)"
# int57: 0200's vendor-Kconfig hunk on its own (choice only).
python3 - "$REPO" <<'PYEOF'
import sys
src=open(sys.argv[1]+"/patches/drm-kms/0200-kms-main-bind.patch",errors='replace').read()
lines=src.splitlines(True)
out=[];on=False
for ln in lines:
    if ln.startswith('diff --git '):
        if on: break
        if 'a/drivers/video/exynos/decon_7580/Kconfig' in ln: on=True
    if on: out.append(ln)
open("/tmp/int57-vendor-kconfig.patch","w").writelines(out)
PYEOF
[ -s /tmp/int57-vendor-kconfig.patch ] || gate_fail "could not extract 0200 vendor Kconfig hunk"
git apply --check /tmp/int57-vendor-kconfig.patch || gate_fail "vendor Kconfig hunk does not apply"
git apply /tmp/int57-vendor-kconfig.patch || gate_fail "vendor Kconfig hunk apply failed"
grep -q 'EXYNOS7580_DISPLAY_KMS' drivers/video/exynos/decon_7580/Kconfig || gate_fail "choice not in vendor Kconfig"
note "int57: 0200 vendor Kconfig choice applied alone (no initcall move, no KMS sources)"

# config: stock + pstore console (int51f basis), display on, DRM=y
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
apply_frag "$REPO"/patches/drm-kms/diag/shim-only.fragment
apply_frag "$REPO"/patches/drm-kms-coexist/display-owner-kms.fragment
force_cfg() {
	grep -v "^$1=\|^# $1 is not set$" .config > .config.tmp && mv .config.tmp .config
	if [ "$2" = n ]; then printf '# %s is not set\n' "$1" >> .config
	else printf '%s=%s\n' "$1" "$2" >> .config; fi
}
force_cfg CONFIG_DRM y
yes "" | make ARCH=arm64 olddefconfig > "$EXT/logs/int57-config.log" 2>&1 \
	|| gate_fail "olddefconfig failed"
grep -qE 'recursive dependency|error:' "$EXT/logs/int57-config.log" \
	&& gate_fail "olddefconfig reported a Kconfig error/recursion"

CONFIG_EXPECT="
CONFIG_DRM=y
CONFIG_PSTORE=y
CONFIG_PSTORE_RAM=y
CONFIG_PSTORE_CONSOLE=y
CONFIG_EXYNOS_SNAPSHOT_PSTORE=y
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_EXYNOS_MIPI_DSI=y
CONFIG_PANEL_EA8061_DYNAMIC=y
CONFIG_EXYNOS7580_DISPLAY_KMS=y
CONFIG_CMA_SIZE_MBYTES=32
# CONFIG_DRM_EXYNOS is not set
# CONFIG_FRAMEBUFFER_CONSOLE is not set
# CONFIG_MODULES is not set"
MISSF=$EXT/logs/int57-config-missing.log
: > "$MISSF"
printf '%s\n' "$CONFIG_EXPECT" | while IFS= read -r line; do
	[ -n "$line" ] || continue
	grep -qxF "$line" .config || printf 'missing: %s\n' "$line" >> "$MISSF"
done
[ -s "$MISSF" ] && { cat "$MISSF" | tee -a "$EVID"; gate_fail "config closure failed"; }
note "config closure: display stack stock (vendor DECON on), DRM=y, modules still off, pstore console on"
grep -E 'CONFIG_(DRM|DRM_KMS_HELPER|EXYNOS_DECON_EXYNOS7580|MODULES|PSTORE)=' .config | tee -a "$EVID" || true

# ---------------------------------------------------------------- build
BUILD_START=$(date +%s)
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image > "$LOG" 2>&1
BUILD_RC=$?
[ -f arch/arm64/boot/Image ] || gate_fail "Image missing (build failed; see $LOG)"
[ "$BUILD_RC" = 0 ] || gate_fail "make -j4 Image failed (rc=$BUILD_RC; see $LOG)"
[ "$(stat -c %Y arch/arm64/boot/Image)" -ge "$BUILD_START" ] || gate_fail "stale Image"
[ "$(stat -c %Y vmlinux)" -ge "$BUILD_START" ] || gate_fail "stale vmlinux"
[ "$(grep -c 'error:' "$LOG")" = 0 ] || gate_fail "build log contains errors"
note "build rc=0, Image mtime $(stat -c %y arch/arm64/boot/Image), log $(wc -l < "$LOG") lines, $(grep -c 'warning:' "$LOG") warnings"

# ---------------------------------------------------------------- gates
note "== int57 gate: DRM core + atomic backport in, no shim/KMS =="
[ -f drivers/gpu/drm/drm_atomic.c ] || gate_fail "drm_atomic.c missing"
[ -f drivers/gpu/drm/drm_atomic.o ] || gate_fail "drm_atomic.o not built (DRM core did not compile it)"
[ "$(aarch64-linux-gnu-nm vmlinux | grep -c ' [tT] drm_mode_atomic_ioctl$')" = 1 ] || gate_fail "drm_mode_atomic_ioctl not linked"
grep -q 'DRM_IOCTL_MODE_ATOMIC' drivers/gpu/drm/drm_drv.c || gate_fail "atomic ioctl not wired"
grep -q 'An atomic encoder only provides the atomic_\* callbacks' drivers/gpu/drm/drm_crtc_helper.c \
	|| gate_fail "core fix 0001 (crtc helper) not applied"
grep -q 'strcpy(info->fix.id, "drmfb");' drivers/gpu/drm/drm_fb_helper.c || gate_fail "core fix 0002 (drmfb id) not applied"
grep -q 'arm64 only has the attribute based DMA API' drivers/gpu/drm/drm_gem_cma_helper.c \
	|| gate_fail "core fix 0003 (cma dma attrs) not applied"
grep -q 'Imported dma-buf: the pages belong to the exporter' drivers/gpu/drm/drm_gem_cma_helper.c \
	|| gate_fail "core fix 0005 (cma import free) not applied"
[ ! -f drivers/video/exynos/decon_7580/decon_kms_api.c ] || gate_fail "0100 shim present (not allowed)"
grep -q 'decon_probe_common' drivers/video/exynos/decon_7580/decon_core.c && gate_fail "0100 applied (decon_probe_common present)"
[ ! -d drivers/gpu/drm/exynos7580 ] || gate_fail "KMS driver present (not allowed)"
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] decon_probe$' || gate_fail "vendor DECON probe missing (display should stay on)"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'decon registered successfully' \
	|| gate_fail "vendor DECON string missing (display off?)"
aarch64-linux-gnu-nm vmlinux | grep -E ' [tT] (decon_kms_get|exynos7580_probe)$' && gate_fail "KMS symbols in Image"
[ ! -d drivers/gpu/drm/exynos7580 ] || gate_fail "KMS sources present (not config-only)"
grep -q '^late_initcall(exynos_decon_register);' drivers/video/exynos/decon_7580/decon_core.c \
	|| gate_fail "int57 must keep the stock late_initcall (initcall move belongs to int58)"
grep -q '^late_initcall_sync(exynos_decon_register);' drivers/video/exynos/decon_7580/decon_core.c \
	&& gate_fail "initcall move leaked into int57"
grep -q '^CONFIG_EXYNOS7580_DISPLAY_KMS=y' .config || gate_fail "KMS choice not =y"
note "KMS config-only: choice=y, CMA 32, fbcon off, DRM_EXYNOS off; vendor probe still at late_initcall; no KMS source or symbol"
note "DRM core: drm_atomic.o + atomic ioctl in vmlinux, all four core fixes in-source; vendor DECON intact; no shim/KMS"

note "== int57 gate: pstore capture available =="
aarch64-linux-gnu-nm vmlinux | grep -q ' [tT] ess_pstore_init$' || gate_fail "ess_pstore_init not linked"
grep -q '^CONFIG_PSTORE_CONSOLE=y' .config || gate_fail "pstore console off"
aarch64-linux-gnu-strings arch/arm64/boot/Image | grep -qF 'ramoops' || gate_fail "ramoops string missing"
note "pstore: ess_pstore_init + PSTORE_CONSOLE, log_pstore region, no DT edit"

# ---------------------------------------------------------------- pack
note "== freshness =="
stat -c 'SCR %n birth=%w mtime=%y' "$SCR" | tee -a "$EVID"
STAGE=${STAGE:-$EXT/stage-int57}
mkdir -p "$STAGE"
for f in dt.img ramdisk-new.gz cmdline; do
	[ -f "$STAGE/$f" ] || cp "/tmp/bimg/$f" "$STAGE/$f"
done
[ -f "$STAGE/mkboot.py" ] || cp /tmp/mkboot.py "$STAGE/mkboot.py"
python3 "$STAGE/mkboot.py" arch/arm64/boot/Image "$STAGE/ramdisk-new.gz" \
	"$STAGE/dt.img" "$EXT/kernel-boot-int57.img" "$(cat "$STAGE/cmdline")"
sha256sum "$EXT/kernel-boot-int57.img" | tee -a "$EVID"
cp arch/arm64/boot/Image "$EXT/Image-int57"
sha256sum "$EXT/Image-int57" | tee -a "$EVID"
note "== int57 gate: PASS (int56 base + KMS fragment config only, choice=y via 0200 Kconfig hunk alone, no KMS sources, stock late_initcall, stock-dt boot.img packed) =="

rm -rf "$SCR" "$STAGE"
note "scratch + stage deleted"
