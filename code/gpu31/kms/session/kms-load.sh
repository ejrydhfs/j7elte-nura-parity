#!/bin/sh
# kms-load.sh <rev> - stop the UI and load one KMS test revision.
#
#   ./kms-load.sh int82          # or: ./kms-load.sh 82
#   ./kms-load.sh 82 --force     # load even if the watchdog cannot be armed
#
# Loads, in this order:
#   $KMS_MODDIR/drm_kms_helper-<rev>.ko
#   $KMS_MODDIR/exynos-drm7580-<rev>.ko
#
# Before touching the modules it arms the harness (hardware watchdog pet +
# filtered dmesg logger) through kms-test-arm(8), so a kernel wedge inside
# insmod still resets the SoC instead of needing a battery pull.  It then stops
# the display UI ("rc-service xsession stop" plus a bounded sweep of leftover
# fbwin/Xorg/nura-shell processes), loads both modules, and prints the DRM
# nodes, the DECON/DSIM interrupt counters and the tail of the matching dmesg.
#
# Every *userspace* step runs under timeout(1).  A wedged kernel cannot be
# bounded from userspace -- the watchdog is the bound there; see
# docs/reports/kms-session-auto.md.
#
# Exit status: 0 only when every step succeeded and card0/fb0 exist.

set -u

usage() {
	echo "usage: $0 <rev> [--force]" >&2
	echo "  rev: int82, 82, exynos-drm7580-int82.ko ..." >&2
	echo "  env: KMS_MODDIR (default /home/user), KMS_ARM (kms-test-arm path)" >&2
}

if [ "$(id -u)" -ne 0 ]; then
	echo "kms-load.sh: must run as root" >&2
	exit 1
fi

rev=
force=0
for arg in "$@"; do
	case "$arg" in
	--force) force=1 ;;
	-h|--help) usage; exit 0 ;;
	-*) echo "kms-load.sh: unknown option '$arg'" >&2; usage; exit 2 ;;
	*) [ -n "$rev" ] && { echo "kms-load.sh: more than one revision" >&2; usage; exit 2; }; rev=$arg ;;
	esac
done

if [ -z "$rev" ]; then
	usage
	exit 2
fi
rev=${rev##*/}
rev=${rev%.ko}
case "$rev" in
int*) ;;
[0-9]*) rev="int$rev" ;;
*) echo "kms-load.sh: cannot parse revision '$rev'" >&2; usage; exit 2 ;;
esac

MODDIR=${KMS_MODDIR:-/home/user}
TRACE=${KMS_TEST_TRACE:-/home/user/kms.trace}
BOOTLOG=${KMS_TEST_LOG:-/var/log/kms-test-arm.log}
HELPER="$MODDIR/drm_kms_helper-$rev.ko"
DRIVER="$MODDIR/exynos-drm7580-$rev.ko"

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -n "${KMS_ARM:-}" ]; then
	ARM=$KMS_ARM
elif [ -x /usr/local/sbin/kms-test-arm ]; then
	ARM=/usr/local/sbin/kms-test-arm
elif [ -x "$here/kms-test-arm" ]; then
	ARM=$here/kms-test-arm
else
	ARM=
fi

say() {
	printf '%s\n' "$*"
}
fail=0
failed() {
	printf '  [FAIL] %s\n' "$*" >&2
	fail=1
}
ok() {
	printf '  [ ok ] %s\n' "$*"
}

# run_bounded <seconds> <description> <cmd...>
run_bounded() {
	secs=$1
	desc=$2
	shift 2
	if out=$(timeout "$secs" "$@" 2>&1); then
		ok "$desc"
		return 0
	else
		rc=$?
	fi
	# timeout(1) returns 124 when it had to kill the command.
	if [ "$rc" -eq 124 ]; then
		failed "$desc (timed out after ${secs}s)"
	else
		failed "$desc (rc=$rc)"
	fi
	[ -n "$out" ] && printf '%s\n' "$out" | sed 's/^/         /' >&2
	return 1
}

mod_loaded() {
	# /proc/modules lists loadable modules only; /sys/module also shows
	# built-ins (drm_kms_helper is built into the daily image), which must not
	# block a test module of the same name.
	stem=$(printf '%s' "$1" | tr '-' '_')
	timeout 5 awk -v n="$stem" '$1 == n { found = 1 } END { exit !found }' /proc/modules
}

pgrep_bounded() {
	timeout 10 pgrep -f "$1" 2>/dev/null
}

say "== kms-load $rev ($(date '+%H:%M:%S'))"
say "   modules: $HELPER"
say "           $DRIVER"

# --- 1. preflight ---------------------------------------------------------
say "== preflight"
for f in "$HELPER" "$DRIVER"; do
	if [ -f "$f" ]; then
		ok "$(basename "$f") present ($(wc -c <"$f" | tr -d ' ') bytes)"
	else
		failed "$f missing"
	fi
done
[ "$fail" -eq 0 ] || { say "kms-load: aborted before touching the UI/modules"; exit 1; }

if mod_loaded drm_kms_helper || mod_loaded exynos-drm7580; then
	failed "a KMS module is already loaded; rmmod drm_kms_helper/exynos_drm7580 first"
	say "kms-load: aborted (a stale module would hide a rev mismatch)"
	exit 1
fi

