#!/bin/sh
# kwin-try.sh - run kwin_wayland on the DECON KMS card0 for N seconds and collect one log.
#
# Runs ON the J7 (j7elte) as root; self-elevates through /usr/local/bin/rootshell when
# started as `user`.  OpenRC is used only opportunistically (`rc-service xsession stop`
# when that service exists) - the script works on any boot.  It never flashes and never
# reboots; a KMS boot stays headless (panel dark) after the run, the fbdev image is the
# way back to the UI.
#
# Usage (from the host, under the shared device lock):
#   scp code/gpu31/kms/session/kwin-try.sh user@172.16.42.1:/data/kwin/
#   flock /tmp/gpsdev.lock ssh -o PreferredAuthentications=password \
#        -o PubkeyAuthentication=no user@172.16.42.1 'sh /data/kwin/kwin-try.sh'
#
#   KWIN_TRY_SECS=30 sh kwin-try.sh          # longer window
#   FIX_KWIN=1 sh kwin-try.sh                # install the pinned kwin=6.8.3-r1 rebuild
#   KWIN_REV=int81 sh kwin-try.sh            # pin the module revision (default: newest intNN)
#   KWIN_UNLOAD=0 sh kwin-try.sh             # leave the modules loaded
#
# Exit codes:
#   0  ran the window, OpenGL initialized, no fatal marker
#   1  a fatal marker matched, or the run was incomplete (see summary)
#   2  not root and no rootshell
#   3  boot lacks the atomic KMS core, or the module pair does not match this kernel
#   4  kwin_wayland unusable (Qt ABI) and FIX_KWIN not set
#   5  modules loaded but card0 / a connected connector is missing
#   6  another process holds card0
#
# Env: KWIN_TRY_SECS KWIN_REV KWIN_MODDIR KWIN_DRM_DEVICE KWIN_RENDER_NODE KWIN_LOG_DIR
#      KWIN_SOCKET FIX_KWIN KWIN_UNLOAD KWIN_LOGGING_RULES KWIN_XDG_SESSION_ID
#      KWIN_COMPOSE KWIN_MESA_OVERRIDE
set -u

SECS=${KWIN_TRY_SECS:-20}
REV=${KWIN_REV:-auto}
MODDIR=${KWIN_MODDIR:-/home/user}
DEV=${KWIN_DRM_DEVICE:-/dev/dri/card0}
RENDERNODE=${KWIN_RENDER_NODE:-/dev/dri/renderD128}
OUT=${KWIN_LOG_DIR:-/data/kwinlogs}
SOCK=${KWIN_SOCKET:-kwin-test}
FIX_KWIN=${FIX_KWIN:-0}
UNLOAD=${KWIN_UNLOAD:-1}
RULES=${KWIN_LOGGING_RULES:-kwin_wayland_drm.debug=true;kwin_core.debug=true;kwin_qpa_plugin.debug=true}
SESSIONID=${KWIN_XDG_SESSION_ID:-kwin-try-noop}
COMPOSE=${KWIN_COMPOSE:-O2ES}

# ------------------------------------------------------------ self-elevate ---
NOELEV=0
[ "${1:-}" = "--as-root" ] && { NOELEV=1; shift; }
if [ "$(id -u)" != 0 ] && [ "$NOELEV" = 0 ]; then
	if [ -x /usr/local/bin/rootshell ]; then
		exec /usr/local/bin/rootshell sh "$0" --as-root "$@"
	fi
	echo "kwin-try: must run as root (no /usr/local/bin/rootshell)" >&2
	exit 2
fi

# ---------------------------------------------------------------- logging ----
mkdir -p "$OUT" 2>/dev/null || OUT=/tmp/kwinlogs
mkdir -p "$OUT" 2>/dev/null || { echo "kwin-try: no writable log dir" >&2; exit 2; }
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG=$OUT/kwin-$STAMP.log
SUMMARY=$OUT/summary-$STAMP.txt
PROPS=$OUT/props-$STAMP.log
: >"$LOG" || { echo "kwin-try: cannot write $LOG" >&2; exit 2; }
: >"$SUMMARY"

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
note() { printf '%s\n' "$*" >>"$SUMMARY"; }

