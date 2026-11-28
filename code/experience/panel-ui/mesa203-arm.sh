#!/bin/sh
# Re-arm the driver trace budget, run SMOKE_DRAWS=2, keep the capture that hangs.
. /data/mesa203-env.sh >/dev/null 2>&1
A=/sys/kernel/debug/p7580/trace_arm
n=0
while [ "$n" -lt 6 ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	rootshell /bin/sh -c "echo 64 > $A" >/dev/null 2>&1
	SMOKE_DRAWS=2 SMOKE_W=64 SMOKE_H=64 /data/mesa203-build/mesa203-smoke \
		surfaceless >/dev/null 2>&1
	rootshell /bin/sh -c 'dmesg' > /data/mesa203-build/dm-arm-$n.txt
	if grep -q 'TIMEOUT slot' /data/mesa203-build/dm-arm-$n.txt; then
		echo "HANG in attempt $n"
		break
	fi
	n=$((n + 1))
done
echo "attempts=$n"
grep -E 'trace re-armed|dispatch slot=|post-start|chain jc=|TIMEOUT slot|recover-done' \
	/data/mesa203-build/dm-arm-$n.txt | sed 's/^\[[ 0-9.]*\] //' | cut -c1-165
echo "--- faults ---"
grep -E 'MMU fault' /data/mesa203-build/dm-arm-$n.txt | sed 's/^\[[ 0-9.]*\] //' | sort | uniq -c | head
