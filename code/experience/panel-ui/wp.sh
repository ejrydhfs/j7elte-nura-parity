#!/bin/sh
F=/data/plasma-run/config/plasma-org.kde.plasma.mobileshell-appletsrc
if ! grep -q "^\[Containments\]\[1\]\[Wallpaper\]\[org.kde.image\]\[General\]" $F; then
cat >> $F <<CFG

[Containments][1][Wallpaper][org.kde.image][General]
Image=file:///usr/share/wallpapers/postmarketos.jpg
FillMode=2
CFG
fi
grep -n "Wallpaper" $F | head -3
