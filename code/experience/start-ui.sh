#!/bin/sh
# One window commit so DECON starts scanning, then keep panning + run X.
/usr/local/bin/fbwin >/dev/null 2>&1
/usr/local/bin/fbwin pand >/dev/null 2>&1 &
sleep 1
exec /usr/bin/xinit /usr/local/bin/nura-launcher -- :0 vt1 \
    -config /etc/X11/xorg.conf.d/99-fbdev.conf -dpms -s 0 -noreset
