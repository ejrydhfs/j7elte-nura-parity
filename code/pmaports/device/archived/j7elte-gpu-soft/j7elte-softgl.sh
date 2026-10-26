# j7elte: the kernel exposes kbase (/dev/mali0), not a DRM render node, and the
# ROM's libGLES_mali.so is 32-bit Android/bionic.  There is no usable hardware
# GL driver for Nura, so force Mesa's llvmpipe software rasterizer.
# Remove this file after a hardware GL/DRM driver is available.
export LIBGL_ALWAYS_SOFTWARE=1
