#!/bin/sh
/usr/local/bin/fbwin >/dev/null 2>&1
/usr/local/bin/fbwin pand >/dev/null 2>&1 &
sleep 1
exec /usr/bin/xinit /usr/local/bin/lxqt-session.sh -- /usr/local/bin/Xorg-preload :0 vt1 \
    -config /etc/X11/xorg.conf.d/99-fbdev.conf -dpms -s 0 -noreset
