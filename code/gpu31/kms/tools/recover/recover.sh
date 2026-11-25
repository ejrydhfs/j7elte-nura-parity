#!/bin/sh
# ---------------------------------------------------------------------------
# recover.sh - one-command host-side rollback of the j7elte BOOT slot
#              (/dev/block/mmcblk0p10) to the known-good int49 kernel.
#
# The script classifies the phone into one of four reachable states and acts:
#
#   TWRP          adb lists a physical device in recovery  -> push + sha256 +
#                 dd p10 + readback sha256 + reboot
#   HEADLESS_KMS  ssh answers, kernel is not int49         -> scp + sha256 +
#                 dd p10 + readback sha256 + reboot
#   DEGRADED      int49 kernel but Xorg/fbwin/720x1280 not up -> diagnose,
#                 print the xsession remedy, exit 1; --force reflashes
#   KNOWN_GOOD    int49 running with Xorg + fbwin + 720x1280 -> verify only,
#                 exit 0; writes nothing (idempotent)
#   WEDGED        no ping and no USB device                -> prints the exact
#                 battery-pull + TWRP key sequence, exit 3
#
# Safety invariants:
#   * dd is impossible until the image's sha256 equals the pinned value AND
#     the ANDROID! magic and the <32 MiB size have been checked. The device
#     re-hashes the staged copy before dd and the BOOT partition after dd;
#     any mismatch aborts before reboot.
#   * p10 only. Recovery (p11) and every other partition are never written.
#   * conv=fsync + sync, then plain reboot. sysrq-b is never used.
#   * Running-system flash target is /dev/mmcblk0p10 (the /dev/block/... path
#     does not exist on the running system); TWRP target is
#     /dev/block/mmcblk0p10. Both are verified with test -b.
#   * Every ssh/adb action runs under flock /tmp/gpsdev.lock (ssh) and a
#     short hold, matching the rest of this repo.
#   * --dry-run is strictly read-only: no scp, no dd, no reboot, no writes.
#
# Usage:
#   ./recover.sh                 detect, then recover (asks before flashing)
#   ./recover.sh --yes           same, non-interactive
#   ./recover.sh --dry-run       detect + verify only, print the plan
#   ./recover.sh --force         flash even if the phone already is int49
#   ./recover.sh --image FILE    use FILE (default: search well-known paths)
#   ./recover.sh --sha256 HEX    override the pinned image hash (discouraged)
#   ./recover.sh --wait SEC      post-reboot budget, default 300
#
# Exit: 0 known-good or recovered, 1 not healthy / verification failed
#       (DEGRADED without --force, or post-flash verification),
#       2 usage, 3 wedged (human hands required), 4 image missing/invalid,
#       5 aborted (confirmation, lock contention, partition missing).
#
# Env overrides: DEV, HOST, LOCK, LOCK_WAIT, INT49_IMG, INT49_SHA256,
#                KMS_SSH_ASKPASS, SSH_OPTS, BOOT_RUN, BOOT_TWRP, TWRP_WAIT
# ---------------------------------------------------------------------------
set -u

PROG=$(basename "$0")

# --- pinned identity of the known-good image -------------------------------
PART_BYTES=33554432                 # mmcblk0p10 = 32 MiB
INT49_IMG_NAME=kernel-boot-int49.img
DEFAULT_INT49_SHA256=62377e6573ed5045e54c8afc4b229b72961e11bc8fe774106155314248b15e73
INT49_KREL='3.10.108'
INT49_KVER='#37 SMP PREEMPT Sun Nov 22 23:16:55 EST 2026'
INT49_VSIZE='720,1280'
INT49_XDIMS='720x1280'

