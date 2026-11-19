#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# hall-flip-test.sh - capture a real flip-cover transition on the phone.
#
# Run as root on the phone, e.g.
#   /usr/local/bin/rootshell sh /tmp/hall-flip-test.sh 30
# and move a magnet close to the top bezel while it waits.
#
# The script:
#   * finds the input device named "hall" (so it does not hardcode event6),
#   * holds a logind "handle-lid-switch" inhibitor for the duration, because
#     elogind's default HandleLidSwitch is "suspend" and a real flip would
#     otherwise suspend the phone mid-test,
#   * prints every raw EV_SW/SW_LID event plus the daemon's LidIsClosed
#     property at that moment,
#   * prints hall_detect, elogind's LidClosed and LidIsClosed before/after.
#
# The reader only reads the input device; it never writes to it.  It needs
# python3 (present on the device image, not a package dependency).

set -eu

secs=${1:-30}
event=

for n in /sys/class/input/input*/name; do
	[ "$(cat "$n" 2>/dev/null)" = hall ] || continue
	d=${n%/name}
	event=/dev/input/event${d##*input}
	break
done

[ -n "$event" ] || {
	echo "no input device named 'hall' (driver not built/bound?)" >&2
	exit 1
}

show_state() {
	echo "hall_detect:  $(cat /sys/class/sec/sec_key/hall_detect 2>/dev/null || echo n/a)"
	echo "elogind LidClosed: $(busctl get-property org.freedesktop.login1 \
		/org/freedesktop/login1 org.freedesktop.login1.Manager \
		LidClosed 2>/dev/null || echo n/a)"
	echo "SensorProxy LidIsClosed: $(busctl get-property \
		net.hadess.SensorProxy /net/hadess/SensorProxy \
		net.hadess.SensorProxy LidIsClosed 2>/dev/null || echo n/a)"
}

echo "hall input device: $event"
show_state
echo
echo "move a magnet near the top bezel now (${secs}s)..."

reader=/tmp/hall_reader.$$.py
cat >"$reader" <<'PY'
import os, select, struct, subprocess, sys, time

path, secs = sys.argv[1], float(sys.argv[2])


def lid_is_closed():
    try:
        return subprocess.run(
            ["busctl", "get-property", "net.hadess.SensorProxy",
             "/net/hadess/SensorProxy", "net.hadess.SensorProxy",
             "LidIsClosed"],
            capture_output=True, text=True, timeout=2).stdout.strip() or "?"
    except Exception:
        return "?"


fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
end = time.time() + secs
while True:
    remaining = end - time.time()
    if remaining <= 0:
        break
    ready, _, _ = select.select([fd], [], [], remaining)
    if not ready:
        break
    data = os.read(fd, 24 * 64)
    for off in range(0, len(data), 24):
        _, _, typ, code, val = struct.unpack_from("qqHHi", data, off)
        if typ != 5:          # EV_SW
            continue
        print("EV_SW code=%d (SW_LID) value=%d -> LidIsClosed=%s"
              % (code, val, lid_is_closed()), flush=True)
os.close(fd)
PY

if command -v elogind-inhibit >/dev/null 2>&1; then
	elogind-inhibit --what=handle-lid-switch --mode=block \
		--who=hall-flip-test --why="capture SW_LID without suspending" \
		python3 "$reader" "$event" "$secs"
else
	echo "warning: elogind-inhibit not found; a close will suspend the phone" >&2
	python3 "$reader" "$event" "$secs"
fi

rm -f "$reader"
echo
show_state
