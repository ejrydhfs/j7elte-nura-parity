#!/bin/sh
# memwatch.sh - read-only memory sampler for the J7 (exynos7580, 1.4 GB) session gate.
#
# Runs on the phone (Alpine/OpenRC, busybox ash) or on the host over ssh:
#   ssh user@172.16.42.1 'sh -s -- -t 1800 -i 5' < memwatch.sh > mem.csv
#   memwatch.sh -t 1800 -i 5 -o /tmp/mem.csv            # when installed on device
#
# Why this is not just `free`:
#   * the kernel is 3.10.108: /proc/meminfo has NO MemAvailable (added in 3.14)
#     and there is no /proc/pressure/memory (added in 4.20). mem_avail_calc is
#     the 3.14 SI_MEM_AVAILABLE formula evaluated in userspace:
#         MemFree + Active(file) + Inactive(file) + SReclaimable - min_free_kbytes
#     floored at 0. It is an upper bound on what a new session may use: page
#     cache counted here has to be dropped first.
#   * zram is started by postmarketos-zram-swap (zramstart). This kernel's zram
#     offers lzo+lz4 only (no zstd), and uses the 2014 mm_stat layout:
#     orig compr mem_used limit max zero migrated. mem_used is the real RAM
#     cost; disksize is only the uncompressed capacity.
#   * dmesg is the only OOM evidence available (no oom_kill in /proc/vmstat on
#     3.10; no cgroup memory controller on this image). The ring held 1359
#     lines / 43 s at the j7elte log rate when measured, so keep -i <= 15 s or
#     an OOM line can scroll out between samples. dmesg_restrict=0, no root.
#
# Output: CSV on stdout (and into -o FILE if given): one header line, one line
# per sample, then #SUMMARY lines and a #RESULT line.
# Exit: 0 pass, 1 threshold exceeded, 2 OOM seen, 3 setup error.
#
# Flags:
#   -i SEC        sampling interval (default 5; keep <= 15 for OOM detection)
#   -n NUM        number of samples (default: 1 if neither -n nor -t given)
#   -t SEC        run duration in seconds (default 0 = unbounded/with -n)
#   -o FILE       also append CSV to FILE
#   --low-mb N    warn when mem_avail_calc < N MiB (default 300)
#   --crit-mb N   fail when mem_avail_calc < N MiB (default 150)
#   -p LIST       comma separated process names to track
#                 (default plasmashell,kwin_wayland,Xorg,nura-shell)
#   -L NAME=MiB   RSS ceiling for a tracked process, repeatable; exceeding it
#                 sets the RSS flag and fails the run
#                 (defaults plasmashell=220,kwin_wayland=160)
#   -q            no per-sample progress on stderr
#
# Nothing is changed: no sysctl, no swap, no zram writes, no files on the
# device unless -o is passed.

set -u

INTERVAL=5
COUNT=0
DURATION=0
OUT=""
LOW_MB=300
CRIT_MB=150
QUIET=0
PROCS="plasmashell,kwin_wayland,Xorg,nura-shell"
RSS_LIMITS="plasmashell=220,kwin_wayland=160"

while [ $# -gt 0 ]; do
	case "$1" in
	-i) INTERVAL="$2"; shift 2 ;;
	-n) COUNT="$2"; shift 2 ;;
	-t) DURATION="$2"; shift 2 ;;
	-o) OUT="$2"; shift 2 ;;
	-p) PROCS="$2"; shift 2 ;;
	-L) RSS_LIMITS="$RSS_LIMITS,$2"; shift 2 ;;
	--low-mb) LOW_MB="$2"; shift 2 ;;
	--crit-mb) CRIT_MB="$2"; shift 2 ;;
	-q) QUIET=1; shift ;;
	-h|--help) sed -n '2,50p' "$0" 2>/dev/null; exit 0 ;;
	*) echo "memwatch: unknown option $1" >&2; exit 3 ;;
	esac
done

