#!/bin/sh
# kms-dmesg.sh - snapshot/delta of DECON/DSIM/DRM error counters from dmesg.
#
#   kms-dmesg.sh snapshot > /tmp/kms-before.txt
#   ... run the test ...
#   kms-dmesg.sh delta /tmp/kms-before.txt /tmp/kms-after.txt
#
# Failure gate (task criteria): underrun, vsync/update timeout and DSIM error
# deltas must be 0. DRM core and panfrost deltas are diagnostics; set
# STRICT_DRM=1 / STRICT_PF=1 to make them fatal too.
#
# Counters are whole-log grep counts, so dmesg must not wrap during the run
# (a 300-flip soak produces no kernel messages on the happy path).
set -u

PAT_UNDERRUN='underrun \(level|UNDERRUN'
PAT_TIMEOUT='wait for vsync timeout|linecnt_is_zero timeout|decon.*timeout|DECON.*timeout'
PAT_DSIM='dsim.*(timeout|error|fail)|DSIM.*(timeout|ERROR)'
PAT_DRM='\[drm:.*(ERROR|\*ERROR\*)'
PAT_PF='p7580_job: (ERR|TIMEOUT)'

_count() {
	dmesg 2>/dev/null | grep -ciE "$1" || true
}

snapshot() {
	printf 'underrun=%s\n' "$(_count "$PAT_UNDERRUN")"
	printf 'timeout=%s\n' "$(_count "$PAT_TIMEOUT")"
	printf 'dsim=%s\n' "$(_count "$PAT_DSIM")"
	printf 'drm_error=%s\n' "$(_count "$PAT_DRM")"
	printf 'p7580_error=%s\n' "$(_count "$PAT_PF")"
}

_get() {
	sed -n "s/^$1=//p" "$2" 2>/dev/null | head -1
}

delta() {
	before=$1
	after=$2
	if [ ! -r "$before" ] || [ ! -r "$after" ]; then
		printf 'kms-dmesg: missing snapshot (%s / %s)\n' "$before" "$after" >&2
		return 2
	fi
	u=$(( $(_get underrun "$after") - $(_get underrun "$before") ))
	t=$(( $(_get timeout "$after") - $(_get timeout "$before") ))
	d=$(( $(_get dsim "$after") - $(_get dsim "$before") ))
	r=$(( $(_get drm_error "$after") - $(_get drm_error "$before") ))
	p=$(( $(_get p7580_error "$after") - $(_get p7580_error "$before") ))

	fail=0
	[ "$u" -ne 0 ] && fail=1
	[ "$t" -ne 0 ] && fail=1
	[ "$d" -ne 0 ] && fail=1
	if [ "${STRICT_DRM:-0}" = 1 ] && [ "$r" -ne 0 ]; then fail=1; fi
	if [ "${STRICT_PF:-0}" = 1 ] && [ "$p" -ne 0 ]; then fail=1; fi

	printf 'KMSDMESG: underrun_delta=%s timeout_delta=%s dsim_delta=%s drm_delta=%s p7580_delta=%s %s\n' \
		"$u" "$t" "$d" "$r" "$p" "$([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
	return "$fail"
}

case "${1:-}" in
snapshot)
	snapshot
	;;
delta)
	[ $# -eq 3 ] || { printf 'usage: %s delta BEFORE AFTER\n' "$0" >&2; exit 2; }
	delta "$2" "$3"
	;;
*)
	printf 'usage: %s snapshot|delta BEFORE AFTER\n' "$0" >&2
	exit 2
	;;
esac
