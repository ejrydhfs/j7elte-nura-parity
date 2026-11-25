# mesaprobe / drmshim - Mesa Gbm/EGL pairing probe for the DECON KMS node
#
# Native on-device build (musl aarch64, libdrm-dev + mesa-dev installed):
#   make            mesaprobe + drmshim.so
#   make check      mesaprobe --info /dev/dri/card0 (needs the KMS modules)
#   make clean
#
# Headers come from the *system* Mesa 26.3 (mesa-dev); the binaries can be
# pointed at /data/mesabuild/mesa20-b at run time through LD_LIBRARY_PATH -
# see run-matrix.sh and ../env.sh.

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE

PKG      := $(shell command -v pkg-config 2>/dev/null)
HAVE_PKG := $(shell $(PKG) --exists libdrm 2>/dev/null && echo 1)
ifeq ($(HAVE_PKG),1)
CPPFLAGS += $(shell $(PKG) --cflags libdrm gbm egl glesv2 2>/dev/null)
LDLIBS   += $(shell $(PKG) --libs libdrm gbm egl glesv2 2>/dev/null || echo -lgbm -lEGL -lGLESv2 -ldrm)
else
CPPFLAGS += -I/usr/include/libdrm
LDLIBS   += -ldrm -lgbm -lEGL -lGLESv2 -ldl
endif

all: mesaprobe drmshim.so

mesaprobe: mesaprobe.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

drmshim.so: drmshim.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -shared -o $@ $< -ldl

# --ver/--opens only: no modeset ioctl, safe on a node whose lock_all is wedged
check: mesaprobe
	./mesaprobe --ver /dev/dri/card0 --opens

clean:
	rm -f mesaprobe drmshim.so

.PHONY: all check clean