# --- defaults --------------------------------------------------------------
DEV=${DEV:-user@172.16.42.1}
HOST=${HOST:-${DEV#*@}}
LOCK=${LOCK:-/tmp/gpsdev.lock}
LOCK_WAIT=${LOCK_WAIT:-240}
BOOT_RUN=${BOOT_RUN:-/dev/mmcblk0p10}
BOOT_TWRP=${BOOT_TWRP:-/dev/block/mmcblk0p10}
REMOTE_DIR=${REMOTE_DIR:-/data/recover}
TWRP_STAGE=${TWRP_STAGE:-/data/recover-int49.img}
TWRP_READBACK=${TWRP_READBACK:-/data/recover-readback.bin}
SSH_ASKPASS=${KMS_SSH_ASKPASS:-/tmp/ap.sh}
SSH_OPTS=${SSH_OPTS:--o StrictHostKeyChecking=no -o PreferredAuthentications=password -o PubkeyAuthentication=no -o ConnectTimeout=8}
DRY=0
YES=0
FORCE=0
WAIT=300

OPT_IMAGE=${INT49_IMG:-}
OPT_SHA=${INT49_SHA256:-}

# --- output ----------------------------------------------------------------
say()  { printf '%s\n' "$*"; }
warn() { printf '%s: warning: %s\n' "$PROG" "$*" >&2; }
err()  { printf '%s: error: %s\n' "$PROG" "$*" >&2; }
die()  { _rc=$1; shift; err "$*"; exit "$_rc"; }

usage() {
	cat <<EOF
usage: $PROG [--dry-run] [--yes] [--force] [--image FILE] [--sha256 HEX]
              [--wait SEC]

  --dry-run    detect and verify only; print planned commands, write nothing
  --yes, -y    do not prompt before flashing
  --force      flash even when the phone already runs int49
  --image FILE image to restore (default: $INT49_IMG_NAME in well-known paths)
  --sha256 HEX override the pinned int49 sha256 (discouraged)
  --wait SEC   post-reboot wait budget in seconds (default 300)

Exit 0 recovered/known-good, 1 not healthy/verification failed,
     2 usage, 3 wedged, 4 image missing/invalid, 5 aborted.
EOF
}

# --- option parsing --------------------------------------------------------
while [ $# -gt 0 ]; do
	case "$1" in
	--dry-run|--status) DRY=1 ;;
	--yes|-y) YES=1 ;;
	--force) FORCE=1 ;;
	--image)
		shift
		[ $# -gt 0 ] || die 2 "--image needs a path"
		OPT_IMAGE=$1
		;;
	--sha256)
		shift
		[ $# -gt 0 ] || die 2 "--sha256 needs a value"
		OPT_SHA=$1
		;;
	--wait)
		shift
		[ $# -gt 0 ] || die 2 "--wait needs seconds"
		WAIT=$1
		;;
	--help|-h) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
	shift
done

case "$WAIT" in ''|*[!0-9]*) die 2 "--wait must be a positive integer" ;; esac
[ "$WAIT" -gt 0 ] || die 2 "--wait must be a positive integer"

EXPECTED_SHA=${OPT_SHA:-$DEFAULT_INT49_SHA256}
[ "$EXPECTED_SHA" = "$DEFAULT_INT49_SHA256" ] ||
	warn "pinned sha256 overridden to $EXPECTED_SHA (the image is still verified before any dd)"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/recover.XXXXXX") ||
	die 5 "cannot create a temporary directory"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

# --- host helpers ----------------------------------------------------------
sha_host() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d' ' -f1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d' ' -f1
	else
		return 1
	fi
}

ping_ok() { ping -c 1 -W 2 "$HOST" >/dev/null 2>&1; }
lock_free() { flock -n "$LOCK" true 2>/dev/null; }

# All ssh runs under the shared device lock with a forced askpass helper.
lock() {
	flock -w "$LOCK_WAIT" -E 42 "$LOCK" \
		env SSH_ASKPASS="$SSH_ASKPASS" SSH_ASKPASS_REQUIRE=force \
		setsid -w "$@"
}
ssh_dev() { lock ssh $SSH_OPTS "$DEV" "$@"; }

# --- adb: find a real device in recovery -----------------------------------
# The host may have unrelated adb targets (this bench has an emulator). Only
# a serial that reports TWRP is accepted; emulator serials are skipped.
ADB_IGNORED=
TWRP_SERIAL=

