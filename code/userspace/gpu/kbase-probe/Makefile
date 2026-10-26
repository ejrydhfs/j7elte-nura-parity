# Cross-build of the kbase kernel-interface probe (aarch64, no libc).
#
#   make                 # uses aarch64-linux-gnu-gcc from PATH
#   make CROSS_COMPILE=/path/to/aarch64-none-linux-gnu-
#   make CROSS_COMPILE=... clean all
#
# The result is freestanding: no dynamic loader, no libc — it runs on the
# musl-based postmarketOS userland as-is.

CROSS_COMPILE ?= aarch64-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror
CFLAGS  += -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-asynchronous-unwind-tables -Wl,--build-id=none

all: kbase-probe

kbase-probe: kbase-probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f kbase-probe

.PHONY: all clean
