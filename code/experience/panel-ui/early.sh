#!/bin/sh
RUN=/data/plasma-run
pkill -9 -f plasmashel[l]; pkill -9 -f kwin[_]wayland; sleep 2
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export PAN_MESA_DEBUG=noafbc LD_PRELOAD=/home/user/addfb_shim.so
export PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell
export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/clean-kwin.log 2>&1 </dev/null >/dev/null 2>&1 &
for i in $(seq 1 100); do [ -S $RUN/wayland-0 ] && break; sleep 0.2; done
export WAYLAND_DISPLAY=wayland-0
nohup plasmashell > /data/pshell_early.log 2>&1 &
sleep 8
cd /home/user && ./shot /data/early.ppm DSI-1 2>&1 | tail -1
echo K=$(pidof kwin_wayland) P=$(pidof plasmashell)