case "$INTERVAL" in ''|*[!0-9]*) echo "memwatch: bad interval" >&2; exit 3 ;; esac
[ "$INTERVAL" -ge 1 ] || { echo "memwatch: bad interval" >&2; exit 3; }

# --- output --------------------------------------------------------------

if [ -n "$OUT" ]; then
	exec 3>>"$OUT" || exit 3
fi

emit() { # emit FORMAT [ARGS...]
	printf "$@"
	[ -n "$OUT" ] && printf "$@" >&3
}

# --- readers -------------------------------------------------------------

meminfo_load() {
	eval "$(awk '
		{
			k = $1
			sub(/:$/, "", k)
			gsub(/\(/, "_", k)
			gsub(/\)/, "", k)
			gsub(/[^A-Za-z0-9_]/, "_", k)
			print "MI_" k "=" $2
		}' /proc/meminfo)"
}

mi() { # mi KEY DEFAULT
	eval "v=\${MI_$1:-$2}"
	echo "$v"
}

vmstat_load() {
	eval "$(awk '
		$1 ~ /^(pgscan_direct|pgsteal_direct|pgscan_kswapd|pgsteal_kswapd|pgmajfault|nr_free_pages|pswpout|pswpin)$/ {
			print "VS_" $1 "=" $2
		}' /proc/vmstat)"
}

vs() { eval "v=\${VS_$1:-0}"; echo "$v"; }

oom_count() {
	dmesg 2>/dev/null | grep -ci -E 'Out of memory|oom-kill|Killed process'
}

zram_field() { # zram_field N DEFAULT
	v=$(awk -v i="$1" '{print $i}' /sys/block/zram0/mm_stat 2>/dev/null)
	[ -n "${v:-}" ] || v="$2"
	echo "$v"
}

proc_state() { # name -> pid/rss/hwm
	pid=$(pidof "$1" 2>/dev/null | awk '{print $1}')
	[ -n "${pid:-}" ] || pid=$(pgrep -x "$1" 2>/dev/null | head -1)
	if [ -z "${pid:-}" ]; then
		echo "0/0/0"
		return
	fi
	set -- $(awk '/^VmRSS:|^VmHWM:/{print $2}' /proc/"$pid"/status 2>/dev/null)
	rss=${1:-0}
	hwm=${2:-0}
	echo "$pid/$rss/$hwm"
}

san() { # shell-safe variable suffix for a process name
	printf '%s' "$1" | tr -c 'A-Za-z0-9' '_'
}

rss_limit() { # name -> MiB or empty; the last -L for a name wins
	echo "$RSS_LIMITS" | tr ',' '\n' | awk -F= -v n="$1" \
		'$1==n && $2 != "" {v=$2} END{if (v == "") print ""; else print v}'
}

# --- state ---------------------------------------------------------------

emit '%s\n' 'ts,elapsed_s,mem_total,mem_free,mem_avail_calc,cached,active_file,inactive_file,sreclaimable,shmem,anon,slab,sunreclaim,swap_total,swap_free,zram_disksize,zram_orig,zram_compr,zram_mem_used,zram_mem_limit,zram_used_max,pgscan_direct,pgsteal_direct,pgmajfault,oom_total,flags,procs'

START=$(date +%s)
OOM_BASE=$(oom_count)
OOM_TOTAL=$OOM_BASE
OOM_SEEN=0
LOW_SEEN=0
CRIT_SEEN=0
RSS_SEEN=0
RESTART_SEEN=0
N=0
MIN_AVAIL=999999999
MAX_ZRAM_USED=0
MAX_OOM=0
PREV_SCAN=""
PREV_MAJ=""

for p in $(echo "$PROCS" | tr ',' ' '); do
	st=$(proc_state "$p")
	eval "FIRSTPID_$(san "$p")=${st%%/*}"
done

if [ "$QUIET" -eq 0 ]; then
	printf 'memwatch: interval=%ss count=%s duration=%ss low=%sMiB crit=%sMiB procs=%s\n' \
		"$INTERVAL" "$COUNT" "$DURATION" "$LOW_MB" "$CRIT_MB" "$PROCS" >&2
