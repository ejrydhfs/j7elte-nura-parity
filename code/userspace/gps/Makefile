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

# Raw UART experiment driver (no NMEA parsing, no PTY): used to capture the
# BCM4752 BRI prologue byte by byte on the device.
gpsprobe: src/gpsprobe.c
	$(CC) $(CFLAGS) -o $@ $< 

# Standalone ESW/RAM-patch upload driver: power cycle, prologue, patch upload
# with selectable frame encodings (-V), bank switches, CRC requests.  The
# encoder variant that the hardware accepts is the one the daemon uses.
gpsesw: src/gpsesw.c
	$(CC) $(CFLAGS) -o $@ $<

check: $(BIN)
	python3 tests/smoke_test.py
	python3 tests/init_test.py

clean:
	rm -f $(BIN) gpsprobe gpsesw

.PHONY: all check clean
