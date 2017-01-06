#!/bin/sh
SPDK_PERF_PATH=/root/spdk/examples/nvme/perf/perf
SPDK_RESET_PATH=/root/spdk/scripts/setup.sh
OUTPUT_PATH=output

if [ ! -d $OUTPUT_PATH ] ; then
    mkdir $OUTPUT_PATH
fi

$SPDK_RESET_PATH configure

#[-q io depth]
#[-s io size in bytes]
#[-w io pattern type, must be one of
#    (read, write, randread, randwrite, rw, randrw)]
#[-M rwmixread (100 for reads, 0 for writes)]
#[-l enable latency tracking, default: disabled]
#[-t time in seconds]
#[-c core mask for I/O submission/completion.]
#    (default: 1)]
#[-r discover info of remote NVMe over Fabrics target:
#    Format: TRTYPE:TRADDR:TRVCSID e.g., rdma:192.168.100.8:4420]
#[-d DPDK huge memory size in MB.]
#[-m max completions per poll]
#    (default: 0 - unlimited)

#for workload in read randread
for workload in randread randwrite
do
    for qdepth in 1 2 4 8 16 32 64 128 256
    do
	if [ $workload = read -o $workload = write ] ; then 
	    block_size=$((64*1024))
	else
	    block_size=$((4096))
	fi
	echo $SPDK_PERF_PATH -q $qdepth -s $block_size -w $workload -t 60
	$SPDK_PERF_PATH -q $qdepth -s $block_size -w $workload -t 60 > ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log
    done
done
