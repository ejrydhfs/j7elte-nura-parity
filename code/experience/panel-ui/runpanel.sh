#!/bin/sh
export DBUS_SESSION_BUS_ADDRESS=unix:path=/data/plasma-run/bus
JS=$(cat /data/panel.js)
gdbus call --address unix:path=/data/plasma-run/bus \
  --dest org.kde.plasmashell --object-path /PlasmaShell \
  --method org.kde.PlasmaShell.evaluateScript "$JS" 2>&1
sleep 2
tail -6 /data/clean-pshell.log
