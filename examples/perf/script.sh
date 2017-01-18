#!/bin/sh

NVFUSE_RESET_PATH=/root/nvfuse/scripts/setup.sh
OUTPUT_PATH=output
RUNTIME=300

if [ ! -d $OUTPUT_PATH ] ; then
    mkdir $OUTPUT_PATH
fi

$NVFUSE_RESET_PATH configure

file_size=128 #giga
for workload in randread randwrite
do
    for qdepth in 1 2 4 8 16 32 64 128 256
    do
		for block_size in 4 8 16 32 64 128
		do
		    block_size=$((block_size*1024))
		    if [ $workload = read ] ; then 
			echo "${file_size}G file size, 128K block size, libaio, $qdepth qdepth, seq workload, direct I/O, read workload"
			sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $block_size -E libaio -Q $qdepth -D -T ${RUNTIME} | tee ${OUTPUT_PATH}/128K_read_${qdepth}_qdepth_${file_size}G_seq.log
		    elif [ $workload = write ] ; then 
			echo "${file_size}G file size, 128K block size, libaio, $qdepth qdepth, seq workload, direct I/O, write workload"
			sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $block_size -E libaio -Q $qdepth -D -W  -T ${RUNTIME} | tee ${OUTPUT_PATH}/128K_write_${qdepth}_qdepth_${file_size}G_seq.log
		    elif [ $workload = randread ] ; then 
			echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, random workload, direct I/O, read workload"
			sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $block_size -E libaio -Q $qdepth -R -D  -T ${RUNTIME}  | tee ${OUTPUT_PATH}/4K_read_${qdepth}_qdepth_${file_size}G_random.log
		    elif [ $workload = randwrite ] ; then 
			echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, random workload, direct I/O, write workload"
			sudo ./perf -t spdk -d 01:00 -f -m -S $((file_size*1024)) -B $block_size -E libaio -Q $qdepth -R -D -W -T ${RUNTIME}  | tee ${OUTPUT_PATH}/4K_write_${qdepth}_qdepth_${file_size}G_random.log
		    fi
		done
    done
done
