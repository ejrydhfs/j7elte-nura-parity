#!/bin/sh
# remote.sh - host-side driver for the on-device KMS tooling (j7elte).
#
#   ./remote.sh push       copy kmsflip.c + scripts to /data/kms/tools
#   ./remote.sh build      push, then native make on the device
#   ./remote.sh headless   GBM + EGL-surfaceless test (works today)
#   ./remote.sh kms        card0 test: modetest + kmsflip + kmscube
#   ./remote.sh soak       300-flip soak
#   ./remote.sh logs       tail of the newest on-device log
#   ./remote.sh pull       fetch /data/kmslogs to ./kmslogs
#
# Every ssh/scp runs under flock /tmp/gpsdev.lock (shared device lock) and a
# setsid askpass helper, matching the rest of this repo. No reboot, no flash.
# Override: DEV=user@host DST=/data/kms/tools LOGDST=/data/kmslogs
#           KMS_SSH_ASKPASS=/tmp/ap.sh SSH_OPTS="..."
set -u

SRC=$(cd "$(dirname "$0")" && pwd)
DEV=${DEV:-user@172.16.42.1}
DST=${DST:-/data/kms/tools}
LOGDST=${LOGDST:-/data/kmslogs}
LOCK=${LOCK:-/tmp/gpsdev.lock}
# Ignore a inherited SSH_ASKPASS=/usr/bin/false from agent shells.
SSH_ASKPASS=${KMS_SSH_ASKPASS:-/tmp/ap.sh}
SSH_OPTS=${SSH_OPTS:--o StrictHostKeyChecking=no -o PreferredAuthentications=password -o PubkeyAuthentication=no}

lock() { flock "$LOCK" env SSH_ASKPASS="$SSH_ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w "$@"; }
rsh() { lock ssh $SSH_OPTS "$DEV" "$@"; }

push() {
	rsh "mkdir -p '$DST' '$LOGDST'" || return 1
	lock scp $SSH_OPTS -q "$SRC/kmsflip.c" "$SRC/Makefile" \
		"$SRC"/build.sh "$SRC"/env.sh "$SRC"/kms-dmesg.sh \
		"$SRC"/install-deps.sh "$SRC"/run-headless.sh \
		"$SRC"/run-kms.sh "$SRC"/run-soak.sh \
		"$DEV:$DST/" || return 1
	printf 'push: %s -> %s:%s\n' "$SRC" "$DEV" "$DST"
}

case "${1:-}" in
push)
	push
	;;
build)
	push || exit 1
	rsh "cd '$DST' && sh build.sh"
	;;
headless)
	rsh "sh '$DST/run-headless.sh'"
	;;
kms)
	rsh "sh '$DST/run-kms.sh'"
	;;
soak)
	rsh "sh '$DST/run-soak.sh'"
	;;
logs)
	rsh 'ls -t /data/kmslogs 2>/dev/null | head -20; echo ---; tail -n 40 "$(ls -t /data/kmslogs/*.log 2>/dev/null | head -1)" 2>/dev/null'
	;;
pull)
	mkdir -p ./kmslogs
	lock scp $SSH_OPTS -q -r "$DEV:$LOGDST/." ./kmslogs/ &&
		printf 'pull: %s:%s -> ./kmslogs\n' "$DEV" "$LOGDST"
	;;
*)
	printf 'usage: %s push|build|headless|kms|soak|logs|pull\n' "$0" >&2
	exit 2
	;;
esac
