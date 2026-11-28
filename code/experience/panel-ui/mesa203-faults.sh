#!/bin/sh
# Correlate fault class (data 0x8100.. vs descriptor 0x8000..) with
# glFinish wall time and slot timeout, for the Mesa 20.3.5 smoke draw.
i=0
while [ "$i" -lt 8 ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	t0=$(cut -d' ' -f1 /proc/uptime)
	SMOKE_W=64 SMOKE_H=64 timeout 25 sh /data/mesa203-run-smoke.sh surfaceless \
		>/dev/null 2>&1
	t1=$(cut -d' ' -f1 /proc/uptime)
	d=$(rootshell /bin/sh -c 'dmesg')
	data=$(printf '%s\n' "$d" | grep -c 'stat 81000')
	desc=$(printf '%s\n' "$d" | grep -c 'stat 80000')
	to=$(printf '%s\n' "$d" | grep -c 'p7580_job: TIMEOUT slot')
	va=$(printf '%s\n' "$d" | sed -n 's/.*MMU fault AS0: [A-Z_]*\/[A-Z]* stat [0-9a-f]* addr 0*\([0-9a-f]*\).*/\1/p' | tr '\n' ' ')
	printf 'run%s: wall=%s data_faults=%s desc_faults=%s timeouts=%s va=%s\n' \
		"$i" "$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')" \
		"$data" "$desc" "$to" "$va"
	i=$((i + 1))
done