find_twrp() {
	command -v adb >/dev/null 2>&1 || return 1
	_tl=$(timeout 20 adb devices 2>/dev/null | awk 'NR>1 && NF {print $1}')
	[ -n "$_tl" ] || return 1
	for _s in $_tl; do
		case "$_s" in
		emulator-*)
			ADB_IGNORED="$ADB_IGNORED $_s(emulator)"
			continue
			;;
		esac
		_tw=$(timeout 10 adb -s "$_s" shell 'getprop ro.twrp.version' 2>/dev/null |
			tr -d '\r\n')
		# ro.twrp.version is the only decisive marker. Stock Android recovery
		# also has /sbin/recovery and ro.boot.mode=recovery; flashing p10 of a
		# random phone in stock recovery must be impossible.
		if [ -n "$_tw" ]; then
			TWRP_SERIAL=$_s
			return 0
		fi
		_pd=$(timeout 10 adb -s "$_s" shell 'getprop ro.product.device' \
			2>/dev/null | tr -d '\r\n')
		ADB_IGNORED="$ADB_IGNORED $_s(${_pd:-unknown})"
	done
	return 1
}

# --- ssh probe -------------------------------------------------------------
# One round trip; every line is KEY:value so parsing needs no word splitting.
PROBE_CMD=$(cat <<'PROBE'
printf 'KREL:%s\n' "$(uname -r 2>/dev/null)"
printf 'KVER:%s\n' "$(uname -v 2>/dev/null)"
printf 'FB0:%s\n' "$([ -e /dev/fb0 ] && echo yes || echo no)"
printf 'VSIZE:%s\n' "$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
printf 'XORG:%s\n' "$(pgrep -x Xorg >/dev/null 2>&1 && echo yes || echo no)"
printf 'FBWIN:%s\n' "$(pgrep -x fbwin >/dev/null 2>&1 && echo yes || echo no)"
printf 'NSHELL:%s\n' "$(pgrep -x nura-shell >/dev/null 2>&1 && echo yes || echo no)"
printf 'CARD0:%s\n' "$([ -e /dev/dri/card0 ] && echo yes || echo no)"
printf 'DIMS:%s\n' "$(DISPLAY=:0 xdpyinfo 2>/dev/null | awk '/dimensions:/{print $2; exit}')"
PROBE
)

probe_ssh() { ssh_dev "$PROBE_CMD" 2>/dev/null; }

pget() { sed -n "s/^$1://p" "$TMP/probe.txt" 2>/dev/null | head -1; }

# evaluate the last probe in $TMP/probe.txt; sets P_* and returns 0 if the
# full known-good signature is present.
evaluate_probe() {
	P_KREL=$(pget KREL)
	P_KVER=$(pget KVER)
	P_FB0=$(pget FB0)
	P_VSIZE=$(pget VSIZE)
	P_XORG=$(pget XORG)
	P_FBWIN=$(pget FBWIN)
	P_NSHELL=$(pget NSHELL)
	P_CARD0=$(pget CARD0)
	P_DIMS=$(pget DIMS)
	P_OK=1
	[ "$P_KREL" = "$INT49_KREL" ] || P_OK=0
	[ "$P_KVER" = "$INT49_KVER" ] || P_OK=0
	[ "$P_FB0" = yes ] || P_OK=0
	[ "$P_VSIZE" = "$INT49_VSIZE" ] || P_OK=0
	[ "$P_XORG" = yes ] || P_OK=0
	[ "$P_FBWIN" = yes ] || P_OK=0
	[ "$P_DIMS" = "$INT49_XDIMS" ] || P_OK=0
	[ "$P_OK" = 1 ]
}

mark() { [ "$1" = "$2" ] && printf 'OK  ' || printf 'FAIL'; }

