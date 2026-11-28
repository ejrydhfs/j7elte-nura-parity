#!/bin/sh
# GPU must be powered: a suspended tiler makes panfrost submits fail
# keep the first-run wizard from covering the shell
[ -e /usr/sbin/plasma-mobile-initial-start.real ] || mv /usr/sbin/plasma-mobile-initial-start /usr/sbin/plasma-mobile-initial-start.real
printf '#!/bin/sh\nexit 0\n' > /usr/sbin/plasma-mobile-initial-start
chmod +x /usr/sbin/plasma-mobile-initial-start
GPUD=/sys/bus/platform/devices/11400000.mali/power
echo on > $GPUD/control 2>/dev/null
[ "$(cat $GPUD/runtime_status 2>/dev/null)" = "active" ] || echo "WARN: GPU not active"
V=$(cat /sys/module/exynos_drm7580/version 2>/dev/null)
[ "$V" = "int129" ] || { echo "module version [$V] != int102 - refusing to start"; exit 1; }
echo "module version: $V"
RUN=/data/plasma-run
if [ -f $RUN/bus.pid ]; then
  BP=$(cat $RUN/bus.pid)
  [ -n "$BP" ] && kill -9 "$BP" 2>/dev/null
fi
pkill -9 -f plasmashel[l]; pkill -9 -f kwin[_]wayland; pkill -9 -f kactivit[y]; pkill -9 -f "initial[_]start"
sleep 3
rm -f $RUN/bus $RUN/bus.pid $RUN/wayland-0 $RUN/wayland-0.lock
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc /data/tmp; chmod 700 $RUN
grep -q fullscreenEnabled $RUN/config/kwinrc 2>/dev/null || printf '[Plugins]\nfullscreenEnabled=false\n' >> $RUN/config/kwinrc
export KWIN_USE_OVERLAYS=0
export KWIN_DRM_NO_DIRECT_SCANOUT=1
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export QT_LOGGING_RULES="*=true"
export PAN_MESA_DEBUG=noafbc
export LD_PRELOAD=/home/user/addfb_shim.so
export XCURSOR_THEME=breeze_cursors XCURSOR_SIZE=24
export PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
dbus-daemon --session --address=unix:path=$RUN/bus --fork --print-pid > $RUN/bus.pid
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
echo "daemon pid: $(cat $RUN/bus.pid)"
kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/clean-kwin.log 2>&1 &
for i in $(seq 1 60); do [ -S $RUN/wayland-0 ] && break; sleep 0.5; done
export WAYLAND_DISPLAY=wayland-0
QT_QPA_PLATFORM=offscreen /usr/libexec/kactivitymanagerd >/dev/null 2>&1 &
sleep 1
kded6 >/dev/null 2>&1 &
sleep 1
nohup plasmashell > /data/clean-pshell.log 2>&1 &
sleep 12
echo "== mobileshell names: $(qdbus6 2>/dev/null | grep -ic mobileshell)"
echo "== fb errors: $(grep -c 'Failed to create framebuffer' /data/clean-kwin.log)"
echo "== dbus lost: $(grep -ac 'Not connected to D-Bus' /data/clean-pshell.log)"
ps -eo comm | grep -E "kwin_wayland|plasmashell" | sort -u
