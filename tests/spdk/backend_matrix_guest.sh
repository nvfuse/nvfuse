#!/usr/bin/env bash
# Reuse only the exact dedicated QEMU fixture; never target a physical device.
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
spdk_dir=${SPDK_ROOT_DIR:-$HOME/src/spdk}
ctrl=
for candidate in /sys/class/nvme/nvme*; do
    if [[ $(xargs < "$candidate/serial") == NVFUSEDEV0001 ]]; then
        test -z "$ctrl"
        ctrl=$candidate
    fi
done
test -n "$ctrl"
bdf=$(basename "$(readlink -f "$ctrl/device")")
disk=/dev/$(basename "$ctrl")n1
test "$(cat /sys/bus/pci/devices/"$bdf"/vendor)" = 0x1b36
test "$(cat /sys/bus/pci/devices/"$bdf"/device)" = 0x0010
test "$(sudo blockdev --getsize64 "$disk")" = 2147483648
test -z "$(lsblk -nr -o MOUNTPOINTS "$disk" | xargs)"
test "$(find /sys/bus/pci/devices/"$bdf"/iommu_group/devices -maxdepth 1 -type l | wc -l)" = 1
test "$#" -le 1
report_dir=${1:-$repo/build/test-results/backend-spdk-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$(dirname "$report_dir")"
mkdir "$report_dir"
# Preserve the prior fixture bytes before reformatting its logical test area.
sudo dd if="$disk" of="$report_dir/nvme-before.img" bs=1M conv=sparse status=none
sudo modprobe vfio_iommu_type1
irq_param=/sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts
old_irq=$(cat "$irq_param")
old_hp=$(cat /proc/sys/vm/nr_hugepages)
active_pid=
stop() {
    trap '' TERM INT
    if [[ -n $active_pid ]]; then
        kill -TERM "$active_pid" 2>/dev/null || true
        wait "$active_pid" || true
    fi
    exit 143
}
trap stop TERM INT
restore() {
    result=$?
    trap - EXIT
    (cd "$spdk_dir" && sudo env PCI_ALLOWED="$bdf" scripts/setup.sh reset) > "$report_dir/reset.log" 2>&1 || result=1
    printf '%s\n' "$old_hp" | sudo tee /proc/sys/vm/nr_hugepages > /dev/null || result=1
    printf '%s\n' "$old_irq" | sudo tee "$irq_param" > /dev/null || result=1
    handoff=not_run
    if [[ $result == 0 ]]; then
        if sudo "$repo/build/fuse/backend_test" after-fuse kernel "$disk" > "$report_dir/spdk-to-kernel.log" 2>&1; then
            handoff=pass
        else
            handoff=fail
            result=1
        fi
    fi
    driver=$(basename "$(readlink /sys/bus/pci/devices/"$bdf"/driver)")
    actual_hp=$(cat /proc/sys/vm/nr_hugepages)
    actual_irq=$(cat "$irq_param")
    [[ $driver == nvme && $actual_hp == "$old_hp" && $actual_irq == "$old_irq" ]] || result=1
    python3 - "$report_dir" "$result" "$driver" "$old_hp" "$actual_hp" "$old_irq" "$actual_irq" "$handoff" <<'PY'
import json
from pathlib import Path
import sys
folder, code, driver, old_hp, hp, old_irq, irq, handoff = sys.argv[1:]
report = dict(status="pass" if code == "0" else "fail", driver=driver,
              expected_hugepages=int(old_hp), hugepages=int(hp),
              expected_interrupts=old_irq, interrupts=irq, spdk_to_kernel=handoff)
(Path(folder) / "cleanup.json").write_text(json.dumps(report, indent=2) + "\n")
PY
    printf 'Driver restoration status: %s\n' "$result"
    exit "$result"
}
trap restore EXIT
# Existing QEMU ARM setup: keep SMMUv3 DMA isolation, allow virtual interrupts.
printf 'Y\n' | sudo tee "$irq_param" > /dev/null
(cd "$spdk_dir" && sudo env PCI_ALLOWED="$bdf" DRIVER_OVERRIDE=vfio-pci HUGEMEM=512 scripts/setup.sh) > "$report_dir/setup.log" 2>&1
test "$(basename "$(readlink /sys/bus/pci/devices/"$bdf"/driver)")" = vfio-pci
# Check that the pre-existing fixture-only API still works before exercising
# the new public opener. The matrix then reformats the same disposable area.
sudo prlimit --core=0 --memlock=unlimited "$repo/build/fuse-spdk/backend_test" \
    legacy-create spdk "$bdf" > "$report_dir/legacy-spdk.log" 2>&1 &
active_pid=$!
wait "$active_pid"
active_pid=
sudo env NVFUSE_TEST_BDF="$bdf" prlimit --core=0 --memlock=unlimited \
    python3 "$repo/tests/run_backend_matrix.py" --backend spdk --target "$bdf" \
    --api-binary "$repo/build/fuse-spdk/backend_test" \
    --fuse-binary "$repo/examples/fuse3/nvfuse-fuse-spdk" \
    --report-dir "$report_dir/results" &
active_pid=$!
wait "$active_pid"
active_pid=
