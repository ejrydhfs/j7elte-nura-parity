# render-node-probe: classify a DRM render node (H1 panfrost UAPI vs H2 kbase shim).
#
#   make                                  # native build
#   make CROSS_COMPILE=aarch64-linux-gnu- # device binary
#   make CROSS_COMPILE=... clean all
#   make check                            # native binary smoke (no hardware)
#   make check-qemu                       # cross binary under qemu-aarch64 (needs sysroot)
#
# Self-contained C99: only <drm/drm.h> + <drm/panfrost_drm.h> + libc, no libdrm.

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c99
QEMU    ?= qemu-aarch64
# qemu user-mode needs the cross sysroot for the dynamic loader.
QEMU_SYSROOT ?= /usr/aarch64-linux-gnu

all: render-node-probe

render-node-probe: render-node-probe.c
	$(CC) $(CFLAGS) -o $@ $<

check: render-node-probe
	./render-node-probe /nonexistent-node-xyz; test $$? -eq 2
	@echo "check OK: missing-node exit-2"

check-qemu: render-node-probe
	$(QEMU) -L $(QEMU_SYSROOT) ./render-node-probe /nonexistent-node-xyz; test $$? -eq 2
	@echo "check-qemu OK: missing-node exit-2"

clean:
	rm -f render-node-probe

.PHONY: all check check-qemu clean
