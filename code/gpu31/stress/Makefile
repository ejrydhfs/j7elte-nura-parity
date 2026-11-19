# kbase-stress: kbase winsys stress + watch tests (UK 10.6 / Mali-T720).
#
#   make            host build (ABI check; ./kbase-stress exits 2, no device)
#   make cross      aarch64 static build for the phone (builds in /data)
#   make run DEV=... on-device run is: copy build dir to /data + ssh run
#
# Only soft-job atoms are submitted (no Midgard chains): safe while X/fbdev
# and other workstreams share the phone. No reboot, no kernel build.

CC ?= gcc
AARCH64_CC ?= aarch64-linux-gnu-gcc

KBASE_DIR = ../../userspace/gpu/kbase
INCLUDES = -I. -I$(KBASE_DIR)

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
          -D_POSIX_C_SOURCE=200809L
LDLIBS = -lrt

SRC = kbase-stress.c $(KBASE_DIR)/kbase.c

all: kbase-stress

kbase-stress: $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRC) $(LDLIBS)

cross: kbase-stress.aarch64

kbase-stress.aarch64: $(SRC)
	$(AARCH64_CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRC) -static $(LDLIBS)
	@command -v aarch64-linux-gnu-strip >/dev/null 2>&1 && \
		aarch64-linux-gnu-strip $@ || true

check: kbase-stress
	./kbase-stress --print-abi

clean:
	rm -f kbase-stress kbase-stress.aarch64

.PHONY: all cross check clean
