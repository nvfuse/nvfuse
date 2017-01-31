#!/bin/sh

NVFUSE_RESET_PATH=/root/nvfuse/scripts/setup.sh
OUTPUT_PATH=output
RUNTIME=60

if [ ! -d $OUTPUT_PATH ] ; then
    mkdir $OUTPUT_PATH
fi

$NVFUSE_RESET_PATH configure

file_size=32 #giga
app_name=perf

for workload in randread
do
	for core_mask in 2 6 14
	do
		for qdepth in 1 2 4 8 16 32 64 128 256
		do
			echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, random workload, direct I/O, read workload"
			cmd="sudo ./perf -a $app_name -c $core_mask -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -R -D  -T ${RUNTIME} | tee ${OUTPUT_PATH}/4K_coremask_${core_mask}_read_${qdepth}_qdepth_${file_size}G_random.log"
			echo $cmd
			eval $cmd
			if [ $? != 0 ] ; then
				break
			fi
		done
	done
done
