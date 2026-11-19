# wsi-hw: hardware WSI path probe for the render-only node. Built on device.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c99
CFLAGS += -D_POSIX_C_SOURCE=200809L
LDLIBS = -lgbm -lX11 -lXext

all: tri-x11 wsi-hw

tri-x11: tri-x11.c
	$(CC) $(CFLAGS) -o $@ $< -lEGL -lGLESv2 -lX11

wsi-hw: wsi-hw.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

check: tri-x11 wsi-hw
	./tri-x11 --frames 5 --hold 1
	./wsi-hw --selftest

clean:
	rm -f tri-x11 wsi-hw

.PHONY: all check clean
