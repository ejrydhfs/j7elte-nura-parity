#!/bin/sh
pkill -9 -f kwin[_]wayland
pkill -9 -f plasmashel[l]
sleep 2
RUN=/data/plasma-run
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export $1
export LD_PRELOAD="/home/user/addfb_shim.so"
echo "MARK-$RANDOM" > /dev/kmsg
setsid nohup kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/kwin-alone.log 2>&1 &
sleep 5
echo "== counts since marker"
dmesg | sed -n '/MARK-/,$p' | grep -ac "p7580_dumb_create"
dmesg | sed -n '/MARK-/,$p' | grep -ac "p7580_create_bo"
dmesg | sed -n '/MARK-/,$p' | grep -ac "p7580_dumb_map"
dmesg | sed -n '/MARK-/,$p' | grep -ac "p7580_gem_mmap"
echo "== ps"
ps -eo pid,comm | grep kwin
