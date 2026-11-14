#!/bin/sh
set -eu
VM_DIR=$(CDPATH= cd -- "${1:?usage: start_vm.sh VM_DIRECTORY}" && pwd)
if [ -f "$VM_DIR/qemu.pid" ] && kill -0 "$(cat "$VM_DIR/qemu.pid")" 2>/dev/null; then
    echo 'VM is already running'
    exit 0
fi
test -f "$VM_DIR/blkdebug.conf" || : > "$VM_DIR/blkdebug.conf"
QEMU_PREFIX=$(brew --prefix qemu)
exec "$QEMU_PREFIX/bin/qemu-system-aarch64" \
    -name nvfuse-rocky9 \
    -machine virt,accel=hvf,gic-version=3,iommu=smmuv3 \
    -cpu host -smp 4 -m 4096 \
    -bios "$QEMU_PREFIX/share/qemu/edk2-aarch64-code.fd" \
    -drive "if=virtio,format=qcow2,file=$VM_DIR/os.qcow2,discard=unmap" \
    -drive "if=virtio,format=raw,file=$VM_DIR/seed.iso,readonly=on" \
    -drive "if=none,id=nvme0,format=raw,file=blkdebug:$VM_DIR/blkdebug.conf:$VM_DIR/nvme.raw" \
    -device pcie-root-port,id=nvme-port,chassis=1,slot=1 \
    -device nvme,drive=nvme0,serial=NVFUSEDEV0001,id=nvfuse-test-nvme,bus=nvme-port \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2229-:22 \
    -device virtio-net-pci,netdev=net0,romfile= \
    -object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,rng=rng0 \
    -display none -serial "file:$VM_DIR/console.log" \
    -monitor "unix:$VM_DIR/monitor.sock,server=on,wait=off" \
    -qmp "unix:$VM_DIR/qmp.sock,server=on,wait=off" \
    -pidfile "$VM_DIR/qemu.pid" -daemonize