fail() { # fail <rc> <text>
	say "KWIN-TRY: FAIL rc=$1 - $2"
	if [ -n "${SUMMARY:-}" ]; then
		note "fail rc=$1: $2"
		note "log: $LOG"
	fi
	say "KWIN-TRY LOG: $LOG"
	exit "$1"
}

say "== kwin-try $STAMP =="
say "kernel: $(uname -a)"

# --------------------------------------------------------------- preflight ---
# Module revision: newest exynos-drm7580-<rev>.ko unless pinned.
# The names are intNN (e.g. int82); the numeric tail picks the newest.
if [ "$REV" = auto ]; then
	newest=0
	newrev=""
	for f in "$MODDIR"/exynos-drm7580-*.ko; do
		[ -e "$f" ] || continue
		b=${f##*/}
		v=${b#exynos-drm7580-}
		v=${v%.ko}
		n=${v##*[!0-9]}
		case "$n" in ''|*[!0-9]*) continue ;; esac
		if [ "$n" -gt "$newest" ]; then
			newest=$n
			newrev=$v
		fi
	done
	[ -n "$newrev" ] || fail 3 "no $MODDIR/exynos-drm7580-*.ko"
	REV=$newrev
fi
HELPER=$MODDIR/drm_kms_helper-$REV.ko
DRIVER=$MODDIR/exynos-drm7580-$REV.ko
[ -f "$HELPER" ] || fail 3 "missing $HELPER"
[ -f "$DRIVER" ] || fail 3 "missing $DRIVER"
say "rev=$REV helper=$(ls -l "$HELPER" | awk '{print $5}')B driver=$(ls -l "$DRIVER" | awk '{print $5}')B"

# vermagic must match the running kernel (module-only iteration, no reflash).
if command -v modinfo >/dev/null 2>&1; then
	vm=$(modinfo -F vermagic "$DRIVER" 2>/dev/null | awk '{print $1}')
	[ -n "$vm" ] && [ "$vm" != "$(uname -r)" ] &&
		fail 3 "module vermagic $vm != kernel $(uname -r)"
fi

# The atomic core is built into the kernel; without it this is not a KMS image.
if [ -r /proc/kallsyms ]; then
	if grep -qE ' (drm_atomic_state_alloc|drm_atomic_helper_commit)$' /proc/kallsyms; then
		say "preflight: atomic KMS core present in /proc/kallsyms"
	else
		fail 3 "kernel has no drm_atomic_state_alloc - this boot is not a KMS image (flash int7x/int8x)"
	fi
else
	say "preflight: WARN /proc/kallsyms unreadable, cannot check the atomic core"
fi

# kwin_wayland must be the Qt 6.12 rebuild, else it dies on relocation.
if ! kwin_wayland --version 2>/dev/null | grep -q 'kwin'; then
	if [ "$FIX_KWIN" = 1 ]; then
		say "preflight: kwin_wayland broken (Qt ABI), installing kwin=6.8.3-r1"
		apk add --no-cache 'kwin=6.8.3-r1' 'kwin-lang=6.8.3-r1' >>"$LOG" 2>&1 ||
			fail 4 "apk add kwin=6.8.3-r1 failed (space? repo?)"
		kwin_wayland --version 2>/dev/null | grep -q 'kwin' ||
			fail 4 "kwin_wayland still unusable after install"
	else
		kwin_wayland --version 2>&1 | tail -2 >>"$SUMMARY"
		fail 4 "kwin_wayland does not start (Qt private ABI); re-run with FIX_KWIN=1"
	fi
fi
say "preflight: $(kwin_wayland --version 2>&1 | head -1)"

# ------------------------------------------------------------------ UI off ---
UI_WAS_UP=0
if command -v rc-service >/dev/null 2>&1; then
	if rc-service xsession status >/dev/null 2>&1; then
		UI_WAS_UP=1
		say "ui: rc-service xsession stop"
		rc-service xsession stop >/dev/null 2>&1
	fi
	if rc-service lightdm status >/dev/null 2>&1; then
		say "ui: rc-service lightdm stop"
		rc-service lightdm stop >/dev/null 2>&1
	fi
fi
pkill -TERM -x fbwin 2>/dev/null
pkill -TERM -x nura-shell 2>/dev/null
pkill -TERM -x Xorg 2>/dev/null
sleep 1
pkill -KILL -x fbwin 2>/dev/null
pkill -KILL -x nura-shell 2>/dev/null
pkill -KILL -x Xorg 2>/dev/null

holders() {
	for d in /dev/fb0 "$DEV"; do
		[ -e "$d" ] || continue
		if command -v fuser >/dev/null 2>&1; then
			fuser "$d" >/dev/null 2>&1 && return 0
		fi
	done
	return 1
}
i=0
while [ "$i" -lt 5 ] && holders; do
	sleep 1
	i=$((i + 1))
done
holders && fail 6 "/dev/fb0 or $DEV still open after stopping the UI"
say "ui: no framebuffer/card0 holders (xsession_was_up=$UI_WAS_UP)"

# ------------------------------------------------------------- test harness --
if [ -x "$MODDIR/wdpet.sh" ]; then
	setsid "$MODDIR/wdpet.sh" >/dev/null 2>&1 </dev/null &
	say "harness: watchdog pet armed (30s DT)"
else
	say "harness: WARN no $MODDIR/wdpet.sh - a kernel hang needs a battery pull"
fi
if [ -x "$MODDIR/logd.sh" ]; then
	setsid "$MODDIR/logd.sh" >/dev/null 2>&1 </dev/null &
	LOGDPID=$!
	say "harness: logd armed (pid $LOGDPID)"
fi
sleep 1

# ---------------------------------------------------------------- modules ----
mod_loaded() { # mod_loaded <name> <name-with-dash>
	grep -qE "^($1|$2) " /proc/modules 2>/dev/null
}

if mod_loaded drm_kms_helper drm-kms-helper; then
	say "mod: drm_kms_helper already loaded"
else
	insmod "$HELPER" >>"$LOG" 2>&1 || fail 3 "insmod $HELPER failed: $(tail -1 "$LOG")"
	say "mod: drm_kms_helper loaded"
fi
if mod_loaded exynos_drm7580 exynos-drm7580; then
	say "mod: exynos_drm7580 already loaded"
else
	insmod "$DRIVER" >>"$LOG" 2>&1 || fail 3 "insmod $DRIVER failed: $(tail -1 "$LOG")"
	say "mod: exynos_drm7580 loaded"
fi

# dmesg -c also preserves everything up to now (boot + load).
dmesg -c >"$OUT/dmesg-load-$STAMP.log" 2>/dev/null || dmesg >"$OUT/dmesg-load-$STAMP.log" 2>/dev/null

i=0
while [ "$i" -lt 10 ] && [ ! -e "$DEV" ]; do
	sleep 1
	i=$((i + 1))
done
[ -e "$DEV" ] || fail 5 "no $DEV after module load (see $OUT/dmesg-load-$STAMP.log)"
say "mod: $DEV present"

# modetest: -M exynos (driver name) first, -D path as fallback.
MT=""
if timeout 10 modetest -M exynos >/dev/null 2>&1; then
	MT="-M exynos"
elif timeout 10 modetest -D "$DEV" >/dev/null 2>&1; then
	MT="-D $DEV"
fi
[ -n "$MT" ] || fail 5 "modetest cannot open the card (see dmesg-load)"

CONN=$(timeout 10 modetest $MT -c 2>/dev/null | awk '$3=="connected"{print $1; exit}')
[ -n "$CONN" ] || fail 5 "no connected connector (modetest -c empty)"
say "drm: connector id=$CONN connected"

{
	echo "== modetest $MT -c"
	timeout 10 modetest $MT -c 2>&1
	echo "== modetest $MT -p"
	timeout 10 modetest $MT -p 2>&1
	echo "== /sys/module/drm/parameters/timestamp_monotonic"
	cat /sys/module/drm/parameters/timestamp_monotonic 2>&1
	echo "== /proc/interrupts (decon)"
	grep -i decon /proc/interrupts 2>&1
} >"$PROPS" 2>&1

if grep -q 'MODE_ID' "$PROPS"; then
	MODEID=present
else
	MODEID=absent
fi
say "drm: MODE_ID property $MODEID (expected absent until the v4.2 blob backport)"

irq_sum() { awk '/decon/ {s+=$2} END{print s+0}' /proc/interrupts 2>/dev/null; }
IRQ0=$(irq_sum)

# ------------------------------------------------------------------- run -----
RUN=/tmp/kwin-run
rm -rf "$RUN"
mkdir -p "$RUN/config" "$RUN/cache" "$RUN/data" || fail 2 "cannot create $RUN"
chmod 700 "$RUN"

unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
unset LD_LIBRARY_PATH LIBGL_DRIVERS_PATH EGL_PLATFORM LIBGL_ALWAYS_SOFTWARE GBM_BACKEND
unset MESA_LOADER_DRIVER_OVERRIDE KWIN_DRM_NO_AMS
[ -n "${KWIN_MESA_OVERRIDE:-}" ] && export MESA_LOADER_DRIVER_OVERRIDE=$KWIN_MESA_OVERRIDE

export XDG_RUNTIME_DIR=$RUN
export XDG_CONFIG_HOME=$RUN/config
export XDG_CACHE_HOME=$RUN/cache
export XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=$SESSIONID          # forces NoopSession (no seat on an SSH boot)
export KWIN_DRM_DEVICES=$DEV
export KWIN_RENDER_NODES=$RENDERNODE
export KWIN_COMPOSE=$COMPOSE
export QT_LOGGING_TO_CONSOLE=1
export QT_FORCE_STDERR_LOGGING=1
export QT_LOGGING_RULES=$RULES
export LIBGL_DEBUG=verbose
export EGL_LOG_LEVEL=debug
export MESA_DEBUG=1

say "run: kwin_wayland --drm for ${SECS}s (compose=$COMPOSE render=$RENDERNODE session=$SESSIONID)"
{
	echo "== kwin-try run $STAMP"
	echo "env: KWIN_DRM_DEVICES=$KWIN_DRM_DEVICES KWIN_RENDER_NODES=$KWIN_RENDER_NODES KWIN_COMPOSE=$KWIN_COMPOSE"
	echo "env: QT_LOGGING_RULES=$QT_LOGGING_RULES XDG_SESSION_ID=$XDG_SESSION_ID"
	echo "cmd: dbus-run-session -- timeout -s TERM -k 5 $SECS kwin_wayland --drm --no-lockscreen --no-global-shortcuts --socket $SOCK"
} >>"$LOG"

dmesg -c >/dev/null 2>&1
START=$(date +%s)
if command -v dbus-run-session >/dev/null 2>&1; then
	dbus-run-session -- timeout -s TERM -k 5 "$SECS" kwin_wayland \
		--drm --no-lockscreen --no-global-shortcuts --socket "$SOCK" >>"$LOG" 2>&1
	RC=$?
else
	timeout -s TERM -k 5 "$SECS" kwin_wayland \
		--drm --no-lockscreen --no-global-shortcuts --socket "$SOCK" >>"$LOG" 2>&1
	RC=$?
fi
ELAPSED=$(($(date +%s) - START))
say "run: kwin_wayland rc=$RC elapsed=${ELAPSED}s"

# KWin catches SIGTERM; give it a moment, then make sure nothing holds card0.
sleep 1
pkill -x kwin_wayland 2>/dev/null && { sleep 1; pkill -KILL -x kwin_wayland 2>/dev/null; say "run: killed a leftover kwin_wayland"; }

IRQ1=$(irq_sum)
say "drm: decon IRQ delta=$((IRQ1 - IRQ0)) (total $IRQ0 -> $IRQ1)"
dmesg >"$OUT/dmesg-run-$STAMP.log" 2>/dev/null

# -------------------------------------------------------------- classify -----
FATALS=""
HITS=""
fatal() { # fatal <literal> <label>
	if grep -qF -- "$1" "$LOG"; then
		say "  FATAL: $2"
		FATALS="$FATALS $2"
	fi
}
hit() {
	if grep -qF -- "$1" "$LOG"; then
		say "  ok: $2"
		HITS="$HITS $2"
	fi
}

say "== markers =="
fatal "Error relocating" "kwin-qt-abi"
fatal "Failed to take control of" "logind-take-control"
fatal "Failed to open drm device" "logind-open-drm"
fatal "Failed to create gbm device for $DEV" "gbm-card0"
fatal "drmSetClientCap for Atomic Mode Setting failed" "no-atomic-cap"
fatal "Atomic Mode Setting requested off via environment variable" "no-ams-env"
fatal "Failed to create any plane" "no-planes"
fatal "Failed to get plane resources" "no-plane-resources"
fatal "Failed to update the basic plane properties" "plane-props"
fatal "Failed to update the basic connector properties" "connector-props"
fatal "Failed to update the basic crtc properties" "crtc-props-no-MODE_ID"
fatal "Modeset failed:" "modeset-failed"
fatal "Atomic commit failed" "atomic-commit-failed"
fatal "Pageflip timed out" "pageflip-timeout"
fatal "Could not fulfill the requested compositing mode" "compose-mode"
fatal "Failed to initialize the Wayland server" "wayland-server"
fatal "Could not find the Qt platform plugin" "qpa-plugin"

hit "Using Atomic Mode Setting on gpu" "atomic-ams"
hit "Number of planes on GPU" "planes-enumerated"
hit "adding GPU" "gpu-added"
hit "OpenGL compositing has been successfully initialized" "gl-compositing"
hit "OpenGL 2 compositing enforced by environment variable" "gl-forced"
if grep -qE "New (non-desktop )?output on GPU" "$LOG"; then
	say "  ok: output-added"
	HITS="$HITS output-added"
fi

note "kwin-try $STAMP rev=$REV rc=$RC elapsed=${ELAPSED}s irq_delta=$((IRQ1 - IRQ0)) MODE_ID=$MODEID"
note "ok:$HITS"
note "fatal:$FATALS"
note "log: $LOG"
note "dmesg: $OUT/dmesg-run-$STAMP.log"
note "props: $PROPS"

# --------------------------------------------------------------- cleanup -----
if [ "$UNLOAD" = 1 ]; then
	for m in exynos_drm7580 exynos-drm7580; do
		if grep -qE "^$m " /proc/modules 2>/dev/null; then
			say "cleanup: rmmod $m"
			timeout 10 rmmod "$m" >>"$LOG" 2>&1 ||
				say "cleanup: WARN rmmod $m failed or hung (module still loaded)"
			break
		fi
	done
	if grep -qE '^drm_kms_helper ' /proc/modules 2>/dev/null; then
		timeout 10 rmmod drm_kms_helper >>"$LOG" 2>&1 ||
			say "cleanup: WARN rmmod drm_kms_helper failed"
	fi
else
	say "cleanup: modules left loaded (KWIN_UNLOAD=0)"
fi
if [ -n "${LOGDPID:-}" ]; then
	kill "$LOGDPID" 2>/dev/null
fi

# ----------------------------------------------------------------- verdict ---
case "$RC" in
124|143) RAN=1 ;;                    # TERM at the end of the window
137) RAN=1; say "run: WARN needed SIGKILL at the end of the window" ;;
*) RAN=0 ;;
esac
if [ "$RAN" = 0 ] && [ "$FATALS" = "" ]; then
	say "KWIN-TRY: WARN kwin_wayland exited early (rc=$RC) without a known fatal marker"
fi
if [ "$FATALS" = "" ] && [ "$RAN" = 1 ]; then
	case "$HITS" in
	*gl-compositing*)
		say "KWIN-TRY: PASS (ran ${ELAPSED}s, decon IRQ +$((IRQ1 - IRQ0)), OpenGL initialized)"
		say "KWIN-TRY LOG: $LOG"
		exit 0
		;;
	esac
	say "KWIN-TRY: INCOMPLETE - no fatal marker, but no 'OpenGL compositing ... initialized'"
	say "KWIN-TRY LOG: $LOG"
	exit 1
fi
say "KWIN-TRY: FAIL (fatal:$FATALS)"
say "KWIN-TRY LOG: $LOG"
exit 1
