#!/bin/sh
# Repeat SMOKE_DRAWS=2 until a slot actually shows 0x08 during polling, and
# print the snapshot(s) that caught it together with the dmesg timestamp.
. /data/mesa203-env.sh >/dev/null 2>&1
D=/sys/kernel/debug/p7580/dump
try=0
while [ "$try" -lt 12 ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	SMOKE_DRAWS=2 SMOKE_W=64 SMOKE_H=64 /data/mesa203-build/mesa203-smoke \
		surfaceless >/dev/null 2>&1 &
	pid=$!
	hit=0
	while kill -0 "$pid" 2>/dev/null; do
		s=$(cat "$D" 2>/dev/null)
		if printf '%s\n' "$s" | grep -qE 'status=0x0*8'; then
			printf 'attempt%s SNAPSHOT %s\n' "$try" \
				"$(printf '%s\n' "$s" | grep -E 'job_int|slot[0-2] status|last slot' | tr '\n' '|')"
			hit=1
		fi
	done
	wait "$pid"
	to=$(rootshell /bin/sh -c 'dmesg' | grep -E 'TIMEOUT slot|recover-done|MMU fault' | sed 's/^\[[ 0-9.]*\] //' | cut -c1-130)
	if [ -n "$to" ]; then
		printf 'attempt%s dmesg: %s\n' "$try" "$(printf '%s' "$to" | tr '\n' ';')"
	fi
	[ "$hit" = 1 ] && break
	try=$((try + 1))
done
echo "tries=$try"
