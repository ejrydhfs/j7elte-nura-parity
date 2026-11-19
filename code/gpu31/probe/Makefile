# render-node-probe v2: classify a DRM render node (H1 panfrost UAPI vs H2 kbase shim).
#
# Extended from code/userspace/gpu/mesa-render/render-node-probe.c (v1):
# deterministic exit codes for the harness plus machine-readable JSON.
#
#   make                                  # native build
#   make CROSS_COMPILE=aarch64-linux-gnu- # device binary (static for the musl/ARM userland)
#   make CROSS_COMPILE=... clean all      # cross clean+rebuild
#   make check                            # native self-tests (host model, no hardware)
#   make check-aarch64                    # cross binary under qemu-aarch64 (needs sysroot)
#
# Self-contained C99: only <drm/drm.h> + <drm/panfrost_drm.h> + libc, no libdrm.
# Exits: 0=H1 (incl. H1+H2), 3=H2, 4=NEITHER, 2=NO_DEVICE, 1=ERROR.

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c99
QEMU    ?= qemu-aarch64
# qemu user-mode needs the cross sysroot for the dynamic loader.
QEMU_SYSROOT ?= /usr/aarch64-linux-gnu

STATIC ?= 1
ifeq ($(STATIC),1)
LDFLAGS_aarch64 := -static
endif

all: render-node-probe

render-node-probe: render-node-probe.c
	$(CC) $(CFLAGS) $(LDFLAGS_$(findstring aarch64,$(CC))) -o $@ $<

check: render-node-probe
	./run-probe-tests.sh ./render-node-probe

check-aarch64: render-node-probe
	PROBE_RUNNER="$(QEMU) -L $(QEMU_SYSROOT)" ./run-probe-tests.sh ./render-node-probe

clean:
	rm -f render-node-probe selftest-*.json selftest-regular.tmp

.PHONY: all check check-aarch64 clean
