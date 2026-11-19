# tri-x11: WSI triangle demo (EGL/GLES2 on X11). Built natively on device.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99
LDLIBS = -lEGL -lGLESv2 -lX11

all: tri-x11

tri-x11: tri-x11.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

check: tri-x11
	./tri-x11 --frames 5 --hold 1

clean:
	rm -f tri-x11

.PHONY: all check clean
