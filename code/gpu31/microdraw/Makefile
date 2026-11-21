# microdraw client (attribute-less) + device run helper
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CFLAGS += -D_POSIX_C_SOURCE=200809L
LDLIBS = -lEGL -lGLESv2 -lm

all: microdraw

microdraw: microdraw.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f microdraw

.PHONY: all clean
