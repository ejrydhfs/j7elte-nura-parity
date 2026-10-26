# egl-probe: load the ROM's Android-23/arm32 libGLES_mali.so under qemu-arm
# with the ROM's own bionic linker.  No NDK, no libhybris, no kernel driver.
ROOT      ?= ../romroot
BLOB      ?= ../egl_extract/libGLES_mali.so
CC         = clang
TARGET    ?= armv7a-linux-gnueabi
CFLAGS    ?= --target=$(TARGET) -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 \
             -O2 -fPIE -fno-stack-protector -fno-builtin -Wall -Wextra
LDFLAGS   ?= -nostdlib -pie -fuse-ld=lld \
             -Wl,-dynamic-linker,/system/bin/linker \
             -Wl,--build-id=none -Wl,-z,noexecstack -Wl,-z,notext

all: egl-probe

egl-probe: start.S harness.c $(BLOB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S harness.c \
	  $(BLOB) \
	  $(ROOT)/system/lib/libdl.so $(ROOT)/system/lib/libc.so \
	  $(ROOT)/system/lib/libm.so

clean:
	rm -f egl-probe
.PHONY: all clean
