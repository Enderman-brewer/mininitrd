#!/bin/sh
# Convenience wrapper: build the initramfs, optionally boot-test it in QEMU.
set -e
cd "$(dirname "$0")"
make -s all
echo
ls -lh initramfs.cpio initramfs.cpio.gz
echo
echo "Boot-test with:  ./run-qemu.sh [/path/to/vmlinuz]"
