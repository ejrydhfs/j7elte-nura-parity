# kbase winsys (UK 10.6) for the Panfrost Gallium driver.
#
#   make                 build the static library + selftest for the host
#   make aarch64         cross-build (aarch64-linux-gnu-gcc or CROSS_COMPILE)
#   make check-abi       verify the GPU-props layout against the kernel headers
#   make check           run the host selftest (exit 2 = no /dev/maliN, OK)
#
# The kbase client (kbase.c/kbase_uapi.h) is reused from ../kbase, which is
# the ABI-verified reference for this kernel.

CC ?= cc
CROSS_COMPILE ?=
AARCH64_CC ?= $(if $(CROSS_COMPILE),$(CROSS_COMPILE)gcc,aarch64-linux-gnu-gcc)

KERNEL_DIR ?= /run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07
R15P0 := $(KERNEL_DIR)/drivers/gpu/arm/t7xx/r15p0

KBASE_DIR := ../kbase
INCLUDES := -Iinclude -I$(KBASE_DIR)

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
          -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS := -lpthread

LIB_SRCS := src/kbase_winsys.c $(KBASE_DIR)/kbase.c
LIB_OBJS := $(LIB_SRCS:%.c=build/%.o)
LIB := build/libkbase-winsys.a

SELFTEST := build/kbw-selftest
AARCH64_LIB := build-aarch64/libkbase-winsys.a
AARCH64_SELFTEST := build-aarch64/kbw-selftest

.PHONY: all lib aarch64 check check-abi clean

all: lib $(SELFTEST)

lib: $(LIB)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(SELFTEST): tests/kbw-selftest.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

aarch64: $(AARCH64_SELFTEST)

$(AARCH64_LIB): $(LIB_SRCS)
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) -c src/kbase_winsys.c -o build-aarch64/kbase_winsys.o
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) -c $(KBASE_DIR)/kbase.c -o build-aarch64/kbase.o
	$(AR) rcs $@ build-aarch64/kbase_winsys.o build-aarch64/kbase.o

$(AARCH64_SELFTEST): tests/kbw-selftest.c $(AARCH64_LIB)
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) $< $(AARCH64_LIB) -static $(LDLIBS) -o $@
	@command -v aarch64-linux-gnu-strip >/dev/null 2>&1 && \
		aarch64-linux-gnu-strip $@ || true

check: $(SELFTEST)
	@$(SELFTEST) --print-abi
	@$(SELFTEST); rc=$$?; \
	 if [ $$rc -eq 2 ]; then echo "no device: ABI checks only (expected on this host)"; exit 0; fi; \
	 exit $$rc

# The GPU-props layout in src/kbase_winsys.c is asserted at compile time; this
# regenerates the numbers from the kernel's own headers so drift is visible.
check-abi:
	@test -f "$(R15P0)/mali_base_kernel.h" || { echo "kernel headers not found: $(R15P0)" >&2; exit 1; }
	@mkdir -p build
	$(CC) -std=c11 -Wall -I"$(R15P0)" tools/kbw-props-layout.c -o build/kbw-props-layout
	@build/kbw-props-layout > build/props-layout.txt
	@echo "--- measured from $(R15P0) ---"
	@grep -E 'size struct mali_base_gpu_props|offset .*raw_props\.(mmu_features|gpu_id)|GPU_MAX_JOB_SLOTS' build/props-layout.txt
	@a=$$(grep 'size struct mali_base_gpu_props' build/props-layout.txt | awk '{print $$NF}'); \
	 if [ "$$a" != "528" ]; then echo "MISMATCH: mali_base_gpu_props is $$a, winsys expects 528" >&2; exit 1; fi
	@echo "props layout matches winsys static asserts"

clean:
	rm -rf build build-aarch64
