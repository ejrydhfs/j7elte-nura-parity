#!/bin/sh
# Knob matrix on the shipped Mesa 20.3.5 stack:
#  A: DRAWS=2 with SMOKE_GAP_MS 0/100/500  (lifetime vs decaying-copy test)
#  B: DRAWS=1 repeated                 (per-draw hang probability)
#  C: DRAWS=2 with SMOKE_PAD=1024      (is the rate allocator-layout dependent?)
# Prints one compact line per run: faults, timeouts, whether the fragment fence
# signalled, and any OVERLAP / tiler-idle warnings.
. /data/mesa203-env.sh >/dev/null 2>&1
S=/data/mesa203-build/mesa203-smoke

run() { # label draws gap pad
	lbl=$1; draws=$2; gap=$3; pad=$4
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	rootshell /bin/sh -c 'echo 64 > /sys/kernel/debug/p7580/trace_arm' >/dev/null 2>&1
	SMOKE_DRAWS=$draws SMOKE_GAP_MS=$gap SMOKE_PAD=$pad \
		SMOKE_W=64 SMOKE_H=64 timeout 30 "$S" surfaceless >/dev/null 2>&1
	d=$(rootshell /bin/sh -c 'dmesg')
	f=$(printf '%s\n' "$d" | grep -c 'MMU fault')
	t=$(printf '%s\n' "$d" | grep -c 'TIMEOUT slot')
	ff=$(printf '%s\n' "$d" | grep -c 'fence signal slot=0')
	ov=$(printf '%s\n' "$d" | grep -c 'OVERLAP')
	ti=$(printf '%s\n' "$d" | grep -ci 'did not idle')
	printf '%-18s faults=%-3s timeouts=%s frag_fence=%s overlap=%s tiler_idle=%s\n' \
		"$lbl" "$f" "$t" "$ff" "$ov" "$ti"
}

i=0
while [ $i -lt 5 ]; do run "A gap=0 #$i"   2 0    0;    i=$((i+1)); done
i=0
while [ $i -lt 5 ]; do run "A gap=100 #$i" 2 100  0;    i=$((i+1)); done
i=0
while [ $i -lt 5 ]; do run "A gap=500 #$i" 2 500  0;    i=$((i+1)); done
i=0
while [ $i -lt 8 ]; do run "B draws=1 #$i" 1 0    0;    i=$((i+1)); done
i=0
while [ $i -lt 4 ]; do run "C pad=1024 #$i" 2 0   1024; i=$((i+1)); done
