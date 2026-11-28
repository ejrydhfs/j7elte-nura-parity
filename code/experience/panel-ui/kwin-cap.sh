#!/bin/sh
# FALSIFICATION CONDITIONS for the descriptor-lifetime mechanism:
#  - a KWin translation fault at a VA that was never mapped then unmapped
#    (i.e. no freed descriptor/BO range) falsifies "freed page still read";
#  - if, with the shadow kept mapped for the file lifetime, 30x 720x1280 with
#    content verify still stalls or renders wrong, the lifetime rule is
#    necessary but not sufficient -> remaining defect is consumer-side;
#  - if map_file_bos alone makes KWin's fault stream vanish while content
#    verify keeps failing, faults were being masked, not fixed.
# KWin mid-animation capture (read-only; run only when Main says the animation
# is up). Records dmesg + per-second debugfs slot dumps + an attribution header.
#   rootshell /bin/sh /data/mesa203-build/kwin-cap.sh [seconds]
secs=${1:-20}
O=/data/mesa203-build
stamp=$(date +%Y%m%dT%H%M%SZ)
hdr="$O/kwin-cap-$stamp.header"
log="$O/kwin-cap-$stamp.txt"
{
	echo "captured: $(date)"
	echo "SOURCES: job.c 6c499118f0e9761e 75757B 11-27_22:12 | submit.c dee90e195f0971a2 81406B 11-28_00:20 | mmu.c 8d94d78332dd71c1 13407B 11-27_23:52 | ioctl.c 82e27080940837ec 16225B 11-27_23:58 | gem.c 897e52805032be2f 74619B | main.c 0dcfcdfc09529a91 32098B | jm_hw.h 048831a59fea02f2 16634B 11-27_16:56 | job.h edbeb91080b56a38 20861B 11-25_13:46 | rootfs_vmlinuz e7ea60747297fdf4 17234120B (rootfs file, NOT the flashed image)"
	uname -a
	echo "renderD128 fds: $(rootshell /bin/sh -c 'ls -l /proc/*/fd 2>/dev/null | grep -c renderD128')"
	echo "kwin pid: $(pgrep -x kwin_wayland | tr '\n' ' ')"
	echo "kwin mesa env lines: $(rootshell /bin/sh -c "tr '\0' '\n' < /proc/$(pgrep -x kwin_wayland)/environ 2>/dev/null" | grep -cE 'LD_LIBRARY_PATH|MESA203')"
	echo "build label: $(uname -v)  (authoritative; rootfs /boot/vmlinuz is NOT the flashed image)"
	echo "job_timeout_ms: $(rootshell /bin/sh -c 'cat /sys/kernel/debug/p7580/job_timeout_ms 2>/dev/null')"
	echo "proc: $(pgrep -a kwin_wayland | head -1)"
} > "$hdr" 2>&1
rootshell /bin/sh -c 'dmesg -c >/dev/null'
i=0
while [ "$i" -lt "$secs" ]; do
	rootshell /bin/sh -c 'cat /sys/kernel/debug/p7580/dump' >/dev/null 2>&1
	sleep 1
	i=$((i + 1))
done
rootshell /bin/sh -c 'dmesg' > "$log"
va=$(sed -n 's/.*MMU fault AS0: [A-Z_]*\/[A-Z]* stat \([0-9a-f]*\) addr 0*\([0-9a-f]*\)/\2 \1/p' "$log" | sort | uniq -c | sort -rn | head -10)
{
	cat "$hdr"
	echo "--- counts ---"
	echo "MMU faults: $(grep -c 'MMU fault' "$log")"
	echo "TRANSLATION: $(grep -c 'TRANSLATION_FAULT' "$log")  ACCESS_FLAG: $(grep -c 'ACCESS_FLAG' "$log")"
	echo "TIMEOUT: $(grep -c 'TIMEOUT slot' "$log")  recover-done: $(grep -c 'recover-done' "$log")"
	echo "OVERLAP: $(grep -c 'OVERLAP' "$log")  tiler-idle: $(grep -ci 'did not idle' "$log")"
	echo "LABEL: shadow_pin=$(grep -c 'shadow pin' "$log") shadow_unpin=$(grep -c 'shadow unpin' "$log") shadow_iova_unmap=$(grep -c 'iova: unmap.*shadow' "$log")"
	echo "--- top VAs (count stat) ---"
	echo "$va"
	dsp=$(grep -c 'dispatch slot=' "$log")
	sub=$(grep -c 'ioctl jc=' "$log")
	flt=$(grep -c 'MMU fault' "$log")
	tmo=$(grep -c 'TIMEOUT slot' "$log")
	rcv=$(grep -c 'recover-done' "$log")
	if [ "$dsp" -eq 0 ] && [ "$sub" -eq 0 ]; then
		verdict="NO-DATA (no dispatches or submits in window; NOT evidence of health)"
	elif [ "$flt" -gt 0 ] || [ "$tmo" -gt 0 ]; then
		verdict="FAIL (faults=$flt timeouts=$tmo)"
	elif [ "$rcv" -gt 0 ]; then
		verdict="VOID (recovery in window)"
	else
		verdict="PASS (submitting: dispatches=$dsp submits=$sub, clean)"
	fi
	echo "VERDICT: $verdict"
	echo "--- distinct VAs ---"
	sed -n 's/.*addr 0*\([0-9a-f]*\)$/\1/p' "$log" | sort -u | tr '\n' ' '
	echo
} | tee "$O/kwin-cap-$stamp.summary"
echo "files: $hdr $log $O/kwin-cap-$stamp.summary"
