# gem slice: panfrost GEM for the T720 port (host-side only; never builds the tree)
# - host test: pure-C IOVA/size validation (gem_validate.c)
# - kernel checks: -fsyntax-only AND real -c object compiles of all three
#   .c against the real 3.10.108 headers with the 0001-0003 backport
#   headers staged in a private include dir (read-only tree, never edited)

CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89

K ?= /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07

# aarch64 cross for the -c object check (same family as the M0 kmod check)
CROSS ?= aarch64-linux-gnu-gcc

# power-slice headers (struct pf7580_power) live beside the repo
POWER ?= ../power

all: check

check: gem_validate
	./gem_validate

gem_validate: gem_validate.c panfrost7580_gem.h
	$(CC) $(CFLAGS) -o $@ gem_validate.c

# Kernel syntax/object check. Needs the 0001-0003 backport headers staged:
#   STAGE=/tmp/gemstage make stage-shims kcheck kobj
# (staging copies the new files out of the series patches into a private
# dir; the shared tree is never touched).
STAGE ?= /tmp/gemstage
KINC = -nostdinc -std=gnu89 -isystem $(shell $(CROSS) -print-file-name=include) \
	-I$(K)/arch/arm64/include -I$(K)/arch/arm64/include/generated \
	-I$(K)/include -I$(K)/include/uapi -I$(K)/arch/arm64/include/uapi \
	-I$(STAGE)/include -I$(STAGE)/drivers/gpu/drm \
	-D__KERNEL__ -include $(K)/include/linux/kconfig.h

stage-shims:
	mkdir -p $(STAGE)/include/linux $(STAGE)/drivers/gpu/drm
	TMPD=$$(mktemp -d) && \
	for p in ../series/0001-dma-fence-minimal-subset.patch \
		 ../series/0002-dma-resv-shim.patch \
		 ../series/0003-drm_syncobj-subset.patch; do \
		git -C "$$TMPD" apply --unsafe-paths \
			--directory="$(STAGE)" "$$PWD/$$p" || exit 1; \
	done; rm -rf "$$TMPD"
	@echo "staged 0001-0003 into $(STAGE)"

kcheck:
	for f in panfrost7580_gem.c panfrost7580_ioctl.c panfrost7580_devfreq.c; do \
		$(CROSS) -fsyntax-only -Wall -Werror -Wno-unused-but-set-variable \
			$(KINC) -DKBUILD_MODNAME='"panfrost7580_gem"' \
			-I. -I$(POWER) $$f || exit 1; \
	done
	@echo "kcheck: 3 files PASS -fsyntax-only (zero warnings)"

kobj:
	mkdir -p /tmp/gemobj
	for f in panfrost7580_gem panfrost7580_ioctl panfrost7580_devfreq; do \
		$(CROSS) -c -Wall -Werror -Wno-unused-but-set-variable \
			$(KINC) -DKBUILD_MODNAME='"panfrost7580_gem"' \
			-I. -I$(POWER) $$f.c -o /tmp/gemobj/$$f.o || exit 1; \
	done
	ls -la /tmp/gemobj/
	@echo "kobj: 3 objects built"

clean:
	rm -f gem_validate
	rm -rf /tmp/gemobj

.PHONY: all check stage-shims kcheck kobj clean
