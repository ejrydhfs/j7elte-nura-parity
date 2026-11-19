CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
TARGET   = esw_dump

all: $(TARGET)

$(TARGET): esw_dump.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
