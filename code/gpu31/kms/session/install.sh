#!/bin/sh
# Install the Nura session stack (Plasma Mobile on KMS + nura-shell fallback).
#
# This only installs files and prepares the device; it does NOT switch the
# session or enable nura-session.  Run /usr/local/bin/nura-session-mode when the
# KMS card is ready.  Package changes (kwin r1, powerdevil) are listed in
# docs/reports/decon-kms-session.md and are intentionally not done here.

set -eu

[ "$(id -u)" -eq 0 ] || { echo "install.sh must run as root" >&2; exit 1; }

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONF=/etc/nura/session.conf
[ -r "$CONF" ] || CONF="$here/session.conf"
[ -r "$CONF" ] || { echo "session.conf not found" >&2; exit 1; }
. "$CONF"

command -v rc-service >/dev/null 2>&1 || { echo "OpenRC not found" >&2; exit 1; }
id "$NURA_SESSION_USER" >/dev/null 2>&1 || { echo "user '$NURA_SESSION_USER' missing" >&2; exit 1; }

install -d /etc/nura /etc/lightdm/lightdm.conf.d \
           /etc/elogind/logind.conf.d \
           /usr/local/bin /usr/local/sbin /usr/local/src/nura-session \
           /usr/share/wayland-sessions

install -m644 "$here/session.conf"              /etc/nura/session.conf
install -m755 "$here/nura-session-mode"         /usr/local/bin/nura-session-mode
install -m755 "$here/nura-plasma-session"       /usr/local/bin/nura-plasma-session
install -m755 "$here/nura-plasma-wrapper"       /usr/local/bin/nura-plasma-wrapper
install -m755 "$here/nura-session-run"         /usr/local/bin/nura-session-run
install -m755 "$here/nura-session.initd"       /etc/init.d/nura-session
install -m755 "$here/nura-zswap-off.initd"     /etc/init.d/nura-zswap-off
install -m644 "$here/nura-plasma.desktop"       /usr/share/wayland-sessions/nura-plasma.desktop
install -m644 "$here/60-nura-session.conf"      /etc/lightdm/lightdm.conf.d/60-nura-session.conf
install -m644 "$here/kms-atomic-probe.c"        /usr/local/src/nura-session/kms-atomic-probe.c

# zram: the 3.10 kernel's zram only has lzo/lz4, while postmarketOS defaults
# to zstd.  /etc/deviceinfo overrides it and is sourced after the device file.
if [ -f /etc/deviceinfo ]; then
    if ! grep -q 'nura-session zram' /etc/deviceinfo 2>/dev/null; then
        {
            echo ''
            echo '# BEGIN nura-session zram (3.10 zram has lzo/lz4 only)'
            cat "$here/deviceinfo-zram"
            echo '# END nura-session zram'
        } >> /etc/deviceinfo
    fi
else
    install -m644 "$here/deviceinfo-zram" /etc/deviceinfo
fi

# Match the nura-shell image, which inhibited suspend on lid close; the
# vendor elogind default would suspend on the hall sensor.  Use a drop-in so
# an existing logind.conf is preserved (elogind 255 reads
# /etc/elogind/logind.conf.d/*.conf).
install -m644 "$here/logind-lid.conf" /etc/elogind/logind.conf.d/60-nura-lid.conf
# elogind reads its config at startup; the service supports SIGHUP reload.
rc-service elogind reload >/dev/null 2>&1 || true

# KWin/libinput runs as the session user and this image does not tag
# touchscreens for elogind uaccess (70-uaccess.rules only tags joysticks).
added=
for grp in input video; do
    if ! id -nG "$NURA_SESSION_USER" | tr ' ' '\n' | grep -qx "$grp"; then
        addgroup "$NURA_SESSION_USER" "$grp"
        added="$added $grp"
    fi
done
if [ -n "$added" ]; then
    # shellcheck disable=SC2086
    printf '%s\n' "$added" > /etc/nura/installed-groups
fi

# Build the atomic KMS gate.  libdrm-dev and gcc are installed in this image.
if command -v cc >/dev/null 2>&1; then
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdrm; then
        cflags=$(pkg-config --cflags libdrm)
        libs=$(pkg-config --libs libdrm)
    else
        cflags="-I/usr/include/libdrm"
        libs="-ldrm"
    fi
    # shellcheck disable=SC2086
    cc -O2 -Wall -Wextra -o /usr/local/sbin/kms-atomic-probe \
        "$here/kms-atomic-probe.c" $cflags $libs
    echo "built /usr/local/sbin/kms-atomic-probe"
else
    echo "WARN: cc not found; install gcc libdrm-dev, then rerun install.sh" >&2
fi

# powerdevil's brightness helper is a system D-Bus service authorized by
# polkit; polkitd is installed but not in any runlevel on this image.
rc-update add polkit default >/dev/null 2>&1 || true
rc-service polkit start >/dev/null 2>&1 || true

# zswap.enabled is 0444; record the current max_pool_percent and block
# stores before zram is enabled.  The real fix is zswap.enabled=0 on the
# kernel cmdline (KMS boot slice).
[ -e /etc/nura/zswap-max-pool ] || \
    cat /sys/module/zswap/parameters/max_pool_percent > /etc/nura/zswap-max-pool 2>/dev/null || true
rc-update add nura-zswap-off default >/dev/null 2>&1 || true
rc-service nura-zswap-off start >/dev/null 2>&1 || true

# zram is already in the default runlevel on this image; it only fails to
# start because zramstart defaults to zstd.  /etc/deviceinfo fixes the algo.
rc-service postmarketos-zram-swap start >/dev/null 2>&1 || \
    echo "WARN: postmarketos-zram-swap did not start (check /etc/deviceinfo)" >&2

cat <<EOF

Installed.  Nothing is enabled: the running xsession and its runlevel entry
are untouched, and nura-session is not in any runlevel yet.

Next:
  /usr/local/bin/nura-session-mode status
  apk add 'kwin=6.8.3-r1' 'kwin-lang=6.8.3-r1'   # required for Qt 6.12
  apk add xwayland powerdevil kscreen            # recommended (X11 apps, brightness, rotation)
  /usr/local/bin/nura-session-mode nura          # opt in, keep nura-shell
  /usr/local/bin/nura-session-mode plasma        # after card0 + kwin preflight

Revert: $here/revert.sh
EOF
