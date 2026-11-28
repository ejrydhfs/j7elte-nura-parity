#!/bin/sh
mkdir -p /data/tmp && export TMPDIR=/data/tmp
cd /data/build/plasma-mobile-6.8.3
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DINSTALL_SYSTEMD_SERVICE=OFF -DBUILD_TESTING=OFF 2>&1 | tail -3
nice -n 5 cmake --build build -j1 2>&1 | tail -4
echo "rc=$?"
free -m | head -2
