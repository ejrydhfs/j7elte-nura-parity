#
# j7elte-gpsd - build the bridge daemon
#
# CC defaults to the Alpine musl cross compiler; override for a quick
# off-target syntax/type check, e.g.
#   make CC=aarch64-none-linux-gnu-gcc
#
CC      ?= aarch64-linux-musl-gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wshadow -Wformat=2 -std=c11
LDFLAGS ?=
SRC     ?= src/j7elte-gpsd.c
BIN     := j7elte-gpsd

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

check: $(BIN)
	python3 tests/smoke_test.py
	python3 tests/init_test.py

clean:
	rm -f $(BIN)

.PHONY: all check clean
