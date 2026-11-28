#!/bin/sh
# Full software Plasma session: llvmpipe over kms_swrast with dumb GBM
# buffers, plasmashell in software QSG.
GPUD=/sys/bus/platform/devices/11400000.mali/power
echo on > $GPUD/control 2>/dev/null
V=$(cat /sys/module/exynos_drm7580/version 2>/dev/null)
[ "$V" = "int125" ] || { echo "module version [$V] != int125 - refusing to start"; exit 1; }
echo "module version: $V"

RUN=/data/plasma-run
if [ -f $RUN/bus.pid ]; then
	BP=$(cat $RUN/bus.pid)
	[ -n "$BP" ] && kill -9 "$BP" 2>/dev/null
fi
pkill -9 -f plasmashel[l]; pkill -9 -f kwin[_]wayland
pkill -9 -f kactivit[y]; pkill -9 -f "initial[_]start"
sleep 3
rm -f $RUN/bus $RUN/bus.pid
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc /data/tmp; chmod 700 $RUN

export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export MESA_LOADER_DRIVER_OVERRIDE=swrast LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=0
export QT_QUICK_BACKEND=software
export LD_PRELOAD=/home/user/addfb_shim.so
export QT_LOGGING_RULES="kwin_*.debug=true"
export XCURSOR_THEME=breeze_cursors XCURSOR_SIZE=24
export PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland

dbus-daemon --session --address=unix:path=$RUN/bus --fork --print-pid > $RUN/bus.pid
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
echo "daemon pid: $(cat $RUN/bus.pid)"

setsid nohup sh -c 'kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/soft-kwin.log 2>&1; echo KWIN_EXIT=$? > /data/soft-kwin.exit' >/dev/null 2>&1 </dev/null &
for i in $(seq 1 60); do [ -S $RUN/wayland-0 ] && break; sleep 0.5; done
export WAYLAND_DISPLAY=wayland-0
QT_QPA_PLATFORM=offscreen setsid nohup /usr/lib/libexec/kactivitymanagerd >/data/soft-kactivity.log 2>&1 </dev/null &
sleep 1
kded6 >/dev/null 2>&1 &
sleep 1
setsid nohup sh -c 'plasmashell > /data/soft-pshell.log 2>&1; echo PLASMA_EXIT=$? > /data/soft-plasma.exit' >/dev/null 2>&1 </dev/null &
sleep 12
echo "== ps"
ps -eo pid,pcpu,comm | grep -E "kwin_wayland|plasmashell" | sort -u
echo "== kwin"
tail -3 /data/soft-kwin.log
