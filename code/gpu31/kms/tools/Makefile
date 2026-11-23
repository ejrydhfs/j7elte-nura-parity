# kmsflip - KMS page-flip + deterministic pattern checker (j7elte)
#
# Native on-device build (musl userland, no cross sysroot with DRM/EGL):
#   make                 build kmsflip with the EGL/GBM headless path
#   make NO_EGL=1        KMS-only build (libdrm headers alone)
#   make check           headless smoke (needs /dev/dri/renderD128)
#   make clean
#
# Links the *system* libdrm; Mesa/EGL/GBM can be redirected at run time to
# /data/mesabuild/mesa20-b via env.sh (see run-headless.sh).

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
CFLAGS  += -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
NO_EGL  ?= 0

PKG      := $(shell command -v pkg-config 2>/dev/null)
HAVE_PKG := $(shell $(PKG) --exists libdrm 2>/dev/null && echo 1)

ifneq ($(HAVE_PKG),1)
$(error pkg-config with libdrm.pc is required (apk add libdrm-dev pkgconf))
endif

CPPFLAGS += $(shell $(PKG) --cflags libdrm)
LDLIBS   += $(shell $(PKG) --libs libdrm)

ifeq ($(NO_EGL),0)
CFLAGS   += -DHAVE_EGL=1
CPPFLAGS += $(shell $(PKG) --cflags gbm egl glesv2 2>/dev/null)
LDLIBS   += $(shell $(PKG) --libs gbm egl glesv2 2>/dev/null || echo -lgbm -lEGL -lGLESv2)
else
CFLAGS   += -DHAVE_EGL=0
endif

all: kmsflip

kmsflip: kmsflip.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

check: kmsflip
	./kmsflip --headless --frames 3

clean:
	rm -f kmsflip

.PHONY: all check clean
