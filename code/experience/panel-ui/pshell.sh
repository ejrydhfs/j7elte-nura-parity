#!/bin/sh
RUN=/data/plasma-run
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop WAYLAND_DISPLAY=wayland-0
if [ ! -S $RUN/bus ]; then
	dbus-daemon --session --address=unix:path=$RUN/bus --fork --print-pid > $RUN/bus.pid
fi
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
export QT_QPA_PLATFORM=wayland QT_QUICK_BACKEND=software
export MESA_LOADER_DRIVER_OVERRIDE=kms_swrast GALLIUM_DRIVER=softpipe
export PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell
export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
pkill -9 -f plasmashel[l]
sleep 1
setsid nohup plasmashell > /data/soft-pshell.log 2>&1 </dev/null &
sleep 12
echo "== ps"
ps -eo pid,etime,pcpu,comm | grep -E "kwin|plasmashell"
echo "== log"
tail -6 /data/soft-pshell.log
