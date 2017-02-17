#!/bin/sh

nvme format /dev/nvme0n1
source preconditioning.sh
source ext2_perf.sh

nvme format /dev/nvme0n1
source preconditioning.sh
source ext4_perf.sh

nvme format /dev/nvme0n1
source preconditioning.sh
source xfs_perf.sh
