#!/usr/bin/env bash
# Only for the dedicated 2 GiB QEMU NVMe in the local development VM.
set -euo pipefail
case ${1:-} in matrix|boundaries|injected|hotremove|stalled|integration|integration-data-prepare|integration-data-check|integration-meta-prepare|integration-meta-check|integration-fault|integration-transactions|integration-stress|integration-powercut-prepare|integration-powercut-run|integration-powercut-check|integration-final-inode|integration-resilience-setup|integration-resilience-crash|integration-resilience-check|integration-resilience-pressure) ;; *) exit 2 ;; esac
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
    if [[ $1 == integration-resilience-* ]]; then
        app="$BIN_DIR/integration_resilience"
        case $1 in
            integration-resilience-setup)
                sudo prlimit --core=0 --memlock=unlimited timeout 180 "$app" setup "pcie:$BDF" ;;
            integration-resilience-pressure)
                sudo prlimit --core=0 --memlock=unlimited timeout 180 "$app" pressure "pcie:$BDF" ;;
            integration-resilience-crash)
                op=${4:-append};case $op in append|move|move-empty) ;; *) exit 2 ;; esac
                case ${3:-} in payload-flush|checkpoint-write|after-fsync) ;; *) exit 2 ;; esac
                sudo env NVFUSE_VM_CUT=1 prlimit --core=0 --memlock=unlimited timeout 180 "$app" data-crash "pcie:$BDF" "$op" "$3" ;;
            integration-resilience-check)
                op=${4:-append};case $op in append|move|move-empty) ;; *) exit 2 ;; esac
                case ${3:-} in old|new) ;; *) exit 2 ;; esac
                sudo prlimit --core=0 --memlock=unlimited timeout 180 "$app" data-check "pcie:$BDF" "$op" "$3" ;;
        esac
        exit 0
    fi
    if [[ $1 == integration-final-inode ]]; then
        for kind in files dirs sharded; do
            log="$LOG_DIR/final-inode-$kind.log"
            sudo prlimit --core=0 --memlock=unlimited timeout 180 "$BIN_DIR/integration_limits" create "pcie:$BDF" "$kind" 128 > "$log" 2>&1 || { tail -n 80 "$log";exit 1; }
            grep -q 'PASS final inode reuse before exhaustion rollback' "$log"
            grep 'SPDK_IO reads=' "$log"
            echo "PASS NVMe final-inode reuse $kind"
        done
        exit 0
    fi
    if [[ $1 == integration-powercut-* ]]; then
        lim="$BIN_DIR/integration_limits"
        case $1 in
            integration-powercut-prepare)
                sudo prlimit --core=0 --memlock=unlimited timeout 180 "$lim" create "pcie:$BDF" size 128 ;;
            integration-powercut-run)
                [[ ${3:-} =~ ^[0-9]+$ ]] || exit 2
                case ${4:-} in payload-flush|checkpoint-write) ;; *) exit 2 ;; esac
                sudo env NVFUSE_VM_CUT=1 prlimit --core=0 --memlock=unlimited timeout 180 "$lim" crash "pcie:$BDF" size 128 "$3" truncate "$4" 1 ;;
            integration-powercut-check|integration-final-inode)
                [[ ${3:-} =~ ^[0-9]+$ ]] || exit 2
                case ${4:-} in old|new) ;; *) exit 2 ;; esac
                sudo prlimit --core=0 --memlock=unlimited timeout 180 "$lim" check-crash "pcie:$BDF" size 128 "$3" truncate "$4" ;;
        esac
        exit 0
    fi
    if [[ $1 == integration-stress ]]; then
        stressrun() {
            local binary=$1 label=$2 expected=$3
            shift 3
            local log="$LOG_DIR/stress-$label.log"
            set +e
            sudo prlimit --core=0 --memlock=unlimited timeout 180 "$BIN_DIR/$binary" "$@" > "$log" 2>&1
            local status=$?
            set -e
            if [[ $status != "$expected" ]]; then tail -n 80 "$log";return 1;fi
            if [[ $expected == 0 ]]; then
                grep -q 'PASS ' "$log"
                grep -q 'SPDK_IO reads=' "$log"
                grep 'SPDK_IO reads=' "$log" | tail -n 1
            fi
            echo "PASS stress $label"
        }
        for meta in 0 1; do
            stressrun integration_mixed "mixed-$meta-seed-$2" 0 "pcie:$BDF" "$2" 3000 "$meta"
        done
        # Both image sizes exercise inode/block exhaustion and two full cycles.
        for mib in 128 256; do
            for kind in files sharded dirs size; do
                stressrun integration_limits "limit-$mib-$kind" 0 create "pcie:$BDF" "$kind" "$mib"
                reached=$(python3 -c 'import json,sys; print(next(json.loads(s[6:])["reached"] for s in open(sys.argv[1]) if s.startswith("LIMIT ")))' "$LOG_DIR/stress-limit-$mib-$kind.log")
                for cycle in 0 1; do
                    stressrun integration_limits "verify-$mib-$kind-$cycle" 0 verify "pcie:$BDF" "$kind" "$mib" "$reached"
                    stressrun integration_limits "cleanup-$mib-$kind-$cycle" 0 cleanup "pcie:$BDF" "$kind" "$mib" "$reached"
                    python3 - "$LOG_DIR/stress-limit-$mib-$kind.log" "$LOG_DIR/stress-cleanup-$mib-$kind-$cycle.log" <<'CHECK_COUNTS'