fi

# --- loop ----------------------------------------------------------------

while :; do
	NOW=$(date +%s)
	ELAPSED=$((NOW - START))

	meminfo_load
	vmstat_load

	TOTAL=$(mi MemTotal 0)
	FREE=$(mi MemFree 0)
	ACTF=$(mi Active_file 0)
	INAF=$(mi Inactive_file 0)
	SRECL=$(mi SReclaimable 0)
	CACHED=$(mi Cached 0)
	SHMEM=$(mi Shmem 0)
	ANON=$(mi AnonPages 0)
	SLAB=$(mi Slab 0)
	SUNRECL=$(mi SUnreclaim 0)
	SWT=$(mi SwapTotal 0)
	SWF=$(mi SwapFree 0)
	MINFREE=$(cat /proc/sys/vm/min_free_kbytes 2>/dev/null || echo 0)

	AVAIL=$((FREE + ACTF + INAF + SRECL - MINFREE))
	[ "$AVAIL" -lt 0 ] && AVAIL=0

	Z_DISK=$(cat /sys/block/zram0/disksize 2>/dev/null || echo 0)
	Z_ORIG=$(zram_field 1 0)
	Z_COMPR=$(zram_field 2 0)
	Z_USED=$(zram_field 3 0)
	Z_LIMIT=$(zram_field 4 0)
	Z_MAX=$(zram_field 5 0)

	SCAN=$(vs pgscan_direct)
	STEAL=$(vs pgsteal_direct)
	MAJ=$(vs pgmajfault)
	if [ -n "$PREV_SCAN" ]; then
		DSCAN=$((SCAN - PREV_SCAN))
		DMAJ=$((MAJ - PREV_MAJ))
	else
		DSCAN=0
		DMAJ=0
	fi
	PREV_SCAN=$SCAN
	PREV_MAJ=$MAJ

	OOM_TOTAL=$(oom_count)
	[ "$OOM_TOTAL" -gt "$OOM_BASE" ] && OOM_SEEN=1

	FLAGS=""
	[ "$OOM_SEEN" -eq 1 ] && FLAGS="$FLAGS OOM"
	if [ "$AVAIL" -lt $((LOW_MB * 1024)) ]; then
		FLAGS="$FLAGS LOW"
		LOW_SEEN=1
	fi
	if [ "$AVAIL" -lt $((CRIT_MB * 1024)) ]; then
		FLAGS="$FLAGS CRIT"
		CRIT_SEEN=1
	fi

	PROCSTR=""
	for p in $(echo "$PROCS" | tr ',' ' '); do
		st=$(proc_state "$p")
		pid=${st%%/*}
		rest=${st#*/}
		rss=${rest%%/*}
		hwm=${rest##*/}
		key=$(san "$p")
		eval "first=\${FIRSTPID_$key:-0}"
		if [ "$first" = "0" ] && [ "$pid" != "0" ]; then
			eval "FIRSTPID_$key=$pid"
		elif [ "$pid" != "$first" ] && [ "$first" != "0" ]; then
			FLAGS="$FLAGS RESTART:$p"
			RESTART_SEEN=1
		fi
		lim=$(rss_limit "$p")
		if [ -n "$lim" ] && [ "$rss" -gt $((lim * 1024)) ]; then
			FLAGS="$FLAGS RSS:$p"
			RSS_SEEN=1
		fi
		PROCSTR="${PROCSTR}${p}=${pid}/${rss}/${hwm};"
	done

	[ "$AVAIL" -lt "$MIN_AVAIL" ] && MIN_AVAIL=$AVAIL
	[ "$Z_USED" -gt "$MAX_ZRAM_USED" ] && MAX_ZRAM_USED=$Z_USED
	[ "$OOM_TOTAL" -gt "$MAX_OOM" ] && MAX_OOM=$OOM_TOTAL

	FLAGS=${FLAGS# }
	emit '%s\n' "$NOW,$ELAPSED,$TOTAL,$FREE,$AVAIL,$CACHED,$ACTF,$INAF,$SRECL,$SHMEM,$ANON,$SLAB,$SUNRECL,$SWT,$SWF,$Z_DISK,$Z_ORIG,$Z_COMPR,$Z_USED,$Z_LIMIT,$Z_MAX,$DSCAN,$STEAL,$DMAJ,$OOM_TOTAL,${FLAGS:-ok},$PROCSTR"

	if [ "$QUIET" -eq 0 ]; then
		printf 'memwatch: t=%ss avail=%dMiB shmem=%dMiB zram=%dMiB oom=%d flags=%s\n' \
			"$ELAPSED" $((AVAIL / 1024)) $((SHMEM / 1024)) $((Z_USED / 1024)) \
			"$OOM_TOTAL" "${FLAGS:-ok}" >&2
	fi

	N=$((N + 1))
	if [ "$COUNT" -gt 0 ] && [ "$N" -ge "$COUNT" ]; then break; fi
	if [ "$DURATION" -gt 0 ] && [ "$ELAPSED" -ge "$DURATION" ]; then break; fi
	if [ "$COUNT" -eq 0 ] && [ "$DURATION" -eq 0 ]; then break; fi
	sleep "$INTERVAL"
done

# --- summary -------------------------------------------------------------

RATIO="-"
if [ "$Z_COMPR" -gt 0 ] && [ "$Z_ORIG" -gt 0 ]; then
	RATIO=$(awk -v o="$Z_ORIG" -v c="$Z_COMPR" 'BEGIN{printf "%.2f", o/c}')
fi

RC=0
[ "$CRIT_SEEN" -eq 1 ] && RC=1
[ "$RSS_SEEN" -eq 1 ] && RC=1
[ "$RESTART_SEEN" -eq 1 ] && RC=1
[ "$OOM_SEEN" -eq 1 ] && RC=2

emit '#SUMMARY samples=%s elapsed=%ss\n' "$N" "$(( $(date +%s) - START ))"
emit '#SUMMARY mem_total=%sMiB min_avail=%sMiB low_limit=%sMiB crit_limit=%sMiB low_hit=%s crit_hit=%s\n' \
	"$((TOTAL / 1024))" $((MIN_AVAIL / 1024)) "$LOW_MB" "$CRIT_MB" "$LOW_SEEN" "$CRIT_SEEN"
emit '#SUMMARY zram disksize=%sMiB orig=%sMiB compr=%sMiB used=%sMiB used_max=%sMiB limit=%sMiB ratio=%s algo=%s\n' \
	$((Z_DISK / 1048576)) $((Z_ORIG / 1048576)) $((Z_COMPR / 1048576)) \
	$((Z_USED / 1048576)) $((Z_MAX / 1048576)) $((Z_LIMIT / 1048576)) "$RATIO" \
	"$(cat /sys/block/zram0/comp_algorithm 2>/dev/null || echo none)"
emit '#SUMMARY oom_base=%s oom_end=%s oom_new=%s rss_limit_hit=%s restart=%s\n' \
	"$OOM_BASE" "$MAX_OOM" $((MAX_OOM - OOM_BASE)) "$RSS_SEEN" "$RESTART_SEEN"
for p in $(echo "$PROCS" | tr ',' ' '); do
	last=$(proc_state "$p")
	eval "first=\${FIRSTPID_$(san "$p"):-0}"
	limv=$(rss_limit "$p")
	[ -n "$limv" ] || limv="-"
	emit '#SUMMARY proc %s first_pid=%s last_pid=%s rss=%s hwm=%s limit=%sMiB\n' \
		"$p" "$first" "${last%%/*}" \
		"$(echo "$last" | cut -d/ -f2)" "$(echo "$last" | cut -d/ -f3)" "$limv"
done
if [ "$RC" -eq 0 ]; then
	emit '#RESULT PASS\n'
else
	emit '#RESULT FAIL rc=%s\n' "$RC"
fi

exit "$RC"
