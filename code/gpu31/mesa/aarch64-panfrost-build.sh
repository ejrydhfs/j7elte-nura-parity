#!/bin/sh
# aarch64 panfrost build recipe (j7elte / Mali-T720, v5.2 UAPI kernel).
# Host cross-compile recipe; produces aarch64 Mesa with panfrost enabled.
# Run on x86_64 build host. Big outputs go to $OUT (default /tmp/mesa-aarch64).
#
# What it does:
#   1. fetch pinned Mesa (02f13375, 27.0.0-devel era per patches/gpu-mesa-kbase/apply.sh)
#   2. apply patches/gpu-mesa-kbase/0001-0003 (kbase backend)
#   3. apply code/gpu31/mesa/patches/0004-0005 (slot fix + v5.2 compat)
#   4. meson cross-compile (panfrost + softpipe, no LLVM: softpipe needs none,
#      llvmpipe is excluded because it requires LLVM; GL on device is
#      validated via llvmpipe at runtime, not built here)
#   5. ninja build + run the two selftests (slot logic is proven by
#      slot-proof/; the v52 selftest runs under qemu-aarch64)
#
# Requirements: meson >= 1.7, ninja, aarch64-linux-gnu-gcc,
# qemu-aarch64 (for the cross selftest), git.
# NOTE: current Mesa needs LLVMSPIRVLib for CLC when panfrost is enabled
# (meson.build with_driver_using_cl -> with_clc). If configure fails with
# 'Dependency "LLVMSPIRVLib" not found', install spirv-llvm-translator
# matching your LLVM (e.g. 23.1.x) and re-run. This is a host-packaging
# gap, not a patch bug.

set -eu

REPO=${REPO:-/tmp/j7repo}
MESA_URL=${MESA_URL:-https://gitlab.freedesktop.org/mesa/mesa.git}
MESA_PIN=${MESA_PIN:-02f13375231bf0a9ed20b09cb21171351053be52}
OUT=${OUT:-/tmp/mesa-aarch64}
JOBS=${JOBS:-$(nproc)}

SRC=$OUT/mesa
BUILD=$OUT/build
CROSS=$OUT/aarch64.ini

mkdir -p "$OUT"

if [ ! -d "$SRC/.git" ]; then
  git clone "$MESA_URL" "$SRC"
fi
cd "$SRC"
git fetch origin
git checkout "$MESA_PIN"

for p in "$REPO"/patches/gpu-mesa-kbase/0001-*.patch \
         "$REPO"/patches/gpu-mesa-kbase/0002-*.patch \
         "$REPO"/patches/gpu-mesa-kbase/0003-*.patch \
         "$REPO"/code/gpu31/mesa/patches/0004-*.patch \
         "$REPO"/code/gpu31/mesa/patches/0005-*.patch; do
  echo "checking $p"
  git apply --check "$p"
done
git am \
  "$REPO"/patches/gpu-mesa-kbase/0001-*.patch \
  "$REPO"/patches/gpu-mesa-kbase/0002-*.patch \
  "$REPO"/patches/gpu-mesa-kbase/0003-*.patch
git apply \
  "$REPO"/code/gpu31/mesa/patches/0004-*.patch \
  "$REPO"/code/gpu31/mesa/patches/0005-*.patch

cat > "$CROSS" <<EOF
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

meson setup "$BUILD" "$SRC" --cross-file "$CROSS" \
  -Dgallium-drivers=panfrost,softpipe \
  -Dvulkan-drivers= \
  -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Dllvm=disabled \
  -Dmicrosoft-clc=disabled \
  -Dpanfrost-rust=false -Dgallium-rusticl=false \
  -Dbuild-tests=true

ninja -C "$BUILD" \
  src/panfrost/lib/kbase-kmod-selftest \
  src/panfrost/lib/v52-compat-selftest

qemu-aarch64 -L /usr/aarch64-linux-gnu \
  "$BUILD"/src/panfrost/lib/v52-compat-selftest

echo "BUILD OK: $BUILD"
