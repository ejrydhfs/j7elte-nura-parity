#!/bin/sh
B=/data/build/plasma-mobile-6.8.3/components/mobileshell/qml
# 1) Enum access through the module alias is undefined at binding time under
#    Qt 6.10; use the declared values (Portrait = 0, Landscape = 1).
sed -i 's/MobileShell\.ActionDrawer\.Portrait/0/g; s/MobileShell\.ActionDrawer\.Landscape/1/g' \
    $B/actiondrawer/ActionDrawer.qml $B/actiondrawer/private/ContentContainer.qml
# 2) contentItem can still be null during initialisation; guard the implicit size.
sed -i 's/implicitHeight: topPadding + bottomPadding + contentItem\.implicitHeight/implicitHeight: topPadding + bottomPadding + (contentItem ? contentItem.implicitHeight : 0)/' \
    $B/components/BaseItem.qml
sed -i 's/implicitWidth: leftPadding + rightPadding + contentItem\.implicitWidth/implicitWidth: leftPadding + rightPadding + (contentItem ? contentItem.implicitWidth : 0)/' \
    $B/components/BaseItem.qml
grep -n "mode: (height" $B/actiondrawer/ActionDrawer.qml
grep -n "implicitHeight\|implicitWidth" $B/components/BaseItem.qml | head -3
