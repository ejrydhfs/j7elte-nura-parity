# devfreq slice: DVFS for panfrost T720 (host-side only; never builds the kernel tree)
# - host test: pure-C OPP ladder + ceil + voltage-order helpers (pf7580_devfreq.h)
# - kernel syntax check: -fsyntax-only against the real 3.10.108 headers
#   (read-only tree, never edited)

CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89

K ?= /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07

CROSS ?= aarch64-linux-gnu-gcc

all: check

check: devfreq_test
	./devfreq_test

devfreq_test: devfreq_test.c pf7580_devfreq.h
	$(CC) $(CFLAGS) -o $@ devfreq_test.c

# Read-only syntax check against the real tree headers (NOT a build).
# Mirrors the power slice KINC recipe: nostdinc + arch/include + kconfig.
KINC = -nostdinc -std=gnu89 -isystem $(shell $(CROSS) -print-file-name=include) \
	-I$(K)/arch/arm64/include -I$(K)/arch/arm64/include/generated \
	-I$(K)/include -I$(K)/include/uapi -I$(K)/arch/arm64/include/uapi \
	-D__KERNEL__ -include $(K)/include/linux/kconfig.h

kcheck:
	$(CROSS) -fsyntax-only -Wall $(KINC) \
		-DKBUILD_MODNAME='"panfrost7580_devfreq"' pf7580_devfreq.c

clean:
	rm -f devfreq_test

.PHONY: all check kcheck clean
