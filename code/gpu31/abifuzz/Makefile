# abifuzz: host fuzzer/validator for the six v5.2 panfrost ioctl encodings
# Host-side only; never part of any kernel build.
CC ?= cc
CFLAGS ?= -Wall -Wextra -std=gnu89

all: check

check: abifuzz compat_smoke
	./abifuzz; status=$$?; \
	if [ $$status -eq 0 ]; then echo "abifuzz: clean (no findings)"; \
	else echo "abifuzz: findings present (exit $$status, expected until F1-F5 fixed)"; fi; \
	./compat_smoke
	@echo "NOTE: exit 1 from ./abifuzz means findings present, not a build failure."

abifuzz: abifuzz.c ../gem/panfrost_drm.h ../drmcore/panfrost_ioctls_310.h \
		stub/drm.h stub/drm/drm.h stub/drm/drmP.h \
		stub/drm/drm_gem_cma_helper.h \
		stub/linux/types.h stub/linux/io.h stub/linux/delay.h \
		stub/linux/jiffies.h stub/linux/fs.h
	$(CC) $(CFLAGS) -Istub -I../gem -I../drmcore -o $@ abifuzz.c

compat_smoke: compat_smoke.c stub/drm_stub.c \
		stub/drm/drmP.h stub/drm/drm_gem_cma_helper.h \
		../drmcore/drm_compat_310.h ../drmcore/panfrost_ioctls_310.h
	$(CC) $(CFLAGS) -Istub -I../gem -I../drmcore -o $@ compat_smoke.c stub/drm_stub.c

clean:
	rm -f abifuzz compat_smoke

.PHONY: all check clean
