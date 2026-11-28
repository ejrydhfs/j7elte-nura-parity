#!/bin/sh
# N x 720x1280 on the current image, content-verified and attributed.
# Classifier is self-tested against recorded positives before any run.
#
# Attribution: our jc set = dispatch lines with seq<=10 (our DRM file's seq
# starts at 0; other clients carry high seq). A fault is OURS when the 4 KiB
# page of its VA (va & ~0xFFF) is the page of one of our jc.
# Verdict: FAIL = verify MISMATCH | own_timeout | own_fault | rc!=0
#          VOID = foreign recover-done inside the run window
#          PASS = none of the above
#
# FALSIFICATION CONDITIONS (descriptor-lifetime mechanism):
#  - a fault at a VA never mapped-then-unmapped falsifies "freed page still read";
#  - shadow kept mapped + N content-verified runs still failing => lifetime
#    necessary but not sufficient (defect is consumer-side);
#  - map_file_bos clearing the fault stream while content verify fails =>
#    faults masked, not fixed.
. /data/mesa203-env.sh >/dev/null 2>&1
S=/data/mesa203-build/mesa203-smoke
D=/data/mesa203-build
N=${1:-30}

# --- classifier: page of VA vs a page list -------------------------------
classify() { # addr pagesfile -> own|foreign
	pg="0x$(printf '%s' "$1" | sed 's/...$//')"
	grep -qx "$pg" "$2" && echo own || echo foreign
}
selftest() {
	t=/tmp/st.$$.pg
	printf '0x3835000\n0x3835100\n0x3835200\n' | sed 's/...$//' > "$t"
	a=$(classify 3835240 "$t")     # recorded failing run8: node2+0x40
	b=$(classify 24864000 "$t")    # recorded foreign (KWin) VA
	rm -f "$t"
	if [ "$a" = own ] && [ "$b" = foreign ]; then
		echo "classifier selftest: OK (3435240->own, 24864000->foreign)"
	else
		echo "classifier selftest: BROKEN (a=$a b=$b) — refusing to run"
		exit 9
	fi
}
if [ "${1:-}" = "selftest" ]; then selftest; exit 0; fi
selftest

echo "### $(uname -v) | rootfs_vmlinuz=$(sha256sum /boot/vmlinuz 2>/dev/null | cut -c1-16) (NOT the flashed image) | fds=$(rootshell /bin/sh -c 'ls -l /proc/*/fd 2>/dev/null | grep -c renderD128') | kwin=$(pgrep -x kwin_wayland | tr '\n' ' ')"
pass=0; fail=0; void=0; nd=0
i=0
while [ "$i" -lt "$N" ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	rootshell /bin/sh -c 'echo 64 > /sys/kernel/debug/p7580/trace_arm' >/dev/null 2>&1
	SMOKE_VERIFY=1 SMOKE_DRAWS=1 timeout 60 "$S" surfaceless >"$D/w-$i.out" 2>&1
	rc=$?
	sleep 0.2
	rootshell /bin/sh -c 'dmesg' > "$D/w-$i.dm"
	grep 'dispatch slot=' "$D/w-$i.dm" |
		sed -n 's/.*jc=0x\([0-9a-f]*\) .*seq=\([0-9]\+\).*/\2 \1/p' |
		awk '$1<=10 {print "0x"$2}' | sort -u > "$D/w-$i.jc"
	sed 's/...$//' "$D/w-$i.jc" > "$D/w-$i.pg"
	ot=0
	while read -r j; do [ -n "$j" ] && grep 'TIMEOUT slot' "$D/w-$i.dm" | grep -q "jc=$j" && ot=$((ot+1)); done < "$D/w-$i.jc"
	of=0; ownva=""
	for a in $(sed -n 's/.*MMU fault AS0: [A-Z_]*\/[A-Z]* stat [0-9a-f]* addr 0*\([0-9a-f]*\)$/\1/p' "$D/w-$i.dm"); do
		if [ "$(classify "$a" "$D/w-$i.pg")" = own ]; then of=$((of+1)); ownva="$ownva $a"; fi
	done
	tf=$(grep -c 'MMU fault' "$D/w-$i.dm")
	tt=$(grep -c 'TIMEOUT slot' "$D/w-$i.dm")
	rec=$(grep -c 'recover-done' "$D/w-$i.dm")
	ver=$(grep -o 'verify .*' "$D/w-$i.out" | tail -1)
	njc=$(wc -l < "$D/w-$i.jc")
	v=PASS
	[ "$njc" -eq 0 ] && v=NO-DATA
	case "$ver" in *MISMATCH*) v=FAIL ;; esac
	[ "$ot" -gt 0 ] && v=FAIL
	[ "$of" -gt 0 ] && v=FAIL
	[ "$rc" -ne 0 ] && v=FAIL
	[ "$v" = PASS ] && [ "$rec" -gt 0 ] && v=VOID
	case "$v" in PASS) pass=$((pass+1));; FAIL) fail=$((fail+1));; VOID) void=$((void+1));; NO-DATA) nd=$((nd+1));; esac
	lat=$(awk '/dispatch slot=0/{if(!d)d=$1}/fence signal slot=0/{if(d&&!f){f=1;printf "%.3f",$1-d}}' "$D/w-$i.dm")
	pin=$(grep -c 'shadow pin' "$D/w-$i.dm")
	unpin=$(grep -c 'shadow unpin' "$D/w-$i.dm")
	printf 'run%-2s %-4s rc=%s own_fault=%s own_timeout=%s foreign_fault=%s foreign_timeout=%s rec=%s d2f=%s pin=%s unpin=%s ownVA=[%s] %s\n' \
		"$i" "$v" "$rc" "$of" "$ot" "$((tf-of))" "$((tt-ot))" "$rec" "${lat:-n/a}" "$pin" "$unpin" "${ownva# }" \
		"$(printf '%s' "$ver" | sed 's/verify //')"
	i=$((i + 1))
done
echo "TOTAL pass=$pass fail=$fail void=$void no_data=$nd of $N"
