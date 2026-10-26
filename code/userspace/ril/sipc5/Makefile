# SPDX-License-Identifier: MIT
#
# Build the SIPC5 framing/transport library and its selftest probe.
#   make            -> sipc5-probe (host)
#   make CC=...     -> cross build (e.g. aarch64-none-linux-gnu-gcc)
#   make check      -> run the selftest

CC	?= cc
CFLAGS	?= -O2 -g
CFLAGS	+= -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror
LDFLAGS	?=

all: sipc5-probe

sipc5.o: sipc5.c sipc5.h
	$(CC) $(CFLAGS) -c -o $@ sipc5.c

sipc5-probe: sipc5-probe.c sipc5.o sipc5.h
	$(CC) $(CFLAGS) -o $@ sipc5-probe.c sipc5.o $(LDFLAGS)

check: sipc5-probe
	./sipc5-probe selftest

clean:
	rm -f sipc5.o sipc5-probe

.PHONY: all check clean