print_probe() {
	printf '  %s kernel release : %s (want %s)\n' \
		"$(mark "$P_KREL" "$INT49_KREL")" "${P_KREL:--}" "$INT49_KREL"
	printf '  %s kernel build   : %s\n' \
		"$(mark "$P_KVER" "$INT49_KVER")" "${P_KVER:--}"
	printf '  %s /dev/fb0       : %s\n' "$(mark "$P_FB0" yes)" "${P_FB0:--}"
	printf '  %s fb virtual_size: %s (want %s)\n' \
		"$(mark "$P_VSIZE" "$INT49_VSIZE")" "${P_VSIZE:--}" "$INT49_VSIZE"
	printf '  %s Xorg :0        : %s\n' "$(mark "$P_XORG" yes)" "${P_XORG:--}"
	printf '  %s fbwin          : %s\n' "$(mark "$P_FBWIN" yes)" "${P_FBWIN:--}"
	printf '  %s nura-shell     : %s\n' "$(mark "$P_NSHELL" yes)" "${P_NSHELL:--}"
	printf '  %s xdpyinfo dims  : %s (want %s)\n' \
		"$(mark "$P_DIMS" "$INT49_XDIMS")" "${P_DIMS:--}" "$INT49_XDIMS"
	printf '  -  /dev/dri/card0 : %s (KMS boot evidence, not required)\n' \
		"${P_CARD0:--}"
}

probe_summary() {
	printf 'kernel=%s %s fb0=%s xorg=%s fbwin=%s dims=%s' \
		"${P_KREL:--}" "${P_KVER:--}" "${P_FB0:--}" "${P_XORG:--}" \
		"${P_FBWIN:--}" "${P_DIMS:--}"
}

# --- image discovery + verification ---------------------------------------
IMG=
IMG_SIZE=0
IMG_SHA=
COPY_COUNT=0
IMAGE_OK=0
IMAGE_ERR=

find_image() {
	if [ -n "$OPT_IMAGE" ]; then
		IMG=$OPT_IMAGE
		[ -f "$IMG" ] || { IMAGE_ERR="image not found: $IMG"; return 1; }
		return 0
	fi
	for _c in \
		"/tmp/$INT49_IMG_NAME" \
		"/run/media/*/*/j7k/gpu31/$INT49_IMG_NAME" \
		"$HOME/j7k/gpu31/$INT49_IMG_NAME" \
		"/data/$INT49_IMG_NAME"; do
		for _f in $_c; do
			[ -f "$_f" ] || continue
			IMG=$_f
			return 0
		done
	done
	IMAGE_ERR="no $INT49_IMG_NAME found (searched /tmp, /run/media/*/*/j7k/gpu31, \$HOME/j7k/gpu31, /data)"
	return 1
}

verify_image() {
	IMAGE_OK=0
	IMAGE_ERR=
	find_image || return 1
	[ -r "$IMG" ] || { IMAGE_ERR="image not readable: $IMG"; return 1; }
	IMG_SIZE=$(wc -c < "$IMG" | awk '{print $1}')
	case "$IMG_SIZE" in ''|*[!0-9]*)
		IMAGE_ERR="cannot stat image size: $IMG"
		return 1
		;;
	esac
	if [ "$IMG_SIZE" -gt "$PART_BYTES" ]; then
		IMAGE_ERR="image is $IMG_SIZE B, larger than p10 ($PART_BYTES B)"
		return 1
	fi
	if [ "$IMG_SIZE" -lt 8388608 ]; then
		IMAGE_ERR="image is only $IMG_SIZE B; refusing a suspiciously small boot image"
		return 1
	fi
	_magic=$(dd if="$IMG" bs=8 count=1 2>/dev/null | tr -d '\000')
	if [ "$_magic" != 'ANDROID!' ]; then
		IMAGE_ERR="image lacks the ANDROID! magic: $IMG"
		return 1
	fi
	IMG_SHA=$(sha_host "$IMG") ||
		{ IMAGE_ERR="no sha256 tool on host (sha256sum/shasum)"; return 1; }
	if [ "$IMG_SHA" != "$EXPECTED_SHA" ]; then
		IMAGE_ERR="image sha256 $IMG_SHA != pinned $EXPECTED_SHA"
		return 1
	fi
	COPY_COUNT=$(( (IMG_SIZE + 511) / 512 ))
	IMAGE_OK=1
	return 0
}

