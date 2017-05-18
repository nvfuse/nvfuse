#!/bin/sh

. ../root_check.sh

NVFUSE_RESET_PATH=/root/nvfuse/scripts/setup.sh
OUTPUT_PATH=output
RUNTIME=60

if [ ! -d $OUTPUT_PATH ] ; then
    mkdir $OUTPUT_PATH
fi

$NVFUSE_RESET_PATH configure

file_size=128 #giga
app_name=perf

#for workload in randread randwrite
for workload in read randread 
do
	#for core_mask in 1 3 5 7
	for core_mask in 7
	do
		for qdepth in 256
		do

			core_mask=$(((1 << core_mask) - 1 << 1))

			echo "${file_size}G file size, 4K block size, libaio, $qdepth qdepth, random workload, direct I/O, $workload workload"
			if [ $workload = write -o $workload = randwrite ] ; then 
				write_workload="-W"
				if [ $workload = write ] ; then 
					cmd="sudo ./perf -a $app_name -c $core_mask -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -R -D  -T ${RUNTIME} $write_workload | tee ${OUTPUT_PATH}/4K_coremask_${core_mask}_write_${qdepth}_qdepth_${file_size}G_sequential.log"
				else
					cmd="sudo ./perf -a $app_name -c $core_mask -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -R -D  -T ${RUNTIME} $write_workload | tee ${OUTPUT_PATH}/4K_coremask_${core_mask}_write_${qdepth}_qdepth_${file_size}G_random.log"

				fi
			else
				write_workload=""
				if [ $workload = read ] ; then 
					cmd="sudo ./perf -a $app_name -c $core_mask -S $((file_size*1024)) -B $((128*1024)) -E libaio -Q $qdepth -D  -T ${RUNTIME} $write_workload | tee ${OUTPUT_PATH}/4K_coremask_${core_mask}_read_${qdepth}_qdepth_${file_size}G_sequential.log"
				else
					cmd="sudo ./perf -a $app_name -c $core_mask -S $((file_size*1024)) -B 4096 -E libaio -Q $qdepth -R -D  -T ${RUNTIME} $write_workload | tee ${OUTPUT_PATH}/4K_coremask_${core_mask}_read_${qdepth}_qdepth_${file_size}G_random.log"

				fi
			fi

			echo $cmd
            eval $cmd
			if [ $? != 0 ] ; then
				break
			fi
		done
	done
done
