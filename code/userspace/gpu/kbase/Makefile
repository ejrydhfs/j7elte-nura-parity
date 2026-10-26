# Userspace client for the r15p0 kbase UK 10.6 interface.
#
#   make                 aarch64 test binaries (static, so they run on the
#                        musl-based Nura rootfs without a glibc runtime)
#   make CC=gcc          host build (ABI self-check / qemu smoke test)
#   make check-abi       compare kbase_uapi.h with the kernel's own headers
#   make clean
#
# Cross toolchain: the session's aarch64 GCC 10.3 or the distro cross gcc.
CROSS_COMPILE ?= aarch64-linux-gnu-
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS ?= -static
KERNEL_DIR ?= /run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07

BINS = kbase-selftest
OBJS = kbase.o

all: $(BINS)

kbase.o: kbase.c kbase.h kbase_uapi.h
	$(CC) $(CFLAGS) -c -o $@ kbase.c

kbase-selftest: kbase-selftest.c kbase.o kbase.h kbase_uapi.h
	$(CC) $(CFLAGS) -o $@ kbase-selftest.c kbase.o $(LDFLAGS)

# Reproducible evidence that the wire header matches the kernel headers.
check-abi:
	KERNEL_DIR=$(KERNEL_DIR) CC=$${HOSTCC:-cc} sh tools/verify-abi.sh

# 32-bit ARM AAPCS layout (what a compat client / armhf DDK sends).
check-abi32:
	KERNEL_DIR=$(KERNEL_DIR) sh tools/verify-abi32.sh

clean:
	rm -f $(BINS) $(OBJS)

.PHONY: all check-abi check-abi32 clean
