# fsonly - solid-colour fragment-path probe (device-native build).
#
# The cross sysroot has no EGL/GLES2 headers; build on the phone, as egltest
# and the polylist hook are built:
#
#   gcc -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L \
#       -o fsonly fsonly.c -lEGL -lGLESv2
#
# Run through code/gpu31/fsonly/run.sh (device) so the diagnostic Mesa
# environment and the polycensus LD_PRELOAD knobs are set consistently.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
LDLIBS = -lEGL -lGLESv2

all: fsonly

fsonly: fsonly.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f fsonly

.PHONY: all clean
