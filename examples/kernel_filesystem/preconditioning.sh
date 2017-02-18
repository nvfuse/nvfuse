#!/bin/sh

fio --name=seq_write --filename=/dev/nvme0n1 --bs=128k --iodepth=128 --direct=1 --ioengine=libaio --rw=write

fio --name=rand_write --filename=/dev/nvme0n1 --bs=4k --iodepth=128 --numjobs=4 --thread --direct=1 --ioengine=libaio --rw=randwrite --runtime=1800 --time_based
