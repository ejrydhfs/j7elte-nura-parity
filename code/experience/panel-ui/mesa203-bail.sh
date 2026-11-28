#!/bin/sh
# Does the #43 stall involve a dispatch bail? A bailed hw_submit prints no
# "dispatch slot=" line, so compare dispatched jc count against the 9 jobs a
# DRAWS=2 run submits (3+4 chain nodes + 2 fragments), and look for WARNINGs.
. /data/mesa203-env.sh >/dev/null 2>&1
S=/data/mesa203-build/mesa203-smoke
D=/data/mesa203-build
i=0
while [ "$i" -lt 4 ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	rootshell /bin/sh -c 'echo 64 > /sys/kernel/debug/p7580/trace_arm' >/dev/null 2>&1
	SMOKE_DRAWS=2 SMOKE_W=64 SMOKE_H=64 timeout 40 "$S" surfaceless >"$D/bail-out-$i.txt" 2>&1
	rc=$?
	sleep 0.3
	rootshell /bin/sh -c 'dmesg' > "$D/bail-dm-$i.txt"
	w=$(grep -c 'WARNING' "$D/bail-dm-$i.txt")
	nd=$(grep -c 'dispatch slot=' "$D/bail-dm-$i.txt")
	nf=$(grep -c 'fence signal' "$D/bail-dm-$i.txt")
	nt=$(grep -c 'TIMEOUT slot' "$D/bail-dm-$i.txt")
	printf 'run%s rc=%s dispatches=%s fences=%s timeouts=%s warnings=%s | probe:%s\n' \
		"$i" "$rc" "$nd" "$nf" "$nt" "$w" \
		"$(grep -c 'dispatch [0-9]/2 done' "$D/bail-out-$i.txt") draws-done"
	i=$((i + 1))
done
echo "--- ordered events of run0 ---"
grep -E 'dispatch slot=|fence signal|TIMEOUT slot|WARNING|MMU fault' "$D/bail-dm-0.txt" |
	sed 's/^\[ *\([0-9.]*\)\]/\1/;s/ p7580_job: / /;s/ panfrost.*panfrost7580: / /' |
	cut -c1-96 | head -30
