#!/bin/sh
# Poll /sys/kernel/debug/p7580/dump across a SMOKE_DRAWS=2 draw and print
# every snapshot that shows a slot busy (status != 0x0/0x4) or head==tail==jc.
. /data/mesa203-env.sh >/dev/null 2>&1
D=/sys/kernel/debug/p7580/dump
i=0
SMOKE_DRAWS=2 SMOKE_W=64 SMOKE_H=64 /data/mesa203-build/mesa203-smoke surfaceless \
	>/dev/null 2>&1 &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
	s=$(cat "$D" 2>/dev/null)
	if printf '%s\n' "$s" | grep -qE 'slot[0-2] status=0x0*8|slot[0-2] status=0x0*[234]|head=0x[1-9a-f]'; then
		printf '%s\n' "$s" | grep -E 'job_int|slot[0-2] status|last slot' | tr '\n' '|'
		printf ' [poll %s]\n' "$i"
	fi
	i=$((i + 1))
done
wait "$pid"
echo "polls=$i"
