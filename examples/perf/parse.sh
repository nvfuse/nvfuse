#!/bin/sh

OUTPUT_PATH=output

READ_IOPS_LOG=${OUTPUT_PATH}/read_iops.log
READ_LAT_AVG_LOG=${OUTPUT_PATH}/read_lat_avg.log
READ_LAT_MIN_LOG=${OUTPUT_PATH}/read_lat_min.log
READ_LAT_MAX_LOG=${OUTPUT_PATH}/read_lat_max.log

WRITE_IOPS_LOG=${OUTPUT_PATH}/write_iops.log
WRITE_LAT_AVG_LOG=${OUTPUT_PATH}/write_lat_avg.log
WRITE_LAT_MIN_LOG=${OUTPUT_PATH}/write_lat_min.log
WRITE_LAT_MAX_LOG=${OUTPUT_PATH}/write_lat_max.log

rm $READ_IOPS_LOG
rm $READ_LAT_AVG_LOG
rm $READ_LAT_MIN_LOG
rm $READ_LAT_MAX_LOG

rm $WRITE_IOPS_LOG
rm $WRITE_LAT_AVG_LOG
rm $WRITE_LAT_MIN_LOG
rm $WRITE_LAT_MAX_LOG

for workload in randread randwrite
do
    for coremask in 2 6 14 30 62 126
    do
	    for qdepth in 1 2 4 8 16 32 64 128 256
	    do
		if [ $workload = read ] ; then 
		    cat ${OUTPUT_PATH}/128K_read_${qdepth}_qdepth_${file_size}G_seq.log 
		elif [ $workload = write ] ; then 
		    cat ${OUTPUT_PATH}/128K_write_${qdepth}_qdepth_${file_size}G_seq.log
		elif [ $workload = randread ] ; then 
		
		    # IOPS
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_read_${qdepth}_qdepth_${file_size}G_random.log | grep 'group iops')
		    echo coremask ${coremask} qdepth	$qdepth	$ret >> $READ_IOPS_LOG

		    # LAT AVG
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_read_${qdepth}_qdepth_${file_size}G_random.log | grep 'group avg latency')
		    echo coremask ${coremask}  qdepth	$qdepth	$ret >> $READ_LAT_AVG_LOG

		    # LAT MIN
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_read_${qdepth}_qdepth_${file_size}G_random.log | grep 'group min latency')
		    echo coremask ${coremask}  qdepth	$qdepth	$ret >> $READ_LAT_MIN_LOG

		    # LAT MAX
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_read_${qdepth}_qdepth_${file_size}G_random.log | grep 'group max latency')
		    echo coremask ${coremask}  qdepth	$qdepth	$ret >> $READ_LAT_MAX_LOG

		elif [ $workload = randwrite ] ; then 

		    # IOPS
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_write_${qdepth}_qdepth_${file_size}G_random.log | grep 'group iops')
		    echo coremask ${coremask}  qdepth	$qdepth	$ret >> $WRITE_IOPS_LOG

		    # LAT AVG
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_write_${qdepth}_qdepth_${file_size}G_random.log | grep 'group avg latency')
		    echo coremask ${coremask}  qdepth	$qdepth	$ret >> $WRITE_LAT_AVG_LOG

		    # LAT MIN
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_write_${qdepth}_qdepth_${file_size}G_random.log | grep 'group min latency')
		    echo coremask ${coremask}  qdepth	$qdepth	$ret >> $WRITE_LAT_MIN_LOG

		    # LAT MAX
		    ret=$(cat ${OUTPUT_PATH}/4K_coremask_${coremask}_write_${qdepth}_qdepth_${file_size}G_random.log | grep 'group max latency')
		    echo  coremask ${coremask} qdepth	$qdepth	$ret >> $WRITE_LAT_MAX_LOG

		fi
	    done
    done
done

cat $READ_IOPS_LOG
cat $READ_LAT_AVG_LOG
cat $READ_LAT_MIN_LOG
cat $READ_LAT_MAX_LOG

cat $WRITE_IOPS_LOG
cat $WRITE_LAT_AVG_LOG
cat $WRITE_LAT_MIN_LOG
cat $WRITE_LAT_MAX_LOG
