# Host-side unit test for the DECON KMS framebuffer validation
# (KmsDmabuf slice, code/gpu31/kms/exynos_drm7580_dmabuf.c).
#
# The kernel file is not rebuilt or re-implemented here: the block between the
# "BEGIN exynos7580_fb_check" / "END exynos7580_fb_check" markers is extracted
# verbatim and compiled into the test, so the test cannot drift from the code
# it exercises.  The marker count is asserted as well.
#
#   make        extract, build, run
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -Werror

SRC     := ../../exynos_drm7580_dmabuf.c
INC     := exynos7580_fb_check.inc
TEST    := check

all: run

$(INC): $(SRC)
	@n=$$(grep -c 'BEGIN exynos7580_fb_check' $(SRC)); \
	 [ "$$n" = 1 ] || { echo "$(SRC): expected 1 BEGIN marker, found $$n" >&2; exit 1; }
	@n=$$(grep -c 'END exynos7580_fb_check' $(SRC)); \
	 [ "$$n" = 1 ] || { echo "$(SRC): expected 1 END marker, found $$n" >&2; exit 1; }
	awk '/BEGIN exynos7580_fb_check/{f=1} f{print} /END exynos7580_fb_check/{f=0}' \
		$(SRC) > $@.tmp && mv $@.tmp $@

$(TEST): check.c $(INC)
	$(CC) $(CFLAGS) -o $@ check.c

run: $(TEST)
	./$(TEST)

clean:
	rm -f $(TEST) $(INC) $(INC).tmp

.PHONY: all run clean
