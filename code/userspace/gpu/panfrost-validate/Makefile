CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
CROSS ?= aarch64-linux-gnu-gcc

all: validate

validate: validate.c
	$(CC) $(CFLAGS) -o $@ $<

aarch64: validate.aarch64

validate.aarch64: validate.c
	$(CROSS) $(CFLAGS) -static -o $@ $<

check: validate
	./validate

check-aarch64: validate.aarch64
	qemu-aarch64 ./validate.aarch64

clean:
	rm -f validate validate.aarch64

.PHONY: all aarch64 check check-aarch64 clean
