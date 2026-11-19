# egltest - EGL/GLES2 textured-triangle smoke test (offscreen + onscreen).
#
#   make                 build egltest (host or device)
#   make check           run offscreen suite (no X needed) + onscreen if DISPLAY set
#
# Built natively on device like code/gpu31/wsi/tri-x11 (the cross sysroot
# has no X11/EGL headers, and the device links its own Mesa/Alpine libs):
#   gcc -O2 -Wall -Wextra -std=c99 -o ~/egltest ~/egltest.c -lEGL -lGLESv2 -lX11
#
# Device run (fbdev X already running; hold the device lock for onscreen):
#   ./egltest                        # both paths, window held 1 s
#   ./egltest --offscreen-only        # headless; works over plain SSH
#   flock /tmp/gpsdev.lock ./egltest  # onscreen under the repo device lock
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CFLAGS += -D_POSIX_C_SOURCE=200809L
LDLIBS = -lEGL -lGLESv2 -lX11

all: egltest

egltest: egltest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

check: egltest
	./egltest --offscreen-only
	@if [ -n "$$DISPLAY" ]; then ./egltest --frames 5 --hold 0; else echo "egltest: no DISPLAY, onscreen skipped"; fi

clean:
	rm -f egltest

.PHONY: all check clean
