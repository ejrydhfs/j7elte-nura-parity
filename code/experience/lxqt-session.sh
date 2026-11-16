#!/bin/sh
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p /run/user/0; chmod 700 /run/user/0
/usr/sbin/openbox &
sleep 1
/usr/sbin/lxqt-panel &
/usr/sbin/pcmanfm-qt --desktop &
wait
