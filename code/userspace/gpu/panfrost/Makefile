# Host (and aarch64/qemu) tests for the Midgard LPAE page-table builder.
#
# These run on the build host, not on the phone: they prove the descriptor
# encoding and register map that the 3.10 kernel milestone driver reuses.
#
#   make          - build native and aarch64 test binaries
#   make check    - run both (aarch64 under qemu-aarch64)
#   make clean

CC       ?= gcc
ARCH_CC  ?= aarch64-linux-gnu-gcc
QEMU     ?= qemu-aarch64

CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra -Werror
CPPFLAGS += -I.

NATIVE  := mali_lpae_test
AARCH64 := mali_lpae_test.aarch64

all: $(NATIVE) $(AARCH64)

$(NATIVE): mali_lpae_test.c mali_lpae.h mali_hw.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ mali_lpae_test.c

$(AARCH64): mali_lpae_test.c mali_lpae.h mali_hw.h
	$(ARCH_CC) $(CPPFLAGS) $(CFLAGS) -static -o $@ mali_lpae_test.c

check: all
	@echo "== native =="
	./$(NATIVE)
	@echo "== aarch64 (qemu) =="
	$(QEMU) ./$(AARCH64)

clean:
	rm -f $(NATIVE) $(AARCH64)

.PHONY: all check clean
