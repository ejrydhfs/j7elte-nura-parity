#!/bin/sh
# Apply the kbase winsys patches to a Mesa checkout (27.0.0-devel era).
#
#   ./apply.sh /path/to/mesa [branch]
#
# The patches add:
#   0001  src/panfrost/lib/kmod/kbase_kmod.c  (pan_kmod kbase backend, UK 10.6)
#   0002  backend-neutral JM context/submit in the Gallium driver
#   0003  panfrost-kbase-skeleton bring-up executable
#
# The standalone winsys library tested without Mesa lives in
# parity/userspace/gpu/kbase-winsys/; the Mesa backend is self-contained so
# the driver does not depend on that tree.
set -eu

mesa=${1:?usage: apply.sh /path/to/mesa [branch]}
branch=${2:-kbase-winsys}

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cd "$mesa"
git checkout -B "$branch"
for p in "$here"/000*.patch; do
	echo "applying $(basename "$p")"
	git am --3way "$p"
done

echo
echo "build (host, no OpenCL/LLVM):"
echo "  meson setup build -Dgallium-drivers=panfrost -Dvulkan-drivers= \\"
echo "    -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \\"
echo "    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \\"
echo "    -Dpanfrost-rust=false -Dbuild-tests=true"
echo "  ninja -C build src/panfrost/lib/kbase-kmod-selftest"
