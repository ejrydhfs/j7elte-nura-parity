#!/bin/sh
F=/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc
sed -i 's/^plugin=org.kde.plasma.mobile.homescreen.folio$/plugin=org.kde.plasma.mobile.homescreen.halcyon/' $F
grep -n "^plugin=" $F
sh /home/user/cleanstart15.sh >/dev/null 2>&1
sleep 2
ps -eo comm | grep -c plasmashell
