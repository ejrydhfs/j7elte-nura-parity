#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host integration test for j7elte-sensors.
#
# Requires:
#   - root (or read access to /dev/input/event* and write access to /dev/uinput)
#   - a uinput-capable kernel
#   - busctl (systemd) for the D-Bus assertions
#
# The test creates virtual input devices with the same names/capabilities as
# the j7elte kernel drivers, runs j7elte-sensors against a private D-Bus
# daemon, and checks the net.hadess.SensorProxy properties.  The emitted
# accelerometer/proximity values are TEST DATA ONLY; the daemon never
# synthesises readings.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
tmp=$(mktemp -d /tmp/j7elte-sensors-test.XXXXXX)
emu=
daemon=
mon=
buspid=

cleanup() {
	[ -n "$daemon" ] && kill "$daemon" 2>/dev/null || true
	[ -n "$emu" ] && kill "$emu" 2>/dev/null || true
	[ -n "$mon" ] && kill "$mon" 2>/dev/null || true
	[ -n "$buspid" ] && kill "$buspid" 2>/dev/null || true
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

command -v busctl >/dev/null || {
	echo "busctl is required for this test" >&2
	exit 1
}

cc -O2 -Wall -Wextra -std=gnu11 -o "$tmp/test-emu" "$here/test-emu.c"
cc -O2 -Wall -Wextra -std=gnu11 -o "$tmp/j7elte-sensors" \
	"$root/j7elte-sensors.c" "$root/dbus-wire.c" -lm

mkfifo "$tmp/fifo"
dbus-daemon --session --address="unix:path=$tmp/bus" --fork \
	--print-address=1 --print-pid=1 >"$tmp/bus.info"
addr="unix:path=$tmp/bus"
buspid=$(tail -1 "$tmp/bus.info")

"$tmp/test-emu" "$tmp/fifo" 2>"$tmp/emu.log" &
emu=$!
sleep 0.6
"$tmp/j7elte-sensors" --address "$addr" --verbose 2>"$tmp/daemon.log" &
daemon=$!
sleep 0.8

getp() {
	busctl --address="$addr" get-property net.hadess.SensorProxy \
		/net/hadess/SensorProxy net.hadess.SensorProxy "$1"
}
call() {
	busctl --address="$addr" call net.hadess.SensorProxy \
		/net/hadess/SensorProxy net.hadess.SensorProxy "$1" >/dev/null
}

expect() {
	[ "$(getp "$1")" = "$2" ] || {
		echo "FAIL: $1 expected '$2', got '$(getp "$1")'" >&2
		exit 1
	}
	echo "ok: $1 = $2"
}

expect HasAccelerometer "b true"
expect HasProximity "b true"
expect HasAmbientLight "b false"
expect AccelerometerOrientation 's "undefined"'

call ClaimAccelerometer
call ClaimProximity
sleep 0.2

# K2HH reports Android-convention proper acceleration in 8192 LSB/g.
echo "accel 0 8192 0" >"$tmp/fifo"    # upright portrait
echo "prox 1" >"$tmp/fifo"            # far
sleep 0.4
expect AccelerometerOrientation 's "normal"'
expect AccelerometerTilt 's "vertical"'
expect ProximityNear "b false"

echo "accel 8192 0 0" >"$tmp/fifo"    # right side up
sleep 0.4
expect AccelerometerOrientation 's "right-up"'

echo "accel 0 -8192 0" >"$tmp/fifo"   # bottom edge up
sleep 0.4
expect AccelerometerOrientation 's "bottom-up"'

echo "accel 0 0 8192" >"$tmp/fifo"    # screen facing up
sleep 0.4
expect AccelerometerTilt 's "face-up"'

echo "prox 0" >"$tmp/fifo"            # close
sleep 0.4
expect ProximityNear "b true"

call ReleaseAccelerometer
call ReleaseProximity
sleep 0.3

grep -q "accelerometer disabled" "$tmp/daemon.log" ||
	echo "warning: daemon did not log accelerometer disable" >&2

echo "PASS"
