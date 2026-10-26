#!/bin/sh
# Build the Nura j7elte 3.10.108 kernel with the GCC 10 ARM toolchain.
# Usage: build.sh <source-tree> <build-dir> [make-target...]
# Optional: CONFIG_FRAGMENT=<file> appends a Kconfig fragment to the baseline
# config before olddefconfig (e.g. parity/patches/drm/config-drm-exynos7580.fragment).
set -eu
SRC=$(realpath "${1:?source tree}")
OUT=$(realpath -m "${2:?build dir}")
shift 2 || true
TARGETS=${*:-Image dtbs}
TC=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin
CROSS="$TC/aarch64-none-linux-gnu-"
# Default to the config pmaports actually ships (WiFi + BT patches applied).
# config-nura-j7elte is the pre-patch copy and no longer matches the package.
CFG=${CFG:-/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/parity/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64}
mkdir -p "$OUT"
cp "$CFG" "$OUT/.config"
if [ -n "${CONFIG_FRAGMENT:-}" ]; then
    cat "$CONFIG_FRAGMENT" >> "$OUT/.config"
fi
export ARCH=arm64
export CROSS_COMPILE="$CROSS"
export KBUILD_BUILD_VERSION=1
export KCFLAGS="-Wno-error"
export HOSTCFLAGS="-std=gnu89 -fcommon -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types"
make -C "$SRC" O="$OUT" olddefconfig
make -C "$SRC" O="$OUT" -j"$(nproc)" $TARGETS
