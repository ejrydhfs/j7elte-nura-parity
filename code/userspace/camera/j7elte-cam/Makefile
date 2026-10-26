# j7elte-cam - FIMC-IS still capture client for the Samsung Galaxy J7.
#
# The program is plain C11 plus Linux UAPI headers (videodev2.h, media.h);
# it has no library dependencies beyond libc, so the same Makefile builds it
# with the postmarketOS aarch64 musl toolchain ($CC in abuild) and with any
# cross compiler for testing.

CC	?= cc
CFLAGS	?= -O2 -g
CPPFLAGS ?=
LDFLAGS	?=
LDLIBS	?=

# _GNU_SOURCE is set in the source; gnu11 keeps the UAPI headers happy.
WARN	:= -std=gnu11 -Wall -Wextra -Wshadow -Wformat=2 -Wpointer-arith \
	   -Wstrict-prototypes -Wmissing-prototypes -Wvla

PREFIX	?= /usr
DESTDIR	?=

all: j7elte-cam

j7elte-cam: j7elte-cam.c j7elte-cam-ioctl.h j7elte-cam-meta.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -o $@ j7elte-cam.c \
		$(LDFLAGS) $(LDLIBS)

install: j7elte-cam
	install -Dm755 j7elte-cam $(DESTDIR)$(PREFIX)/bin/j7elte-cam

clean:
	rm -f j7elte-cam

.PHONY: all install clean
