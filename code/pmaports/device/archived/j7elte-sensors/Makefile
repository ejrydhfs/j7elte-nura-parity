# SPDX-License-Identifier: GPL-3.0-or-later
PREFIX ?= /usr
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11
LDFLAGS ?=
LDLIBS += -lm

SRCS = j7elte-sensors.c dbus-wire.c
OBJS = $(SRCS:.c=.o)
BIN = j7elte-sensors

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c dbus-wire.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 j7elte-sensors.service \
		$(DESTDIR)$(PREFIX)/lib/systemd/system/j7elte-sensors.service
	install -Dm755 j7elte-sensors.initd \
		$(DESTDIR)/etc/init.d/j7elte-sensors
	install -Dm644 j7elte-sensors.conf \
		$(DESTDIR)$(PREFIX)/share/dbus-1/system.d/j7elte-sensors.conf

clean:
	rm -f $(BIN) $(OBJS)

.PHONY: all install clean
