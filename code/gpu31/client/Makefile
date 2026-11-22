# pf7580 client - userspace test driver for the six v5.2 panfrost ioctls.
#
#   make                 build the static library + selftest for the host
#   make CROSS_COMPILE=aarch64-linux-gnu-
#                        cross-build (static, for the musl/ARM userland)
#   make check           run the mock self-tests (no hardware needed)
#   make check-aarch64   run the cross binary under qemu-aarch64
#
# Self-contained C11: only <drm/drm.h> + libc, no libdrm. The driver UAPI
# mirror is vendored as panfrost_drm.h (byte-identical to Linux v5.2).
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
AARCH64_CC ?= $(if $(CROSS_COMPILE),$(CROSS_COMPILE)gcc,aarch64-linux-gnu-gcc)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter \
	   -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS  ?=

QEMU    ?= qemu-aarch64
# qemu user-mode needs the cross sysroot for the dynamic loader.
QEMU_SYSROOT ?= /usr/aarch64-linux-gnu

STATIC ?= 1
ifeq ($(STATIC),1)
LDFLAGS_aarch64 := -static
endif

GEM_DIR := ../gem

INCLUDES := -I. -I$(GEM_DIR)

LIB_SRCS := pf7580.c
LIB_OBJS := build/pf7580.o
LIB      := build/libpf7580.a

SELFTEST := build/pf7580-selftest

AARCH64_LIB := build-aarch64/libpf7580.a
AARCH64_WDARM := build-aarch64/wdarm
AARCH64_SELFTEST := build-aarch64/pf7580-selftest


.PHONY: all lib check check-aarch64 clean

all: lib $(SELFTEST)

lib: $(LIB)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

build/pf7580.o: pf7580.c pf7580.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(SELFTEST): pf7580-selftest.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

check: $(SELFTEST)
	@$(SELFTEST) --print-abi
	@$(SELFTEST)

check-aarch64: $(AARCH64_SELFTEST)
	$(QEMU) -L $(QEMU_SYSROOT) $(AARCH64_SELFTEST) --print-abi
	$(QEMU) -L $(QEMU_SYSROOT) $(AARCH64_SELFTEST)

$(AARCH64_LIB): pf7580.c pf7580.h
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) -c pf7580.c -o build-aarch64/pf7580.o
	$(AR) rcs $@ build-aarch64/pf7580.o

$(AARCH64_SELFTEST): pf7580-selftest.c $(AARCH64_LIB)
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) $< $(AARCH64_LIB) $(LDFLAGS_aarch64) $(LDLIBS) -o $@

$(AARCH64_WDARM): wdarm.c
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) $< -static -o $@

clean:
	rm -rf build build-aarch64