import json,sys
def row(path,tag):
    return next(json.loads(s[len(tag):]) for s in open(path) if s.startswith(tag))
a=row(sys.argv[1],'LIMIT '); b=row(sys.argv[2],'CLEANUP ')
assert (a['baseline_blocks'],a['baseline_inodes'])==(b['free_blocks'],b['free_inodes']), (a,b)
CHECK_COUNTS
                    if [[ $cycle == 0 ]]; then
                        stressrun integration_limits "refill-$mib-$kind" 0 refill "pcie:$BDF" "$kind" "$mib"
                        python3 - "$LOG_DIR/stress-limit-$mib-$kind.log" "$LOG_DIR/stress-refill-$mib-$kind.log" <<'CHECK_REFILL'
import json,sys
def capacity(path):
    return next(json.loads(s[6:])['reached'] for s in open(path) if s.startswith('LIMIT '))
assert capacity(sys.argv[1])==capacity(sys.argv[2])
CHECK_REFILL
                    fi
                done
            done
        done
        # Representative ordinary API mutations at exhaustion, on the real
        # NVMe/cache/journal path; the host suite covers all seven mutations.
        for item in files:create dirs:delete size:truncate; do
            kind=${item%:*};op=${item#*:}
            for phase in payload-flush commit-flush checkpoint-write retire-flush; do
                label="crash-$kind-$op-$phase"
                stressrun integration_limits "$label-fill" 0 create "pcie:$BDF" "$kind" 128
                reached=$(python3 -c 'import json,sys; print(next(json.loads(s[6:])["reached"] for s in open(sys.argv[1]) if s.startswith("LIMIT ")))' "$LOG_DIR/stress-$label-fill.log")
                if [[ $op == create ]]; then stressrun integration_limits "$label-reserve" 0 reserve "pcie:$BDF" "$kind" 128 "$reached" "$op";fi
                stressrun integration_limits "$label-kill" 86 crash "pcie:$BDF" "$kind" 128 "$reached" "$op" "$phase" 1
                if [[ $phase == payload-flush ]]; then outcome=old;else outcome=new;fi
                for repeat in 0 1; do
                    stressrun integration_limits "$label-check-$repeat" 0 check-crash "pcie:$BDF" "$kind" 128 "$reached" "$op" "$outcome"
                done
                stressrun integration_limits "$label-reuse" 0 reuse "pcie:$BDF" "$kind" 128 "$reached" "$op"
            done
        done
        echo 'PASS NVMe mixed, exhaustion, complete reclamation and boundary crash recovery'
        exit 0
    fi
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
