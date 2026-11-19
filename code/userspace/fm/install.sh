#!/bin/sh
# install.sh - deploy the bcmfm FM userspace onto the target device.
# Run ON the device (postmarketOS/Alpine) after copying the built tree over,
# or from the host via: scp bcmfm fm-mixer.sh install.sh root@172.16.42.1:/tmp/
# then ssh in and run: sh /tmp/install.sh
#
# What it does:
#   1. installs `bcmfm` to $BINDIR (default /usr/local/bin)
#   2. grants it CAP_NET_ADMIN via setcap (so non-root can drive hci0),
#      falling back to root-only mode when setcap/libcap is unavailable
#   3. installs `fm-mixer.sh` alongside it (executable)
#   4. prints usage + BT-safety notes
set -eu

BINDIR="${BINDIR:-/usr/local/bin}"
SRCBIN="${SRCBIN:-bcmfm}"
SRCMIXER="${SRCMIXER:-fm-mixer.sh}"

die() { echo "install.sh: $*" >&2; exit 1; }

[ -f "$SRCBIN" ] || die "bcmfm binary not found at ./$SRCBIN (run 'make' on the host first)"
[ -f "$SRCMIXER" ] || die "fm-mixer.sh not found at ./$SRCMIXER"

mkdir -p "$BINDIR"
install -m 0755 "$SRCBIN" "$BINDIR/bcmfm"
install -m 0755 "$SRCMIXER" "$BINDIR/fm-mixer.sh"

# Capabilities: raw HCI needs CAP_NET_ADMIN. Prefer setcap so bcmfm works
# unprivileged; if setcap is missing, fall back to root-owned 0755 and warn.
if command -v setcap >/dev/null 2>&1; then
    if setcap 'cap_net_admin+ep' "$BINDIR/bcmfm"; then
        echo "capability: cap_net_admin+ep set on $BINDIR/bcmfm"
    else
        echo "warning: setcap failed; $BINDIR/bcmfm will require root" >&2
    fi
else
    echo "warning: setcap not found (apk add libcap); $BINDIR/bcmfm will require root" >&2
    if [ "$(id -u)" -eq 0 ]; then
        chmod 0755 "$BINDIR/bcmfm"
        echo "capability: running as root, installed root-executable instead"
    else
        echo "hint: re-run as root or install libcap, then: setcap 'cap_net_admin+ep' $BINDIR/bcmfm" >&2
    fi
fi

command -v getcap >/dev/null 2>&1 && getcap "$BINDIR/bcmfm" || true

cat <<EOF
installed:
  $BINDIR/bcmfm
  $BINDIR/fm-mixer.sh

usage:
  bcmfm [-d 0] [-t 2000] <command> [args]
    info | enable [eu|us|jp|world] | disable | tune <MHz> | seek up|down
    abort | mute on|off | rfmute on|off | vol <0-100> | rds on|off
    region eu|us|jp|world | step 50|100|200 | path 0|1|2|3
    deemph 50|75 | dump | raw <hex...>
  fm-mixer.sh speaker|headset|record|revert   # route FM audio via cod3022x+S2801

example:
  bcmfm enable eu && bcmfm tune 94.0 && fm-mixer.sh speaker

BT SAFETY - READ THIS:
  * NEVER run 'hciconfig hci0 down' and NEVER stop bluetoothd: hci0 is the
    shared BT/FM transport. Bringing it down kills Bluetooth and strands FM.
  * NEVER touch the LDO38/LDO39 regulators while FM or BT is up.
  * Vendor-event flood warning: enabling audio path W1B(0x4D,1,8) starts a
    persistent spontaneous 04ff0108 event flood (~2.9k/s) on hci0. It drowns
    naive HCI Command-Complete waits but does NOT affect ALSA audio.
    Mitigation (filter + drain + short windows) is documented in
    rds-coex.txt in this directory - read it before scripting HCI polls.
EOF
