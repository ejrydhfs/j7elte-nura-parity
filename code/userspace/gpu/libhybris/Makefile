# Two prototypes, one question: can the ROM's 32-bit Mali GL stack run here?
#
#   eglprobe - ELF32 bionic executable (PT_INTERP /system/bin/linker, NEEDED
#              libc.so/libdl.so from the ROM) that dlopen()s the blob.
#              The faithful Android path: the ROM's own linker loads it.
#
#   mloader  - self-contained ELF32 ARM loader (no bionic at all) that maps
#              the blob, applies its Android APS2 relocations, resolves its
#              bionic/C++/HAL imports against a shim layer and calls the EGL
#              entry points.  Used because bionic itself deadlocks under
#              qemu-user (see parity/reports/gpu-libhybris.md §4).
#
# Toolchain: upstream clang + ld.lld targeting armv7a-linux-gnueabi.  The host
# binutils has no ARM emulation, and no ARM sysroot exists, hence -nostdlib.

TARGET   ?= armv7a-linux-gnueabi
ROOT     ?= romroot
ifeq ($(origin CC),default)
CC       := clang
endif
CFLAGS   ?= -O2 -Wall -Wextra -ffreestanding
CFLAGS   += --target=$(TARGET) -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16
CFLAGS   += -fno-builtin -fno-stack-protector
LDFLAGS  ?= -fuse-ld=lld -Wl,-e,_start

# --- eglprobe: bionic PIE, linked against the ROM's own libc/libdl ----------
PIE_CFLAGS  = $(CFLAGS) -nostdlib -fPIC
PIE_LDFLAGS = $(LDFLAGS) -Wl,-pie -Wl,--dynamic-linker=/system/bin/linker
PIE_LIBS    = $(ROOT)/system/lib/libdl.so $(ROOT)/system/lib/libc.so

# --- mloader: static freestanding, no runtime at all ------------------------
ML_CFLAGS   = $(CFLAGS) -nostdlib -static -fno-pic
ML_LDFLAGS  = $(LDFLAGS)

all: eglprobe mloader

eglprobe: eglprobe.c
	$(CC) $(PIE_CFLAGS) $(PIE_LDFLAGS) -o $@ $< $(PIE_LIBS)

mloader: mloader.c
	$(CC) $(ML_CFLAGS) $(ML_LDFLAGS) -o $@ $<

check: all
	@echo "--- eglprobe"
	readelf -h eglprobe | grep -E 'Class|Machine|Type'
	readelf -d eglprobe | grep -E 'NEEDED' || true
	readelf -lW eglprobe | grep -A1 INTERP | tail -1
	@echo "--- mloader"
	readelf -h mloader | grep -E 'Class|Machine|Type'
	readelf -lW mloader | grep -c INTERP || echo "no PT_INTERP (static)"

clean:
	rm -f eglprobe mloader

.PHONY: all check clean
