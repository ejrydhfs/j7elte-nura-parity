#!/bin/sh
# Verify every parity patch applies against the baseline index of the right
# repository (kernel tree, pmaports, or skip source-tree patches).
set -u
ROOT=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session
KT=$(ls -d "$ROOT"/android_kernel_samsung_universal7580-*)
PM="$ROOT/parity/pmaports"
fail=0
for p in $(find "$ROOT/parity/patches" -name '*.patch' | sort); do
	if echo "$p" | grep -q '/gpu-mesa-kbase/'; then
		echo "SKIP ${p#$ROOT/}  (Mesa source patch; checked against Mesa main)"
		continue
	fi
	case "$p" in
	*"/userspace/review-"*)
		echo "SKIP ${p#$ROOT/}  (dependent fix; covered by the full series)"
		continue
		;;
	esac
	if grep -q '^+++ b/parity/' "$p" || grep -q '^--- a/parity/' "$p"; then
		echo "SKIP ${p#$ROOT/}  (source-tree patch)"
		continue
	fi
	if grep -q '^+++ b/device/archived/' "$p" || grep -q '^--- a/device/archived/' "$p"; then
		tree="$PM"
	else
		tree="$KT"
	fi
	if git -C "$tree" apply --check --cached "$p" 2>/dev/null; then
		echo "OK   ${p#$ROOT/}  -> $(basename "$tree")"
	else
		echo "FAIL ${p#$ROOT/}  -> $(basename "$tree")"
		git -C "$tree" apply --check --cached "$p" 2>&1 | sed 's/^/     /'
		fail=1
	fi
done
exit $fail
