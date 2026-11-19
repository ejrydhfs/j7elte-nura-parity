# Host contract test for the r7p0 compat stride fix (review section 11 F1).
#
#   make          build native binary
#   make check    build + run (native and aarch64 under qemu)
#   make clean
#
# The compat header itself lives in the shared tree
# (patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h) and is NOT edited
# here; a copy is staged into build/ so the test compiles without touching
# the kernel tree.  stub/ supplies the four kernel headers the compat header
# includes, with r15p0-measured offsets (see stub/mali_base_kernel.h).
# Nothing here builds/flashes the kernel or touches the device.
HEADER   := ../../../patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h
CC       ?= gcc
ARCH_CC  ?= aarch64-linux-gnu-gcc
QEMU     ?= qemu-aarch64
CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra -Werror

NATIVE   := build/stride_test
AARCH64  := build/stride_test.aarch64

all: $(NATIVE) $(AARCH64)

build/staged/mali_kbase_uku_r7p0_compat.h: $(HEADER)
	@mkdir -p build/staged
	cp $(HEADER) $@

$(NATIVE): stride_test.c build/staged/mali_kbase_uku_r7p0_compat.h stub/linux/types.h stub/linux/stddef.h stub/linux/string.h stub/mali_uk.h stub/mali_base_kernel.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Ibuild/staged -Istub -o $@ stride_test.c

$(AARCH64): stride_test.c build/staged/mali_kbase_uku_r7p0_compat.h stub/linux/types.h stub/linux/stddef.h stub/linux/string.h stub/mali_uk.h stub/mali_base_kernel.h
	@mkdir -p build
	$(ARCH_CC) $(CFLAGS) -static -Ibuild/staged -Istub -o $@ stride_test.c

check: all
	@echo "== native =="
	./$(NATIVE)
	@echo "== aarch64 (qemu) =="
	$(QEMU) ./$(AARCH64)

clean:
	rm -rf build

.PHONY: all check clean
