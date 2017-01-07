#!/bin/sh
OUTPUT_PATH=output

READ_IOPS_LOG=${OUTPUT_PATH}/spdk_read_iops.log
READ_LAT_AVG_LOG=${OUTPUT_PATH}/spdk_read_lat_avg.log
READ_LAT_MIN_LOG=${OUTPUT_PATH}/spdk_read_lat_min.log
READ_LAT_MAX_LOG=${OUTPUT_PATH}/spdk_read_lat_max.log

WRITE_IOPS_LOG=${OUTPUT_PATH}/spdk_write_iops.log
WRITE_LAT_AVG_LOG=${OUTPUT_PATH}/spdk_write_lat_avg.log
WRITE_LAT_MIN_LOG=${OUTPUT_PATH}/spdk_write_lat_min.log
WRITE_LAT_MAX_LOG=${OUTPUT_PATH}/spdk_write_lat_max.log

rm $READ_IOPS_LOG
rm $READ_LAT_AVG_LOG
rm $READ_LAT_MIN_LOG
rm $READ_LAT_MAX_LOG

rm $WRITE_IOPS_LOG
rm $WRITE_LAT_AVG_LOG
rm $WRITE_LAT_MIN_LOG
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

	if [ $workload = randread ] ; then 

	    # IOPS
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $3}')
	    echo qdepth $qdepth $ret >> $READ_IOPS_LOG

	    # LAT AVG
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $5}')
	    echo qdepth $qdepth $ret >> $READ_LAT_AVG_LOG
	    
	    # LAT MIN
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $6}')
	    echo qdepth $qdepth $ret >> $READ_LAT_MIN_LOG

	    # LAT MAX
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $7}')
	    echo qdepth $qdepth $ret >> $READ_LAT_MAX_LOG

	elif [ $workload = randwrite ] ; then 

	    # IOPS
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $3}')
	    echo qdepth $qdepth $ret >> $WRITE_IOPS_LOG

	    # LAT AVG
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $5}')
	    echo qdepth $qdepth $ret >> $WRITE_LAT_AVG_LOG
	    
	    # LAT MIN
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $6}')
	    echo qdepth $qdepth $ret >> $WRITE_LAT_MIN_LOG

	    # LAT MAX
	    ret=$(cat ${OUTPUT_PATH}/spdk_q_${qdepth}_block_${block_size}_workload_${workload}.log |  grep Total | awk '{print $7}')
	    echo qdepth $qdepth $ret >> $WRITE_LAT_MAX_LOG
	fi
    done
done

#Device Information                                     :       IOPS       MB/s    Average        min        max
#SKT_AIC_RevB         (8D0B88C761C1        ) from core 0:   13140.40      51.33      76.06       9.81     116.60
#========================================================
#Total                                                  :   13140.40      51.33      76.06       9.81     116.60

cat $READ_IOPS_LOG
cat $READ_LAT_AVG_LOG
cat $READ_LAT_MIN_LOG
cat $READ_LAT_MAX_LOG

cat $WRITE_IOPS_LOG
cat $WRITE_LAT_AVG_LOG
cat $WRITE_LAT_MIN_LOG
cat $WRITE_LAT_MAX_LOG
