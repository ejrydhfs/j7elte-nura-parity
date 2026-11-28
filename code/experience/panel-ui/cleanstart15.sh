#!/bin/sh
pkill -9 -f kwin_wayland; pkill -9 -f plasmashell; pkill -9 -f kactivity; pkill -9 -f "initial[_]start"; sleep 3
echo "strays: $(pgrep -f kwin_wayland | wc -l) kwin, $(pgrep -f plasmashell | wc -l) pshell"
RUN=/data/plasma-run
mkdir -p $RUN/config $RUN/cache $RUN/data $RUN/doc /data/tmp
chmod 700 $RUN
export XDG_RUNTIME_DIR=$RUN XDG_CONFIG_HOME=$RUN/config XDG_CACHE_HOME=$RUN/cache XDG_DATA_HOME=$RUN/data
export XDG_SESSION_ID=kwin-try-noop
export KWIN_DRM_DEVICES=/dev/dri/card0 KWIN_RENDER_NODES=/dev/dri/renderD128 KWIN_COMPOSE=O2ES QT_QPA_PLATFORM=wayland
export QT_LOGGING_RULES=kwin_core.debug=true
export PAN_MESA_DEBUG=noafbc
export LD_PRELOAD=/home/user/addfb_shim.so
export XCURSOR_THEME=breeze_cursors XCURSOR_SIZE=24
export PLASMA_PLATFORM=phone:handset PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland
# keep the shell from launching the (blank) first-run wizard
[ -e /usr/sbin/plasma-mobile-initial-start.real ] || mv /usr/sbin/plasma-mobile-initial-start /usr/sbin/plasma-mobile-initial-start.real
printf '#!/bin/sh\nexit 0\n' > /usr/sbin/plasma-mobile-initial-start
chmod +x /usr/sbin/plasma-mobile-initial-start
# wallpaper (persisted config)
F=$RUN/config/plasma-org.kde.plasma.mobileshell-appletsrc
if [ -f $F ] && ! grep -q '^\[Containments\]\[1\]\[Wallpaper\]' $F; then
cat >> $F <<CFG

[Containments][1][Wallpaper][org.kde.image][General]
Image=file:///usr/share/wallpapers/postmarketos.jpg
FillMode=2
CFG
fi
dbus-daemon --session --address=unix:path=$RUN/bus --fork --print-pid > $RUN/bus.pid 2>/dev/null
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUN/bus
kwin_wayland --drm --socket wayland-0 > /data/clean-kwin.log 2>&1 &
for i in $(seq 1 60); do [ -S $RUN/wayland-0 ] && break; sleep 0.5; done
export WAYLAND_DISPLAY=wayland-0
QT_QPA_PLATFORM=offscreen /usr/libexec/kactivitymanagerd >/dev/null 2>&1 &
sleep 1
kded6 >/dev/null 2>&1 &
sleep 1
nohup plasmashell > /data/clean-pshell.log 2>&1 &
sleep 12
echo "== kwin"; grep -aicE "Failed to create framebuffer" /data/clean-kwin.log
echo "== pshell"; wc -l < /data/clean-pshell.log; grep -aiE "error|Cannot" /data/clean-pshell.log | tail -3
ps -eo comm | grep -E "kwin_wayland|plasmashell" | sort -u
