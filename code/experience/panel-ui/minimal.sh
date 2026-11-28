#!/bin/sh
RUN=/data/plasma-run
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export PAN_MESA_DEBUG=noafbc LD_PRELOAD=/home/user/addfb_shim.so
export WAYLAND_DISPLAY=wayland-0
echo omp-bringup > /sys/power/wake_lock
insmod /home/user/drm_kms_helper-int95.ko 2>/dev/null
insmod /home/user/exynos-drm7580-int116.ko 2>/dev/null
rm -f $RUN/wayland-0
kwin_wayland --drm --no-lockscreen --socket wayland-0 > /data/min-kwin.log 2>&1 </dev/null >/dev/null 2>&1 &
for i in $(seq 1 100); do [ -S $RUN/wayland-0 ] && break; sleep 0.2; done
echo "socket up K=$(pidof kwin_wayland)"
cd /data
XDG_RUNTIME_DIR=$RUN WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland PAN_MESA_DEBUG=noafbc nohup qml6 /data/plain.qml >/data/min-cl.log 2>&1 &
sleep 3
echo "client Q=$(pidof qml6)"
echo 1 > /sys/module/exynos_drm7580/parameters/exynos7580_dump_fb
kill -9 $(pidof qml6) 2>/dev/null
sleep 1
dmesg | grep -a scanout-dump | tail -6
echo "armed=$(cat /sys/module/exynos_drm7580/parameters/exynos7580_dump_fb)"
