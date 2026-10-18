# SPDX-License-Identifier: GPL-2.0
#
# Build the evergo (MT6833) bring-up initramfs.
#
#   make          compile init.c into ./init
#   make clean    remove the generated binary
#
# The tree itself (root/lib/firmware, root/dev, root/proc, root/sys) is
# version controlled and the binary is generated, so root/init is listed in
# .gitignore.  Packing the tree into a boot image is deliberately NOT done
# here: build_boot.py in the mt6833-port tree walks root/ and writes the cpio
# itself, which keeps exactly one place that decides the ramdisk contents.

CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
STRIP ?= $(CROSS)strip

ROOT := root
INIT := $(ROOT)/init

# init.c is a normal static glibc program (stdio, mount(2) wrappers, the
# watchdog ioctls, mmap), so the -nostdlib / -ffreestanding flags used by the
# xaga reference initramfs do not apply here.
CFLAGS := -static -Os -fno-stack-protector -Wall

.PHONY: all clean

all: $(INIT)

$(INIT): init.c
	$(CC) $(CFLAGS) -o $@ $<
	$(STRIP) -s $@ 2>/dev/null || true

clean:
	rm -f $(INIT)