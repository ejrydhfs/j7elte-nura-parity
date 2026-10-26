#!/bin/sh
# verify-abi.sh - prove kbase_uapi.h matches the kernel's r15p0 wire layout.
#
#   KERNEL_DIR=/path/to/kernel-shared/... tools/verify-abi.sh
#
# 1. builds tools/kbase-layout.c against the r15p0 and r7p0 kernel headers
#    and prints the layouts the kernel really compiles;
# 2. builds kbase-selftest --print-sizes from our header;
# 3. compares the r15p0 layout with ours (and shows the r7p0 deltas, which are
#    the ABI break documented in analysis.md).
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KERNEL_DIR=${KERNEL_DIR:-/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07}
R15="$KERNEL_DIR/drivers/gpu/arm/t7xx/r15p0"
R7="$KERNEL_DIR/drivers/gpu/arm/t72x/r7p0"
CC=${CC:-cc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

[ -f "$R15/mali_kbase_uku.h" ] || { echo "r15p0 headers not found under $R15" >&2; exit 1; }

"$CC" -std=c11 -Wall -Wextra -I"$R15" -DKBASE_LAYOUT_R15P0 \
	"$here/tools/kbase-layout.c" -o "$OUT/layout-r15" 2>/dev/null
"$CC" -std=c11 -Wall -Wextra -I"$R7" \
	"$here/tools/kbase-layout.c" -o "$OUT/layout-r7" 2>/dev/null
"$CC" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -I"$here" \
	"$here/kbase-selftest.c" "$here/kbase.c" -o "$OUT/selftest"

"$OUT/layout-r15" > "$OUT/r15.txt"
"$OUT/layout-r7" > "$OUT/r7.txt"
"$OUT/selftest" --print-sizes > "$OUT/ours.txt"

echo "=== kernel r15p0 (what Nura builds) ==="
cat "$OUT/r15.txt"
echo
echo "=== kernel r7p0 (what the ROM's libGLES_mali.so speaks) ==="
cat "$OUT/r7.txt"
echo
echo "=== kbase_uapi.h ==="
cat "$OUT/ours.txt"

# Compare the entries both files share.  Normalise away the "struct " prefix
# and the banner lines that are not ABI facts.
norm() {
	grep -E '^(offset )?(struct )?(uku_version_check_args|kbase_uk_[a-z_]+|base_jd_[a-z0-9_]+|base_dump_cpu_gpu_counters)([. ])' "$1" \
		| sed 's/struct //g; s/  */ /g' | sort
}
norm "$OUT/r15.txt" > "$OUT/r15.n"
norm "$OUT/ours.txt" > "$OUT/ours.n"
if diff -u "$OUT/r15.n" "$OUT/ours.n"; then
	echo
	echo "ABI MATCH: kbase_uapi.h == r15p0 kernel layout"
else
	echo
	echo "ABI MISMATCH between kbase_uapi.h and r15p0 (see diff above)" >&2
	exit 1
fi
