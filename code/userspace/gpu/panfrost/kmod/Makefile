# Out-of-tree build of the M0 bring-up module against the Nura 3.10 tree.
#
#   make KDIR=/path/to/prepared/3.10/build \
#        CROSS_COMPILE=/path/to/aarch64-none-linux-gnu-
#
# KDIR must point at the *output* directory of a tree that has had
#   make ARCH=arm64 O=$KDIR <defconfig> prepare
# run on it (3.10 has no modules_prepare target).  See design.md, M0.
KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
CROSS_COMPILE ?=

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
