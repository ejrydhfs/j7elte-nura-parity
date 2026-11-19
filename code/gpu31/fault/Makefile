# fault slice: host fault-injection matrix (no kernel tree, no device)
#
#   make                 build + run the 12-case matrix (native)
#   make check           same as make (CI entry)
#   make check-aarch64   cross-build + run under qemu-aarch64
#
# C89-clean to match the jobs slice (-std=gnu89) since fault_matrix.c
# includes the pure-C job-manager header.
CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89 -O2 -g

AARCH64_CC ?= aarch64-linux-gnu-gcc
QEMU ?= qemu-aarch64
QEMU_SYSROOT ?= /usr/aarch64-linux-gnu

JOBS_DIR := ../jobs

INCLUDES := -I. -I$(JOBS_DIR)

MATRIX := build/fault_matrix
AARCH64_MATRIX := build-aarch64/fault_matrix

.PHONY: all check check-aarch64 clean

all: check

check: $(MATRIX)
	./$(MATRIX)

$(MATRIX): fault_matrix.c pf7580_mock_gpu.c pf7580_mock_gpu.h pf7580_fault.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) fault_matrix.c pf7580_mock_gpu.c -o $@

check-aarch64: $(AARCH64_MATRIX)
	$(QEMU) -L $(QEMU_SYSROOT) $(AARCH64_MATRIX)

$(AARCH64_MATRIX): fault_matrix.c pf7580_mock_gpu.c pf7580_mock_gpu.h pf7580_fault.h
	@mkdir -p $(dir $@)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) -static fault_matrix.c pf7580_mock_gpu.c -o $@

clean:
	rm -rf build build-aarch64
