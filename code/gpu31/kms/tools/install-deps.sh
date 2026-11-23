#!/bin/sh
# install-deps.sh - install the stock KMS test tools on the device (root).
#
# modetest lives in Alpine's libdrm-tests and kmscube is its own package;
# both exist in the postmarketOS/Alpine repos configured on the phone, so
# neither has to be built. Everything else (libdrm-dev, mesa-dev, gcc) is
# already installed - see docs/reports/decon-kms-tooling.md section 1.
#
#   sudo sh install-deps.sh
#
# Network is the USB-NAT host link; `apk add --simulate` was verified before
# this script was written. No reboot, no flash.
set -u

if [ "$(id -u)" -ne 0 ]; then
	printf 'install-deps: needs root (run under /usr/local/bin/rootshell or sudo -n)\n' >&2
	exit 1
fi

printf '== before ==\n'
for p in libdrm libdrm-dev mesa mesa-dev mesa-egl mesa-gbm kmscube libdrm-tests; do
	printf '%-14s %s\n' "$p" "$(apk info -v "$p" 2>/dev/null || echo 'not installed')"
done

# kmscube: reference GBM/EGL/KMS client for M2.
# libdrm-tests: modetest (KMS resource listing + pattern animation).
if ! apk info -e kmscube >/dev/null 2>&1 || ! apk info -e libdrm-tests >/dev/null 2>&1; then
	printf '== apk add kmscube libdrm-tests ==\n'
	apk add kmscube libdrm-tests || exit 1
fi

printf '== after ==\n'
for p in kmscube libdrm-tests libdrm mesa; do
	printf '%-14s %s\n' "$p" "$(apk info -v "$p" 2>/dev/null || echo 'not installed')"
done
printf 'modetest: %s\n' "$(command -v modetest || echo MISSING)"
printf 'kmscube:  %s\n' "$(command -v kmscube || echo MISSING)"
printf 'libdrm:   %s\n' "$(pkg-config --modversion libdrm 2>/dev/null || echo '?')"

command -v modetest >/dev/null && command -v kmscube >/dev/null || exit 1
printf 'INSTALL-DEPS: PASS\n'
