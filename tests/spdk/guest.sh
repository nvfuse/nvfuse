#!/usr/bin/env bash
# Only for the dedicated 2 GiB QEMU NVMe in the local development VM.
set -euo pipefail
case ${1:-} in matrix|boundaries|injected|hotremove|stalled) ;; *) exit 2 ;; esac
if [[ $1 == stalled ]]; then rm -f /tmp/nvfuse-stall-go; fi
[[ ${2:-} =~ ^[0-9]+$ ]] || exit 2
SPDK_ROOT_DIR=${SPDK_ROOT_DIR:-$HOME/src/spdk}
cd "$SPDK_ROOT_DIR"
CTRL=
for candidate in /sys/class/nvme/nvme*; do
    if [[ $(xargs < "$candidate/serial") == NVFUSEDEV0001 ]]; then
        test -z "$CTRL"
        CTRL=$candidate
    fi
done
test -n "$CTRL"
BDF=$(basename "$(readlink -f "$CTRL/device")")
DISK=/dev/$(basename "$CTRL")n1
test "$(cat /sys/bus/pci/devices/$BDF/vendor)" = 0x1b36
test "$(cat /sys/bus/pci/devices/$BDF/device)" = 0x0010
test "$(sudo blockdev --getsize64 "$DISK")" = 2147483648
test -z "$(lsblk -nr -o MOUNTPOINTS "$DISK" | xargs)"
test "$(find /sys/bus/pci/devices/$BDF/iommu_group/devices -maxdepth 1 -type l | wc -l)" = 1
sudo modprobe vfio_iommu_type1
IRQ_PARAM=/sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts
OLD_IRQ=$(cat "$IRQ_PARAM")
OLD_HP=$(cat /proc/sys/vm/nr_hugepages)
LOG_DIR=$HOME/nvfuse-spdk-logs
mkdir -p "$LOG_DIR"
restore() {
    result=$?
    trap - EXIT
    sudo env PCI_ALLOWED="$BDF" scripts/setup.sh reset > "$LOG_DIR/reset.log" 2>&1 || result=1
    printf '%s\n' "$OLD_HP" | sudo tee /proc/sys/vm/nr_hugepages > /dev/null || result=1
    printf '%s\n' "$OLD_IRQ" | sudo tee "$IRQ_PARAM" > /dev/null || result=1
    exit "$result"
}
trap restore EXIT
# QEMU ARM has no interrupt remapping. DMA remains protected by SMMUv3.
printf 'Y\n' | sudo tee "$IRQ_PARAM" > /dev/null
sudo env PCI_ALLOWED="$BDF" DRIVER_OVERRIDE=vfio-pci HUGEMEM=512 \
    scripts/setup.sh > "$LOG_DIR/setup.log" 2>&1
test "$(basename "$(readlink /sys/bus/pci/devices/$BDF/driver)")" = vfio-pci
sudo prlimit --memlock=unlimited timeout 45 "$HOME/src/nvfuse-framework/io_suite" "$1" \
    "trtype:PCIe traddr:$BDF" "$2"
