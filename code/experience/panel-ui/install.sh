#!/bin/sh
set -e
rm -rf /data/stage
cd /data/build/plasma-mobile-6.8.3
DESTDIR=/data/stage cmake --install build 2>&1 | tail -3
cp -a /data/stage/usr/. /usr/
echo "== installed"; ls -l /usr/lib/qt6/plugins/plasma/applets/org.kde.plasma.mobile.homescreen.folio.so
