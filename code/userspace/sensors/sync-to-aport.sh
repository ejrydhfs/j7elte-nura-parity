#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copy the canonical daemon sources into the pmaports aport directory.
# Run from anywhere; the aport is found relative to this script.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
aport="$here/../../pmaports/device/archived/j7elte-sensors"

mkdir -p "$aport"
for f in j7elte-sensors.c dbus-wire.c dbus-wire.h Makefile \
	 j7elte-sensors.service j7elte-sensors.initd j7elte-sensors.conf; do
	cp -f "$here/$f" "$aport/$f"
done

echo "copied sources to $aport"
echo "update sha512sums in APKBUILD with:"
( cd "$aport" && sha512sum j7elte-sensors.c dbus-wire.c dbus-wire.h \
	Makefile j7elte-sensors.service j7elte-sensors.initd \
	j7elte-sensors.conf )
