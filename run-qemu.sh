#!/bin/sh
# Boot-test the kernel-test initramfs in QEMU.
#   ./run-qemu.sh                     -> use /boot/vmlinuz-linux, power off after tests
#   KERNEL=/path/to/vmlinuz ./run-qemu.sh
#   EXTRA="kerneltest=stress" ./run-qemu.sh
#   VGA=1 ./run-qemu.sh               -> VGA framebuffer mode: boots with a
#                                        graphics console, waits for the tests
#                                        to finish, captures a screenshot
#                                        (run-vga.png), and keeps a serial log
#                                        (run-vga.serial.log). Requires socat.
#
# By default the VM powers itself off after the summary (kerneltest.poweroff=1)
# so the script exits. Remove that knob to watch it sleep forever.
set -e
cd "$(dirname "$0")"

KERNEL="${KERNEL:-/boot/vmlinuz-linux}"
IMG="initramfs.cpio.gz"
EXTRA="${EXTRA:-}"

[ -f "$IMG" ] || { echo "build first: ./build.sh"; exit 1; }
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL (set KERNEL=...)"; exit 1; }

# prefer KVM when available, else slow TCG emulation.
# NOTE: the statically-linked glibc init contains unconditional AVX
# instructions, so the vCPU must expose AVX: -cpu host (KVM) or -cpu max
# (TCG).  QEMU's default qemu64 CPU has no AVX and will SIGILL the init.
ACCEL="-accel kvm -cpu host"
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    ACCEL="-accel tcg -cpu max"
fi

if [ -n "$VGA" ]; then
    # ---------- VGA framebuffer mode ----------
    # Headless (-display none) but with a real VGA console; after the tests
    # finish we grab a screenshot through the QEMU monitor.  console=tty0
    # makes kernel printk go to the VGA too, and the harness's sink list
    # writes to /dev/tty0 as well, so the full test log appears on screen.
    # The serial port is captured over a unix socket (a plain `-serial file:`
    # backend is flaky and drops bytes under load).
    MON="$(mktemp -u /tmp/mininitrd-mon-XXXXXX.sock)"
    PORT=$((20000 + ($$ % 10000)))   # pid-scoped to avoid collisions
    rm -f run-vga.serial.log run-vga.png
    echo "VGA mode: serial log -> run-vga.serial.log, screen -> run-vga.png"
    qemu-system-x86_64 $ACCEL -m 512M -smp 2 \
        -kernel "$KERNEL" \
        -initrd "$IMG" \
        -vga std -display none \
        -monitor "unix:$MON,server,nowait" \
        -serial "tcp:127.0.0.1:$PORT,server,nowait" \
        -append "console=tty0 console=ttyS0,115200 rdinit=/init kerneltest.timeout=8 $EXTRA" \
        -no-reboot >/dev/null 2>&1 &
    QPID=$!
    # connect the TCP serial client *late* (after ~2s): QEMU buffers the
    # early serial output and a late connect drains it reliably, whereas an
    # immediate client connection makes the chardev drop bytes mid-suite.
    sleep 2
    socat -u "TCP:127.0.0.1:$PORT" "CREATE:run-vga.serial.log" & SPID=$!
    # wait for the summary to appear in the serial capture, then grab the
    # VGA screen while the result is still displayed (VM powers off soon
    # after).  Works for every test intensity.
    i=0
    while [ $i -lt 80 ]; do
        grep -q 'kerneltest complete' run-vga.serial.log 2>/dev/null && break
        sleep 0.1; i=$((i + 1))
    done
    sleep 0.3
    echo "screendump /tmp/mininitrd-shot.ppm" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 \
        || echo "WARNING: screendump failed (screenshot will be missing)"
    if [ -f /tmp/mininitrd-shot.ppm ]; then
        magick /tmp/mininitrd-shot.ppm run-vga.png 2>/dev/null \
            || convert /tmp/mininitrd-shot.ppm run-vga.png
        rm -f /tmp/mininitrd-shot.ppm
    fi
    echo "quit" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 || true
    wait $QPID
    kill $SPID 2>/dev/null || true
    [ -f run-vga.png ] && echo "screenshot saved: run-vga.png"
    exit 0
fi

qemu-system-x86_64 $ACCEL -m 512M -smp 2 \
    -kernel "$KERNEL" \
    -initrd "$IMG" \
    -append "console=ttyS0 rdinit=/init kerneltest.poweroff=1 $EXTRA" \
    -nographic -no-reboot
