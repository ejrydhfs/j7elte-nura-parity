#!/bin/sh
# Contended existence-only matrix, kernel #43. Our submits are the dispatch
# lines with seq<=40 (our DRM file's seq counter starts at 0); KWin's are the
# high-seq lines from its own file. Faults are attributed by the page of our jc.
. /data/mesa203-env.sh >/dev/null 2>&1
S=/data/mesa203-build/mesa203-smoke
T=/tmp/mx
mkdir -p "$T"
KB=$(uname -a | cut -d' ' -f1-9)
FD=$(rootshell /bin/sh -c 'ls -l /proc/*/fd 2>/dev/null | grep -c renderD128')
KW=$(pgrep -x kwin_wayland | tr '\n' ' ')
echo "### $KB | renderD128_fds=$FD | kwin=$KW"
echo "### job.c sha256 6c499118f0e9761ee753a3f9a183b28ed0e5f6ebba990f1b7a637e8a48728c41 (75757B 22:12) | submit.c 505be084..f81a3b5 (80467B Nov25)"

run() { # label draws gap pad [W H]
	lbl=$1; draws=$2; gap=$3; pad=$4
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	rootshell /bin/sh -c 'echo 64 > /sys/kernel/debug/p7580/trace_arm' >/dev/null 2>&1
	SMOKE_DRAWS=$draws SMOKE_GAP_MS=$gap SMOKE_PAD=$pad \
		SMOKE_W=${5:-64} SMOKE_H=${6:-64} timeout 40 "$S" surfaceless \
		>"$T/out" 2>&1
	rc=$?
	rootshell /bin/sh -c 'dmesg' > "$T/dm"

	grep 'dispatch slot=' "$T/dm" |
		sed -n 's/.*jc=0x\([0-9a-f]*\) .*seq=\([0-9]\+\).*/\2 \1/p' |
		awk '$1<=40 {print "0x"$2}' | sort -u > "$T/ourjc"

	# own fences: fence lines whose seq is one of ours (seq<=40 in a dispatch)
	sed -n 's/.*jc=0x[0-9a-f]* .*seq=\([0-9]\+\).*/\1/p' "$T/dm" | awk '$1<=40' | sort -u > "$T/ourseq"
	of_fence=0
	while read -r q; do
		[ -n "$q" ] && grep -q "fence signal slot=0 seq=$q " "$T/dm" && of_fence=$((of_fence+1))
	done < "$T/ourseq"

	# own timeouts: TIMEOUT line whose jc is ours
	ot=0
	while read -r j; do
		[ -n "$j" ] && grep 'TIMEOUT slot' "$T/dm" | grep -q "jc=$j" && ot=$((ot+1))
	done < "$T/ourjc"
	tt=$(grep -c 'TIMEOUT slot' "$T/dm")

	# own faults: fault addr page equals the page of one of our jcs
	sed 's/...$//' "$T/ourjc" > "$T/ourpg"
	of=0
	for a in $(sed -n 's/.*MMU fault AS0: [A-Z_]*\/[A-Z]* stat [0-9a-f]* addr 0*\([0-9a-f]\{4\}\)$/\1/p' "$T/dm" | sed 's/^/0x/'); do
		grep -qx "$a" "$T/ourpg" && of=$((of+1))
	done
	tf=$(grep -c 'MMU fault' "$T/dm")
	lat=$(grep -o 'recover-done.*latched=0x[0-9a-f]*' "$T/dm" | sed 's/.*latched=//' | tr '\n' ',' )

	printf '%-20s rc=%s own_jc=%-2s own_fence=%s own_timeout=%s own_fault=%s | foreign: timeout=%s fault=%s | latched=%s\n' \
		"$lbl" "$rc" "$(wc -l < "$T/ourjc")" "$of_fence" "$ot" "$of" \
		"$((tt-ot))" "$((tf-of))" "${lat:-none}"
}

i=0; while [ $i -lt 15 ]; do run "D1 #$i" 1 0 0; i=$((i+1)); done
i=0; while [ $i -lt 8 ];  do run "D2 #$i" 2 0 0; i=$((i+1)); done
i=0; while [ $i -lt 5 ];  do run "D2 gap500 #$i" 2 500 0; i=$((i+1)); done
i=0; while [ $i -lt 5 ];  do run "D2 g500pad #$i" 2 500 1024; i=$((i+1)); done
run "720x1280" 1 0 0 720 1280