print_image() {
	printf '  image : %s\n' "$IMG"
	printf '  size  : %s B (p10 is %s B)\n' "$IMG_SIZE" "$PART_BYTES"
	printf '  sha256: %s\n' "$IMG_SHA"
	printf '  pin   : %s\n' "$EXPECTED_SHA"
	printf '  readback: %s x 512 B\n' "$COPY_COUNT"
}

# --- state detection -------------------------------------------------------
STATE=
detect() {
	ADB_IGNORED=
	if find_twrp; then
		STATE=TWRP
		return 0
	fi
	: > "$TMP/probe.txt"
	probe_ssh > "$TMP/probe.txt" 2>/dev/null
	_probe_rc=$?
	if [ "$_probe_rc" = 0 ]; then
		evaluate_probe
		if [ "$P_OK" = 1 ]; then
			STATE=KNOWN_GOOD
		elif [ "$P_KREL" = "$INT49_KREL" ] && [ "$P_KVER" = "$INT49_KVER" ]; then
			# Right kernel but the session check failed. Xorg can be
			# restarting; give it one grace period before declaring DEGRADED
			# (a needless reflash/reboot is still a disruption).
			say "   int49 kernel up but session incomplete; rechecking in 10s"
			sleep 10
			: > "$TMP/probe.txt"
			if probe_ssh > "$TMP/probe.txt" 2>/dev/null && evaluate_probe; then
				STATE=KNOWN_GOOD
			else
				STATE=DEGRADED
			fi
		else
			STATE=HEADLESS_KMS
		fi
		return 0
	fi
	# 42 is the flock conflict code: the probe never ran, the phone may be fine.
	if [ "$_probe_rc" = 42 ] && ! lock_free; then
		STATE=LOCK_BUSY
		return 0
	fi
	if ping_ok; then
		STATE=WEDGED_SSH
	else
		STATE=WEDGED
	fi
}

report_state() {
	case "$STATE" in
	TWRP)
		say "state    : TWRP"
		say "evidence : adb device $TWRP_SERIAL reports TWRP/recovery"
		[ -n "$ADB_IGNORED" ] &&
			say "           ignored adb targets:$ADB_IGNORED"
		;;
	KNOWN_GOOD)
		say "state    : KNOWN_GOOD (int49 running, session healthy)"
		print_probe
		;;
	DEGRADED)
		say "state    : DEGRADED (int49 kernel, UI/session not healthy)"
		print_probe
		;;
	HEADLESS_KMS)
		say "state    : HEADLESS_KMS (ssh alive, kernel is not int49)"
		print_probe
		;;
	LOCK_BUSY)
		say "state    : UNKNOWN (device lock busy)"
		;;
	WEDGED_SSH)
		say "state    : WEDGED_SSH ($HOST answers ping, ssh does not)"
		;;
	WEDGED)
		say "state    : WEDGED (no ping, no USB recovery device)"
		;;
	esac
	if [ -n "$ADB_IGNORED" ] && [ "$STATE" != TWRP ]; then
		say "           ignored adb targets:$ADB_IGNORED"
	fi
}

