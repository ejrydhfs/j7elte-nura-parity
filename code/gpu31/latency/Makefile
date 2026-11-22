# iotrace.so - LD_PRELOAD ioctl timeline for the gpu31 latency work.
# Build ON the target (musl) or with a matching aarch64 toolchain; the
# resulting .so must match the app's libc.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall

all: iotrace.so

iotrace.so: iotrace.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -ldl

clean:
	rm -f iotrace.so

.PHONY: all clean
