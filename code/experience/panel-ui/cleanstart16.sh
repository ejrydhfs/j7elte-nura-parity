#!/bin/sh
pkill -9 -f plasmashel[l]; pkill -9 -f kwin[_]wayland; pkill -9 -f kactivit[y]; pkill -9 -f "initial[_]start"; pkill -9 -f dbus-daemo[n]
sleep 3
RUN=/data/plasma-run
rm -f $RUN/bus $RUN/bus.pid
n=0; while pidof dbus-daemon >/dev/null 2>&1; do sleep 1; n=$((n+1)); [ $n -gt 10 ] && break; done
echo "bus daemons: $(pidof dbus-daemon | wc -w)"
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc /data/tmp; chmod 700 $RUN
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export QT_LOGGING_RULES="kwin_core.debug=true"
export PAN_MESA_DEBUG=noafbc
export LD_PRELOAD=/home/user/addfb_shim.so
export XCURSOR_THEME=breeze_cursors XCURSOR_SIZE=24
export PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
dbus-daemon --session --address=unix:path=$RUN/bus --fork --print-pid > $RUN/bus.pid
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
echo "daemon pid: $(cat $RUN/bus.pid)"
kwin_wayland --drm --socket wayland-0 > /data/clean-kwin.log 2>&1 &
for i in $(seq 1 60); do [ -S $RUN/wayland-0 ] && break; sleep 0.5; done
export WAYLAND_DISPLAY=wayland-0
QT_QPA_PLATFORM=offscreen /usr/libexec/kactivitymanagerd >/dev/null 2>&1 &
sleep 1
kded6 >/dev/null 2>&1 &
sleep 1
nohup plasmashell > /data/clean-pshell.log 2>&1 &
sleep 12
echo "== dbus names"; qdbus6 2>/dev/null | grep -icE "mobileshell"
echo "== fb errors"; grep -c "Failed to create framebuffer" /data/clean-kwin.log
echo "== pshell errors"; grep -ac "Not connected to D-Bus" /data/clean-pshell.log
ps -eo comm | grep -E "kwin_wayland|plasmashell" | sort -u
