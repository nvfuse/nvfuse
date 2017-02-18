#!/bin/sh
FIO_PERF_PATH=fio
SPDK_RESET_PATH=/root/spdk/scripts/setup.sh
OUTPUT_PATH=output

if [ ! -d $OUTPUT_PATH ] ; then
    mkdir $OUTPUT_PATH
fi

$SPDK_RESET_PATH reset
DEV_NAME=/dev/nvme0n1
MOUNT_PATH=/media/xfs

mkfs.xfs $DEV_NAME -f -K

if [ ! -d $MOUNT_PATH ] ; then
    mkdir $MOUNT_PATH
fi
mount $DEV_NAME $MOUNT_PATH

#for workload in read randread
for workload in randread randwrite
do
    for numjobs in {1..7}
    do
	    for qdepth in 1 2 4 8 16 32 64 128 256
	    do
		if [ $workload = read -o $workload = write ] ; then 
		    block_size=$((64*1024))
		else
		    block_size=$((4096))
		fi

		str="$FIO_PERF_PATH --name=test --directory=${MOUNT_PATH} --direct=1 --size=128G --ioengine=libaio --iodepth=$qdepth --bs=$block_size --rw=$workload --runtime=120 --numjobs=${numjobs} --thread=1  --group_reporting  --group_reporting --minimal --output=${OUTPUT_PATH}/kernel_xfs_aio_numjobs_${numjobs}_q_${qdepth}_block_${block_size}_workload_${workload}.log"
        echo $str
        eval $str

	    done
    done
done

for numjobs in {1..7}
do
    str="$FIO_PERF_PATH --name=test --directory=${MOUNT_PATH}/ --size=16g --ioengine=sync --bs=4k --rw=randwrite --numjobs=$numjobs --thread=1 --fsync=1  --group_reporting  --group_reporting --minimal --runtime=120 --output=${OUTPUT_PATH}/kernel_xfs_sync_numjobs_${numjobs}_q_${qdepth}_block_${block_size}_workload_${workload}.log"
    echo $str
    eval $str
done

umount $MOUNT_PATH
