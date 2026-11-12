#!/usr/bin/env bash
# Only for the dedicated 2 GiB QEMU NVMe in the local development VM.
set -euo pipefail
case ${1:-} in matrix|boundaries|injected|hotremove|stalled|integration|integration-data-prepare|integration-data-check|integration-meta-prepare|integration-meta-check|integration-fault|integration-transactions) ;; *) exit 2 ;; esac
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
if [[ $1 == integration* ]]; then
    BIN_DIR="$HOME/src/nvfuse/tests/spdk"
    if [[ $1 == integration-transactions ]]; then
        tx="$BIN_DIR/integration_transactions"
        txrun() { sudo prlimit --core=0 --memlock=unlimited timeout 60 "$tx" "$1" "pcie:$BDF" "${@:2}"; }
        txrun setup
        txrun functional
        txrun new
        for phase in payload-flush commit-flush checkpoint-write retire-flush; do
            txrun setup
            set +e
            txrun crash "$phase" 1
            status=$?
            set -e
            test "$status" = 86
            if [[ $phase == payload-flush ]]; then outcome=old; else outcome=new; fi
            txrun "$outcome"
            txrun "$outcome"
        done
        echo 'PASS NVMe transactions and original-image crash replay'
        exit 0
    fi
    if [[ $1 == integration-fault ]]; then
        case ${3:-} in locate|unclean|read-eio|write-eio|stall-resume|stall-timeout|hotremove) ;; *) exit 2 ;; esac
        [[ ${4:-} =~ ^[0-9]+$ ]] || exit 2
        rm -f /tmp/nvfuse-fault-go
        log="$LOG_DIR/fault-$3-seed-$2.log"
        set +e
        sudo prlimit --core=0 --memlock=unlimited timeout 45 "$BIN_DIR/integration_fault" \
            "$3" "pcie:$BDF" "$2" "$4" 2>&1 | tee "$log"
        status=${PIPESTATUS[0]}
        set -e
        case $3 in
            locate|unclean|stall-resume)
                test "$status" = 0
                grep -q 'PASS ' "$log" ;;
            *)
                test "$status" = 134
                grep -q "SUBMITTED fault $3" "$log"
                case $3 in
                    read-eio|write-eio)
                        grep -Eq '(NVFUSE SPDK: block I/O failed: -5|file backend I/O transfer error: -5)' "$log"
                        grep -q "completion error op=${3%-eio} " "$log" ;;
                    stall-timeout) grep -q 'NVFUSE SPDK: I/O timeout after 10 seconds' "$log" ;;
                    hotremove) grep -Eq 'NVFUSE SPDK: (block I/O failed:|completion polling failed:|I/O timeout)' "$log" ;;
                esac
                echo "PASS fault $3 expected SIGABRT" ;;
        esac
        exit 0
    fi
    stage=0
    run_fs() {
        local binary=$1 mode=$2
        stage=$((stage + 1))
        local log="$LOG_DIR/$CASE-$stage-$2-seed-$3.log"
        local args=("$mode" "pcie:$BDF")
        if [[ $binary == integration_framework ]]; then args+=("$3"); fi
        sudo prlimit --memlock=unlimited timeout 90 "$BIN_DIR/$binary" "${args[@]}" > "$log" 2>&1 || {
            tail -n 50 "$log"; return 1;
        }
        grep -q "PASS $mode" "$log"
        grep -q 'SPDK_IO reads=' "$log"
        echo "PASS integration $mode seed=$3"
        grep SPDK_IO "$log"
    }
    CASE=$1
    case $1 in
        integration-data-prepare) run_fs integration_framework data-create "$2"; exit 0 ;;
        integration-data-check) run_fs integration_framework data-verify "$2"; exit 0 ;;
        integration-meta-prepare) run_fs integration_framework meta-create "$2"; exit 0 ;;
        integration-meta-check)
            for mode in meta-verify meta-delete meta-final; do
                run_fs integration_framework "$mode" "$2"
            done
            exit 0 ;;
    esac
    # Each invocation is a fresh process; 'reopen' additionally remounts three
    # times in the same process. Reformat only this exact disposable QEMU disk.
    for mode in create verify reopen mutate final; do
        run_fs integration_test "$mode" "$2"
    done
    for mode in data-create data-verify reject-meta stored-create stored-verify; do
        run_fs integration_framework "$mode" "$2"
    done
    for mode in meta-create reject-full meta-verify meta-verify meta-delete meta-final; do
        run_fs integration_framework "$mode" "$2"
    done
    echo 'PASS integration suite'
    exit 0
fi
sudo prlimit --memlock=unlimited timeout 45 "$HOME/src/nvfuse-framework/io_suite" "$1" \
    "trtype:PCIe traddr:$BDF" "$2"
