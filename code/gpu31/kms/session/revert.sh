#!/bin/sh
# Remove the Nura session stack and restore the proven xsession boot path.
#
# This stops nura-session/LightDM, restores xsession in the default
# runlevel, removes the files installed by install.sh and reverses the group
# and zram changes.  It does not downgrade packages (kwin r1, powerdevil).

set -eu

[ "$(id -u)" -eq 0 ] || { echo "revert.sh must run as root" >&2; exit 1; }

CONF=/etc/nura/session.conf
if [ -r "$CONF" ]; then
    . "$CONF"
fi
: "${NURA_SESSION_USER:=user}"

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Stop the new stack first.
rc-service nura-session stop >/dev/null 2>&1 || true
rc-service lightdm stop >/dev/null 2>&1 || true
rc-service xsession stop >/dev/null 2>&1 || true

# Restore the runlevel to the pre-slice state: only xsession.
rc-update del nura-session default >/dev/null 2>&1 || true
rc-update del nura-session-guard default >/dev/null 2>&1 || true
rc-update del lightdm default >/dev/null 2>&1 || true
rc-update add xsession default >/dev/null 2>&1 || true
rc-service xsession start >/dev/null 2>&1 || true

# Groups added by install.sh (only those we recorded).
if [ -f /etc/nura/installed-groups ]; then
    for grp in $(cat /etc/nura/installed-groups); do
        delgroup "$NURA_SESSION_USER" "$grp" >/dev/null 2>&1 || true
    done
fi

# polkit was not in a runlevel before the slice; it is only needed by the
# optional powerdevil brightness helper.
rc-update del polkit default >/dev/null 2>&1 || true
rc-service polkit stop >/dev/null 2>&1 || true

# zram: the service was already enabled in the default runlevel but stopped
# (its default zstd algo fails on this kernel).  Stop it again; leave the
# runlevel entry alone.
rc-service postmarketos-zram-swap stop >/dev/null 2>&1 || true

# /etc/deviceinfo: remove our block, or the file if it is exactly ours.
if [ -f /etc/deviceinfo ]; then
    if cmp -s /etc/deviceinfo "$here/deviceinfo-zram"; then
        rm -f /etc/deviceinfo
    elif grep -q 'BEGIN nura-session zram' /etc/deviceinfo 2>/dev/null; then
        sed -i '/# BEGIN nura-session zram/,/# END nura-session zram/d' /etc/deviceinfo
    fi
fi

# logind drop-in (ours by name).
rm -f /etc/elogind/logind.conf.d/60-nura-lid.conf

rm -f /usr/local/bin/nura-session-mode \
      /usr/local/bin/nura-plasma-session \
      /usr/local/bin/nura-plasma-wrapper \
      /etc/init.d/nura-session \
      /usr/local/bin/nura-session-run \
      /usr/share/wayland-sessions/nura-plasma.desktop \
      /etc/lightdm/lightdm.conf.d/60-nura-session.conf \
      /usr/local/sbin/kms-atomic-probe
rm -rf /usr/local/src/nura-session /etc/nura /run/nura-session

echo "Reverted to the xsession/nura-shell boot path."
echo "Package changes are untouched.  To undo the optional packages:"
echo "  apk del powerdevil powerdevil-libs ddcutil ddcutil-udev kscreen xwayland"