# --- 2. arm the harness ---------------------------------------------------
say "== harness"
if [ -z "$ARM" ]; then
	if [ "$force" -eq 1 ]; then
		say "   WARN: kms-test-arm not found; continuing due to --force (no watchdog!)"
	else
		failed "kms-test-arm not found (install it, set KMS_ARM, or pass --force)"
		exit 1
	fi
elif run_bounded 30 "arm watchdog + dmesg logger" "$ARM" start; then
	printf '%s\n' "$out" | sed 's/^/         /'
else
	if [ "$force" -eq 1 ]; then
		say "   WARN: continuing due to --force (a wedge would need a battery pull)"
		fail=0
	else
		say "kms-load: aborted -- a kernel wedge during insmod would need a battery pull."
		say "          Fix /dev/watchdog or pass --force to load unprotected."
		exit 1
	fi
fi

# --- 3. stop the UI -------------------------------------------------------
# fbwin/fbdev Xorg hold the framebuffer and the modeset path; the service stop
# covers the supervised tree, the sweep catches survivors of earlier runs.
say "== stop UI"
if timeout 30 rc-service xsession stop >/dev/null 2>&1; then
	ok "rc-service xsession stop"
else
	# "not started"/"stopped" is fine -- the sweep below is the real check.
	say "   note: rc-service xsession stop returned non-zero (probably already stopped)"
fi
for pat in 'start-ui\.sh' 'fbwin' 'Xorg'; do
	timeout 10 pkill -TERM -f "$pat" 2>/dev/null || true
done
i=0
while [ "$i" -lt 20 ] && [ -n "$(pgrep_bounded 'start-ui\.sh|fbwin|Xorg')" ]; do
	sleep 0.25 2>/dev/null || sleep 1
	i=$((i + 1))
done
left=$(pgrep_bounded 'start-ui\.sh|fbwin|Xorg')
if [ -n "$left" ]; then
	timeout 10 pkill -KILL -f 'start-ui\.sh|fbwin|Xorg' 2>/dev/null || true
	sleep 1
	left=$(pgrep_bounded 'start-ui\.sh|fbwin|Xorg')
fi
if [ -n "$left" ]; then
	failed "UI processes still alive (pid(s): $(printf '%s' "$left" | tr '\n' ' '))"
	say "kms-load: aborted; the framebuffer is still held"
	exit 1
fi
ok "no fbwin/Xorg/start-ui.sh left"

# --- 4. load modules ------------------------------------------------------
say "== load"
run_bounded 30 "insmod drm_kms_helper-$rev.ko" insmod "$HELPER" || { say "   see $TRACE"; }
if [ "$fail" -ne 0 ]; then
	if [ -d /sys/module/drm_kms_helper ]; then
		say "   note: drm_kms_helper is BUILT INTO this kernel; the test kernel"
		say "         must build it as a module (CONFIG_DRM_KMS_HELPER=m)"
	fi
	say "== dmesg tail"
	timeout 10 dmesg 2>/dev/null | grep -iE 'drm|exynos|decon|insmod|module' | tail -n 20 | sed 's/^/  /'
	say "kms-load: FAILED loading the helper (no driver load attempted)"
	exit 1
fi
run_bounded 60 "insmod exynos-drm7580-$rev.ko" insmod "$DRIVER"

# --- 5. verify nodes ------------------------------------------------------
say "== nodes"
timeout 10 ls -l /dev/dri/ 2>&1 | sed 's/^/  /'
timeout 10 ls -l /dev/fb0 2>&1 | sed 's/^/  /'
if [ -c /dev/dri/card0 ]; then
	ok "/dev/dri/card0 present"
else
	failed "/dev/dri/card0 missing"
fi
if [ -c /dev/fb0 ] || [ -e /sys/class/graphics/fb0 ]; then
	ok "fb0 present ($(timeout 5 cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || echo '?') @ $(timeout 5 cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null || echo '?')bpp)"
else
	failed "fb0 missing"
fi

# --- 6. interrupts --------------------------------------------------------
say "== irqs"
timeout 10 grep -E 'decon|dsim' /proc/interrupts | sed 's/^/  /' || say "   no decon/dsim lines"

irq_sum() {
	timeout 5 awk '/decon_fb/ {for (i = 2; i <= NF; i++) if ($i ~ /^[0-9]+$/) s += $i} END {print s + 0}' /proc/interrupts 2>/dev/null || echo 0
}
irq_a=$(irq_sum)
sleep 1
irq_b=$(irq_sum)
if [ "$irq_b" -gt "$irq_a" ] 2>/dev/null; then
	ok "decon_fb IRQ total $irq_b (+$((irq_b - irq_a)) in 1s)"
else
	failed "decon_fb IRQ not advancing (total $irq_b, +0 in 1s)"
fi

# --- 7. dmesg tail --------------------------------------------------------
say "== dmesg"
timeout 10 dmesg 2>/dev/null | grep -iE 'decon|exynos|drm|teardown|vsync' | tail -n 15 | sed 's/^/  /'
[ -e "$TRACE" ] && say "   persisted trace: $TRACE"
[ -e "$BOOTLOG" ] && say "   harness log:     $BOOTLOG"

if [ "$fail" -ne 0 ]; then
	say "kms-load: FAILED for $rev (see above; the trace keeps the last good snapshot)"
	say "          to retry cleanly: rmmod exynos_drm7580; rmmod drm_kms_helper"
	exit 1
fi
say "kms-load: OK for $rev"
exit 0
