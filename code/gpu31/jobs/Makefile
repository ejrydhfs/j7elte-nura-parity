# jobs slice: build + checks (host-side only; never builds the kernel tree)
# - host test: pure-C header surface (slot map, JS_CONFIG, fault decode)
# - kernel compile check: -fsyntax-only AND real -c object compiles of the
#   .c against the real 3.10.108 headers with the 0001-0004 backport
#   headers staged in a private include dir (read-only tree, never edited)

CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89

K ?= /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07

# aarch64 cross for the -c object check (same family as the M0 kmod check)
CROSS ?= aarch64-linux-gnu-gcc

all: check

check: job_test
	./job_test

job_test: job_test.c panfrost7580_jm_hw.h panfrost7580_job.h
	$(CC) $(CFLAGS) -o $@ job_test.c

# Kernel syntax/object check. Needs the 0001-0004 backport headers staged:
#   STAGE=/tmp/jobstage make stage-shims kcheck
# (staging copies the 8 new files out of patches/drm-backport into a
# private dir; the shared tree is never touched).
STAGE ?= /tmp/jobstage
KINC = -nostdinc -isystem $(shell $(CROSS) -print-file-name=include) \
	-I$(K)/arch/arm64/include -I$(K)/arch/arm64/include/generated \
	-I$(K)/include -I$(K)/include/uapi -I$(K)/arch/arm64/include/uapi \
	-I$(STAGE)/include -I$(STAGE)/drivers/gpu/drm \
	-D__KERNEL__ -include $(K)/include/linux/kconfig.h

stage-shims:
	mkdir -p $(STAGE)/include/linux $(STAGE)/drivers/gpu/drm
	TMPD=$$(mktemp -d) && \
	for p in ../series/0001-dma-fence-minimal-subset.patch \
		 ../series/0002-dma-resv-shim.patch \
		 ../series/0003-drm_syncobj-subset.patch \
		 ../series/0004-drm_sched-panfrost-job-only-shim.patch; do \
		git -C "$$TMPD" apply --unsafe-paths \
			--directory="$(STAGE)" "$$PWD/$$p" || exit 1; \
	done; rm -rf "$$TMPD"
	@echo "staged 0001-0004 into $(STAGE)"
clean:
	rm -f job_test

.PHONY: all check stage-shims kcheck clean
