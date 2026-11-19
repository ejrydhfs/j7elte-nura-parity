# bcmfm: BCM43438 FM radio over raw HCI (host + cross friendly).
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -D_DEFAULT_SOURCE
LDFLAGS ?=
# Static-ish: prefer static libgcc where supported; plain libc otherwise.
STATIC_LDFLAGS := -static-libgcc

SRCS := bcmfm.c bcmfm-cli.c hci_raw.c
OBJS := $(SRCS:.c=.o)
HDRS := bcmfm.h hci_raw.h

all: bcmfm

bcmfm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(STATIC_LDFLAGS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f bcmfm $(OBJS)

.PHONY: all clean
