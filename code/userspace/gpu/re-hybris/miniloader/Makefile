CC = clang
CFLAGS = --target=armv7a-linux-gnueabi -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 \
         -O2 -fno-builtin -fno-stack-protector -fno-pic -ffreestanding -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -nostdlib -static -fuse-ld=lld -Wl,-e,_start -Wl,-Ttext=0x10000 -Wl,-z,noexecstack

all: miniloader

miniloader: start.S miniloader.c common.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S miniloader.c

clean:
	rm -f miniloader
.PHONY: all clean
