#!/bin/sh
# Resolve the gap=100 fence/timeout counting: keep probe stdout and list every
# dispatch/fence/timeout line in order for each run.
. /data/mesa203-env.sh >/dev/null 2>&1
S=/data/mesa203-build/mesa203-smoke
i=0
while [ "$i" -lt 3 ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	rootshell /bin/sh -c 'echo 64 > /sys/kernel/debug/p7580/trace_arm' >/dev/null 2>&1
	SMOKE_DRAWS=2 SMOKE_GAP_MS=100 SMOKE_W=64 SMOKE_H=64 timeout 30 "$S" \
		surfaceless > /data/mesa203-build/mx-out-$i.txt 2>&1
	echo "rc=$?"
	rootshell /bin/sh -c 'dmesg' > /data/mesa203-build/mx-dm-$i.txt
	echo "--- run $i: probe says ---"
	grep -E 'dispatch [0-9]+/|RESULT|SMOKE: \[' /data/mesa203-build/mx-out-$i.txt | tail -5
	echo "--- run $i: kernel order ---"
	grep -E 'dispatch slot=|fence signal|TIMEOUT slot|MMU fault' \
		/data/mesa203-build/mx-dm-$i.txt |
		sed 's/^\[ *\([0-9.]*\)\]/\1/;s/ p7580_job: / /;s/ MMU fault AS0: / MMU-FAULT /' |
		cut -c1-110
	i=$((i + 1))
done