# --- wedged: exact hands-on sequence --------------------------------------
wedged_instructions() {
	cat <<EOF

The phone cannot be recovered from this host. Do this exactly:

  1. Remove the back cover and pull the battery. Wait 10 s.
     (If the battery is not accessible, hold Power ~20 s instead.)
  2. Reinsert the battery. Do not press Power.
  3. Hold VolUp + Home + Power TOGETHER until the TWRP splash appears,
     then release. This boots recovery (mmcblk0p11); it is never flashed.
  4. Connect USB and confirm on this host:

         adb devices -l

     The phone must appear as a physical device (usb:..., not emulator-*).
  5. Re-run this script with --yes:

         $0 --yes

     It will push $INT49_IMG_NAME, verify its sha256 on the phone, dd it to
     mmcblk0p10, read the partition back, verify sha256 again, reboot, and
     then verify the kernel and Xorg/fbwin/720x1280.

If adb still shows no device in TWRP: try another USB port/cable, then
\`adb kill-server; adb devices\`. Do not dd anything by hand.
EOF
}

# --- flashing --------------------------------------------------------------
confirm() {
	[ "$YES" = 1 ] && return 0
	if [ ! -t 0 ]; then
		warn "stdin is not a tty; refusing to flash without --yes"
		return 1
	fi
	printf 'Restore %s to %s and reboot? [y/N] ' "$IMG" \
		"$([ "$STATE" = TWRP ] && echo "$BOOT_TWRP" || echo "$BOOT_RUN")"
	read -r _a || return 1
	case "$_a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

twrp_sh() {
	timeout 180 adb -s "$TWRP_SERIAL" shell "$1" 2>/dev/null | tr -d '\r'
}

twrp_sha() { # $1 = device path; TWRP's toybox/busybox may hide the applet
	twrp_sh "sha256sum '$1' 2>/dev/null || busybox sha256sum '$1' 2>/dev/null || toybox sha256sum '$1' 2>/dev/null" |
		awk '{print $1}' | tail -1
}

remote_sha_ssh() {
	ssh_dev "sha256sum '$1' 2>/dev/null | cut -d' ' -f1"
}

plan_ssh() {
	cat <<EOF
plan (ssh $DEV):
  scp $IMG -> $DEV:$REMOTE_DIR/$INT49_IMG_NAME
  sha256 on device == $EXPECTED_SHA (else abort)
  rootshell: dd if=$REMOTE_DIR/$INT49_IMG_NAME of=$BOOT_RUN bs=512 conv=fsync; sync
  read back $COPY_COUNT x 512 B from $BOOT_RUN, sha256 == $EXPECTED_SHA (else abort)
  rootshell: sync; reboot
  wait <= ${WAIT}s; verify uname -v == "$INT49_KVER" and Xorg/fbwin/720x1280
EOF
}

plan_twrp() {
	cat <<EOF
plan (adb $TWRP_SERIAL):
  adb push $IMG $TWRP_STAGE
  sha256 on device == $EXPECTED_SHA (else abort)
  adb shell: dd if=$TWRP_STAGE of=$BOOT_TWRP bs=512 conv=fsync; sync
  read back $COPY_COUNT x 512 B from $BOOT_TWRP, sha256 == $EXPECTED_SHA (else abort)
  adb shell: reboot
  wait <= ${WAIT}s for ssh; verify uname -v == "$INT49_KVER" and Xorg/fbwin/720x1280
EOF
}

flash_ssh() {
	_rel=$REMOTE_DIR/$INT49_IMG_NAME
	say "== staging image on device"
	ssh_dev "mkdir -p '$REMOTE_DIR'" ||
		die 5 "cannot create $REMOTE_DIR on device"
	lock scp $SSH_OPTS -q "$IMG" "$DEV:$_rel" ||
		die 5 "scp failed"

	_rsize=$(ssh_dev "wc -c < '$_rel'" 2>/dev/null | tr -d ' \r\n')
	[ "$_rsize" = "$IMG_SIZE" ] ||
		die 5 "staged size $_rsize != $IMG_SIZE; aborting before dd"

	_rsha=$(remote_sha_ssh "$_rel")
	[ "$_rsha" = "$EXPECTED_SHA" ] ||
		die 5 "staged sha256 ${_rsha:-none} != $EXPECTED_SHA; aborting before dd"
	say "   staged copy verified: $IMG_SIZE B, sha256 $_rsha"

	ssh_dev "test -x /usr/local/bin/rootshell" ||
		die 5 "/usr/local/bin/rootshell missing on the running system"
	ssh_dev "rootshell sh -c 'test -b $BOOT_RUN' 2>/dev/null" ||
		die 5 "$BOOT_RUN is not a block device on the running system"
	_bsize=$(ssh_dev "cat /sys/class/block/mmcblk0p10/size 2>/dev/null" |
		tr -d ' \r\n')
	case "$_bsize" in
	''|*[!0-9]*) : ;;
	*) [ $((_bsize * 512)) -ge "$IMG_SIZE" ] ||
		die 5 "p10 reports $((_bsize * 512)) B, image needs $IMG_SIZE B" ;;
	esac

	say "== writing $BOOT_RUN (conv=fsync)"
	ssh_dev "rootshell sh -c 'dd if=\"$_rel\" of=\"$BOOT_RUN\" bs=512 conv=fsync 2>/dev/null; rc=\$?; sync; exit \$rc'" ||
		die 5 "dd to $BOOT_RUN failed"

	say "== reading back $COPY_COUNT x 512 B and comparing sha256"
	_rback=$(ssh_dev "rootshell sh -c 'dd if=\"$BOOT_RUN\" bs=512 count=$COPY_COUNT 2>/dev/null'" 2>/dev/null |
		sha_host /dev/stdin)
	if [ "$_rback" != "$EXPECTED_SHA" ]; then
		err "readback sha256 ${_rback:-none} != $EXPECTED_SHA"
		err "BOOT was written but NOT verified; do not reboot-loop the phone."
		die 5 "readback verification failed"
	fi
	say "   readback verified: sha256 $_rback"

	ssh_dev "rm -f '$_rel'" >/dev/null 2>&1

	say "== rebooting"
	ssh_dev "rootshell sh -c 'sync; reboot'" >/dev/null 2>&1 || true
	say "   reboot issued"
}

flash_twrp() {
	_node=
	for _p in "$BOOT_TWRP" /dev/mmcblk0p10; do
		[ "$(twrp_sh "[ -b $_p ] && echo yes")" = yes ] && { _node=$_p; break; }
	done
	if [ -z "$_node" ]; then
		err "no p10 block node visible in TWRP ($BOOT_TWRP, /dev/mmcblk0p10)"
		err "inspect first: adb shell 'ls -l /dev/block/mmcblk0p*'"
		err "prior art (do not guess other minors):"
		err "  adb shell 'mknod /dev/block/mmcblk0p10 b 259 2'"
		die 5 "BOOT partition not found in recovery"
	fi
	say "== pushing image to TWRP"
	timeout 300 adb -s "$TWRP_SERIAL" push "$IMG" "$TWRP_STAGE" >/dev/null ||
		die 5 "adb push failed"

	_rmagic=$(twrp_sh "dd if='$TWRP_STAGE' bs=8 count=1 2>/dev/null" | tr -d '\000\r\n')
	[ "$_rmagic" = 'ANDROID!' ] ||
		die 5 "staged copy lacks ANDROID! magic; aborting before dd"

	_rsha=$(twrp_sha "$TWRP_STAGE")
	[ "$_rsha" = "$EXPECTED_SHA" ] ||
		die 5 "staged sha256 ${_rsha:-none} != $EXPECTED_SHA; aborting before dd"
	say "   staged copy verified: sha256 $_rsha"

	_bsize=$(twrp_sh "cat /sys/class/block/mmcblk0p10/size 2>/dev/null" |
		tr -d ' \r\n')
	case "$_bsize" in
	''|*[!0-9]*) : ;;
	*) [ $((_bsize * 512)) -ge "$IMG_SIZE" ] ||
		die 5 "p10 reports $((_bsize * 512)) B, image needs $IMG_SIZE B" ;;
	esac

	say "== writing $_node (conv=fsync)"
	_out=$(twrp_sh "dd if='$TWRP_STAGE' of='$_node' bs=512 conv=fsync; echo RC=\$?; sync")
	_rc=$(printf '%s\n' "$_out" | sed -n 's/^RC=//p' | tail -1)
	[ "$_rc" = 0 ] || die 5 "dd to $_node failed (rc=${_rc:-unknown})"

	say "== reading back $COPY_COUNT x 512 B and comparing sha256"
	_out=$(twrp_sh "dd if='$_node' of='$TWRP_READBACK' bs=512 count=$COPY_COUNT conv=fsync; echo RC=\$?; sync")
	_rc=$(printf '%s\n' "$_out" | sed -n 's/^RC=//p' | tail -1)
	[ "$_rc" = 0 ] || die 5 "readback dd failed (rc=${_rc:-unknown})"
	_rback=$(twrp_sha "$TWRP_READBACK")
	twrp_sh "rm -f '$TWRP_READBACK'" >/dev/null 2>&1
	if [ "$_rback" != "$EXPECTED_SHA" ]; then
		err "readback sha256 ${_rback:-none} != $EXPECTED_SHA"
		err "BOOT was written but NOT verified; do not reboot-loop the phone."
		die 5 "readback verification failed"
	fi
	say "   readback verified: sha256 $_rback"

	twrp_sh "rm -f '$TWRP_STAGE'" >/dev/null 2>&1

	say "== rebooting"
	timeout 30 adb -s "$TWRP_SERIAL" reboot >/dev/null 2>&1 || true
	say "   reboot issued"
}

# --- post-flash verification ----------------------------------------------
verify_restored() {
	_start=$(date +%s)
	_end=$((_start + WAIT))
	_next=$((_start + 30))
	say "== waiting up to ${WAIT}s for int49 + Xorg/fbwin"
	while :; do
		: > "$TMP/probe.txt"
		if probe_ssh > "$TMP/probe.txt" 2>/dev/null; then
			if evaluate_probe; then
				say "VERIFIED: restored kernel and session"
				print_probe
				return 0
			fi
			_last=$(probe_summary)
		else
			_last="ssh unreachable"
		fi
		_now=$(date +%s)
		[ "$_now" -ge "$_end" ] && break
		if [ "$_now" -ge "$_next" ]; then
			say "   waiting (${_now}s): $_last"
			_next=$((_now + 30))
		fi
		sleep 5
	done
	say "FAILED: device did not reach the known-good signature within ${WAIT}s"
	if [ -s "$TMP/probe.txt" ]; then
		print_probe
	else
		say "  no ssh response from $DEV"
	fi
	return 1
}

# --- main ------------------------------------------------------------------
say "== detecting phone state"
detect
report_state

case "$STATE" in
LOCK_BUSY)
	err "device access lock $LOCK is held by another process"
	err "this is contention, not a wedged phone; retry when the holder exits"
	exit 5
	;;
WEDGED|WEDGED_SSH)
	[ "$STATE" = WEDGED_SSH ] && {
		err "network is up but ssh is dead (kernel hung or sshd down)"
	}
	wedged_instructions
	exit 3
	;;
DEGRADED)
	if [ "$FORCE" = 0 ]; then
		say ""
		say "int49 is running, but Xorg/fbwin/nura-shell are not. Do NOT"
		say "reflash the same image for this. A stopped xsession is a normal"
		say "bench state (framebuffer tests); bring it back with:"
		say ""
		say "  ssh $DEV \"rootshell sh -c 'rc-service xsession start'\""
		say ""
		say "or reboot the phone. Only if the session cannot start at all:"
		say "  $0 --force"
		exit 1
	fi
	;;
esac

# Image check: mandatory before any flash, informational in --dry-run when
# the phone is already healthy.
if verify_image; then
	say "== image"
	print_image
else
	if [ "$STATE" = KNOWN_GOOD ] && [ "$FORCE" = 0 ]; then
		warn "$IMAGE_ERR"
		warn "no recovery image at hand, but the phone is already known-good"
	elif [ "$DRY" = 1 ]; then
		err "$IMAGE_ERR"
		exit 4
	else
		die 4 "$IMAGE_ERR"
	fi
fi

if [ "$STATE" = KNOWN_GOOD ] && [ "$FORCE" = 0 ]; then
	say "== nothing to do: phone already runs int49 with Xorg + fbwin + 720x1280"
	say "   (use --force to reflash anyway)"
	exit 0
fi

# A flash will happen: the image MUST be valid and verified.
[ "$IMAGE_OK" = 1 ] || die 4 "${IMAGE_ERR:-image not verified}"

if [ "$DRY" = 1 ]; then
	say "== dry-run: no writes, no reboot"
	if [ "$STATE" = TWRP ]; then plan_twrp; else plan_ssh; fi
	exit 0
fi

confirm || die 5 "aborted by operator"

case "$STATE" in
TWRP) flash_twrp ;;
*) flash_ssh ;;
esac

verify_restored || exit 1

say "RECOVERED: phone is back on int49 ($INT49_KVER), Xorg + fbwin up at $INT49_VSIZE"
exit 0
