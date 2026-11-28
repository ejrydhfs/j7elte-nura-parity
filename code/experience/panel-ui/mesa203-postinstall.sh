#!/bin/sh
# Runs on the device after the build: installs into /data/mesa203-install,
# adds the libgallium_dri.so compatibility link, prints the tree.
set -eu
cd /data/mesa203-build
ninja -C build install
D=/data/mesa203-install
ln -f "$D/lib/dri/panfrost_dri.so" "$D/lib/dri/libgallium_dri.so"
echo "=== libs ==="
ls -l "$D/lib"/*.so* 2>/dev/null | sed 's/^/  /'
echo "=== dri ==="
ls -l "$D/lib/dri" | sed 's/^/  /'
echo "=== needed (panfrost_dri) ==="
readelf -d "$D/lib/dri/panfrost_dri.so" | grep NEEDED
echo "=== needed (libEGL) ==="
readelf -d "$D/lib/libEGL.so.1" | grep NEEDED
echo "=== sonames ==="
for f in "$D"/lib/libEGL.so.1 "$D"/lib/libGLESv2.so.2 "$D"/lib/libgbm.so.1 "$D"/lib/libglapi.so.0; do
	printf '%s: ' "$f"; readelf -d "$f" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p'
done
echo "=== arch ==="
file "$D/lib/dri/panfrost_dri.so" "$D/lib/libEGL.so.1"
