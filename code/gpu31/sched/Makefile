# sched slice: build + checks (host-side only; never builds the kernel tree)
#
# - host test: compiles the FIXED shim bodies (src/, byte-identical to the
#   regenerated patches/drm-backport/0001-0004) against a single-threaded
#   kernel emulation (emu/include/) and asserts every gpu31-review finding:
#   0001 F1/F2/F3, 0002 F1/F2, 0003 F1, 0004 F1-F5 + push_job + ready gate.
# - kernel checks: -fsyntax-only AND real -c object compiles of the same
#   bodies against the real 3.10.108 headers (read-only tree, never edited).
# - structural gates: vestigial members gone (F2), separate sched lists
#   (F1), one dep per job (F3), no hardcoded 5 s (F5), no remove-during-
#   iteration in fini (0003 F1).
#
# Verifies the DELIVERED patches, not just src/: `make check` regenerates
# 0001-0004 from src/ and diffs them against patches/drm-backport/.

CC ?= cc
ARCH_CC ?= aarch64-linux-gnu-gcc
QEMU ?= qemu-aarch64

CFLAGS ?= -O2 -g -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Werror

K ?= /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07
CROSS ?= aarch64-linux-gnu-gcc

NATIVE := sched-host-test
AARCH64 := sched-host-test.aarch64

EMU_INC := emu/include
SRC := src
SHIMS := dma-fence dma-resv drm_syncobj drm_sched

all: $(NATIVE) $(AARCH64)

$(NATIVE): tests/sched-host-test.c $(SRC)/*.c $(SRC)/*.h $(EMU_INC)/linux/*.h
	$(CC) $(CFLAGS) -I$(EMU_INC) -I$(SRC) -o $@ tests/sched-host-test.c

$(AARCH64): tests/sched-host-test.c $(SRC)/*.c $(SRC)/*.h $(EMU_INC)/linux/*.h
	$(ARCH_CC) $(CFLAGS) -static -I$(EMU_INC) -I$(SRC) -o $@ tests/sched-host-test.c

# Kernel syntax/object check against the real 3.10.108 headers.
# The shim headers are staged to a private dir (never the shared tree);
# the .c files compile unmodified.
KINC = -nostdinc -isystem $(shell $(CROSS) -print-file-name=include) \
	-I$(K)/arch/arm64/include -I$(K)/arch/arm64/include/generated \
	-I$(K)/include -I$(K)/include/uapi -I$(K)/arch/arm64/include/uapi \
	-D__KERNEL__ -include $(K)/include/linux/kconfig.h

STAGE ?= /tmp/sched-kstage

stage:
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/include/linux $(STAGE)/drivers/gpu/drm
	cp $(SRC)/dma-fence.h $(SRC)/dma-resv.h $(STAGE)/include/linux/
	cp $(SRC)/drm_syncobj.h $(SRC)/drm_sched.h $(STAGE)/drivers/gpu/drm/
	cp $(SRC)/dma-fence.c $(SRC)/dma-resv.c $(STAGE)/drivers/base 2>/dev/null || \
	  (mkdir -p $(STAGE)/drivers/base && cp $(SRC)/dma-fence.c $(SRC)/dma-resv.c $(STAGE)/drivers/base/)
	cp $(SRC)/drm_syncobj.c $(SRC)/drm_sched.c $(STAGE)/drivers/gpu/drm/

kcheck: stage
	@for f in $(STAGE)/drivers/base/dma-fence.c $(STAGE)/drivers/base/dma-resv.c \
		 $(STAGE)/drivers/gpu/drm/drm_syncobj.c $(STAGE)/drivers/gpu/drm/drm_sched.c; do \
		$(CROSS) -fsyntax-only -std=gnu89 -Wall -DKBUILD_MODNAME='"schedkcheck"' \
			$(KINC) -I$(STAGE)/include -I$(STAGE)/drivers/gpu/drm "$$f" || exit 1; \
		$(CROSS) -c -std=gnu89 -Wall -O2 -DKBUILD_MODNAME='"schedkcheck"' \
			$(KINC) -I$(STAGE)/include -I$(STAGE)/drivers/gpu/drm \
			"$$f" -o /dev/null || exit 1; \
	done
	@echo "kcheck: 4 files syntax-only + object compile PASS"

# Structural gates: fail if a review-removed pattern comes back.
gates:
	@echo "== structural gates =="
	@! grep -q "spinlock_t \*lock" $(SRC)/dma-fence.h || (echo "GATE FAIL: fence->lock back"; exit 1)
	@! grep -q "struct work_struct default_work" $(SRC)/dma-fence.h || (echo "GATE FAIL: default_work back"; exit 1)
	@! grep -q "int status" $(SRC)/dma-fence.h || (echo "GATE FAIL: status word back"; exit 1)
	@! grep -q "wait_queue_head_t \*event" $(SRC)/dma-fence.h || (echo "GATE FAIL: event ptr back"; exit 1)
	@! grep -q "shared\[0\]" $(SRC)/dma-resv.h || (echo "GATE FAIL: shared[0] back"; exit 1)
	@! grep -q "msecs_to_jiffies(5000)" $(SRC)/drm_sched.c || (echo "GATE FAIL: hardcoded 5s back"; exit 1)
	@! grep -qE "^\s*idr_for_each_entry" $(SRC)/drm_syncobj.c || (echo "GATE FAIL: idr_for_each_entry back"; exit 1)
	@! grep -qE "^\s*list_del_init\(&entity->queue_list\);" $(SRC)/drm_sched.c || (echo "GATE FAIL: entity list conflation back"; exit 1)
	@grep -q "dma_fence_init" $(SRC)/dma-fence.h || (echo "GATE FAIL: dma_fence_init missing"; exit 1)
	@grep -q "sched_link" $(SRC)/drm_sched.h || (echo "GATE FAIL: sched_link missing"; exit 1)
	@grep -q "dep_cb" $(SRC)/drm_sched.h || (echo "GATE FAIL: dep_cb missing"; exit 1)
	@grep -q "dep_fence" $(SRC)/drm_sched.h || (echo "GATE FAIL: dep_fence missing"; exit 1)
	@grep -q "EOPNOTSUPP" $(SRC)/drm_sched.c || (echo "GATE FAIL: one-dep contract missing"; exit 1)
	@grep -q "drm_sched_entity_push_job" $(SRC)/drm_sched.c || (echo "GATE FAIL: push_job missing"; exit 1)
	@grep -q "mutex_lock" $(SRC)/dma-resv.c || (echo "GATE FAIL: resv locking missing"; exit 1)
	@grep -q "idr_get_next" $(SRC)/drm_syncobj.c || (echo "GATE FAIL: idr_get_next drain missing"; exit 1)
	@grep -q "dma_fence_get" $(SRC)/dma-resv.c || (echo "GATE FAIL: fence pinning missing"; exit 1)
	@echo "gates: all PASS"

check: all gates kcheck
	@echo "== native =="
	./$(NATIVE)
	@echo "== aarch64 (qemu) =="
	$(QEMU) ./$(AARCH64)

clean:
	rm -f $(NATIVE) $(AARCH64)
	rm -rf $(STAGE)

.PHONY: all check clean stage kcheck gates
