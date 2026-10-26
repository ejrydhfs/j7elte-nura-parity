#!/bin/sh
# Offline validation harness for j7elte-power.
#
# The daemon is exercised against a fake /sys/class/power_supply tree, so no
# hardware is needed.  It verifies:
#   * charge limit latch on boot when already above the limit,
#   * resume below the hysteresis point (safety poll path),
#   * charge disable/enable is only ever written to batt_slate_mode,
#   * release of the latch on SIGTERM,
#   * capacity_max persistence (opt-in) and its 801..1199 guard,
#   * config parsing (unknown key, out-of-range values).
#
# Usage: ./test-j7elte-power.sh /path/to/j7elte-power-binary

set -u

BIN=${1:?usage: $0 <j7elte-power binary>}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

pass() { echo "PASS: $*"; }
die()  { echo "FAIL: $*" >&2; fail=1; }

mkpsy() {
	rm -rf "$TMP/ps"
	mkdir -p "$TMP/ps/battery" "$TMP/ps/ac" "$TMP/ps/usb" "$TMP/ps/wireless"
	printf '85\n' > "$TMP/ps/battery/capacity"
	printf 'Discharging\n' > "$TMP/ps/battery/status"
	printf '990\n' > "$TMP/ps/battery/batt_capacity_max"
	printf '0\n' > "$TMP/ps/battery/batt_slate_mode"
	printf '0\n' > "$TMP/ps/ac/online"
	printf '1\n' > "$TMP/ps/usb/online"
	printf '0\n' > "$TMP/ps/wireless/online"
}

conf() {
	# $1 = charge_limit, $2 = persist, $3 = efs file
	cat > "$TMP/conf" <<EOF
battery_dir=$TMP/ps/battery
efs_capacity_max_file=${3:-$TMP/efs}
charge_limit=$1
resume_margin=5
poll_interval=5
persist_capacity_max=$2
EOF
}

# ---- 1. latch at boot ------------------------------------------------------
mkpsy
conf 80 0
"$BIN" --config "$TMP/conf" & pid=$!
i=0
while [ "$i" -lt 20 ]; do
	[ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 1 ] && break
	i=$((i + 1)); sleep 0.25
done
if [ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 1 ]; then
	pass "charge stopped at 85% with limit 80"
else
	die "charge was not stopped at 85% with limit 80"
fi

# ---- 2. resume below hysteresis -------------------------------------------
printf '70\n' > "$TMP/ps/battery/capacity"
i=0
while [ "$i" -lt 40 ]; do
	[ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 0 ] && break
	i=$((i + 1)); sleep 0.25
done
if [ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 0 ]; then
	pass "charge resumed at 70% (limit 80, margin 5)"
else
	die "charge did not resume at 70%"
fi

# ---- 3. release on SIGTERM -----------------------------------------------
printf '85\n' > "$TMP/ps/battery/capacity"
i=0
while [ "$i" -lt 40 ]; do
	[ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 1 ] && break
	i=$((i + 1)); sleep 0.25
done
kill -TERM "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
if [ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 0 ]; then
	pass "charge limit released on SIGTERM"
else
	die "charge limit still latched after SIGTERM"
fi

# ---- 3b. a latch left behind by a killed instance is adopted --------------
# SIGKILL cannot run the release path, and the service restarts on failure:
# the new instance must recognise the engaged latch and clear it at the
# resume point instead of leaving charging disabled forever.
mkpsy
conf 80 0
printf '70\n' > "$TMP/ps/battery/capacity"
printf '1\n' > "$TMP/ps/battery/batt_slate_mode"
"$BIN" --config "$TMP/conf" --once >/dev/null 2>&1
if [ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 0 ]; then
	pass "stale batt_slate_mode released at the resume point"
else
	die "stale batt_slate_mode from a killed instance was not released"
fi

# ---- 4. no writes when limiting is disabled -------------------------------
mkpsy
conf 0 0
printf '1\n' > "$TMP/ps/battery/batt_slate_mode"   # foreign state
"$BIN" --config "$TMP/conf" --once
if [ "$(cat "$TMP/ps/battery/batt_slate_mode")" = 1 ]; then
	pass "foreign batt_slate_mode left untouched when limit disabled"
else
	die "daemon cleared a batt_slate_mode it did not set"
fi

# ---- 5. capacity_max persistence -----------------------------------------
mkpsy
printf '950\n' > "$TMP/efs"
printf '1000\n' > "$TMP/ps/battery/batt_capacity_max"
conf 0 1 "$TMP/efs"
"$BIN" --config "$TMP/conf" --once
if [ "$(cat "$TMP/efs" 2>/dev/null)" = 1000 ]; then
	pass "learned capacity_max persisted to EFS file"
else
	die "capacity_max was not persisted"
fi

# unchanged value must not be rewritten (mtime check is racy; use inode too)
before=$(stat -c '%i:%Y' "$TMP/efs")
sleep 1
"$BIN" --config "$TMP/conf" --once
after=$(stat -c '%i:%Y' "$TMP/efs")
if [ "$before" = "$after" ]; then
	pass "persistence is a no-op when the value did not change"
else
	die "EFS file rewritten although capacity_max did not change"
fi

# out-of-range value must not be persisted
printf '1500\n' > "$TMP/ps/battery/batt_capacity_max"
printf '950\n' > "$TMP/efs"
"$BIN" --config "$TMP/conf" --once
if [ "$(cat "$TMP/efs")" = 950 ]; then
	pass "out-of-range capacity_max is not persisted"
else
	die "out-of-range capacity_max was persisted"
fi

# ---- 6. config parsing ----------------------------------------------------
conf 120 0
out=$("$BIN" --config "$TMP/conf" --once 2>&1)
case "$out" in
*"out of range"*) pass "charge_limit=120 rejected" ;;
*) die "charge_limit=120 was not rejected: $out" ;;
esac

printf 'bogus_key=1\n' >> "$TMP/conf"
out=$("$BIN" --config "$TMP/conf" --once 2>&1)
case "$out" in
*"unknown key 'bogus_key'"*) pass "unknown config key reported" ;;
*) die "unknown config key not reported: $out" ;;
esac

# missing config file -> defaults (battery_dir=/sys/class/power_supply/battery,
# which does not exist on a build host: exit 1 with that specific message, not
# a config parse error)
out=$("$BIN" --config "$TMP/does-not-exist" --once 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "not present"; then
	pass "missing config file falls back to defaults"
else
	die "missing config file handling wrong (rc=$rc): $out"
fi

# ---- 7. missing battery device -> clean failure ---------------------------
sed -i "s|^battery_dir=.*|battery_dir=$TMP/nope|" "$TMP/conf"
"$BIN" --config "$TMP/conf" --once >/dev/null 2>&1
if [ $? -eq 1 ]; then
	pass "missing battery device reported with exit 1"
else
	die "missing battery device did not fail"
fi

if [ "$fail" -ne 0 ]; then
	echo "RESULT: FAILED" >&2
	exit 1
fi
echo "RESULT: all checks passed"
