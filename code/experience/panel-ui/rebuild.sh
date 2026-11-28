#!/bin/sh
set -e
export TMPDIR=/data/tmp
cd /data/build/plasma-mobile-6.8.3
cmake --build build -j1 2>&1 | tail -3
rm -rf /data/stage
DESTDIR=/data/stage cmake --install build >/dev/null 2>&1
cp -a /data/stage/usr/. /usr/
echo "== reinstalled"
grep -n "contentItem ? contentItem" /usr/lib/qt6/qml/org/kde/plasma/private/mobileshell/BaseItem.qml | head -2
grep -n "mode: (height" /usr/lib/qt6/qml/org/kde/plasma/private/mobileshell/ActionDrawer.qml 2>/dev/null | head -1
