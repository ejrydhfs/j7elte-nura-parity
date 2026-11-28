#!/bin/sh
# KWin alone under kms_swrast/llvmpipe, no plasmashell.
pkill -9 -f kwin[_]wayland
pkill -9 -f plasmashel[l]
sleep 2
RUN=/data/plasma-run
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export MESA_LOADER_DRIVER_OVERRIDE=kms_swrast LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_BACKEND=software PAN_MESA_DEBUG=noafbc
export LD_PRELOAD="/home/user/segv.so /home/user/addfb_shim.so"
setsid nohup kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/kwin-alone.log 2>&1 &
sleep 8
echo "== ps"
ps -eo pid,comm | grep kwin
echo "== exit"
cat /data/kwin.exit 2>/dev/null
echo "== log"
tail -6 /data/kwin-alone.log
