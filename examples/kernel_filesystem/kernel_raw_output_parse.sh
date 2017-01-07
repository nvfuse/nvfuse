#!/bin/sh

FIO_MINIMAL_DECODER=~/fio_minimal_decoder/fio_minimal_output_decode.py
OUTPUT_PATH=output
PREFIXS=(kernel_ext2 kernel_xfs)

for PREFIX in "${PREFIXS[@]}"
do

    READ_IOPS_LOG=${OUTPUT_PATH}/${PREFIX}_read_iops.log
    WRITE_IOPS_LOG=${OUTPUT_PATH}/${PREFIX}_write_iops.log

    READ_LAT_AVG_LOG=${OUTPUT_PATH}/${PREFIX}_read_lat_avg.log
    WRITE_LAT_AVG_LOG=${OUTPUT_PATH}/${PREFIX}_write_lat_avg.log

    READ_LAT_MIN_LOG=${OUTPUT_PATH}/${PREFIX}_read_lat_min.log
    WRITE_LAT_MIN_LOG=${OUTPUT_PATH}/${PREFIX}_write_lat_min.log

    READ_LAT_MAX_LOG=${OUTPUT_PATH}/${PREFIX}_read_lat_max.log
    WRITE_LAT_MAX_LOG=${OUTPUT_PATH}/${PREFIX}_write_lat_max.log

    rm $READ_IOPS_LOG
    rm $WRITE_IOPS_LOG

    rm $READ_LAT_AVG_LOG
    rm $WRITE_LAT_AVG_LOG

    rm $READ_LAT_MIN_LOG
    rm $WRITE_LAT_MIN_LOG

    rm $READ_LAT_MAX_LOG
    rm $WRITE_LAT_MAX_LOG

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

	    output=${OUTPUT_PATH}/${PREFIX}_q_${qdepth}_block_${block_size}_workload_${workload}.log
	    if [ $workload = randread ] ; then 

		# IOPS
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_iops | head -n 1)
		echo $ret
		echo qd $qdepth  $ret >> ${READ_IOPS_LOG}

		# AVG LAT 
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_lat_mean)
		echo $ret
		echo qd $qdepth  $ret >> ${READ_LAT_AVG_LOG}

		# MAX LAT 
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_lat_max)
		echo $ret
		echo qd $qdepth  $ret >> ${READ_LAT_MAX_LOG}

		# MIN LAT 
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep read_lat_min)
		echo $ret
		echo qd $qdepth  $ret >> ${READ_LAT_MIN_LOG}
	    else

		# IOPS
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_iops | head -n 1)
		echo $ret
		echo qd $qdepth  $ret >> ${WRITE_IOPS_LOG}

		# AVG LAT 
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_lat_mean)
		echo $ret
		echo qd $qdepth  $ret >> ${WRITE_LAT_AVG_LOG}

		# MAX LAT 
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_lat_max)
		echo $ret
		echo qd $qdepth  $ret >> ${WRITE_LAT_MAX_LOG}

		# MIN LAT 
		ret=$($FIO_MINIMAL_DECODER $output | awk '{print $2"\t"$3}' | grep write_lat_min)
		echo $ret
		echo qd $qdepth  $ret >> ${WRITE_LAT_MIN_LOG}
	    fi
	done
    done

    cat $READ_IOPS_LOG
    cat $WRITE_IOPS_LOG

    cat $READ_LAT_AVG_LOG
    cat $WRITE_LAT_AVG_LOG

    cat $READ_LAT_MIN_LOG
    cat $WRITE_LAT_MIN_LOG

    cat $READ_LAT_MAX_LOG
    cat $WRITE_LAT_MAX_LOG
done
