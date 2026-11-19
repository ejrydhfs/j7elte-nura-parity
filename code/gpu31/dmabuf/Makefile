# Host (and aarch64/qemu) contract tests for the DMA-BUF + CMA + attrs
# prerequisites panfrost GEM import/export needs on Nura 3.10.108.
#
# These run on the build host, not on the phone: they assert the tree
# facts (file:line cited in dmabuf-check.c) that the kernel patches in
# this slice rely on, so a divergence fails here first.
#
#   make          - build native and aarch64 binaries
#   make check    - run both (aarch64 under qemu-aarch64)
#   make clean

CC       ?= gcc
ARCH_CC  ?= aarch64-linux-gnu-gcc
QEMU     ?= qemu-aarch64

CFLAGS   ?= -O2 -g -std=c11 -Wall -Wextra -Werror

NATIVE  := dmabuf-check
AARCH64 := dmabuf-check.aarch64

all: $(NATIVE) $(AARCH64)

$(NATIVE): dmabuf-check.c
	$(CC) $(CFLAGS) -o $@ dmabuf-check.c

$(AARCH64): dmabuf-check.c
	$(ARCH_CC) $(CFLAGS) -static -o $@ dmabuf-check.c

check: all
	@echo "== native =="
	./$(NATIVE)
	@echo "== aarch64 (qemu) =="
	$(QEMU) ./$(AARCH64)

clean:
	rm -f $(NATIVE) $(AARCH64)

.PHONY: all check clean
