# Build the kernel-test initramfs.
#
#   make              -> initramfs.cpio (uncompressed, most universal) + .gz
#   make initramfs.cpio.zst   -> zstd-compressed (use CONFIG_RD_ZSTD)
#   make initramfs.cpio.xz    -> xz-compressed (use CONFIG_RD_XZ)
#   CROSS_COMPILE=arm-linux-gnueabi- make   -> cross build
#   make clean
#
# Result: an initramfs containing exactly one program, /init, which mounts
# the basic pseudo-filesystems, runs a kernel test suite, logs to the
# console + serial + kmsg, and sleeps forever.

CC      ?= cc
CROSS   ?=
CFLAGS  ?= -O2 -static -Wall -Wextra -D_GNU_SOURCE
LDLIBS  += -pthread

all: initramfs.cpio initramfs.cpio.gz

init: init.c
	$(CROSS)$(CC) $(CFLAGS) -o $@ init.c $(LDLIBS)
	@ls -lh $@

rootfs: init
	rm -rf rootfs
	mkdir -p rootfs/dev rootfs/proc rootfs/sys rootfs/tmp
	cp init rootfs/init
	cp hello rootfs/hello

initramfs.cpio: rootfs
	( cd rootfs && find . -print0 | cpio --null -o -H newc ) > initramfs.cpio

initramfs.cpio.gz: initramfs.cpio
	gzip -9 -c initramfs.cpio > initramfs.cpio.gz

initramfs.cpio.zst: initramfs.cpio
	zstd -19 -c initramfs.cpio > initramfs.cpio.zst

initramfs.cpio.xz: initramfs.cpio
	xz -9 -c initramfs.cpio > initramfs.cpio.xz

clean:
	rm -rf rootfs init initramfs.cpio initramfs.cpio.gz \
	       initramfs.cpio.zst initramfs.cpio.xz

.PHONY: all clean
