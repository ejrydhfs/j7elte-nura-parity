#!/bin/sh
RUN=/data/plasma-run
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc /data/tmp; chmod 700 $RUN
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128
export QT_LOGGING_RULES="kwin_core.debug=true"
export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=swrast GALLIUM_DRIVER=llvmpipe
export XCURSOR_THEME=breeze_cursors XCURSOR_SIZE=24
rm -f $RUN/bus $RUN/bus.pid
dbus-daemon --session --address=unix:path=$RUN/bus --fork --print-pid > $RUN/bus.pid
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/kwinonly.log 2>&1 &
for i in $(seq 1 60); do [ -S $RUN/wayland-0 ] && break; sleep 0.5; done
export WAYLAND_DISPLAY=wayland-0
sleep 4
echo "kwin=$(pidof kwin_wayland) sock=$(ls $RUN/wayland-0 2>/dev/null)"
